# Wire protocol — Cyber Rats Co-op

Transport-agnostic message layer (runs over ENet in dev, Steam GNS in prod). 2 peers only:
**host** = authority over the shared world; each peer is locally authoritative over its own rat.

## Framing

Every datagram carries one or more messages, packed back-to-back:

```
struct MsgHeader { u8 channel; u8 type; u16 bodyLen; }   // 4 bytes, little-endian
[ MsgHeader ][ body (bodyLen bytes) ] [ MsgHeader ][ body ] ...
```

Channels (map onto ENet channels / GNS lanes):

| ch | name | reliability |
|---|---|---|
| 0 | control | reliable, ordered |
| 1 | player-state | unreliable |
| 2 | world/enemy-state | unreliable |
| 3 | bulk | reliable, fragmented (maze payload, rosters) |

`protoVersion` (u8) is negotiated in Hello/HelloAck; a mismatch hard-refuses the connection.

## Scalar encodings

| field | encoding | bytes |
|---|---|---|
| position | 3× f32 (cm) | 12 |
| yaw | u16 (0..65535 ↔ 0..360°) | 2 |
| full rotation (rare) | 3× u16 | 6 |
| velocity | 3× i16 (cm/s) | 6 |
| animState | u8 enum | 1 |
| montageId | u8 (index into shared montage table) | 1 |
| montageMs | u16 (playback position, ms) | 2 |
| flags | u8 bitfield | 1 |
| health | u8 | 1 |
| entityId | u16 | 2 |
| playerId | u8 (0 = host rat, 1 = client rat) | 1 |

`flags`: bit0 sprint · bit1 downed · bit2 dead · bit3 inAir · bit4 montagePlaying.

## Message set

| type | name | dir | ch/rel | body | freq |
|---|---|---|---|---|---|
| 0x01 | Hello | both | 0/rel | protoVer u8, modVer u32, steamID u64, requestedRole u8, mazeGenHash u32 | once |
| 0x02 | HelloAck | both | 0/rel | accepted u8, role u8, playerId u8, agreedVer u8 | once |
| 0x03 | RatSelect | both | 0/rel | playerId u8, ratVariantId u8, nameLen u8, name[] | menu |
| 0x10 | MazeSeed | H→C | 0/rel | seed u64, genMode u8 (0 seed / 1 payload), paramHash u32 | run start |
| 0x11 | MazeReady | C→H | 0/rel | playerId u8, layoutHash u32 | after gen |
| 0x12 | MazePayload | H→C | 3/rel-frag | ver u8, dimX u16, dimY u16, packed cells/walls/spawns/pickups | fallback |
| 0x20 | PlayerState | both | 1/unrel | playerId u8, seq u16, pos(12), yaw u16, vel(6), animState u8, montageId u8, montageMs u16, flags u8, health u8 | 20–30 Hz |
| 0x30 | PickupCollected | C→H req / H→C confirm | 0/rel | pickupId u16, byPlayer u8, kind u8, tick u32 | event |
| 0x31 | PickupStateSync | H→C | 2/unrel | count u8, [pickupId u16, state u8]… | 1 Hz + change |
| 0x40 | ObjectiveReached | both | 0/rel | objectiveId u16, byPlayer u8 | event |
| 0x50 | EnemySpawn | H→C | 0/rel | enemyId u16, archetype u8, pos(12), yaw u16, spawnerId u16 | event |
| 0x51 | EnemyState | H→C | 2/unrel | count u16, [enemyId u16, pos(12), yaw u16, anim u8, flags u8, hp u8]… | 15–20 Hz (Δ + 1 Hz key) |
| 0x52 | EnemyDespawn | H→C | 0/rel | enemyId u16, reason u8 | event |
| 0x53 | EnemyHit | C→H | 0/rel | enemyId u16, byPlayer u8, dmg u16, hitPos(12) | event (M5) |
| 0x60 | Death | H→C | 0/rel | playerId u8, cause u8, killerEnemyId u16, pos(12) | event |
| 0x61 | DownedState | H→C | 0/rel | playerId u8, downed u8, revivePct u8 | event/throttled |
| 0x62 | ReviveStart | both | 0/rel | targetPlayer u8, byPlayer u8 | event |
| 0x63 | ReviveComplete | H→C | 0/rel | targetPlayer u8, restoredHp u8 | event |
| 0x70 | RunStart | H→C | 0/rel | runId u32, seed u64, countdownMs u16 | event |
| 0x71 | RunEnd | H→C | 0/rel | runId u32, result u8, reason u8 | event |
| 0x72 | Restart | H→C | 0/rel | newRunId u32, newSeed u64 | event |
| 0x7F | Ping | both | 1/unrel | sendMs u32, lastRecvSeq u16 | 2 Hz |
| 0x7E | Bye | both | 0/rel | reason u8 | quit |

## Stable cross-machine entity IDs

GUIDs/FNames/pointers differ per process, so IDs are derived deterministically:

- **Pickups / static objective** → keyed by maze grid cell: `id = hash16(cellX, cellY, slot)`.
  Both machines build the same maze, so each cheese sits in a known cell. Host sends the id↔cell map
  once (in `MazePayload` or an initial `PickupStateSync` keyframe); client binds its locally
  generated pickups to IDs by matching cell — robust even if local spawn order differs.
- **Enemies** → host is the **only** allocator: monotonically increasing u16 assigned at spawn and
  broadcast via `EnemySpawn`. The client never spawns enemies, so IDs are trivially consistent.
  `archetype` indexes a shared class table.
- **Rats** → fixed `playerId` 0/1 from the handshake.

## Tick rates & interpolation

- PlayerState 20 Hz (default) / 30 Hz (opt), unreliable, full snapshot per packet.
- EnemyState 15–20 Hz, unreliable, delta the set with a 1 Hz full keyframe for resync.
- Events reliable + event-driven. Ping/heartbeat 2 Hz.
- Render remote entities ~100 ms behind newest state; extrapolate via velocity up to ~250 ms on
  starvation, then freeze + snap.
