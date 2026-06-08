// INetTransport.h — transport abstraction so the message layer is backend-agnostic
// (UDP/ENet for dev/LAN, Steam GNS for production). Pure C++.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace crc {

enum class ConnState { Disconnected, Listening, Connecting, Connected, Failed };

// One received datagram (already de-fragmented at the transport level).
struct RecvPacket {
    std::vector<uint8_t> data;
};

// Backend-agnostic 2-peer transport. Host listens; client connects. After a peer is
// connected, Send/Receive move opaque byte blobs (the message layer frames them).
class INetTransport {
public:
    virtual ~INetTransport() = default;

    // Lifecycle
    virtual bool startHost(uint16_t port) = 0;
    virtual bool connectTo(const std::string& ip, uint16_t port) = 0;
    virtual void shutdown() = 0;

    // Pump sockets / callbacks. Call every tick on the owning thread.
    virtual void tick() = 0;

    // Send one blob to the peer. reliable=true → guaranteed, ordered on its channel.
    virtual void send(const uint8_t* data, size_t len, bool reliable, uint8_t channel) = 0;

    // Drain received blobs since last call (peer→us), already reassembled & de-duplicated.
    virtual void receive(std::vector<RecvPacket>& out) = 0;

    virtual ConnState state() const = 0;
    virtual const char* backendName() const = 0;
};

} // namespace crc
