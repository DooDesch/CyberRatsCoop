// dllmain.cpp — Cyber Rats Co-op, UE4SS C++ mod entry (M1 transport + M2 player presence).
//
// Threading: on_update() runs on the UE4SS loop thread (NOT the game thread) — safe for sockets,
// NOT for UObject mutation. All game work (read pawn transform, spawn/drive the puppet rat) happens
// inside a global ProcessEvent pre-hook, which fires on the game thread. A mutex-protected snapshot
// passes state between the two threads.
//
// Build: scripts/build_mod.ps1 (see docs/build.md). Transport/protocol loopback-tested separately.

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/World.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/Rotator.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>

#include "Util/Config.h"
#include "Transport/UdpTransport.h"
#include "Net/Protocol.h"

#include <memory>
#include <string>
#include <chrono>
#include <mutex>
#include <cmath>
#include <vector>
#include <array>
#include <unordered_map>
#include <set>
#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace RC;
namespace U = RC::Unreal;

namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Parse "-CRCoopRole=host" from the process command line; fall back to an env var.
std::string optValue(const std::string& cliKey, const wchar_t* envKey) {
    std::string cl = narrow(GetCommandLineW());
    std::string needle = cliKey + "=";
    auto pos = cl.find(needle);
    if (pos != std::string::npos) {
        pos += needle.size();
        auto end = cl.find_first_of(" \t\"", pos);
        return cl.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    }
    wchar_t buf[256];
    DWORD n = GetEnvironmentVariableW(envKey, buf, 256);
    if (n > 0 && n < 256) return narrow(std::wstring(buf, n));
    return "";
}

uint64_t nowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

constexpr double TWO16 = 65536.0;
uint16_t yawToU16(double deg) { double y = std::fmod(deg, 360.0); if (y < 0) y += 360.0; return (uint16_t)(y / 360.0 * TWO16); }
double u16ToYaw(uint16_t v) { return (double)v / TWO16 * 360.0; }

} // namespace

class CyberRatsCoopMod : public CppUserModBase {
public:
    CyberRatsCoopMod() : CppUserModBase() {
        ModName = STR("CyberRatsCoop");
        ModVersion = STR("0.6.0");
        ModDescription = STR("2-player shared-maze co-op (M4 cheese, M5 enemies, M6 death)");
        ModAuthors = STR("CyberRatsCoop");
    }
    ~CyberRatsCoopMod() override = default;

    auto on_unreal_init() -> void override {
        m_cfg.load("ue4ss/Mods/CyberRatsCoop/coop.ini");

        std::string role = optValue("-CRCoopRole", L"CRCOOP_ROLE");
        if (role.empty()) role = m_cfg.getStr("role", "role", "auto");

        m_transport = std::make_unique<crc::UdpTransport>();
        if (role == "host") {
            uint16_t port = (uint16_t)cfgIntOpt("-CRCoopPort", L"CRCOOP_PORT", "enet", "host_bind_port", 7777);
            m_role = crc::Role::Host;
            if (m_transport->startHost(port)) Output::send<LogLevel::Default>(STR("[CRCoop] hosting on UDP {}\n"), port);
            else Output::send<LogLevel::Error>(STR("[CRCoop] host bind FAILED\n"));
        } else if (role == "client") {
            std::string ip = optValue("-CRCoopConnect", L"CRCOOP_CONNECT");
            if (ip.empty()) ip = m_cfg.getStr("enet", "connect_ip", "127.0.0.1");
            uint16_t port = (uint16_t)cfgIntOpt("-CRCoopPort", L"CRCOOP_PORT", "enet", "connect_port", 7777);
            m_role = crc::Role::Client;
            std::wstring wip(ip.begin(), ip.end());
            if (m_transport->connectTo(ip, port)) Output::send<LogLevel::Default>(STR("[CRCoop] connecting to {}:{}\n"), wip, port);
            else Output::send<LogLevel::Error>(STR("[CRCoop] client connect FAILED\n"));
        } else {
            Output::send<LogLevel::Default>(STR("[CRCoop] role=auto; idle until host/join chosen\n"));
        }

        m_playerId = (m_role == crc::Role::Host) ? 0 : 1;
        // Maze seed to force into the generator (shared so both peers build the same maze).
        // -1 = don't force (use the game's natural seed). Host overrides via network in M3b.
        m_forceSeed = m_cfg.getInt("maze", "force_seed", -1);

        // Game-thread work goes through the global ProcessEvent pre-hook.
        U::Hook::RegisterProcessEventPreCallback([this](U::UObject* ctx, U::UFunction* fn, void* parms) {
            onProcessEvent(ctx, fn, parms);
        });
        Output::send<LogLevel::Default>(STR("[CRCoop] ProcessEvent hook registered (M2 player sync)\n"));
    }

    auto on_update() -> void override {
        if (!m_transport) return;
        m_transport->tick();

        if (m_transport->state() == crc::ConnState::Connected && !m_helloSent) {
            crc::HelloMsg h; h.requestedRole = m_role;
            auto f = crc::packFrame(crc::Chan::Control, crc::Msg::Hello, h.encode());
            m_transport->send(f.data(), f.size(), true, 0);
            m_helloSent = true;
        }

        std::vector<crc::RecvPacket> pkts;
        m_transport->receive(pkts);
        for (auto& p : pkts) {
            bool ok = false;
            for (auto& fr : crc::parseFrames(p.data.data(), p.data.size(), &ok)) dispatch(fr);
        }

        // Flush frames enqueued by game-thread hooks (cheese collects, seed share) — sent here so
        // only this thread ever drives the socket / reliability layer.
        {
            std::vector<std::pair<bool, std::vector<uint8_t>>> out;
            { std::lock_guard<std::mutex> lk(m_outMtx); out.swap(m_outFrames); }
            for (auto& fr : out)
                m_transport->send(fr.second.data(), fr.second.size(), fr.first, fr.first ? 0 : 2);
        }

        // Outbound player state at ~20 Hz (snapshot filled by the game thread).
        uint64_t t = nowMs();
        if (m_peerReady && t - m_lastSend >= 50) {
            crc::PlayerStateMsg snap; bool have;
            { std::lock_guard<std::mutex> lk(m_mtx); snap = m_outLocal; have = m_haveLocal; }
            if (have) {
                snap.playerId = m_playerId; snap.seq = m_seq++;
                auto f = crc::packFrame(crc::Chan::PlayerState, crc::Msg::PlayerState, snap.encode());
                m_transport->send(f.data(), f.size(), false, 1);
                m_lastSend = t;
            }
        }
    }

private:
    void dispatch(const crc::Frame& fr) {
        using crc::Msg;
        switch (fr.type) {
        case Msg::Hello: {
            auto h = crc::HelloMsg::decode(fr.body);
            if (h.protoVer != crc::kProtoVersion) return;
            if (m_role == crc::Role::Host) {
                crc::ByteWriter w; w.u8(1); w.u8((uint8_t)crc::Role::Client); w.u8(1); w.u8(crc::kProtoVersion);
                auto f = crc::packFrame(crc::Chan::Control, Msg::HelloAck, w.buf);
                m_transport->send(f.data(), f.size(), true, 0);
            }
            m_peerReady = true;
            Output::send<LogLevel::Default>(STR("[CRCoop] peer Hello ok; co-op link established\n"));
            break;
        }
        case Msg::HelloAck:
            m_peerReady = true;
            Output::send<LogLevel::Default>(STR("[CRCoop] HelloAck; co-op link established\n"));
            break;
        case Msg::PlayerState: {
            auto ps = crc::PlayerStateMsg::decode(fr.body);
            std::lock_guard<std::mutex> lk(m_mtx);
            m_inRemote = ps; m_haveRemote = true;
            break;
        }
        case Msg::MazeSeed: {
            auto ms = crc::MazeSeedMsg::decode(fr.body);
            std::lock_guard<std::mutex> lk(m_mtx);
            m_netSeed = ms.seed; m_haveNetSeed = true;
            Output::send<LogLevel::Default>(STR("[CRCoop] received maze seed {}\n"), (uint32_t)ms.seed);
            break;
        }
        case Msg::PickupCollected: {
            // Peer collected a cheese: queue it for the game thread to replay via Interact().
            auto m = crc::PickupCollectedMsg::decode(fr.body);
            std::lock_guard<std::mutex> lk(m_mtx);
            m_pendingReplay.push_back(m.pickupId);
            break;
        }
        case Msg::EnemySpawn: {   // host -> client (client spawns a puppet)
            auto m = crc::EnemySpawnMsg::decode(fr.body);
            std::lock_guard<std::mutex> lk(m_mtx);
            m_inEnemySpawn.push_back(m);
            break;
        }
        case Msg::EnemyState: {   // host -> client (latest wins)
            auto m = crc::EnemyStateMsg::decode(fr.body);
            std::lock_guard<std::mutex> lk(m_mtx);
            m_inEnemyState = m; m_haveEnemyState = true;
            break;
        }
        case Msg::EnemyDespawn: {
            auto m = crc::EnemyDespawnMsg::decode(fr.body);
            std::lock_guard<std::mutex> lk(m_mtx);
            m_inEnemyDespawn.push_back(m.enemyId);
            break;
        }
        case Msg::Death: {   // peer's rat died (M6: awareness; downed/revive is a later tested pass)
            auto d = crc::DeathMsg::decode(fr.body);
            Output::send<LogLevel::Default>(STR("[CRCoop] peer rat {} died\n"), (int)d.playerId);
            break;
        }
        default: break;
        }
    }

    // ---- game thread (inside ProcessEvent hook) --------------------------------
    void onProcessEvent(U::UObject* ctx, U::UFunction* fn, void*) {
        if (!ctx || !fn) return;

        // Cheese collect is the hottest co-op event after player state. Once m_cheeseInteractFn is
        // cached (post maze-gen), this is a single pointer-compare on the ProcessEvent fast path.
        if (m_cheeseInteractFn && fn == m_cheeseInteractFn) {
            auto* cheese = U::Cast<U::AActor>(ctx);
            onCheeseInteract(cheese, cheese ? cheese->GetWorld() : nullptr);
            return;
        }
        // M6: local rat death.
        if (m_killRatFn && fn == m_killRatFn) { onRatKilled(U::Cast<U::AActor>(ctx)); return; }

        // Lazily resolve target classes.
        if (!m_labRatClass)
            m_labRatClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(nullptr, nullptr, STR("/Game/Characters/Lab_Rat/BP_LabRat.BP_LabRat_C"));
        if (!m_mazeGenClass)
            m_mazeGenClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(nullptr, nullptr, STR("/Game/Procedural_Maze/Maze_Generator.Maze_Generator_C"));
        if (!m_cheeseClass)
            m_cheeseClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(nullptr, nullptr, STR("/Game/Interactions/Cheese_Pickup/BP_Cheese_Pickup.BP_Cheese_Pickup_C"));

        // Cache the UFunctions we care about (then pointer-compare; avoids per-call string work).
        if (!m_ratTickFn || !m_mazeGenBeginFn) {
            auto name = fn->GetName();
            if (!m_ratTickFn && m_labRatClass && name == STR("ReceiveTick") && ctx->IsA(m_labRatClass)) m_ratTickFn = fn;
            if (!m_mazeGenBeginFn && m_mazeGenClass && name == STR("ReceiveBeginPlay") && ctx->IsA(m_mazeGenClass)) m_mazeGenBeginFn = fn;
        }

        if (fn == m_mazeGenBeginFn) { onMazeGenBegin(); return; }  // pre-hook: before generation runs
        if (fn != m_ratTickFn) return;                            // else only act on a LabRat tick

        // Per-frame work piggy-backs on the rat tick.
        if (m_mazeGenSeen) {
            ++m_mazeGenFrames;
            auto* tickActor = U::Cast<U::AActor>(ctx);
            U::UWorld* world = tickActor ? tickActor->GetWorld() : nullptr;
            // Build the cheese registry + cache Interact fn once cheese have spawned (~0.5s post-gen).
            if (!m_cheeseRegistryBuilt && m_mazeGenFrames > 30) {
                buildCheeseRegistry(world);
                if (!m_cheeseInteractFn && !m_cheeseById.empty() && m_cheeseById[0])
                    m_cheeseInteractFn = m_cheeseById[0]->GetFunctionByNameInChain(STR("Interact"));
            }
            // Apply any peer cheese-collects (game thread).
            drainPickupReplays(world);

            // M5 enemies (host-authoritative). Client snaps puppets every tick; both poll ~10 Hz.
            if (m_role == crc::Role::Client) drainEnemyEvents(world);
            if (world && ++m_enemyPollCtr >= 6) {
                m_enemyPollCtr = 0;
                if (m_role == crc::Role::Host)        hostEnemyPoll(world);
                else if (m_role == crc::Role::Client) clientEnemyNeutralize(world);
            }

            if (!m_fpLogged && m_mazeGenFrames > 120) { logMazeFingerprint(); m_fpLogged = true; }
        }

        auto* rat = U::Cast<U::AActor>(ctx);
        if (!rat || rat == (U::AActor*)m_puppet) return;  // never treat the puppet as local

        // Local rat = the one possessed by a controller (the puppet is neutralized / uncontrolled).
        auto** ctrl = rat->GetValuePtrByPropertyNameInChain<U::UObject*>(STR("Controller"));
        bool isLocal = ctrl && *ctrl;
        if (!isLocal) return;
        m_localPawn = rat;
        if (!m_killRatFn) m_killRatFn = rat->GetFunctionByNameInChain(STR("Kill Rat"));  // M6

        // Read local transform -> outbound snapshot.
        U::FVector loc = rat->K2_GetActorLocation();
        U::FRotator rot = rat->K2_GetActorRotation();
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_outLocal.x = (float)loc.GetX(); m_outLocal.y = (float)loc.GetY(); m_outLocal.z = (float)loc.GetZ();
            m_outLocal.yaw = yawToU16(rot.GetYaw());
            m_outLocal.health = 100;
            m_haveLocal = true;
        }

        // Once per frame: maintain + drive the remote puppet.
        maintainPuppet(rat);
    }

    void maintainPuppet(U::AActor* localRat) {
        crc::PlayerStateMsg rem; bool have;
        { std::lock_guard<std::mutex> lk(m_mtx); rem = m_inRemote; have = m_haveRemote; }
        if (!have) return;

        if (!m_puppet) {
            U::UWorld* world = localRat->GetWorld();
            if (!world || !m_labRatClass) return;
            U::FVector spawnLoc((double)rem.x, (double)rem.y, (double)rem.z);
            U::FRotator spawnRot(0.0, u16ToYaw(rem.yaw), 0.0);
            U::AActor* p = world->SpawnActor(m_labRatClass, &spawnLoc, &spawnRot);
            if (!p) return;
            m_puppet = p;
            Output::send<LogLevel::Default>(STR("[CRCoop] spawned remote puppet rat\n"));
            // Neutralize: stop its own logic so it's a pure visual driven by net data.
            if (auto* turnOff = p->GetFunctionByNameInChain(STR("Turn Off Rat"))) {
                p->ProcessEvent(turnOff, nullptr);
                Output::send<LogLevel::Default>(STR("[CRCoop] puppet neutralized (Turn Off Rat)\n"));
            }
        }

        // Drive the puppet transform from the latest remote snapshot (snap for now; interp in M2b).
        U::FVector loc((double)rem.x, (double)rem.y, (double)rem.z);
        U::FRotator rot(0.0, u16ToYaw(rem.yaw), 0.0);
        U::FHitResult hit{};
        ((U::AActor*)m_puppet)->K2_SetActorLocationAndRotation(loc, rot, false, hit, true);
    }

    // Pre-hook on Maze_Generator:ReceiveBeginPlay — runs BEFORE generation. Force the shared seed
    // into the GameInstance so both peers build the same maze.
    void onMazeGenBegin() {
        m_mazeGenSeen = true; m_mazeGenFrames = 0; m_fpLogged = false;
        // New maze => fresh cheese set. Drop the old registry/dedup so IDs rebuild for this maze.
        m_cheeseRegistryBuilt = false;
        m_cheeseById.clear(); m_cheeseIdOf.clear(); m_cheeseProcessed.clear();
        { std::lock_guard<std::mutex> lk(m_mtx); m_pendingReplay.clear(); }
        m_enemyPollCtr = 0;
        resetEnemies();   // fresh enemy ids/puppets for the new maze
        m_localRatDead = false;  // M6: new run, rat alive again
        int32_t seed = m_forceSeed;
        { std::lock_guard<std::mutex> lk(m_mtx); if (m_haveNetSeed) seed = (int32_t)m_netSeed; }
        if (seed < 0) { Output::send<LogLevel::Default>(STR("[CRCoop] maze gen (seed not forced)\n")); return; }
        auto* gi = U::UObjectGlobals::FindFirstOf(STR("GameInstance_LabRats_C"));
        if (gi) {
            if (auto* p = gi->GetValuePtrByPropertyName<int32_t>(STR("Random Seed Roll"))) {
                *p = seed;
                Output::send<LogLevel::Default>(STR("[CRCoop] forced maze seed = {}\n"), seed);
            } else {
                Output::send<LogLevel::Error>(STR("[CRCoop] 'Random Seed Roll' prop not found\n"));
            }
        }
        // Host shares its seed with the client (reliable; queued -> sent on the loop thread).
        if (m_role == crc::Role::Host && m_peerReady) {
            crc::MazeSeedMsg ms; ms.seed = (uint64_t)(uint32_t)seed; ms.genMode = 0;
            queueOut(crc::packFrame(crc::Chan::Control, crc::Msg::MazeSeed, ms.encode()), true);
        }
    }

    // Layout-sensitive maze fingerprint: XOR-combine (order-independent) a hash of each spawned
    // room's world position from the generator's "Final Room List". Identical across peers => same maze.
    void logMazeFingerprint() {
        auto* gen = U::UObjectGlobals::FindFirstOf(STR("Maze_Generator_C"));
        if (!gen) { Output::send<LogLevel::Default>(STR("[CRCoop] MAZE FINGERPRINT: no generator\n")); return; }
        int32_t roomAmt = 0;
        if (auto* p = gen->GetValuePtrByPropertyName<int32_t>(STR("Spawned Room Amount"))) roomAmt = *p;

        uint32_t combined = 0; int counted = 0;
        if (auto* arr = gen->GetValuePtrByPropertyName<U::TArray<U::UObject*>>(STR("Final Room List"))) {
            int n = arr->Num();
            U::UObject** data = arr->GetData();
            for (int i = 0; i < n && data; ++i) {
                auto* room = U::Cast<U::AActor>(data[i]);
                if (!room) continue;
                U::FVector l = room->K2_GetActorLocation();
                int coords[3] = { (int)l.GetX(), (int)l.GetY(), (int)l.GetZ() };
                uint32_t h = 2166136261u;
                for (int c = 0; c < 3; ++c) for (int k = 0; k < 4; ++k) { h ^= (uint8_t)(coords[c] >> (8 * k)); h *= 16777619u; }
                combined ^= h; ++counted;
            }
        }
        Output::send<LogLevel::Default>(STR("[CRCoop] MAZE FINGERPRINT rooms={} counted={} hash=0x{:08X}\n"),
                                        roomAmt, counted, combined);
    }

    // ---- M4 cheese sync (all game-thread) ---------------------------------
    // Deterministic pickupId = index in the position-sorted cheese list. Both peers build identical
    // mazes (seed-sync), so identical cheese positions -> identical sort order -> identical IDs,
    // without the host having to broadcast a map.
    void buildCheeseRegistry(U::UWorld* world) {
        m_cheeseById.clear(); m_cheeseIdOf.clear();
        if (!m_cheeseClass) return;
        std::vector<U::UObject*> found;
        U::UObjectGlobals::FindAllOf(STR("BP_Cheese_Pickup_C"), found);
        std::vector<std::pair<std::array<int, 3>, U::AActor*>> items;
        items.reserve(found.size());
        for (auto* o : found) {
            auto* a = U::Cast<U::AActor>(o);
            if (!a) continue;
            U::FVector l = a->K2_GetActorLocation();
            // quantize to a 4-unit grid (absorbs sub-unit float jitter; cheese are rooms apart)
            auto q = [](double v) { return (int)std::llround(v / 4.0); };
            items.push_back({ { q(l.GetX()), q(l.GetY()), q(l.GetZ()) }, a });
        }
        std::sort(items.begin(), items.end(),
                  [](const auto& A, const auto& B) { return A.first < B.first; });
        for (size_t i = 0; i < items.size(); ++i) {
            m_cheeseById.push_back(items[i].second);
            m_cheeseIdOf[items[i].second] = (uint16_t)i;
        }
        m_cheeseRegistryBuilt = true;
        Output::send<LogLevel::Default>(STR("[CRCoop] cheese registry: {} pickups\n"), (int)m_cheeseById.size());
    }

    // Local rat collected a cheese (observed via the Interact pre-hook). Broadcast it; the peer
    // replays the same Interact so both instances count + open the exit identically.
    void onCheeseInteract(U::AActor* cheese, U::UWorld* world) {
        if (m_replayingInteract) return;             // our own replayed call — don't rebroadcast
        if (!cheese) return;
        if (!m_cheeseRegistryBuilt) buildCheeseRegistry(world);
        auto it = m_cheeseIdOf.find(cheese);
        if (it == m_cheeseIdOf.end()) return;        // unknown cheese (registry stale) — ignore
        uint16_t id = it->second;
        if (m_cheeseProcessed.count(id)) return;     // already counted
        m_cheeseProcessed.insert(id);
        crc::PickupCollectedMsg m; m.pickupId = id; m.byPlayer = m_playerId; m.kind = 0; m.tick = m_seq;
        queueOut(crc::packFrame(crc::Chan::WorldState, crc::Msg::PickupCollected, m.encode()), true);
        Output::send<LogLevel::Default>(STR("[CRCoop] cheese {} collected locally -> sync\n"), id);
    }

    // Apply queued peer cheese-collects on the game thread by calling each cheese's own Interact()
    // (no params) — the faithful path: increments the count, destroys the actor, runs the
    // dungeon-complete check (-> BP_EnterExit.Open?). Dedup + re-entrancy-guarded.
    void drainPickupReplays(U::UWorld* world) {
        std::vector<uint16_t> ids;
        { std::lock_guard<std::mutex> lk(m_mtx); ids.swap(m_pendingReplay); }
        if (ids.empty()) return;
        if (!m_cheeseRegistryBuilt) buildCheeseRegistry(world);
        for (uint16_t id : ids) {
            if (m_cheeseProcessed.count(id)) continue;
            m_cheeseProcessed.insert(id);
            if (id >= m_cheeseById.size()) continue;
            U::AActor* cheese = m_cheeseById[id];
            if (!cheese) continue;
            if (auto* f = cheese->GetFunctionByNameInChain(STR("Interact"))) {
                m_replayingInteract = true;
                cheese->ProcessEvent(f, nullptr);
                m_replayingInteract = false;
                Output::send<LogLevel::Default>(STR("[CRCoop] replayed peer cheese {}\n"), id);
            }
        }
    }

    // ---- M5 enemies (all game-thread) -------------------------------------
    static const wchar_t* enemyName(uint8_t a) {
        switch (a) {
            case 0: return STR("BP_Canister_Rat_C");
            case 1: return STR("BP_SmokeBomb_Rat_C");
            case 2: return STR("BP_SpiderRat_C");
            case 3: return STR("BP_Rat_Critter_C");
            case 4: return STR("BP_DAVE_C");
            case 5: return STR("BP_TeamRat_C");
            default: return STR("");
        }
    }

    void neutralizeActor(U::AActor* a, bool hide) {
        if (!a) return;
        a->SetActorEnableCollision(false);   // no overlap damage to the local rat
        a->SetActorTickEnabled(false);       // stop the BP AI ubergraph (Random Roam / chase)
        if (hide) a->SetActorHiddenInGame(true);
    }

    // HOST: enumerate live enemies, assign monotonic ids, broadcast spawn (reliable) + state (unrel)
    // + despawn (reliable). Called at ~10 Hz.
    void hostEnemyPoll(U::UWorld* world) {
        std::set<U::AActor*> current;
        crc::EnemyStateMsg st;
        for (uint8_t a = 0; a < 6; ++a) {
            std::vector<U::UObject*> found;
            U::UObjectGlobals::FindAllOf(enemyName(a), found);
            for (auto* o : found) {
                auto* e = U::Cast<U::AActor>(o);
                if (!e) continue;
                current.insert(e);
                uint16_t id;
                auto it = m_enemyIdOf.find(e);
                if (it == m_enemyIdOf.end()) {
                    id = m_nextEnemyId++;
                    m_enemyIdOf[e] = id; m_enemyById[id] = e; m_enemyArch[id] = a;
                    U::FVector l = e->K2_GetActorLocation();
                    U::FRotator r = e->K2_GetActorRotation();
                    crc::EnemySpawnMsg sp;
                    sp.enemyId = id; sp.archetype = a;
                    sp.x = (float)l.GetX(); sp.y = (float)l.GetY(); sp.z = (float)l.GetZ();
                    sp.yaw = yawToU16(r.GetYaw()); sp.spawnerId = 0;
                    queueOut(crc::packFrame(crc::Chan::Control, crc::Msg::EnemySpawn, sp.encode()), true);
                } else {
                    id = it->second;
                }
                U::FVector l = e->K2_GetActorLocation();
                U::FRotator r = e->K2_GetActorRotation();
                crc::EnemyStateMsg::Entry en{};
                en.enemyId = id;
                en.x = (float)l.GetX(); en.y = (float)l.GetY(); en.z = (float)l.GetZ();
                en.yaw = yawToU16(r.GetYaw()); en.anim = 0; en.flags = 0; en.hp = 100;
                st.entries.push_back(en);
            }
        }
        // Despawn ids whose actor disappeared.
        std::vector<uint16_t> gone;
        for (auto& kv : m_enemyById) if (!current.count(kv.second)) gone.push_back(kv.first);
        for (uint16_t id : gone) {
            crc::EnemyDespawnMsg dm; dm.enemyId = id; dm.reason = 0;
            queueOut(crc::packFrame(crc::Chan::Control, crc::Msg::EnemyDespawn, dm.encode()), true);
            auto* a = m_enemyById[id];
            m_enemyIdOf.erase(a); m_enemyById.erase(id); m_enemyArch.erase(id);
        }
        if (!st.entries.empty())
            queueOut(crc::packFrame(crc::Chan::WorldState, crc::Msg::EnemyState, st.encode()), false);
    }

    // CLIENT: hide its own AI-spawned enemies (host drives puppets instead). Also caches each
    // archetype's UClass from a live instance so puppets can be spawned.
    void clientEnemyNeutralize(U::UWorld* world) {
        for (uint8_t a = 0; a < 6; ++a) {
            std::vector<U::UObject*> found;
            U::UObjectGlobals::FindAllOf(enemyName(a), found);
            for (auto* o : found) {
                auto* e = U::Cast<U::AActor>(o);
                if (!e) continue;
                if (!m_archetypeClass[a]) m_archetypeClass[a] = e->GetClassPrivate();
                if (m_puppetSet.count(e)) continue;          // my own puppet
                if (m_neutralizedLocal.count(e)) continue;   // already hidden
                neutralizeActor(e, true);
                m_neutralizedLocal.insert(e);
            }
        }
    }

    // CLIENT: apply host enemy events on the game thread — spawn/drive/destroy puppets.
    void drainEnemyEvents(U::UWorld* world) {
        std::vector<crc::EnemySpawnMsg> spawns;
        std::vector<uint16_t> despawns;
        crc::EnemyStateMsg state; bool haveState = false;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            spawns.swap(m_inEnemySpawn);
            despawns.swap(m_inEnemyDespawn);
            if (m_haveEnemyState) { state = m_inEnemyState; haveState = true; m_haveEnemyState = false; }
        }
        std::vector<crc::EnemySpawnMsg> deferred;
        for (auto& sp : spawns) {
            if (m_enemyPuppet.count(sp.enemyId)) continue;
            U::UClass* cls = (sp.archetype < 6) ? m_archetypeClass[sp.archetype] : nullptr;
            if (!cls || !world) { deferred.push_back(sp); continue; } // class not cached yet -> retry
            U::FVector loc((double)sp.x, (double)sp.y, (double)sp.z);
            U::FRotator rot(0.0, u16ToYaw(sp.yaw), 0.0);
            U::AActor* p = world->SpawnActor(cls, &loc, &rot);
            if (!p) continue;
            neutralizeActor(p, false);   // visible, but no collision/AI; transform-driven
            m_enemyPuppet[sp.enemyId] = p; m_puppetSet.insert(p);
        }
        if (!deferred.empty()) {
            std::lock_guard<std::mutex> lk(m_mtx);
            for (auto& s : deferred) m_inEnemySpawn.push_back(s);
        }
        if (haveState) {
            for (auto& en : state.entries) {
                auto it = m_enemyPuppet.find(en.enemyId);
                if (it == m_enemyPuppet.end() || !it->second) continue;
                U::FVector loc((double)en.x, (double)en.y, (double)en.z);
                U::FRotator rot(0.0, u16ToYaw(en.yaw), 0.0);
                U::FHitResult hit{};
                it->second->K2_SetActorLocationAndRotation(loc, rot, false, hit, true);
            }
        }
        for (uint16_t id : despawns) {
            auto it = m_enemyPuppet.find(id);
            if (it != m_enemyPuppet.end()) {
                if (it->second) { m_puppetSet.erase(it->second); it->second->K2_DestroyActor(); }
                m_enemyPuppet.erase(it);
            }
        }
    }

    void resetEnemies() {
        m_enemyIdOf.clear(); m_enemyById.clear(); m_enemyArch.clear(); m_nextEnemyId = 1;
        for (auto& kv : m_enemyPuppet) if (kv.second) kv.second->K2_DestroyActor();
        m_enemyPuppet.clear(); m_puppetSet.clear(); m_neutralizedLocal.clear();
        std::lock_guard<std::mutex> lk(m_mtx);
        m_inEnemySpawn.clear(); m_inEnemyDespawn.clear(); m_haveEnemyState = false;
        // m_archetypeClass cache persists across mazes (classes don't change).
    }

    // ---- M6 death detection (game thread) ---------------------------------
    void onRatKilled(U::AActor* rat) {
        if (!rat || rat == (U::AActor*)m_puppet) return;  // ignore the peer's puppet
        if (m_localRatDead) return;
        m_localRatDead = true;
        U::FVector l = rat->K2_GetActorLocation();
        crc::DeathMsg d;
        d.playerId = m_playerId; d.cause = 0; d.killerEnemyId = 0;
        d.x = (float)l.GetX(); d.y = (float)l.GetY(); d.z = (float)l.GetZ();
        queueOut(crc::packFrame(crc::Chan::Control, crc::Msg::Death, d.encode()), true);
        Output::send<LogLevel::Default>(STR("[CRCoop] local rat died -> broadcast death\n"));
    }

    int cfgIntOpt(const char* cliKey, const wchar_t* envKey, const char* sec, const char* key, int def) {
        std::string v = optValue(cliKey, envKey);
        if (!v.empty()) { try { return std::stoi(v); } catch (...) {} }
        return m_cfg.getInt(sec, key, def);
    }

    crc::Config m_cfg;
    std::unique_ptr<crc::UdpTransport> m_transport;
    crc::Role m_role = crc::Role::Unknown;
    uint8_t m_playerId = 0;
    bool m_helloSent = false;
    bool m_peerReady = false;
    uint64_t m_lastSend = 0;
    uint16_t m_seq = 0;

    // shared between net thread (on_update) and game thread (hook)
    std::mutex m_mtx;
    crc::PlayerStateMsg m_outLocal{}; bool m_haveLocal = false;
    crc::PlayerStateMsg m_inRemote{}; bool m_haveRemote = false;
    uint64_t m_netSeed = 0; bool m_haveNetSeed = false;

    // game-thread only
    U::UClass* m_labRatClass = nullptr;
    U::UClass* m_mazeGenClass = nullptr;
    U::UFunction* m_ratTickFn = nullptr;
    U::UFunction* m_mazeGenBeginFn = nullptr;
    U::AActor* m_localPawn = nullptr;
    U::UObject* m_puppet = nullptr;
    int m_forceSeed = -1;
    bool m_mazeGenSeen = false;
    bool m_fpLogged = false;
    int m_mazeGenFrames = 0;

    // ---- M4 cheese / pickup sync ------------------------------------------
    // Outbound queue: game-thread hooks enqueue frames; on_update() (loop thread) flushes them so
    // we never touch the socket / reliability layer from two threads at once.
    std::mutex m_outMtx;
    std::vector<std::pair<bool, std::vector<uint8_t>>> m_outFrames;  // (reliable, frame)

    U::UClass* m_cheeseClass = nullptr;
    U::UFunction* m_cheeseInteractFn = nullptr;
    std::vector<U::AActor*> m_cheeseById;                  // pickupId -> actor (position-sorted)
    std::unordered_map<U::AActor*, uint16_t> m_cheeseIdOf; // actor -> pickupId
    std::set<uint16_t> m_cheeseProcessed;                  // collected (local or replayed)
    std::vector<uint16_t> m_pendingReplay;                 // peer collects to replay (guard: m_mtx)
    bool m_cheeseRegistryBuilt = false;
    bool m_replayingInteract = false;                      // re-entrancy guard for replayed Interact

    // ---- M5 host-authoritative enemies ------------------------------------
    // Enemies are AI-driven (Random Roam, chase, NavMesh MoveTo) -> non-deterministic across peers.
    // Host owns them: assigns a monotonic id per enemy, broadcasts EnemySpawn/State/Despawn. Client
    // hides its OWN (locally AI-spawned) enemies and shows host-driven puppets (collision + tick
    // disabled, transform snapped from EnemyState). Archetype byte = index into kEnemyNames.
    U::UClass* m_archetypeClass[6] = {};                    // UClass per archetype (cached from instance)
    std::unordered_map<U::AActor*, uint16_t> m_enemyIdOf;   // host: actor -> id
    std::unordered_map<uint16_t, U::AActor*> m_enemyById;   // host: id -> actor
    std::unordered_map<uint16_t, uint8_t>   m_enemyArch;    // host: id -> archetype
    uint16_t m_nextEnemyId = 1;
    std::unordered_map<uint16_t, U::AActor*> m_enemyPuppet; // client: id -> puppet actor
    std::set<U::AActor*> m_puppetSet;                       // client: actors I spawned (skip neutralize)
    std::set<U::AActor*> m_neutralizedLocal;               // client: local AI enemies already hidden
    int m_enemyPollCtr = 0;
    // incoming enemy events (loop thread -> game thread; guarded by m_mtx)
    std::vector<crc::EnemySpawnMsg> m_inEnemySpawn;
    crc::EnemyStateMsg m_inEnemyState; bool m_haveEnemyState = false;
    std::vector<uint16_t> m_inEnemyDespawn;

    // ---- M6 death (minimal): detect local rat death + broadcast. The full co-op downed/revive
    // mechanic needs a blocking hook on "Kill Rat" (the global ProcessEvent pre-cb is observe-only)
    // plus host-authoritative damage, so it's deferred to a tested pass.
    U::UFunction* m_killRatFn = nullptr;
    bool m_localRatDead = false;

    void queueOut(std::vector<uint8_t>&& frame, bool reliable) {
        std::lock_guard<std::mutex> lk(m_outMtx);
        m_outFrames.emplace_back(reliable, std::move(frame));
    }
};

#define CRCOOP_API __declspec(dllexport)
extern "C" {
CRCOOP_API CppUserModBase* start_mod() { return new CyberRatsCoopMod(); }
CRCOOP_API void uninstall_mod(CppUserModBase* mod) { delete mod; }
}
