// protocol_roundtrip_test.cpp — standalone (no UE4SS) wire-format round-trip test for Protocol.h.
// Build:  clang++ -std=c++20 -O1 protocol_roundtrip_test.cpp -o protocol_test.exe  &&  ./protocol_test.exe
// Verifies encode()->decode() identity for every message + multi-frame packFrame/parseFrames.
#include "Protocol.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static bool feq(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
    using namespace crc;

    // PlayerState
    {
        PlayerStateMsg m; m.playerId=1; m.seq=4242; m.x=12.5f; m.y=-7.25f; m.z=300.125f;
        m.yaw=40000; m.vx=-12; m.vy=34; m.vz=-56; m.animState=3; m.montageId=7; m.montageMs=1234;
        m.flags=PF_Sprint|PF_InAir; m.health=88;
        auto d = PlayerStateMsg::decode(m.encode());
        CHECK(d.playerId==1); CHECK(d.seq==4242); CHECK(feq(d.x,12.5f)); CHECK(feq(d.y,-7.25f));
        CHECK(feq(d.z,300.125f)); CHECK(d.yaw==40000); CHECK(d.vx==-12); CHECK(d.vy==34);
        CHECK(d.vz==-56); CHECK(d.animState==3); CHECK(d.montageId==7); CHECK(d.montageMs==1234);
        CHECK(d.flags==(PF_Sprint|PF_InAir)); CHECK(d.health==88);
    }
    // Hello
    {
        HelloMsg m; m.protoVer=kProtoVersion; m.modVer=0x00010000; m.steamId=0x1122334455667788ull;
        m.requestedRole=Role::Client; m.mazeGenHash=0xDEADBEEF;
        auto d = HelloMsg::decode(m.encode());
        CHECK(d.protoVer==kProtoVersion); CHECK(d.modVer==0x00010000);
        CHECK(d.steamId==0x1122334455667788ull); CHECK(d.requestedRole==Role::Client);
        CHECK(d.mazeGenHash==0xDEADBEEF);
    }
    // MazeSeed
    {
        MazeSeedMsg m; m.seed=1337; m.genMode=1; m.paramHash=0xABCD1234;
        auto d = MazeSeedMsg::decode(m.encode());
        CHECK(d.seed==1337); CHECK(d.genMode==1); CHECK(d.paramHash==0xABCD1234);
    }
    // PickupCollected (M4)
    {
        PickupCollectedMsg m; m.pickupId=9; m.byPlayer=1; m.kind=0; m.tick=99999;
        auto d = PickupCollectedMsg::decode(m.encode());
        CHECK(d.pickupId==9); CHECK(d.byPlayer==1); CHECK(d.kind==0); CHECK(d.tick==99999);
    }
    // PickupStateSync (M4)
    {
        PickupStateSyncMsg m;
        m.entries.push_back({0,1}); m.entries.push_back({5,0}); m.entries.push_back({9,1});
        auto d = PickupStateSyncMsg::decode(m.encode());
        CHECK(d.entries.size()==3);
        CHECK(d.entries[0].pickupId==0 && d.entries[0].state==1);
        CHECK(d.entries[1].pickupId==5 && d.entries[1].state==0);
        CHECK(d.entries[2].pickupId==9 && d.entries[2].state==1);
    }
    // ObjectiveReached + Run lifecycle (M4)
    {
        ObjectiveReachedMsg m; m.objectiveId=2; m.byPlayer=0;
        auto d = ObjectiveReachedMsg::decode(m.encode());
        CHECK(d.objectiveId==2 && d.byPlayer==0);
        RunStartMsg rs; rs.runId=7; rs.seed=1337; rs.countdownMs=3000;
        auto rd = RunStartMsg::decode(rs.encode());
        CHECK(rd.runId==7 && rd.seed==1337 && rd.countdownMs==3000);
        RunEndMsg re; re.runId=7; re.result=1; re.reason=2;
        auto rde = RunEndMsg::decode(re.encode());
        CHECK(rde.runId==7 && rde.result==1 && rde.reason==2);
        RestartMsg rm; rm.newRunId=8; rm.newSeed=4242;
        auto rdm = RestartMsg::decode(rm.encode());
        CHECK(rdm.newRunId==8 && rdm.newSeed==4242);
    }
    // EnemySpawn / EnemyState (batched!) / EnemyDespawn / EnemyHit (M5)
    {
        EnemySpawnMsg m; m.enemyId=300; m.archetype=4; m.x=1.0f; m.y=2.0f; m.z=3.0f; m.yaw=12345; m.spawnerId=11;
        auto d = EnemySpawnMsg::decode(m.encode());
        CHECK(d.enemyId==300 && d.archetype==4 && feq(d.x,1.0f) && feq(d.y,2.0f) && feq(d.z,3.0f)
              && d.yaw==12345 && d.spawnerId==11);

        EnemyStateMsg s;
        for (int i = 0; i < 5; ++i) {
            EnemyStateMsg::Entry e{}; e.enemyId=(uint16_t)(100+i); e.x=(float)i; e.y=(float)(i*2);
            e.z=(float)(i*3); e.yaw=(uint16_t)(1000*i); e.anim=(uint8_t)i; e.flags=(uint8_t)(i&1); e.hp=(uint8_t)(100-i);
            s.entries.push_back(e);
        }
        auto sd = EnemyStateMsg::decode(s.encode());
        CHECK(sd.entries.size()==5);
        for (int i = 0; i < 5 && i < (int)sd.entries.size(); ++i) {
            auto& e = sd.entries[i];
            CHECK(e.enemyId==(uint16_t)(100+i)); CHECK(feq(e.x,(float)i)); CHECK(feq(e.y,(float)(i*2)));
            CHECK(feq(e.z,(float)(i*3))); CHECK(e.yaw==(uint16_t)(1000*i)); CHECK(e.anim==(uint8_t)i);
            CHECK(e.flags==(uint8_t)(i&1)); CHECK(e.hp==(uint8_t)(100-i));
        }
        EnemyDespawnMsg dm; dm.enemyId=300; dm.reason=1;
        auto dmd = EnemyDespawnMsg::decode(dm.encode());
        CHECK(dmd.enemyId==300 && dmd.reason==1);
        EnemyHitMsg hm; hm.enemyId=300; hm.byPlayer=1; hm.dmg=25; hm.hx=1; hm.hy=2; hm.hz=3;
        auto hmd = EnemyHitMsg::decode(hm.encode());
        CHECK(hmd.enemyId==300 && hmd.byPlayer==1 && hmd.dmg==25 && feq(hmd.hx,1) && feq(hmd.hy,2) && feq(hmd.hz,3));
    }
    // Death / Downed / Revive (M6)
    {
        DeathMsg m; m.playerId=1; m.cause=2; m.killerEnemyId=300; m.x=10; m.y=20; m.z=30;
        auto d = DeathMsg::decode(m.encode());
        CHECK(d.playerId==1 && d.cause==2 && d.killerEnemyId==300 && feq(d.x,10) && feq(d.y,20) && feq(d.z,30));
        DownedStateMsg ds; ds.playerId=0; ds.downed=1; ds.revivePct=45;
        auto dsd = DownedStateMsg::decode(ds.encode());
        CHECK(dsd.playerId==0 && dsd.downed==1 && dsd.revivePct==45);
        ReviveStartMsg rs; rs.targetPlayer=0; rs.byPlayer=1;
        auto rsd = ReviveStartMsg::decode(rs.encode());
        CHECK(rsd.targetPlayer==0 && rsd.byPlayer==1);
        ReviveCompleteMsg rc; rc.targetPlayer=0; rc.restoredHp=60;
        auto rcd = ReviveCompleteMsg::decode(rc.encode());
        CHECK(rcd.targetPlayer==0 && rcd.restoredHp==60);
    }
    // Multi-frame: pack 3 frames into one datagram, parse them all back.
    {
        std::vector<uint8_t> dgram;
        PickupCollectedMsg p; p.pickupId=3; p.byPlayer=1;
        DeathMsg dth; dth.playerId=1;
        EnemyDespawnMsg ed; ed.enemyId=77; ed.reason=0;
        for (auto& f : { packFrame(Chan::WorldState, Msg::PickupCollected, p.encode()),
                         packFrame(Chan::Control,    Msg::Death,           dth.encode()),
                         packFrame(Chan::Control,    Msg::EnemyDespawn,    ed.encode()) })
            dgram.insert(dgram.end(), f.begin(), f.end());
        bool ok = false;
        auto frames = parseFrames(dgram.data(), dgram.size(), &ok);
        CHECK(ok); CHECK(frames.size()==3);
        if (frames.size()==3) {
            CHECK(frames[0].type==Msg::PickupCollected);
            CHECK(frames[1].type==Msg::Death);
            CHECK(frames[2].type==Msg::EnemyDespawn);
            CHECK(PickupCollectedMsg::decode(frames[0].body).pickupId==3);
            CHECK(DeathMsg::decode(frames[1].body).playerId==1);
            CHECK(EnemyDespawnMsg::decode(frames[2].body).enemyId==77);
        }
        // Truncated datagram must set ok=false, not crash.
        bool ok2 = true;
        parseFrames(dgram.data(), dgram.size()-1, &ok2);
        CHECK(!ok2);
    }

    std::printf("\nPROTOCOL ROUND-TRIP: %d checks, %d failures -> %s\n",
                g_checks, g_fail, g_fail==0 ? "OK" : "FAILED");
    return g_fail==0 ? 0 : 1;
}
