// UdpTransport.h — Winsock UDP transport with a minimal reliable layer, for LAN/loopback
// development (the default before Steam GNS). 2 peers. Pure C++ + Winsock.
#pragma once
#include "INetTransport.h"
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>

// fwd-declare to avoid pulling <winsock2.h> into headers
struct sockaddr_in;

namespace crc {

class UdpTransport : public INetTransport {
public:
    UdpTransport();
    ~UdpTransport() override;

    bool startHost(uint16_t port) override;
    bool connectTo(const std::string& ip, uint16_t port) override;
    void shutdown() override;
    void tick() override;
    void send(const uint8_t* data, size_t len, bool reliable, uint8_t channel) override;
    void receive(std::vector<RecvPacket>& out) override;
    ConnState state() const override { return m_state; }
    const char* backendName() const override { return "udp"; }

private:
    // wire envelope: [type u8][seq u32 (reliable/ack only)][channel u8][payload...]
    enum WireType : uint8_t { WT_Unreliable = 0, WT_Reliable = 1, WT_Ack = 2, WT_Hello = 3 };

    void rawSendTo(const void* buf, int len);
    void sendAck(uint32_t seq);
    void resendPending(uint64_t nowMs);

    uintptr_t m_sock = ~uintptr_t(0);     // SOCKET (opaque)
    void* m_peerAddr = nullptr;            // sockaddr_in*
    bool m_peerKnown = false;
    bool m_isHost = false;
    ConnState m_state = ConnState::Disconnected;

    uint32_t m_nextSeq = 1;
    struct Pending { std::vector<uint8_t> bytes; uint64_t lastSendMs; uint32_t seq; };
    std::deque<Pending> m_pending;            // unacked reliable
    std::unordered_set<uint32_t> m_recvReliable; // seen reliable seqs (dedupe)
    std::vector<RecvPacket> m_inbox;

    static uint64_t nowMs();
};

} // namespace crc
