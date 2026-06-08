// Game/MazeModule.cpp — Cyber Rats Co-op, milestone S1 (M3 full validation).
// See MazeModule.hpp for the threading contract. All UObject access in this file is reached ONLY
// through on_process_event (game thread); on_net_tick / on_message touch POD under *m_mtx only.

#include "Game/MazeModule.hpp"
#include "Transport/INetTransport.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>

using namespace RC;
namespace U = RC::Unreal;

namespace {

// CharType == wchar_t on Windows (namespace RC); STR() yields CharType string literals. Use it
// rather than a bare TCHAR (which lives in RC::Unreal and isn't pulled into this scope).
using RC::CharType;

// Blueprint object paths (verified in docs/hooks.md). Spaced display names are accepted by UE4SS.
constexpr const CharType* kMazeGenPath = STR("/Game/Procedural_Maze/Maze_Generator.Maze_Generator_C");
constexpr const CharType* kLabRatPath  = STR("/Game/Characters/Lab_Rat/BP_LabRat.BP_LabRat_C");

// UFunction display names targeted on the cached classes.
constexpr const CharType* kFnBeginPlay  = STR("ReceiveBeginPlay");
constexpr const CharType* kFnRatTick    = STR("ReceiveTick");
constexpr const CharType* kFnSetupDiff  = STR("Setup Dungeon Difficulty");

// GameInstance properties (docs/hooks.md).
constexpr const CharType* kPropSeed     = STR("Random Seed Roll");
constexpr const CharType* kPropRatId    = STR("Selected LabRat ID");

// Generator properties read for the param cross-check (all Int per docs/hooks.md).
constexpr const CharType* kPropRoomAmt  = STR("Spawned Room Amount");
constexpr const CharType* kPropBiome    = STR("Biome Enum");

// Frames to wait after ReceiveBeginPlay before fingerprinting (gen ~96ms + async spawns; ~2s@60fps).
constexpr int kSettleFrames = 120;

// Room-actor class-name heuristic (docs/sessions.md S1 + docs/hooks.md): spawned maze rooms are
// biome-specific classes prefixed "MazeRunner_Biome_" or the base "Master_Maze_Room". Matched once
// on the literal class name, then the UClass* is cached and IsA-tested in the hot loop.
bool classNameLooksLikeRoom(const RC::StringType& n) {
    return n.rfind(STR("MazeRunner_Biome_"), 0) == 0   // starts-with
        || n.rfind(STR("Master_Maze_Room"), 0) == 0;
}

// FNV-1a over a small POD blob (mirrors crc::fnv1a but local so we can fold ints without a buffer).
uint32_t fnvFold(uint32_t h, const int32_t* vals, int count) {
    for (int c = 0; c < count; ++c)
        for (int k = 0; k < 4; ++k) { h ^= (uint8_t)((uint32_t)vals[c] >> (8 * k)); h *= 16777619u; }
    return h;
}

} // namespace

namespace crc {

// ---------------------------------------------------------------------------------------------
// bind / configure / init
// ---------------------------------------------------------------------------------------------
void MazeModule::bind(INetTransport* transport, Role role, uint8_t playerId,
                      const bool* peerReady, std::mutex* mtx) {
    m_transport = transport;
    m_role = role;
    m_playerId = playerId;
    m_peerReady = peerReady;
    m_mtx = mtx;
}

void MazeModule::configure(int32_t forceSeed, bool regenOnLateSeed) {
    m_forceSeed = forceSeed;
    m_regenOnLateSeed = regenOnLateSeed;
}

void MazeModule::on_unreal_init() {
    // Classes may not exist yet at init (Maze_LVL not loaded); resolution is also done lazily in
    // on_process_event. This is a best-effort early cache.
    if (!m_mazeGenClass)
        m_mazeGenClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(nullptr, nullptr, kMazeGenPath);
    if (!m_labRatClass)
        m_labRatClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(nullptr, nullptr, kLabRatPath);
    Output::send<LogLevel::Default>(STR("[CRMaze] init (role={}, forceSeed={}, regenOnLateSeed={})\n"),
                                    (int)m_role, m_forceSeed, m_regenOnLateSeed ? 1 : 0);
}

// ---------------------------------------------------------------------------------------------
// GAME THREAD — global ProcessEvent pre-hook dispatch
// ---------------------------------------------------------------------------------------------
void MazeModule::on_process_event(U::UObject* ctx, U::UFunction* fn) {
    if (!ctx || !fn) return;

    // Lazy-resolve target classes (Maze_LVL may load after init).
    if (!m_mazeGenClass)
        m_mazeGenClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(nullptr, nullptr, kMazeGenPath);
    if (!m_labRatClass)
        m_labRatClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(nullptr, nullptr, kLabRatPath);

    // Cache the UFunctions we care about, then pointer-compare (avoids per-call string work).
    if (!m_genBeginFn || !m_ratTickFn || !m_setupDiffFn) {
        auto name = fn->GetName();
        if (!m_genBeginFn && m_mazeGenClass && ctx->IsA(m_mazeGenClass) && name == kFnBeginPlay) m_genBeginFn = fn;
        if (!m_setupDiffFn && m_mazeGenClass && ctx->IsA(m_mazeGenClass) && name == kFnSetupDiff) m_setupDiffFn = fn;
        if (!m_ratTickFn  && m_labRatClass  && ctx->IsA(m_labRatClass)  && name == kFnRatTick)   m_ratTickFn = fn;
    }

    // Seed inject / gating — pre-hook runs BEFORE the generation ubergraph.
    if (fn == m_genBeginFn) { onMazeGenBegin(ctx); return; }

    // Capture resolved difficulty/level params for the cross-check (runs with the real values).
    if (m_setupDiffFn && fn == m_setupDiffFn) { captureParamHash(ctx); return; }

    // Everything below piggy-backs on a LabRat tick.
    if (fn != m_ratTickFn) return;

    // Client safety net: if gen ran before the seed and the seed has now arrived, self-heal once.
    if (m_genRanUnseeded && !m_lateRegenDone) {
        bool have;
        { std::lock_guard<std::mutex> lk(*m_mtx); have = m_haveNetSeed && m_lateSeedArrived; }
        if (have) tryRegenIfLateSeed();
    }

    // Delayed, once-per-run fingerprint compute.
    if (m_genSeen && !m_fpComputed && ++m_genFrames > kSettleFrames) maybeComputeFingerprint();
}

// ---------------------------------------------------------------------------------------------
// GAME THREAD — Maze_Generator:ReceiveBeginPlay pre-hook (before generation runs)
// ---------------------------------------------------------------------------------------------
void MazeModule::onMazeGenBegin(U::UObject* generator) {
    // Reset per-run latches (game thread).
    m_genSeen = true; m_genFrames = 0; m_fpComputed = false;
    m_genRanUnseeded = false; m_lateRegenDone = false;

    // Reset the net-facing validation latches so a SECOND real run in the same session re-validates
    // cleanly instead of reporting a stale MAZE MATCH (S1 scope is one run; full run lifecycle = S2).
    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        m_readyToSend = false; m_mazeReadySent = false;
        m_havePeerHash = false; m_peerHash = 0;
        m_localHash = 0; m_localRooms = 0;
        m_verdictLogged = false;
    }

    // Pick the seed: a forced test seed wins, otherwise the host's net seed (client), otherwise the
    // game-rolled value (host reads it below from the GameInstance).
    int32_t seed = m_forceSeed;
    bool haveNet; uint64_t netSeed; uint32_t netParam;
    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        haveNet = m_haveNetSeed; netSeed = m_netSeed; netParam = m_netParamHash;
    }

    auto* gi = U::UObjectGlobals::FindFirstOf(STR("GameInstance_LabRats_C"));

    if (m_role == Role::Host) {
        // HOST = authority. Read the game-rolled seed (the menu flow already rolled it) and broadcast
        // the REAL value so a genuine run is reproduced on the client. A forced test seed overrides.
        int32_t rolled = 0;
        if (gi) if (auto* p = gi->GetValuePtrByPropertyName<int32_t>(kPropSeed)) rolled = *p;
        if (m_forceSeed >= 0) { rolled = m_forceSeed; applySeed(rolled); }   // test mode: force GI too
        seed = rolled;

        // Build the param cross-check (difficulty params + selected rat id) from what we can read now.
        captureParamHash(generator);
        uint32_t paramHash = m_capturedParamHash;

        {
            std::lock_guard<std::mutex> lk(*m_mtx);
            m_hostSeedToSend = (uint64_t)(uint32_t)seed;
            m_hostParamHash = paramHash;
            m_hostSeedPending = true;   // net thread broadcasts it (no socket I/O here)
        }
        Output::send<LogLevel::Default>(STR("[CRMaze] host run start: seed={} paramHash=0x{:08X}\n"),
                                        seed, paramHash);
        return;  // generation proceeds with the host's own rolled seed
    }

    // CLIENT — never generate with its own rolled seed.
    if (m_forceSeed >= 0) {
        // Test override (lets a fixed seed be validated even without the host).
        seed = m_forceSeed;
    } else if (haveNet) {
        seed = (int32_t)netSeed;
    } else {
        // Late-seed race: the reliable MazeSeed has not landed. We cannot cancel ProcessEvent, so we
        // let generation run with the local seed, flag it, and self-heal when MazeSeed arrives.
        m_genRanUnseeded = true;
        Output::send<LogLevel::Warning>(STR("[CRMaze] WARN client gen ran BEFORE seed arrived; will self-heal on MazeSeed\n"));
        captureParamHash(generator);
        return;
    }

    applySeed(seed);
    Output::send<LogLevel::Default>(STR("[CRMaze] client forced maze seed = {}\n"), seed);

    // Cross-check the param hash the host sent against what we resolve locally.
    captureParamHash(generator);
    if (haveNet && netParam != 0 && netParam != m_capturedParamHash) {
        Output::send<LogLevel::Warning>(
            STR("[CRMaze] WARN paramHash mismatch local=0x{:08X} net=0x{:08X} (difficulty/level differ?)\n"),
            m_capturedParamHash, netParam);
    }
}

// Write the shared seed into the GameInstance BEFORE generation consumes it.
void MazeModule::applySeed(int32_t seed) {
    auto* gi = U::UObjectGlobals::FindFirstOf(STR("GameInstance_LabRats_C"));
    if (!gi) { Output::send<LogLevel::Error>(STR("[CRMaze] GameInstance not found; cannot apply seed\n")); return; }
    if (auto* p = gi->GetValuePtrByPropertyName<int32_t>(kPropSeed)) *p = seed;
    else Output::send<LogLevel::Error>(STR("[CRMaze] '{}' prop not found\n"), kPropSeed);
}

// Fold difficulty/level params + selected rat id into a 32-bit cross-check hash. Read whatever is
// available now; the host and client read the same sources so an equal seed yields an equal hash.
void MazeModule::captureParamHash(U::UObject* generator) {
    int32_t vals[4] = {0, 0, 0, 0};
    if (generator) {
        if (auto* p = generator->GetValuePtrByPropertyName<int32_t>(kPropRoomAmt)) vals[0] = *p;
        if (auto* p = generator->GetValuePtrByPropertyName<uint8_t>(kPropBiome))   vals[1] = (int32_t)*p;
    }
    if (auto* gi = U::UObjectGlobals::FindFirstOf(STR("GameInstance_LabRats_C"))) {
        if (auto* p = gi->GetValuePtrByPropertyName<int32_t>(kPropRatId)) vals[2] = *p;
        if (auto* p = gi->GetValuePtrByPropertyName<int32_t>(kPropSeed))  vals[3] = *p;
    }
    m_capturedParamHash = fnvFold(2166136261u, vals, 4);
}

// ---------------------------------------------------------------------------------------------
// GAME THREAD — fingerprint over actually-spawned room actors
// ---------------------------------------------------------------------------------------------
bool MazeModule::isRoomActor(U::UObject* obj) {
    if (m_roomClass && obj->IsA(m_roomClass)) return true;
    // Not yet matched against the cached class: test by class name and cache the class on first hit.
    auto* cls = obj->GetClassPrivate();
    if (!cls) return false;
    RC::StringType cn = cls->GetName();
    if (!classNameLooksLikeRoom(cn)) return false;
    if (!m_roomClass) {
        m_roomClass = cls;   // cache the first matched room class for cheap IsA-tests afterwards
        Output::send<LogLevel::Default>(STR("[CRMaze] room class matched: {}\n"), cn);
    }
    return true;
}

uint32_t MazeModule::computeRoomFingerprint(uint32_t* outCount) {
    // Anchor to the local pawn's world so we only fingerprint rooms in the live maze level.
    U::UWorld* myWorld = nullptr;
    if (m_labRatClass) {
        if (auto* pawn = U::UObjectGlobals::FindFirstOf(STR("BP_LabRat_C")))
            myWorld = pawn->GetWorld();
    }

    uint32_t combined = 0; uint32_t count = 0;
    U::UObjectGlobals::ForEachUObject([&](U::UObject* obj, U::int32 /*idx*/, U::int32 /*chunk*/) -> RC::LoopAction {
        if (!obj) return RC::LoopAction::Continue;
        // Must be a spawned actor (skips CDOs / non-actors / the puppet's components).
        auto* room = U::Cast<U::AActor>(obj);
        if (!room) return RC::LoopAction::Continue;
        // Same world as the local maze (cheap, robust against per-process pointer/name differences).
        if (myWorld && room->GetWorld() != myWorld) return RC::LoopAction::Continue;
        // Class-name heuristic (+ cached UClass IsA after the first match).
        if (!isRoomActor(obj)) return RC::LoopAction::Continue;

        // Quantize world position to integer cm and FNV-1a hash, then XOR-combine (order-independent,
        // so local spawn-order differences between peers do not change the result).
        U::FVector l = room->K2_GetActorLocation();
        int32_t coords[3] = { (int32_t)l.GetX(), (int32_t)l.GetY(), (int32_t)l.GetZ() };
        combined ^= fnvFold(2166136261u, coords, 3);
        ++count;
        return RC::LoopAction::Continue;
    });

    if (outCount) *outCount = count;
    return combined;
}

void MazeModule::maybeComputeFingerprint() {
    uint32_t count = 0;
    uint32_t hash = computeRoomFingerprint(&count);
    Output::send<LogLevel::Default>(STR("[CRMaze] local fingerprint rooms={} hash=0x{:08X}\n"), count, hash);
    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        m_localHash = hash; m_localRooms = count;
        m_readyToSend = true;
    }
    m_fpComputed = true;
}

// ---------------------------------------------------------------------------------------------
// GAME THREAD — client safety net (late seed)
// ---------------------------------------------------------------------------------------------
void MazeModule::tryRegenIfLateSeed() {
    m_lateRegenDone = true;  // one-shot regardless of which path we take

    int32_t seed;
    { std::lock_guard<std::mutex> lk(*m_mtx); seed = (int32_t)m_netSeed; }
    applySeed(seed);
    Output::send<LogLevel::Default>(STR("[CRMaze] late seed {} applied; attempting deterministic regen\n"), seed);

    // Preferred: call a clean single-entry regen UFunction on the generator if one exists. (Confirm
    // the exact name live during integration; re-invoking the ubergraph directly is unsafe.)
    if (auto* gen = U::UObjectGlobals::FindFirstOf(STR("Maze_Generator_C"))) {
        const CharType* candidates[] = { STR("Generate Maze"), STR("Generate"), STR("Build Maze"), STR("Spawn Start Room") };
        for (const CharType* fnName : candidates) {
            if (auto* regen = gen->GetFunctionByNameInChain(fnName)) {
                gen->ProcessEvent(regen, nullptr);
                Output::send<LogLevel::Default>(STR("[CRMaze] regen via generator fn '{}'\n"), fnName);
                m_genSeen = true; m_genFrames = 0; m_fpComputed = false;  // re-arm the fingerprint
                return;
            }
        }
    }

    // Fallback: full level reload via the engine console (the only map-load path proven in this
    // build, docs/hooks.md — "open Maze_LVL"). The seed is already in GI, so the fast path applies
    // after the reload. ProcessConsoleExec is the verified UObject wrapper; we route through a player
    // controller (the engine's console executor). Gated by config so it can be disabled in testing.
    //
    // NOTE: the precise console-exec entry is confirmed live at integration (docs/sessions/s1.md).
    // We deliberately do NOT hand-build an unconfirmed ProcessEvent param struct here (that risks a
    // crash); the verified ProcessConsoleExec wrapper takes a plain Cmd string.
    if (m_regenOnLateSeed) {
        if (auto* pc = U::UObjectGlobals::FindFirstOf(STR("PlayerController"))) {
            if (auto* conCmd = pc->GetFunctionByNameInChain(STR("ConsoleCommand"))) {
                // BP-exposed APlayerController::ConsoleCommand(FString Command, bool bWriteToLog) -> FString.
                struct { U::FString Command; bool bWriteToLog; U::FString ReturnValue; } params{};
                params.Command = U::FString(STR("open Maze_LVL"));
                params.bWriteToLog = true;
                pc->ProcessEvent(conCmd, &params);
                Output::send<LogLevel::Default>(STR("[CRMaze] regen via console reload (open Maze_LVL)\n"));
                m_genSeen = false; m_fpComputed = false;  // a fresh ReceiveBeginPlay will re-arm
                return;
            }
        }
        Output::send<LogLevel::Warning>(STR("[CRMaze] WARN no regen path available; maze may differ this run (confirm console entry, docs/sessions/s1.md)\n"));
    } else {
        Output::send<LogLevel::Warning>(STR("[CRMaze] late seed: regen_on_late_seed disabled; maze may differ this run\n"));
    }
}

// ---------------------------------------------------------------------------------------------
// LOOP THREAD — sends + verdict (NO UObject access)
// ---------------------------------------------------------------------------------------------
void MazeModule::on_net_tick() {
    if (!m_transport) return;
    const bool peerReady = m_peerReady && *m_peerReady;
    if (!peerReady) return;

    // Snapshot the outbound flags under the lock; do socket I/O outside it.
    bool sendSeed = false; uint64_t seed = 0; uint32_t paramHash = 0;
    bool sendReady = false; uint32_t localHash = 0; uint32_t localRooms = 0;
    bool doVerdict = false; uint32_t verdictLocal = 0, verdictPeer = 0, verdictRooms = 0;
    {
        std::lock_guard<std::mutex> lk(*m_mtx);
        if (m_role == Role::Host && m_hostSeedPending) {
            sendSeed = true; seed = m_hostSeedToSend; paramHash = m_hostParamHash;
            m_hostSeedPending = false;
        }
        if (m_readyToSend && !m_mazeReadySent) {
            sendReady = true; localHash = m_localHash; localRooms = m_localRooms;
            m_mazeReadySent = true;
        }
        if (!m_verdictLogged && m_localRooms && m_havePeerHash && (m_readyToSend || m_mazeReadySent)) {
            doVerdict = true; verdictLocal = m_localHash; verdictPeer = m_peerHash; verdictRooms = m_localRooms;
            m_verdictLogged = true;
        }
    }

    if (sendSeed) {
        MazeSeedMsg ms; ms.seed = seed; ms.genMode = 0; ms.paramHash = paramHash;
        auto f = packFrame(Chan::Control, Msg::MazeSeed, ms.encode());
        m_transport->send(f.data(), f.size(), true, 0);
        Output::send<LogLevel::Default>(STR("[CRMaze] sent MazeSeed seed={} paramHash=0x{:08X}\n"),
                                        (uint32_t)seed, paramHash);
    }

    if (sendReady) {
        ByteWriter w; w.u8(m_playerId); w.u32(localHash);   // body per docs/protocol.md 0x11
        auto f = packFrame(Chan::Control, Msg::MazeReady, w.buf);
        m_transport->send(f.data(), f.size(), true, 0);
        Output::send<LogLevel::Default>(STR("[CRMaze] sent MazeReady player={} hash=0x{:08X} rooms={}\n"),
                                        (int)m_playerId, localHash, localRooms);
    }

    if (doVerdict) {
        if (verdictLocal == verdictPeer)
            Output::send<LogLevel::Default>(STR("[CRMaze] MAZE MATCH hash=0x{:08X} rooms={}\n"),
                                            verdictLocal, verdictRooms);
        else
            Output::send<LogLevel::Error>(STR("[CRMaze] MAZE MISMATCH local=0x{:08X} peer=0x{:08X} rooms={}\n"),
                                          verdictLocal, verdictPeer, verdictRooms);
    }
}

// ---------------------------------------------------------------------------------------------
// LOOP THREAD — inbound dispatch (POD only)
// ---------------------------------------------------------------------------------------------
void MazeModule::on_message(const Frame& fr) {
    switch (fr.type) {
    case Msg::MazeSeed: {
        auto ms = MazeSeedMsg::decode(fr.body);
        {
            std::lock_guard<std::mutex> lk(*m_mtx);
            m_netSeed = ms.seed; m_haveNetSeed = true; m_netParamHash = ms.paramHash;
            m_lateSeedArrived = true;   // game thread acts on this for the late-seed safety net
        }
        Output::send<LogLevel::Default>(STR("[CRMaze] received MazeSeed seed={} paramHash=0x{:08X}\n"),
                                        (uint32_t)ms.seed, ms.paramHash);
        break;
    }
    case Msg::MazeReady: {
        ByteReader r(fr.body.data(), fr.body.size());
        uint8_t peerPlayer = r.u8();
        uint32_t peerHash = r.u32();
        {
            std::lock_guard<std::mutex> lk(*m_mtx);
            m_peerHash = peerHash; m_havePeerHash = true;
        }
        Output::send<LogLevel::Default>(STR("[CRMaze] received MazeReady player={} hash=0x{:08X}\n"),
                                        (int)peerPlayer, peerHash);
        break;
    }
    default: break;
    }
}

} // namespace crc
