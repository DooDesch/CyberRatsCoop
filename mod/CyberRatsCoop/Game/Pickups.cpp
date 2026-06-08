// Pickups.cpp — Cyber Rats Co-op, Milestone M4: pickups & objective (host-authoritative).
// See Pickups.hpp for the threading contract and authority model.
//
// API patterns copied verbatim from dllmain.cpp (StaticFindObject<UClass*>, FindFirstOf, Cast,
// IsA, GetValuePtrByPropertyName, K2_GetActorLocation, GetFunctionByNameInChain, ProcessEvent,
// TArray Num/GetData, Output::send). The only enumeration primitive not already in dllmain is
// UObjectGlobals::FindAllOf (confirmed present + implemented in
// third_party/RE-UE4SS/deps/first/Unreal: UObjectGlobals.hpp L244, UObjectGlobals.cpp L439).

#include "Game/Pickups.hpp"

#include <DynamicOutput/DynamicOutput.hpp>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/Rotator.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace RC;

namespace crc::game {

namespace {

uint64_t nowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// --- stable cross-machine pickup id (docs/protocol.md: id = hash16(cell, slot)) -----------------
// Quantize world position to a coarse grid to absorb sub-cm FP noise between the two processes,
// then FNV-1a -> low 16 bits. slot distinguishes a co-located last-cheese from a normal cheese.
constexpr double kCell = 100.0;          // 1 m quantization (maze cells are far larger)
constexpr double kPickupRadiusSq = 150.0 * 150.0; // client proximity-collect radius (cm^2)
constexpr uint16_t kExitObjectiveId = 0x0001;     // single shared "reached exit" objective

// Pickup id from quantized cell coords. Delegates to the header's hash16() so the wire id is the
// single source of truth (docs/protocol.md: id = hash16(cellX, cellY, slot)). cellZ is intentionally
// folded into cellY's mix below so co-located-in-XY pickups on different floors still differ.
uint16_t pickupId(int32_t qx, int32_t qy, int32_t qz, uint8_t slot) {
    // Combine the Z cell deterministically into the Y term before hashing (both peers do this).
    // The fold is done in UNSIGNED arithmetic: signed integer overflow is UB in C++, and
    // qy*73856093 overflows int32_t for any non-trivial cell. Unsigned wraparound is well-defined
    // and bit-identical on both peers; we reinterpret the bit pattern back to int32_t purely as the
    // value fed into hash16's FNV byte mix (hash16 consumes the raw bytes; sign is irrelevant).
    uint32_t foldedY = (uint32_t)qy * 73856093u + (uint32_t)qz;
    return crc::game::hash16(qx, (int32_t)foldedY, slot);
}

} // namespace

// ================================================================================================
//  bind / init
// ================================================================================================
void PickupsModule::bind(crc::INetTransport* transport, crc::Role role, uint8_t playerId,
                         bool* peerReady, std::mutex* sharedMtx,
                         std::function<U::AActor*()> getLocalPawn) {
    m_transport    = transport;
    m_role         = role;
    m_playerId     = playerId;
    m_peerReady    = peerReady;
    m_mtx          = sharedMtx;
    m_getLocalPawn = std::move(getLocalPawn);
}

void PickupsModule::on_unreal_init() {
    // Classes also lazy-resolve in resolveClassesAndFns(); nothing required here.
    Output::send<LogLevel::Default>(STR("[CRCoop] M4 pickups module ready (role={})\n"),
                                    (uint32_t)m_role);
}

// ================================================================================================
//  GAME THREAD — on_process_event
// ================================================================================================
void PickupsModule::on_process_event(U::UObject* ctx, U::UFunction* fn) {
    if (!ctx || !fn) return;

    resolveClassesAndFns(ctx, fn);

    // H1: cheese spawn just kicked off (pre-hook fires BEFORE the body — cheese not built yet).
    if (fn == m_spawnCheeseFn) {
        m_cheeseSpawnSeen = true; m_enumFrames = 0; m_cheeseEnumerated = false;
        m_objectiveSent = false;
        Output::send<LogLevel::Default>(STR("[CRCoop] M4: Spawn All Cheese seen; deferring enumeration\n"));
        return;
    }

    // H5: host-only run-complete condition.
    if (fn == m_dungeonDoneFn) { if (isHost()) onDungeonComplete(); return; }

    // H2/H3: a cheese collect fired (host applies+broadcasts; client should not normally reach here
    // because collision is disabled, but we reconcile defensively).
    if (m_cheeseCollectFn && fn == m_cheeseCollectFn) { onCheeseCollectHook(ctx); return; }

    // H4: maze-exit overlap.
    if (m_exitOverlapFn && fn == m_exitOverlapFn) { onExitOverlap(ctx); return; }

    // Cadence is driven off the LabRat tick (same trick dllmain uses for the maze fingerprint).
    if (fn != m_ratTickFn) return;

    // Deferred cheese enumeration a few frames after Spawn All Cheese (actors fully constructed).
    if (m_cheeseSpawnSeen && !m_cheeseEnumerated && ++m_enumFrames > 30) {
        enumerateAndAssignIds();
        m_cheeseEnumerated = true;
    }

    if (!m_cheeseEnumerated) return;

    // Apply any host-confirmed deltas the net thread handed us.
    drainInboundToWorld();

    // Client: detect own pickup intent by proximity and request it.
    if (!isHost()) clientProximityScan();
}

// ================================================================================================
//  GAME THREAD — class/function resolution (cached; pointer-compared in the hot path)
// ================================================================================================
void PickupsModule::resolveClassesAndFns(U::UObject* ctx, U::UFunction* fn) {
    // Fast bail: once every structural fn we hook is cached AND the collect probe is settled, there is
    // nothing left to resolve — return immediately so the global ProcessEvent hot path stays cheap.
    // (Class lookups below only retry while a pointer is still null, mirroring dllmain's pattern.)
    if (m_ratTickFn && m_spawnCheeseFn && m_dungeonDoneFn && m_exitOverlapFn &&
        (m_cheeseCollectFn || m_collectProbeGaveUp))
        return;

    if (!m_mazeGenClass)
        m_mazeGenClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(
            nullptr, nullptr, STR("/Game/Procedural_Maze/Maze_Generator.Maze_Generator_C"));
    if (!m_cheeseClass)
        m_cheeseClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(
            nullptr, nullptr, STR("/Game/Pickups/Chese_Pickup.Chese_Pickup_C"));
    if (!m_lastCheeseClass)
        m_lastCheeseClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(
            nullptr, nullptr, STR("/Game/Pickups/Last_Chese_Pickup.Last_Chese_Pickup_C"));
    if (!m_exitClass)
        m_exitClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(
            nullptr, nullptr, STR("/Game/Pickups/BP_EnterExit.BP_EnterExit_C"));
    if (!m_labRatClass)
        m_labRatClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(
            nullptr, nullptr, STR("/Game/Characters/Lab_Rat/BP_LabRat.BP_LabRat_C"));
    if (!m_gi)
        m_gi = U::UObjectGlobals::FindFirstOf(STR("GameInstance_LabRats_C"));  // holds "Cheese Number Array"
    if (!m_genActor)
        m_genActor = U::UObjectGlobals::FindFirstOf(STR("Maze_Generator_C"));  // owns "Cheese Amount : Int" (hooks.md)

    // Cache UFunctions once by name (then pointer-compare; no per-call string work).
    if (!m_ratTickFn || !m_spawnCheeseFn || !m_dungeonDoneFn || !m_exitOverlapFn) {
        auto name = fn->GetName();
        if (!m_ratTickFn && m_labRatClass && name == STR("ReceiveTick") && ctx->IsA(m_labRatClass))
            m_ratTickFn = fn;
        if (!m_spawnCheeseFn && m_mazeGenClass && name == STR("Spawn All Cheese") && ctx->IsA(m_mazeGenClass))
            m_spawnCheeseFn = fn;
        if (!m_dungeonDoneFn && m_mazeGenClass && name == STR("Check for Dungeon Complete") && ctx->IsA(m_mazeGenClass))
            m_dungeonDoneFn = fn;
        if (!m_exitOverlapFn && m_exitClass && name == STR("ReceiveActorBeginOverlap") && ctx->IsA(m_exitClass))
            m_exitOverlapFn = fn;
    }

    // Cheese collect fn: name not yet dumped (difficulty-0 open = 0 cheese). Probe candidate names
    // on the cheese class once it's known — defensive, same style as "Turn Off Rat" in dllmain.
    // Bounded: after kMaxCollectProbes cheese-class ProcessEvent calls with no match we GIVE UP, so the
    // hot path stops doing IsA+GetName+N string compares forever when the real name isn't a candidate.
    if (!m_cheeseCollectFn && !m_collectProbeGaveUp && m_cheeseClass && ctx->IsA(m_cheeseClass)) {
        static const wchar_t* kCheeseCollectCandidates[] = {
            STR("On Collected"), STR("Collect"), STR("Collected"), STR("Pickup"),
            STR("On Pickup"), STR("Interact"), STR("On Interact"),
        };
        constexpr int kMaxCollectProbes = 4096;   // generous; cheese ProcessEvent calls are infrequent
        auto name = fn->GetName();
        bool matched = false;
        for (const wchar_t* cand : kCheeseCollectCandidates) {
            if (name == cand) {
                m_cheeseCollectFn = fn;
                Output::send<LogLevel::Default>(STR("[CRCoop] M4: cheese collect fn resolved = {}\n"), name);
                matched = true;
                break;
            }
        }
        if (!matched && ++m_collectProbeAttempts >= kMaxCollectProbes) {
            m_collectProbeGaveUp = true;
            Output::send<LogLevel::Default>(STR("[CRCoop] M4: cheese collect fn not in candidate list; "
                                               "relying on host natural-collect + client proximity path\n"));
        }
    }
}

// ================================================================================================
//  GAME THREAD — enumerate cheese actors, assign deterministic ids
// ================================================================================================
void PickupsModule::enumerateAndAssignIds() {
    m_idToActor.clear();
    m_actorToId.clear();

    struct Found { U::AActor* actor; int32_t qx, qy, qz; uint8_t slot; };
    std::vector<Found> found;
    std::unordered_map<U::AActor*, size_t> seen;   // actor -> index in `found` (dedupe)

    // outClass: capture the real UClass* from a live instance so kind/IsA checks work even if the
    // guessed /Game/Pickups/... path in resolveClassesAndFns() is wrong (hooks.md only confirms the
    // short class NAMES, not the package paths — see runbook "must verify in-game").
    // NOTE: FindAllOf matches SUBCLASSES (it walks the super-struct chain — verified in
    // UObjectGlobals.cpp FindAllOf). If Last_Chese_Pickup_C derives from Chese_Pickup_C, the
    // base-name query also returns last-cheese instances, so we MUST dedupe by actor and let the
    // more-specific slot (last-cheese, collected second) win — otherwise the last cheese gets two
    // ids on both peers.
    auto collect = [&](const wchar_t* className, uint8_t slot, U::UClass** outClass) {
        if (!className) return;
        std::vector<U::UObject*> objs;
        U::UObjectGlobals::FindAllOf(className, objs);
        for (auto* o : objs) {
            auto* a = U::Cast<U::AActor>(o);
            if (!a) continue;
            // Path-independent class capture, but ONLY from an instance whose class name EXACTLY
            // matches the queried name — FindAllOf also returns subclasses, so an unguarded capture
            // could store the subclass as the base class.
            U::UClass* ac = a->GetClassPrivate();
            if (outClass && !*outClass && ac && ac->GetName() == className) *outClass = ac;
            auto dup = seen.find(a);
            if (dup != seen.end()) { found[dup->second].slot = slot; continue; }  // re-tag, no duplicate
            U::FVector l = a->K2_GetActorLocation();
            Found f;
            f.actor = a;
            f.qx = (int32_t)std::lround(l.GetX() / kCell);
            f.qy = (int32_t)std::lround(l.GetY() / kCell);
            f.qz = (int32_t)std::lround(l.GetZ() / kCell);
            f.slot = slot;
            seen.emplace(a, found.size());
            found.push_back(f);
        }
    };
    collect(STR("Chese_Pickup_C"), 0, &m_cheeseClass);
    collect(STR("Last_Chese_Pickup_C"), 1, &m_lastCheeseClass);

    // Canonical order so BOTH peers resolve hash collisions identically (sort by quantized cell+slot).
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        if (a.qx != b.qx) return a.qx < b.qx;
        if (a.qy != b.qy) return a.qy < b.qy;
        if (a.qz != b.qz) return a.qz < b.qz;
        return a.slot < b.slot;
    });

    for (auto& f : found) {
        uint16_t id = pickupId(f.qx, f.qy, f.qz, f.slot);
        // Deterministic collision perturbation (identical on both peers because `found` is sorted).
        // MUST advance monotonically: a fixed-XOR nudge (`id ^= 0x9E37`) only toggles between TWO
        // values, so a 3-way id collision makes this loop spin forever and HANGS THE GAME THREAD.
        // Adding an odd step instead walks all 65536 ids before repeating; the `guard` bounds the
        // (practically impossible) fully-saturated table so the loop can never wedge.
        for (int guard = 0; m_idToActor.find(id) != m_idToActor.end() && guard < 0x10000; ++guard)
            id = (uint16_t)(id + 0x9E37u); // odd increment -> full-period walk over uint16
        m_idToActor[id]      = f.actor;
        m_actorToId[f.actor] = id;
    }

    // Seed shared state with all-present and, on the client, disable collision (suppression, §4).
    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        for (auto& kv : m_idToActor) {
            if (m_pickupState.find(kv.first) == m_pickupState.end()) m_pickupState[kv.first] = 0;
        }
        if (isHost()) m_keyframeReady = true;   // tell net thread to push the initial keyframe
    }

    if (!isHost()) {
        for (auto& kv : m_idToActor) {
            kv.second->SetActorEnableCollision(false);  // client never collects locally; requests instead
        }
    }

    Output::send<LogLevel::Default>(STR("[CRCoop] M4: enumerated {} cheese pickups (role={})\n"),
                                    (uint32_t)m_idToActor.size(), (uint32_t)m_role);
}

// ================================================================================================
//  GAME THREAD — host cheese collected via the natural game path (H2/H3)
// ================================================================================================
void PickupsModule::onCheeseCollectHook(U::UObject* ctx) {
    if (!m_cheeseEnumerated) return;            // can't have collected what we haven't mapped
    auto* actor = U::Cast<U::AActor>(ctx);
    if (!actor) return;
    auto it = m_actorToId.find(actor);
    if (it == m_actorToId.end()) return;
    uint16_t id = it->second;
    uint8_t  kind = (m_lastCheeseClass && actor->IsA(m_lastCheeseClass)) ? 1 : 0;

    if (!isHost()) return;                      // client never finalizes locally (handled by request path)

    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        auto st = m_pickupState.find(id);
        if (st != m_pickupState.end() && st->second == 0) {
            st->second = 1;
            ++m_cheeseCount;
            changed = true;
            // Queue a delta broadcast (the StateSync IS the client's confirm).
            PickupStateSyncMsg sync; sync.entries.push_back({ id, 1 });
            m_outStateDeltas.push_back(std::move(sync));
        }
    }
    if (changed) {
        writeCheeseCountToGI();   // host updates its own HUD immediately (game thread)
        Output::send<LogLevel::Default>(STR("[CRCoop] M4: host collected cheese id=0x{:04X} kind={}\n"),
                                        (uint32_t)id, (uint32_t)kind);
    }
}

// ================================================================================================
//  GAME THREAD — maze-exit overlap (H4)
// ================================================================================================
void PickupsModule::onExitOverlap(U::UObject* /*ctx*/) {
    if (m_objectiveSent) return;
    // Only the LOCAL player's overlap should count; the puppet has its own logic disabled, so an
    // overlap event here is the local rat reaching the exit. Host confirms; client requests.
    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        ObjectiveReachedMsg obj; obj.objectiveId = kExitObjectiveId; obj.byPlayer = m_playerId;
        m_outObjective.push_back(obj);
    }
    m_objectiveSent = true;
    Output::send<LogLevel::Default>(STR("[CRCoop] M4: exit reached (byPlayer={}) -> ObjectiveReached\n"),
                                    (uint32_t)m_playerId);
}

// ================================================================================================
//  GAME THREAD — host run-complete (H5)
// ================================================================================================
void PickupsModule::onDungeonComplete() {
    // Host trusts the game's own win check. `Check for Dungeon Complete` can fire many times per run
    // (polled), so guard with m_runEnded to emit exactly one RunEnd(win) per run.
    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        if (m_runEnded) return;
        m_runEnded = true;
        RunEndMsg re; re.runId = m_runId; re.result = 1; re.reason = 0;
        auto f = crc::packFrame(crc::Chan::Control, crc::Msg::RunEnd, re.encode());
        m_outRunLifecycle.push_back(std::move(f));
    }
    Output::send<LogLevel::Default>(STR("[CRCoop] M4: dungeon complete -> RunEnd(win) runId={}\n"),
                                    (uint32_t)m_runId);
}

// ================================================================================================
//  GAME THREAD — apply host-confirmed state to the world (hide/destroy collected cheese)
// ================================================================================================
void PickupsModule::drainInboundToWorld() {
    std::vector<uint16_t> ids;
    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        if (m_applyQueue.empty()) return;
        ids.swap(m_applyQueue);                 // copy-out under lock, then act with lock RELEASED
    }
    for (uint16_t id : ids) {
        auto it = m_idToActor.find(id);
        if (it == m_idToActor.end() || !it->second) continue;
        U::AActor* a = it->second;              // game-thread-only map; safe here
        a->SetActorHiddenInGame(true);          // idempotent (hide-already-hidden is a no-op)
        a->SetActorEnableCollision(false);
        // a->K2_DestroyActor();                // alternative: destroy outright (kept hidden for safety)
        Output::send<LogLevel::Default>(STR("[CRCoop] M4: applied collected cheese id=0x{:04X}\n"),
                                        (uint32_t)id);
    }
    writeCheeseCountToGI();                      // converge the HUD count to the authoritative total
}

// Mirror the authoritative cheese count so the local HUD updates. GAME THREAD only (touches a UObject).
// hooks.md is explicit: "Cheese Amount : Int" lives on the Maze_Generator_C instance (under "Counts
// (read)"), NOT on the GameInstance — the GI holds "Cheese Number Array" (per-level). Writing
// "Cheese Amount" onto the GI (the previous target) silently no-ops because the property is absent
// there. So we write the generator's "Cheese Amount" as the primary target; the guarded write is a
// no-op if the property is somehow absent, and the host's 1 Hz keyframe converges both peers anyway.
// Which object the HUD actually reads is an in-game-verify item (see runbook) — if it reads a GI field,
// confirm the real GI property name and add it here.
//
// The count is DERIVED from m_pickupState (number of ids in state==1) rather than from m_cheeseCount,
// because m_cheeseCount is only incremented on the HOST. The client mirrors per-id state via
// PickupStateSync but never tracks a running count, so reading m_cheeseCount on the client would
// always write 0. Counting collected entries here makes BOTH peers converge to the same HUD value.
void PickupsModule::writeCheeseCountToGI() {
    int32_t count = 0;
    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        for (auto& kv : m_pickupState) if (kv.second == 1) ++count;  // 1 == collected (2 == client-pending)
    }
    // Primary: the generator's "Cheese Amount : Int" (hooks.md owner of this property).
    // WARNING (in-game verify, load-bearing): hooks.md lists generator "Cheese Amount" under
    // *Counts (read)*, so it is most likely the SPAWNED TOTAL, not a collected counter. If the host's
    // `Check for Dungeon Complete` compares collected-vs-"Cheese Amount", overwriting it with our
    // collected count would corrupt the host's own win check. Confirm on a real run which field the HUD
    // reads; if "Cheese Amount" is the spawn-total, retarget this write to the true collected field and
    // only read the spawn-total here.
    if (m_genActor) {
        if (auto* p = m_genActor->GetValuePtrByPropertyNameInChain<int32_t>(STR("Cheese Amount"))) *p = count;
    }
}

// ================================================================================================
//  GAME THREAD — client proximity-collect intent (primary suppression mechanic, §4)
// ================================================================================================
void PickupsModule::clientProximityScan() {
    U::AActor* pawn = m_getLocalPawn ? m_getLocalPawn() : nullptr;
    if (!pawn) return;
    U::FVector pl = pawn->K2_GetActorLocation();

    // Find nearest still-present cheese within radius that we haven't already requested.
    uint16_t bestId = 0; double bestSq = kPickupRadiusSq; bool have = false; U::AActor* bestActor = nullptr;
    for (auto& kv : m_idToActor) {
        uint16_t id = kv.first; U::AActor* a = kv.second;
        bool present;
        {
            std::lock_guard<std::mutex> lk(*m_mtx);
            auto st = m_pickupState.find(id);
            present = (st == m_pickupState.end()) || (st->second == 0);
        }
        if (!present || !a) continue;
        U::FVector cl = a->K2_GetActorLocation();
        double dx = cl.GetX() - pl.GetX(), dy = cl.GetY() - pl.GetY(), dz = cl.GetZ() - pl.GetZ();
        double sq = dx * dx + dy * dy + dz * dz;
        if (sq < bestSq) { bestSq = sq; bestId = id; bestActor = a; have = true; }
    }
    if (!have) return;

    uint8_t kind = (m_lastCheeseClass && bestActor->IsA(m_lastCheeseClass)) ? 1 : 0;
    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        // Mark present->collected locally as "requested" so we don't spam the same id every tick.
        auto st = m_pickupState.find(bestId);
        if (st != m_pickupState.end() && st->second != 0) return; // already requested/collected
        if (st != m_pickupState.end()) st->second = 2;            // 2 = locally-requested (pending host)
        PickupCollectedMsg req;
        req.pickupId = bestId; req.byPlayer = m_playerId; req.kind = kind; req.tick = (uint32_t)nowMs();
        m_outReqs.push_back(req);
    }
    Output::send<LogLevel::Default>(STR("[CRCoop] M4: client request collect id=0x{:04X} kind={}\n"),
                                    (uint32_t)bestId, (uint32_t)kind);
}

// ================================================================================================
//  LOOP THREAD — on_net_tick (sends only; NO UObject access)
// ================================================================================================
void PickupsModule::on_net_tick() {
    if (!m_transport || !m_peerReady || !*m_peerReady) return;

    std::vector<PickupCollectedMsg>   reqs;
    std::vector<PickupStateSyncMsg>   deltas;
    std::vector<ObjectiveReachedMsg>  objectives;
    std::vector<std::vector<uint8_t>> lifecycle;
    bool sendKeyframe = false;
    PickupStateSyncMsg keyframe;
    uint64_t t = nowMs();

    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        reqs.swap(m_outReqs);
        deltas.swap(m_outStateDeltas);
        objectives.swap(m_outObjective);
        lifecycle.swap(m_outRunLifecycle);

        // Host: initial keyframe right after enumeration, then a 1 Hz full keyframe for resync.
        if (isHost() && (m_keyframeReady || t - m_lastKeyframe >= 1000)) {
            for (auto& kv : m_pickupState) {
                keyframe.entries.push_back({ kv.first, (uint8_t)(kv.second == 1 ? 1 : 0) });
            }
            sendKeyframe = m_keyframeReady || !keyframe.entries.empty();
            m_keyframeReady = false;
            m_lastKeyframe  = t;
        }
    }

    // Client -> host collect requests (reliable, control).
    for (auto& r : reqs) {
        auto f = crc::packFrame(crc::Chan::Control, crc::Msg::PickupCollected, r.encode());
        m_transport->send(f.data(), f.size(), true, (uint8_t)crc::Chan::Control);
    }
    // Host -> client state deltas. PickupStateSync is ch2 (WorldState) per docs/protocol.md; send the
    // single-collect delta RELIABLY (reliability flag is independent of the channel index) so a
    // collect is never lost. The transport channel index MUST match the framed channel.
    for (auto& d : deltas) {
        auto f = crc::packFrame(crc::Chan::WorldState, crc::Msg::PickupStateSync, d.encode());
        m_transport->send(f.data(), f.size(), true, (uint8_t)crc::Chan::WorldState);
    }
    // Objective events (reliable, control).
    for (auto& o : objectives) {
        auto f = crc::packFrame(crc::Chan::Control, crc::Msg::ObjectiveReached, o.encode());
        m_transport->send(f.data(), f.size(), true, (uint8_t)crc::Chan::Control);
    }
    // Run lifecycle frames (already packed; reliable, control).
    for (auto& f : lifecycle) {
        m_transport->send(f.data(), f.size(), true, (uint8_t)crc::Chan::Control);
    }
    // Host periodic keyframe (unreliable world-state lane).
    if (sendKeyframe) {
        auto f = crc::packFrame(crc::Chan::WorldState, crc::Msg::PickupStateSync, keyframe.encode());
        m_transport->send(f.data(), f.size(), false, (uint8_t)crc::Chan::WorldState);
    }
}

// ================================================================================================
//  LOOP THREAD — on_message (inbound dispatch; PODs only, NO UObject access)
// ================================================================================================
void PickupsModule::on_message(const crc::Frame& fr) {
    using crc::Msg;
    switch (fr.type) {

    case Msg::PickupCollected: {            // host receives a client request
        if (!isHost()) return;
        auto req = PickupCollectedMsg::decode(fr.body);
        std::lock_guard<std::mutex> lk(*m_mtx);
        auto st = m_pickupState.find(req.pickupId);
        if (st != m_pickupState.end() && st->second == 0) {
            st->second = 1; ++m_cheeseCount;     // validate + finalize on the authority
            PickupStateSyncMsg sync; sync.entries.push_back({ req.pickupId, 1 });
            m_outStateDeltas.push_back(std::move(sync));   // confirm = state sync
            Output::send<LogLevel::Default>(STR("[CRCoop] M4: host confirmed client collect id=0x{:04X}\n"),
                                            (uint32_t)req.pickupId);
        } else {
            // Already collected / unknown -> push current state so the client reconciles.
            PickupStateSyncMsg sync;
            uint8_t cur = (st != m_pickupState.end()) ? (uint8_t)(st->second == 1 ? 1 : 0) : 1;
            sync.entries.push_back({ req.pickupId, cur });
            m_outStateDeltas.push_back(std::move(sync));
        }
        break;
    }

    case Msg::PickupStateSync: {            // client receives authoritative state
        if (isHost()) return;
        auto sync = PickupStateSyncMsg::decode(fr.body);
        std::lock_guard<std::mutex> lk(*m_mtx);
        for (auto& e : sync.entries) {
            uint8_t prev = m_pickupState.count(e.id) ? m_pickupState[e.id] : 0;
            m_pickupState[e.id] = e.state;          // mirror authority (0 present / 1 collected)
            if (e.state == 1 && prev != 1) m_applyQueue.push_back(e.id);  // game thread hides it
        }
        break;
    }

    case Msg::ObjectiveReached: {
        auto obj = ObjectiveReachedMsg::decode(fr.body);
        Output::send<LogLevel::Default>(STR("[CRCoop] M4: ObjectiveReached id=0x{:04X} byPlayer={}\n"),
                                        (uint32_t)obj.objectiveId, (uint32_t)obj.byPlayer);
        // Host: if a client reached the exit, re-broadcast as authoritative confirm (idempotent).
        if (isHost()) {
            std::lock_guard<std::mutex> lk(*m_mtx);
            m_outObjective.push_back(obj);
        }
        break;
    }

    case Msg::RunStart: {
        auto m = RunStartMsg::decode(fr.body);
        Output::send<LogLevel::Default>(STR("[CRCoop] M4: RunStart runId={} countdown={}ms\n"),
                                        (uint32_t)m.runId, (uint32_t)m.countdownMs);
        m_runId = m.runId;
        break;
    }
    case Msg::RunEnd: {
        auto m = RunEndMsg::decode(fr.body);
        Output::send<LogLevel::Default>(STR("[CRCoop] M4: RunEnd runId={} result={} reason={}\n"),
                                        (uint32_t)m.runId, (uint32_t)m.result, (uint32_t)m.reason);
        break;
    }
    case Msg::Restart: {
        auto m = RestartMsg::decode(fr.body);
        Output::send<LogLevel::Default>(STR("[CRCoop] M4: Restart newRunId={}\n"), (uint32_t)m.newRunId);
        m_runId = m.newRunId;
        // On restart, drop pickup state so the next maze re-enumerates cleanly (game thread rebuilds).
        std::lock_guard<std::mutex> lk(*m_mtx);
        m_pickupState.clear(); m_applyQueue.clear(); m_cheeseCount = 0; m_keyframeReady = false;
        break;
    }

    default: break;
    }
}

// ================================================================================================
//  GAME THREAD — run-begin notification (from dllmain's Maze_Generator:ReceiveBeginPlay pre-hook)
// ================================================================================================
// Called by dllmain right after the shared seed is chosen/forced for a new run (game thread). Resets
// per-run pickup/objective state on BOTH peers; the host additionally queues a RunStart for the next
// net tick. Dropping the id<->actor maps forces a clean rebind against the freshly generated maze.
void PickupsModule::notify_run_begin(uint64_t seed) {
    m_idToActor.clear();
    m_actorToId.clear();
    m_cheeseSpawnSeen  = false;
    m_cheeseEnumerated = false;
    m_enumFrames       = 0;
    m_objectiveSent    = false;
    // Re-arm the collect-fn probe budget for the new run (cached UFunction*/UClass* pointers are
    // stable across runs and are intentionally NOT cleared). A difficulty-0 open may have exhausted
    // the budget with no cheese present; a real run should get a fresh chance to resolve the fn.
    if (!m_cheeseCollectFn) { m_collectProbeAttempts = 0; m_collectProbeGaveUp = false; }

    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        m_pickupState.clear();
        m_applyQueue.clear();
        m_cheeseCount   = 0;
        m_keyframeReady = false;
        m_lastKeyframe  = 0;
        m_runEnded      = false;
        if (isHost()) {
            ++m_runId;
            RunStartMsg rs; rs.runId = m_runId; rs.seed = seed; rs.countdownMs = 0;
            m_outRunLifecycle.push_back(
                crc::packFrame(crc::Chan::Control, crc::Msg::RunStart, rs.encode()));
        }
    }
    Output::send<LogLevel::Default>(STR("[CRCoop] M4: run begin (runId={}, seed={})\n"),
                                    (uint32_t)m_runId, (uint32_t)seed);
}

// ================================================================================================
//  GAME THREAD — small helpers
// ================================================================================================
// Hide+disable, then destroy, a bound pickup by id (host-confirmed removal). Idempotent: a missing or
// already-cleared id is a no-op. Never keeps a destroyed actor pointer.
void PickupsModule::applyCollected(uint16_t id) {
    auto it = m_idToActor.find(id);
    if (it == m_idToActor.end() || !it->second) return;
    U::AActor* a = it->second;
    a->SetActorHiddenInGame(true);          // hide first so there's no one-frame flicker/overlap
    a->SetActorEnableCollision(false);
    a->K2_DestroyActor();                   // remove it the same way the game does
    m_actorToId.erase(a);
    it->second = nullptr;
}

uint16_t PickupsModule::idForActor(U::AActor* actor) const {
    auto it = m_actorToId.find(actor);
    return (it == m_actorToId.end()) ? (uint16_t)0xFFFF : it->second;
}

bool PickupsModule::isLocalRat(U::UObject* ctx) const {
    auto* rat = U::Cast<U::AActor>(ctx);
    if (!rat) return false;
    if (m_labRatClass && !rat->IsA(m_labRatClass)) return false;
    auto** ctrl = rat->GetValuePtrByPropertyNameInChain<U::UObject*>(STR("Controller"));
    return ctrl && *ctrl;
}

} // namespace crc::game
