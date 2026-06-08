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
        ModVersion = STR("0.2.0");
        ModDescription = STR("2-player shared-maze co-op");
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
        default: break;
        }
    }

    // ---- game thread (inside ProcessEvent hook) --------------------------------
    void onProcessEvent(U::UObject* ctx, U::UFunction* fn, void*) {
        if (!ctx || !fn) return;

        // Lazily resolve target classes.
        if (!m_labRatClass)
            m_labRatClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(nullptr, nullptr, STR("/Game/Characters/Lab_Rat/BP_LabRat.BP_LabRat_C"));
        if (!m_mazeGenClass)
            m_mazeGenClass = U::UObjectGlobals::StaticFindObject<U::UClass*>(nullptr, nullptr, STR("/Game/Procedural_Maze/Maze_Generator.Maze_Generator_C"));

        // Cache the two UFunctions we care about (then pointer-compare; avoids per-call string work).
        if (!m_ratTickFn || !m_mazeGenBeginFn) {
            auto name = fn->GetName();
            if (!m_ratTickFn && m_labRatClass && name == STR("ReceiveTick") && ctx->IsA(m_labRatClass)) m_ratTickFn = fn;
            if (!m_mazeGenBeginFn && m_mazeGenClass && name == STR("ReceiveBeginPlay") && ctx->IsA(m_mazeGenClass)) m_mazeGenBeginFn = fn;
        }

        if (fn == m_mazeGenBeginFn) { onMazeGenBegin(); return; }  // pre-hook: before generation runs
        if (fn != m_ratTickFn) return;                            // else only act on a LabRat tick

        // Per-frame work piggy-backs on the rat tick: log the maze fingerprint shortly after gen.
        if (m_mazeGenSeen && !m_fpLogged && ++m_mazeGenFrames > 120) { logMazeFingerprint(); m_fpLogged = true; }

        auto* rat = U::Cast<U::AActor>(ctx);
        if (!rat || rat == (U::AActor*)m_puppet) return;  // never treat the puppet as local

        // Local rat = the one possessed by a controller (the puppet is neutralized / uncontrolled).
        auto** ctrl = rat->GetValuePtrByPropertyNameInChain<U::UObject*>(STR("Controller"));
        bool isLocal = ctrl && *ctrl;
        if (!isLocal) return;
        m_localPawn = rat;

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
        // Host shares its seed with the client (reliable).
        if (m_role == crc::Role::Host && m_peerReady) {
            crc::MazeSeedMsg ms; ms.seed = (uint64_t)(uint32_t)seed; ms.genMode = 0;
            auto f = crc::packFrame(crc::Chan::Control, crc::Msg::MazeSeed, ms.encode());
            m_transport->send(f.data(), f.size(), true, 0);
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
};

#define CRCOOP_API __declspec(dllexport)
extern "C" {
CRCOOP_API CppUserModBase* start_mod() { return new CyberRatsCoopMod(); }
CRCOOP_API void uninstall_mod(CppUserModBase* mod) { delete mod; }
}
