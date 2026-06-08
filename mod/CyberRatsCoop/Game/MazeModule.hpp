// Game/MazeModule.hpp — Cyber Rats Co-op, milestone S1 (M3 full validation).
//
// Self-contained module that owns the shared-maze seed-sync: it (a) forces the host's game-rolled
// "Random Seed Roll" into the client's GameInstance BEFORE generation so both peers build the same
// maze, (b) gates / self-heals the client when the host's seed lands late, and (c) computes a
// layout-sensitive fingerprint over the ACTUALLY spawned room actors and exchanges it host<->client
// (MazeSeed 0x10, MazeReady 0x11) to confirm the mazes are identical in a real run.
//
// THREADING RULE (project-wide, see docs/sessions.md): UObject access is ONLY safe on the GAME
// THREAD, i.e. inside on_process_event (driven by the global ProcessEvent pre-hook). Socket sends
// happen on the UE4SS loop thread (on_net_tick). Inbound frames are dispatched on the loop thread
// (on_message). State crosses threads through a std::mutex-protected POD snapshot — NO UObject
// pointer ever appears in the shared snapshot, and the net thread NEVER touches a UObject.
//
// The module is bound (bind()/configure()) by dllmain during integration; it shares dllmain's own
// std::mutex so there is a single lock order (no second lock, no deadlock). dllmain and Protocol.h
// are NOT edited by this milestone — the integration wiring is documented in docs/sessions/s1.md.
#pragma once

#include "Net/Protocol.h"

#include <cstdint>
#include <mutex>

// Forward-declare the engine types so this header has no UE4SS include dependency (the .cpp pulls
// in the real headers). Matches dllmain.cpp's 'namespace U = RC::Unreal' usage in the .cpp.
namespace RC::Unreal {
class UObject;
class UFunction;
class UClass;
class AActor;
}

namespace crc {

class INetTransport;

// Milestone S1: shared-maze validation + seed-gating. One instance owned by dllmain.
class MazeModule {
public:
    // Inject dllmain's refs. Called once from dllmain on_unreal_init AFTER transport+role are set.
    //  - transport : the live INetTransport (not owned).
    //  - role      : Host or Client (host is the single seed authority).
    //  - playerId  : 0 host / 1 client (for MazeReady).
    //  - peerReady : pointer to dllmain's m_peerReady (read-only; true once the handshake completed).
    //  - mtx       : THE SAME std::mutex dllmain uses for its snapshots (single lock order).
    void bind(INetTransport* transport, Role role, uint8_t playerId,
              const bool* peerReady, std::mutex* mtx);

    // Optional config forwarded from coop.ini by dllmain.
    //  - forceSeed        : >=0 overrides the game-rolled seed (test mode); -1 = use game/net seed.
    //  - regenOnLateSeed  : client safety net — reload Maze_LVL if the seed arrived after gen ran.
    void configure(int32_t forceSeed, bool regenOnLateSeed);

    // Cache classes/functions where possible (lazy resolution also happens in on_process_event).
    void on_unreal_init();

    // GAME THREAD. Fires for every UFunction call (hot path: cache + pointer-compare). Drives seed
    // injection (Maze_Generator:ReceiveBeginPlay), param capture (Setup Dungeon Difficulty), and the
    // delayed fingerprint compute (piggy-backed on a LabRat ReceiveTick).
    void on_process_event(RC::Unreal::UObject* ctx, RC::Unreal::UFunction* fn);

    // LOOP THREAD. Sends pending MazeSeed (host) / MazeReady (both), logs the MATCH/MISMATCH verdict.
    // NO UObject access here.
    void on_net_tick();

    // LOOP THREAD. Inbound dispatch for MazeSeed (0x10) and MazeReady (0x11). POD only.
    void on_message(const Frame& fr);

private:
    // ---- game-thread helpers (UObject work only) ----
    void onMazeGenBegin(RC::Unreal::UObject* generator);          // seed inject + gating
    void applySeed(int32_t seed);                                 // write GI "Random Seed Roll"
    void captureParamHash(RC::Unreal::UObject* generator);        // difficulty/room/level cross-check
    void maybeComputeFingerprint();                               // frame-gated, once per run
    uint32_t computeRoomFingerprint(uint32_t* outCount);          // ForEachUObject enumeration
    void tryRegenIfLateSeed();                                    // client safety net
    bool isRoomActor(RC::Unreal::UObject* obj);                   // class-name heuristic + cache

    // ---- refs (not owned) ----
    INetTransport* m_transport = nullptr;
    Role m_role = Role::Unknown;
    uint8_t m_playerId = 0;
    const bool* m_peerReady = nullptr;
    std::mutex* m_mtx = nullptr;

    // ---- cached UObject refs (GAME THREAD ONLY) ----
    RC::Unreal::UClass* m_mazeGenClass = nullptr;
    RC::Unreal::UClass* m_labRatClass = nullptr;
    RC::Unreal::UClass* m_roomClass = nullptr;       // resolved on first room sighting (heuristic)
    RC::Unreal::UFunction* m_genBeginFn = nullptr;   // Maze_Generator_C:ReceiveBeginPlay
    RC::Unreal::UFunction* m_ratTickFn = nullptr;    // BP_LabRat_C:ReceiveTick (frame driver)
    RC::Unreal::UFunction* m_setupDiffFn = nullptr;  // Maze_Generator_C:Setup Dungeon Difficulty

    // ---- run / frame state (GAME THREAD ONLY) ----
    bool m_genSeen = false;          // ReceiveBeginPlay observed for the current run
    bool m_fpComputed = false;       // fingerprint computed for the current run
    int  m_genFrames = 0;            // frames since ReceiveBeginPlay (settle window)
    bool m_genRanUnseeded = false;   // client: gen ran with no net seed in hand
    bool m_lateRegenDone = false;    // client safety-net one-shot latch
    int32_t m_forceSeed = -1;
    bool m_regenOnLateSeed = false;
    uint32_t m_capturedParamHash = 0;

    // ---- shared with net thread (guarded by *m_mtx) ----
    // inbound from net (MazeSeed) -> consumed on the game thread
    uint64_t m_netSeed = 0; bool m_haveNetSeed = false; uint32_t m_netParamHash = 0;
    bool m_lateSeedArrived = false;  // set by on_message; acted on by the game thread
    // produced on the game thread -> sent by the net thread
    uint32_t m_localHash = 0; uint32_t m_localRooms = 0;
    bool m_readyToSend = false; bool m_mazeReadySent = false;
    // host -> net: the game-rolled seed/params it must broadcast
    uint64_t m_hostSeedToSend = 0; uint32_t m_hostParamHash = 0; bool m_hostSeedPending = false;
    // inbound MazeReady from the peer
    uint32_t m_peerHash = 0; bool m_havePeerHash = false;
    // verdict latch (net thread)
    bool m_verdictLogged = false;
};

} // namespace crc
