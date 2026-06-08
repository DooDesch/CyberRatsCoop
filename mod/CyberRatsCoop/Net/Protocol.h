// Protocol.h — Cyber Rats Co-op wire format (pure C++, no UE4SS/engine dependency).
// 2 peers; little-endian; see docs/protocol.md for the full spec.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace crc {

constexpr uint8_t  kProtoVersion = 1;
constexpr uint32_t kModVersion   = 0x00010000; // 0.1.0

// Transport channels (map to ENet channels / GNS lanes / our UDP reliability classes).
enum class Chan : uint8_t { Control = 0, PlayerState = 1, WorldState = 2, Bulk = 3 };

// Message types (see docs/protocol.md).
enum class Msg : uint8_t {
    Hello = 0x01, HelloAck = 0x02, RatSelect = 0x03,
    MazeSeed = 0x10, MazeReady = 0x11, MazePayload = 0x12,
    PlayerState = 0x20,
    PickupCollected = 0x30, PickupStateSync = 0x31,
    ObjectiveReached = 0x40,
    EnemySpawn = 0x50, EnemyState = 0x51, EnemyDespawn = 0x52, EnemyHit = 0x53,
    Death = 0x60, DownedState = 0x61, ReviveStart = 0x62, ReviveComplete = 0x63,
    RunStart = 0x70, RunEnd = 0x71, Restart = 0x72,
    Ping = 0x7F, Bye = 0x7E,
};

enum class Role : uint8_t { Unknown = 0, Host = 1, Client = 2 };

// PlayerState flag bits.
enum PlayerFlags : uint8_t {
    PF_Sprint   = 1 << 0,
    PF_Downed   = 1 << 1,
    PF_Dead     = 1 << 2,
    PF_InAir    = 1 << 3,
    PF_Montage  = 1 << 4,
    PF_Biting   = 1 << 5,
};

// ---- little-endian byte writer/reader -------------------------------------
struct ByteWriter {
    std::vector<uint8_t> buf;
    void u8(uint8_t v)  { buf.push_back(v); }
    void u16(uint16_t v){ buf.push_back(v & 0xFF); buf.push_back((v >> 8) & 0xFF); }
    void u32(uint32_t v){ for (int i = 0; i < 4; ++i) buf.push_back((v >> (8*i)) & 0xFF); }
    void u64(uint64_t v){ for (int i = 0; i < 8; ++i) buf.push_back((uint8_t)((v >> (8*i)) & 0xFF)); }
    void i16(int16_t v) { u16((uint16_t)v); }
    void f32(float v)   { uint32_t t; std::memcpy(&t, &v, 4); u32(t); }
    void str(const std::string& s) { u8((uint8_t)std::min<size_t>(s.size(), 255)); buf.insert(buf.end(), s.begin(), s.begin() + std::min<size_t>(s.size(), 255)); }
    void bytes(const uint8_t* p, size_t n) { buf.insert(buf.end(), p, p + n); }
};

struct ByteReader {
    const uint8_t* p; size_t n; size_t off = 0; bool ok = true;
    ByteReader(const uint8_t* d, size_t len) : p(d), n(len) {}
    bool need(size_t k) { if (off + k > n) { ok = false; return false; } return true; }
    uint8_t  u8()  { if (!need(1)) return 0; return p[off++]; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = p[off] | (p[off+1] << 8); off += 2; return v; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i=0;i<4;++i) v |= (uint32_t)p[off+i] << (8*i); off += 4; return v; }
    uint64_t u64() { if (!need(8)) return 0; uint64_t v = 0; for (int i=0;i<8;++i) v |= (uint64_t)p[off+i] << (8*i); off += 8; return v; }
    int16_t  i16() { return (int16_t)u16(); }
    float    f32() { uint32_t t = u32(); float v; std::memcpy(&v, &t, 4); return v; }
    std::string str() { uint8_t len = u8(); if (!need(len)) return {}; std::string s((const char*)p + off, len); off += len; return s; }
};

// ---- framed message: [Chan u8][Msg u8][len u16][body] ----------------------
struct Frame {
    Chan chan; Msg type; std::vector<uint8_t> body;
};

inline std::vector<uint8_t> packFrame(Chan c, Msg t, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    out.reserve(4 + body.size());
    out.push_back((uint8_t)c);
    out.push_back((uint8_t)t);
    uint16_t len = (uint16_t)body.size();
    out.push_back(len & 0xFF); out.push_back((len >> 8) & 0xFF);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Parse possibly-multiple frames from one datagram. Returns frames; sets ok=false on malformed.
inline std::vector<Frame> parseFrames(const uint8_t* data, size_t len, bool* ok = nullptr) {
    std::vector<Frame> frames;
    size_t off = 0;
    while (off + 4 <= len) {
        Frame f;
        f.chan = (Chan)data[off];
        f.type = (Msg)data[off + 1];
        uint16_t blen = data[off + 2] | (data[off + 3] << 8);
        off += 4;
        if (off + blen > len) { if (ok) *ok = false; return frames; }
        f.body.assign(data + off, data + off + blen);
        off += blen;
        frames.push_back(std::move(f));
    }
    if (ok) *ok = (off == len);
    return frames;
}

// ---- typed payloads --------------------------------------------------------
struct PlayerStateMsg {
    uint8_t  playerId = 0;
    uint16_t seq = 0;
    float    x = 0, y = 0, z = 0;
    uint16_t yaw = 0;                 // 0..65535 == 0..360deg
    int16_t  vx = 0, vy = 0, vz = 0;  // cm/s
    uint8_t  animState = 0;
    uint8_t  montageId = 0;
    uint16_t montageMs = 0;
    uint8_t  flags = 0;
    uint8_t  health = 0;

    std::vector<uint8_t> encode() const {
        ByteWriter w;
        w.u8(playerId); w.u16(seq);
        w.f32(x); w.f32(y); w.f32(z);
        w.u16(yaw);
        w.i16(vx); w.i16(vy); w.i16(vz);
        w.u8(animState); w.u8(montageId); w.u16(montageMs);
        w.u8(flags); w.u8(health);
        return w.buf;
    }
    static PlayerStateMsg decode(const std::vector<uint8_t>& b) {
        ByteReader r(b.data(), b.size());
        PlayerStateMsg m;
        m.playerId = r.u8(); m.seq = r.u16();
        m.x = r.f32(); m.y = r.f32(); m.z = r.f32();
        m.yaw = r.u16();
        m.vx = r.i16(); m.vy = r.i16(); m.vz = r.i16();
        m.animState = r.u8(); m.montageId = r.u8(); m.montageMs = r.u16();
        m.flags = r.u8(); m.health = r.u8();
        return m;
    }
};

struct HelloMsg {
    uint8_t  protoVer = kProtoVersion;
    uint32_t modVer   = kModVersion;
    uint64_t steamId  = 0;
    Role     requestedRole = Role::Unknown;
    uint32_t mazeGenHash = 0;
    std::vector<uint8_t> encode() const {
        ByteWriter w; w.u8(protoVer); w.u32(modVer); w.u64(steamId); w.u8((uint8_t)requestedRole); w.u32(mazeGenHash); return w.buf;
    }
    static HelloMsg decode(const std::vector<uint8_t>& b) {
        ByteReader r(b.data(), b.size()); HelloMsg m;
        m.protoVer = r.u8(); m.modVer = r.u32(); m.steamId = r.u64(); m.requestedRole = (Role)r.u8(); m.mazeGenHash = r.u32(); return m;
    }
};

struct MazeSeedMsg {
    uint64_t seed = 0;       // GameInstance "Random Seed Roll" value (host-chosen)
    uint8_t  genMode = 0;    // 0 = seed, 1 = payload
    uint32_t paramHash = 0;  // difficulty/level params hash for cross-check
    std::vector<uint8_t> encode() const { ByteWriter w; w.u64(seed); w.u8(genMode); w.u32(paramHash); return w.buf; }
    static MazeSeedMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); MazeSeedMsg m; m.seed = r.u64(); m.genMode = r.u8(); m.paramHash = r.u32(); return m; }
};

// ---- M4 pickups & objective & run lifecycle --------------------------------
struct PickupCollectedMsg {            // 0x30 — client req / host confirm
    uint16_t pickupId = 0; uint8_t byPlayer = 0; uint8_t kind = 0; uint32_t tick = 0;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u16(pickupId); w.u8(byPlayer); w.u8(kind); w.u32(tick); return w.buf; }
    static PickupCollectedMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); PickupCollectedMsg m; m.pickupId=r.u16(); m.byPlayer=r.u8(); m.kind=r.u8(); m.tick=r.u32(); return m; }
};
struct PickupStateSyncMsg {            // 0x31 — host->client keyframe
    struct Entry { uint16_t pickupId; uint8_t state; };   // state: 1=present, 0=collected
    std::vector<Entry> entries;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u8((uint8_t)std::min<size_t>(entries.size(),255)); for (auto& e : entries) { w.u16(e.pickupId); w.u8(e.state); } return w.buf; }
    static PickupStateSyncMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); PickupStateSyncMsg m; uint8_t n=r.u8(); for (uint8_t i=0;i<n;++i){ Entry e; e.pickupId=r.u16(); e.state=r.u8(); m.entries.push_back(e);} return m; }
};
struct ObjectiveReachedMsg {           // 0x40
    uint16_t objectiveId = 0; uint8_t byPlayer = 0;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u16(objectiveId); w.u8(byPlayer); return w.buf; }
    static ObjectiveReachedMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); ObjectiveReachedMsg m; m.objectiveId=r.u16(); m.byPlayer=r.u8(); return m; }
};
struct RunStartMsg {                   // 0x70
    uint32_t runId = 0; uint64_t seed = 0; uint16_t countdownMs = 0;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u32(runId); w.u64(seed); w.u16(countdownMs); return w.buf; }
    static RunStartMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); RunStartMsg m; m.runId=r.u32(); m.seed=r.u64(); m.countdownMs=r.u16(); return m; }
};
struct RunEndMsg {                     // 0x71
    uint32_t runId = 0; uint8_t result = 0; uint8_t reason = 0;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u32(runId); w.u8(result); w.u8(reason); return w.buf; }
    static RunEndMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); RunEndMsg m; m.runId=r.u32(); m.result=r.u8(); m.reason=r.u8(); return m; }
};
struct RestartMsg {                    // 0x72
    uint32_t newRunId = 0; uint64_t newSeed = 0;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u32(newRunId); w.u64(newSeed); return w.buf; }
    static RestartMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); RestartMsg m; m.newRunId=r.u32(); m.newSeed=r.u64(); return m; }
};

// ---- M5 enemies (host-authoritative) ---------------------------------------
struct EnemySpawnMsg {                 // 0x50
    uint16_t enemyId = 0; uint8_t archetype = 0; float x=0,y=0,z=0; uint16_t yaw=0; uint16_t spawnerId=0;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u16(enemyId); w.u8(archetype); w.f32(x); w.f32(y); w.f32(z); w.u16(yaw); w.u16(spawnerId); return w.buf; }
    static EnemySpawnMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); EnemySpawnMsg m; m.enemyId=r.u16(); m.archetype=r.u8(); m.x=r.f32(); m.y=r.f32(); m.z=r.f32(); m.yaw=r.u16(); m.spawnerId=r.u16(); return m; }
};
struct EnemyStateMsg {                 // 0x51 — batched (Δ + 1 Hz keyframe)
    struct Entry { uint16_t enemyId; float x,y,z; uint16_t yaw; uint8_t anim; uint8_t flags; uint8_t hp; };
    std::vector<Entry> entries;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u16((uint16_t)entries.size()); for (auto& e : entries){ w.u16(e.enemyId); w.f32(e.x); w.f32(e.y); w.f32(e.z); w.u16(e.yaw); w.u8(e.anim); w.u8(e.flags); w.u8(e.hp);} return w.buf; }
    static EnemyStateMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); EnemyStateMsg m; uint16_t n=r.u16(); for (uint16_t i=0;i<n;++i){ Entry e; e.enemyId=r.u16(); e.x=r.f32(); e.y=r.f32(); e.z=r.f32(); e.yaw=r.u16(); e.anim=r.u8(); e.flags=r.u8(); e.hp=r.u8(); m.entries.push_back(e);} return m; }
};
struct EnemyDespawnMsg {               // 0x52
    uint16_t enemyId = 0; uint8_t reason = 0;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u16(enemyId); w.u8(reason); return w.buf; }
    static EnemyDespawnMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); EnemyDespawnMsg m; m.enemyId=r.u16(); m.reason=r.u8(); return m; }
};
struct EnemyHitMsg {                    // 0x53 — client->host
    uint16_t enemyId = 0; uint8_t byPlayer = 0; uint16_t dmg = 0; float hx=0,hy=0,hz=0;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u16(enemyId); w.u8(byPlayer); w.u16(dmg); w.f32(hx); w.f32(hy); w.f32(hz); return w.buf; }
    static EnemyHitMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); EnemyHitMsg m; m.enemyId=r.u16(); m.byPlayer=r.u8(); m.dmg=r.u16(); m.hx=r.f32(); m.hy=r.f32(); m.hz=r.f32(); return m; }
};

// ---- M6 death / downed / revive --------------------------------------------
struct DeathMsg {                      // 0x60
    uint8_t playerId = 0; uint8_t cause = 0; uint16_t killerEnemyId = 0; float x=0,y=0,z=0;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u8(playerId); w.u8(cause); w.u16(killerEnemyId); w.f32(x); w.f32(y); w.f32(z); return w.buf; }
    static DeathMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); DeathMsg m; m.playerId=r.u8(); m.cause=r.u8(); m.killerEnemyId=r.u16(); m.x=r.f32(); m.y=r.f32(); m.z=r.f32(); return m; }
};
struct DownedStateMsg {                // 0x61
    uint8_t playerId = 0; uint8_t downed = 0; uint8_t revivePct = 0;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u8(playerId); w.u8(downed); w.u8(revivePct); return w.buf; }
    static DownedStateMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); DownedStateMsg m; m.playerId=r.u8(); m.downed=r.u8(); m.revivePct=r.u8(); return m; }
};
struct ReviveStartMsg {                // 0x62
    uint8_t targetPlayer = 0; uint8_t byPlayer = 0;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u8(targetPlayer); w.u8(byPlayer); return w.buf; }
    static ReviveStartMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); ReviveStartMsg m; m.targetPlayer=r.u8(); m.byPlayer=r.u8(); return m; }
};
struct ReviveCompleteMsg {             // 0x63
    uint8_t targetPlayer = 0; uint8_t restoredHp = 0;
    std::vector<uint8_t> encode() const { ByteWriter w; w.u8(targetPlayer); w.u8(restoredHp); return w.buf; }
    static ReviveCompleteMsg decode(const std::vector<uint8_t>& b) { ByteReader r(b.data(), b.size()); ReviveCompleteMsg m; m.targetPlayer=r.u8(); m.restoredHp=r.u8(); return m; }
};

// FNV-1a 32-bit (matches the Lua fingerprint in CRToolkit for cross-checking).
inline uint32_t fnv1a(const uint8_t* d, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= d[i]; h *= 16777619u; }
    return h;
}

} // namespace crc
