// Pickups.hpp — Cyber Rats Co-op, Milestone M4: pickups & objective (host-authoritative).
//
// Self-contained module (no shared CoopCtx). dllmain owns the transport/role/mutex and forwards
// the lifecycle calls below. See docs/sessions/s2.md for the exact dllmain wiring (m4.md is an
// earlier draft of that runbook).
//
// THREADING (mirrors dllmain.cpp exactly):
//   - on_process_event(ctx, fn)  : GAME THREAD (fires from the global ProcessEvent pre-hook).
//                                  ONLY place UObjects are touched (FindAllOf, IsA, K2_*, props).
//   - on_net_tick()              : UE4SS LOOP THREAD. Sockets only. NEVER touch a UObject here.
//   - on_message(frame)          : UE4SS LOOP THREAD (called from dllmain::dispatch). PODs only.
//   Cross-thread data passes through the SHARED std::mutex* handed in via bind(). No UObject* ever
//   crosses the mutex — only uint16_t ids and small PODs. AActor*/UClass*/UFunction* are
//   game-thread-only and are never locked.
//
// Authority: HOST is the sole authority over pickup state + run lifecycle (docs/protocol.md).
//   Host: collects locally (natural game path) -> marks state -> broadcasts PickupStateSync.
//   Client: suppresses local collection (collision disabled), detects its own pickup intent by
//           proximity, sends PickupCollected REQUEST, applies only on host-confirmed StateSync.
//
// NOTE ON MESSAGE STRUCTS: the Msg enum values (0x30/0x31/0x40/0x70/0x71/0x72) exist in
// Net/Protocol.h, but the typed encode/decode structs do NOT, and the task forbids editing
// Protocol.h. So the M4 message PODs live here and (de)serialize with crc::ByteWriter /
// crc::ByteReader, byte-compatible with docs/protocol.md. Promote into Protocol.h in a later pass.
#pragma once

#include "Net/Protocol.h"
#include "Transport/INetTransport.h"

#include <algorithm>   // std::min in PickupStateSyncMsg::encode (don't rely on Protocol.h's transitive include)
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

// Forward-declare the Unreal types we hold so the header has no UE4SS include dependency.
namespace RC::Unreal {
    class UObject;
    class UClass;
    class UFunction;
    class AActor;
}

namespace crc::game {

namespace U = RC::Unreal;

// Deterministic 16-bit pickup id from a maze grid cell + intra-cell slot (docs/protocol.md:
// "id = hash16(cellX, cellY, slot)"). Both peers build the identical maze (M3), so a cheese in a
// given cell hashes to the same id on both machines regardless of local spawn order. FNV-1a based,
// matching the 32-bit fnv1a in Protocol.h / the CRToolkit fingerprint, folded to 16 bits.
inline uint16_t hash16(int32_t cx, int32_t cy, uint8_t slot) {
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) { for (int k = 0; k < 4; ++k) { h ^= (uint8_t)(v >> (8 * k)); h *= 16777619u; } };
    mix((uint32_t)cx); mix((uint32_t)cy);
    h ^= slot; h *= 16777619u;
    return (uint16_t)(h ^ (h >> 16));
}

// ---- M4 wire messages (layouts per docs/protocol.md "Message set") -----------------------------

// 0x30 PickupCollected — ch0 reliable. C->H request, H->C confirm (the StateSync is the confirm,
// but the typed message is retained for the request and for symmetry with the protocol table).
struct PickupCollectedMsg {
    uint16_t pickupId = 0;
    uint8_t  byPlayer = 0;   // 0 = host rat, 1 = client rat
    uint8_t  kind     = 0;   // 0 = cheese, 1 = last-cheese
    uint32_t tick     = 0;

    std::vector<uint8_t> encode() const {
        crc::ByteWriter w; w.u16(pickupId); w.u8(byPlayer); w.u8(kind); w.u32(tick); return w.buf;
    }
    static PickupCollectedMsg decode(const std::vector<uint8_t>& b) {
        crc::ByteReader r(b.data(), b.size()); PickupCollectedMsg m;
        m.pickupId = r.u16(); m.byPlayer = r.u8(); m.kind = r.u8(); m.tick = r.u32(); return m;
    }
};

// 0x31 PickupStateSync — ch2 unreliable (also sent reliably once as the spawn keyframe). H->C.
// body: count u8, then count x [ pickupId u16, state u8 ]. state: 0 = present, 1 = collected.
struct PickupStateSyncMsg {
    struct Entry { uint16_t id = 0; uint8_t state = 0; };
    std::vector<Entry> entries;

    std::vector<uint8_t> encode() const {
        crc::ByteWriter w;
        w.u8((uint8_t)std::min<size_t>(entries.size(), 255));
        for (size_t i = 0; i < entries.size() && i < 255; ++i) { w.u16(entries[i].id); w.u8(entries[i].state); }
        return w.buf;
    }
    static PickupStateSyncMsg decode(const std::vector<uint8_t>& b) {
        crc::ByteReader r(b.data(), b.size()); PickupStateSyncMsg m;
        uint8_t count = r.u8();
        for (uint8_t i = 0; i < count && r.ok; ++i) { Entry e; e.id = r.u16(); e.state = r.u8(); m.entries.push_back(e); }
        return m;
    }
};

// 0x40 ObjectiveReached — ch0 reliable. Both directions (client requests, host confirms/broadcasts).
struct ObjectiveReachedMsg {
    uint16_t objectiveId = 0;
    uint8_t  byPlayer    = 0;

    std::vector<uint8_t> encode() const { crc::ByteWriter w; w.u16(objectiveId); w.u8(byPlayer); return w.buf; }
    static ObjectiveReachedMsg decode(const std::vector<uint8_t>& b) {
        crc::ByteReader r(b.data(), b.size()); ObjectiveReachedMsg m;
        m.objectiveId = r.u16(); m.byPlayer = r.u8(); return m;
    }
};

// 0x70 RunStart — ch0 reliable. H->C.
struct RunStartMsg {
    uint32_t runId       = 0;
    uint64_t seed        = 0;
    uint16_t countdownMs = 0;

    std::vector<uint8_t> encode() const { crc::ByteWriter w; w.u32(runId); w.u64(seed); w.u16(countdownMs); return w.buf; }
    static RunStartMsg decode(const std::vector<uint8_t>& b) {
        crc::ByteReader r(b.data(), b.size()); RunStartMsg m;
        m.runId = r.u32(); m.seed = r.u64(); m.countdownMs = r.u16(); return m;
    }
};

// 0x71 RunEnd — ch0 reliable. H->C.  result: 0 = loss, 1 = win.
struct RunEndMsg {
    uint32_t runId  = 0;
    uint8_t  result = 0;
    uint8_t  reason = 0;

    std::vector<uint8_t> encode() const { crc::ByteWriter w; w.u32(runId); w.u8(result); w.u8(reason); return w.buf; }
    static RunEndMsg decode(const std::vector<uint8_t>& b) {
        crc::ByteReader r(b.data(), b.size()); RunEndMsg m;
        m.runId = r.u32(); m.result = r.u8(); m.reason = r.u8(); return m;
    }
};

// 0x72 Restart — ch0 reliable. H->C.
struct RestartMsg {
    uint32_t newRunId = 0;
    uint64_t newSeed  = 0;

    std::vector<uint8_t> encode() const { crc::ByteWriter w; w.u32(newRunId); w.u64(newSeed); return w.buf; }
    static RestartMsg decode(const std::vector<uint8_t>& b) {
        crc::ByteReader r(b.data(), b.size()); RestartMsg m;
        m.newRunId = r.u32(); m.newSeed = r.u64(); return m;
    }
};

// ------------------------------------------------------------------------------------------------

struct PickupsModule {
    // Called once from dllmain::on_unreal_init AFTER role/transport are set up.
    //   transport  : dllmain's m_transport.get()
    //   role       : dllmain's m_role
    //   playerId   : dllmain's m_playerId (0 host / 1 client)
    //   peerReady  : &dllmain.m_peerReady (link established)
    //   sharedMtx  : &dllmain.m_mtx (the SAME mutex dllmain uses for its snapshots)
    //   getLocalPawn: [this]{ return m_localPawn; } from dllmain — fetch the local rat (game thread)
    void bind(crc::INetTransport* transport, crc::Role role, uint8_t playerId,
              bool* peerReady, std::mutex* sharedMtx,
              std::function<U::AActor*()> getLocalPawn);

    void on_unreal_init();                                       // optional pre-warm; classes also lazy-resolve
    void on_process_event(U::UObject* ctx, U::UFunction* fn);    // GAME THREAD
    void on_net_tick();                                          // LOOP THREAD (sends)
    void on_message(const crc::Frame& fr);                      // LOOP THREAD (inbound dispatch)

    // Called by dllmain from its EXISTING Maze_Generator:ReceiveBeginPlay pre-hook (game thread),
    // right after the shared seed is chosen/forced. Host queues a RunStart for the next net tick;
    // both peers reset their per-run pickup/objective state here.
    void notify_run_begin(uint64_t seed);                        // GAME THREAD

private:
    // ---- game-thread helpers (UObject access only here) ----
    void resolveClassesAndFns(U::UObject* ctx, U::UFunction* fn);
    void enumerateAndAssignIds();
    void onCheeseCollectHook(U::UObject* ctx);
    void onExitOverlap(U::UObject* ctx);
    void onDungeonComplete();
    void drainInboundToWorld();
    void clientProximityScan();
    // Hide+disable (or destroy) a bound pickup actor by id; updates auth state. GAME THREAD.
    void applyCollected(uint16_t id);
    // Mirror the authoritative cheese count onto the generator's "Cheese Amount : Int" (hooks.md) so
    // the HUD updates. GAME THREAD. (Name kept for history; target is the generator, not the GI.)
    void writeCheeseCountToGI();
    // Look up the bound id of a cheese actor (0xFFFF if not bound). GAME THREAD.
    uint16_t idForActor(U::AActor* actor) const;
    bool isHost() const { return m_role == crc::Role::Host; }
    bool isLocalRat(U::UObject* ctx) const;                       // controller-possessed test

    // ---- injected refs (set in bind) ----
    crc::INetTransport*           m_transport = nullptr;
    crc::Role                     m_role      = crc::Role::Unknown;
    uint8_t                       m_playerId  = 0;
    bool*                         m_peerReady = nullptr;
    std::mutex*                   m_mtx       = nullptr;   // SHARED with dllmain
    std::function<U::AActor*()>   m_getLocalPawn;

    // ---- cross-thread state (guarded by *m_mtx) ----
    std::unordered_map<uint16_t, uint8_t> m_pickupState;     // id -> 0 present / 1 collected (host=auth)
    std::vector<PickupCollectedMsg>       m_outReqs;         // client->host collect requests to send
    std::vector<PickupStateSyncMsg>       m_outStateDeltas;  // host->client state syncs (= collect confirm)
    std::vector<ObjectiveReachedMsg>      m_outObjective;    // objective events to send
    std::vector<std::vector<uint8_t>>     m_outRunLifecycle; // pre-packed RunStart/RunEnd/Restart frames
    std::vector<uint16_t>                 m_applyQueue;      // ids the game thread must hide/destroy
    uint8_t                               m_cheeseCount = 0; // authoritative collected count (telemetry)
    bool                                  m_keyframeReady = false; // host: cheese enumerated, send keyframe

    // ---- game-thread-only (NEVER under mutex) ----
    U::UClass*    m_mazeGenClass    = nullptr;
    U::UClass*    m_cheeseClass     = nullptr;
    U::UClass*    m_lastCheeseClass = nullptr;
    U::UClass*    m_exitClass       = nullptr;
    U::UClass*    m_labRatClass     = nullptr;

    U::UFunction* m_spawnCheeseFn   = nullptr;   // H1: generator "Spawn All Cheese"
    U::UFunction* m_dungeonDoneFn   = nullptr;   // H5: generator "Check for Dungeon Complete"
    U::UFunction* m_ratTickFn       = nullptr;   // cadence: LabRat ReceiveTick
    U::UFunction* m_cheeseCollectFn = nullptr;   // H2/H3: resolved by candidate-name probe
    U::UFunction* m_exitOverlapFn   = nullptr;   // H4: BP_EnterExit overlap

    std::unordered_map<uint16_t, U::AActor*> m_idToActor;    // id -> cheese actor (game thread)
    std::unordered_map<U::AActor*, uint16_t> m_actorToId;

    U::UObject*   m_gi              = nullptr;   // cached GameInstance_LabRats_C (holds "Cheese Number Array")
    U::UObject*   m_genActor        = nullptr;   // cached Maze_Generator_C instance (owns "Cheese Amount : Int")

    bool m_cheeseSpawnSeen  = false;
    bool m_cheeseEnumerated = false;
    int  m_enumFrames       = 0;
    bool m_objectiveSent    = false;
    // Hot-path guards: stop doing per-call string work once everything resolvable is resolved.
    // m_collectProbeAttempts bounds the cheese-collect-name probe so it can't run IsA+GetName+N
    // string compares on EVERY ProcessEvent for a cheese-class object for the whole process lifetime
    // when the real collect fn name isn't in the candidate list (the client's proximity path and the
    // host's natural collect both still work without it).
    int  m_collectProbeAttempts = 0;
    bool m_collectProbeGaveUp   = false;
    bool m_runEnded         = false;            // guard so RunEnd fires once per run
    uint64_t m_lastKeyframe = 0;
    uint32_t m_runId        = 1;
};

} // namespace crc::game
