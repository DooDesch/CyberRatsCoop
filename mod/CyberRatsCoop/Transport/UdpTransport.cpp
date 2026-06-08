// UdpTransport.cpp — Winsock UDP + minimal reliable layer.
#include "UdpTransport.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#pragma comment(lib, "ws2_32.lib")

namespace crc {

static bool g_wsaInit = false;
static constexpr uint64_t kReliableRtoMs = 120;   // resend interval for unacked reliable
static constexpr int      kMaxDatagram   = 1400;

uint64_t UdpTransport::nowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

UdpTransport::UdpTransport() {
    if (!g_wsaInit) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) == 0) g_wsaInit = true; }
    m_peerAddr = new sockaddr_in{};
}

UdpTransport::~UdpTransport() {
    shutdown();
    delete reinterpret_cast<sockaddr_in*>(m_peerAddr);
}

static void setNonBlocking(SOCKET s) { u_long nb = 1; ioctlsocket(s, FIONBIO, &nb); }

bool UdpTransport::startHost(uint16_t port) {
    m_isHost = true;
    m_sock = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ((SOCKET)m_sock == INVALID_SOCKET) { m_state = ConnState::Failed; return false; }
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (bind((SOCKET)m_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { m_state = ConnState::Failed; return false; }
    setNonBlocking((SOCKET)m_sock);
    m_state = ConnState::Listening;
    return true;
}

bool UdpTransport::connectTo(const std::string& ip, uint16_t port) {
    m_isHost = false;
    m_sock = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ((SOCKET)m_sock == INVALID_SOCKET) { m_state = ConnState::Failed; return false; }
    setNonBlocking((SOCKET)m_sock);
    auto* peer = reinterpret_cast<sockaddr_in*>(m_peerAddr);
    peer->sin_family = AF_INET; peer->sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &peer->sin_addr);
    m_peerKnown = true;
    m_state = ConnState::Connecting;
    // Send a Hello so the host learns our address and the link opens.
    uint8_t hello = WT_Hello;
    rawSendTo(&hello, 1);
    return true;
}

void UdpTransport::shutdown() {
    if ((SOCKET)m_sock != INVALID_SOCKET && (SOCKET)m_sock != ~SOCKET(0)) { closesocket((SOCKET)m_sock); }
    m_sock = ~uintptr_t(0);
    m_peerKnown = false;
    m_state = ConnState::Disconnected;
    m_pending.clear(); m_recvReliable.clear(); m_inbox.clear();
}

void UdpTransport::rawSendTo(const void* buf, int len) {
    if (!m_peerKnown) return;
    auto* peer = reinterpret_cast<sockaddr_in*>(m_peerAddr);
    sendto((SOCKET)m_sock, (const char*)buf, len, 0, (sockaddr*)peer, sizeof(*peer));
}

void UdpTransport::sendAck(uint32_t seq) {
    uint8_t pkt[5]; pkt[0] = WT_Ack;
    std::memcpy(pkt + 1, &seq, 4);
    rawSendTo(pkt, 5);
}

void UdpTransport::send(const uint8_t* data, size_t len, bool reliable, uint8_t channel) {
    if ((SOCKET)m_sock == INVALID_SOCKET) return;
    std::vector<uint8_t> pkt;
    if (reliable) {
        uint32_t seq = m_nextSeq++;
        pkt.reserve(6 + len);
        pkt.push_back(WT_Reliable);
        pkt.insert(pkt.end(), (uint8_t*)&seq, (uint8_t*)&seq + 4);
        pkt.push_back(channel);
        pkt.insert(pkt.end(), data, data + len);
        rawSendTo(pkt.data(), (int)pkt.size());
        m_pending.push_back(Pending{ pkt, nowMs(), seq });
    } else {
        pkt.reserve(2 + len);
        pkt.push_back(WT_Unreliable);
        pkt.push_back(channel);
        pkt.insert(pkt.end(), data, data + len);
        rawSendTo(pkt.data(), (int)pkt.size());
    }
}

void UdpTransport::resendPending(uint64_t nowMs_) {
    for (auto& p : m_pending) {
        if (nowMs_ - p.lastSendMs >= kReliableRtoMs) {
            rawSendTo(p.bytes.data(), (int)p.bytes.size());
            p.lastSendMs = nowMs_;
        }
    }
}

void UdpTransport::tick() {
    if ((SOCKET)m_sock == INVALID_SOCKET) return;
    char buf[2048];
    sockaddr_in from{}; int fromLen = sizeof(from);
    for (;;) {
        int n = recvfrom((SOCKET)m_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) break;
        // Host learns the peer address from the first datagram.
        if (m_isHost && !m_peerKnown) {
            *reinterpret_cast<sockaddr_in*>(m_peerAddr) = from;
            m_peerKnown = true;
            m_state = ConnState::Connected;
        }
        uint8_t type = (uint8_t)buf[0];
        if (type == WT_Hello) { m_state = ConnState::Connected; continue; }
        if (type == WT_Ack) {
            if (n >= 5) {
                uint32_t seq; std::memcpy(&seq, buf + 1, 4);
                for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
                    if (it->seq == seq) { m_pending.erase(it); break; }
                }
            }
            continue;
        }
        if (type == WT_Unreliable) {
            if (n >= 2) m_inbox.push_back(RecvPacket{ std::vector<uint8_t>((uint8_t*)buf + 2, (uint8_t*)buf + n) });
            m_state = ConnState::Connected;
            continue;
        }
        if (type == WT_Reliable) {
            if (n >= 6) {
                uint32_t seq; std::memcpy(&seq, buf + 1, 4);
                sendAck(seq);
                if (m_recvReliable.insert(seq).second) {
                    m_inbox.push_back(RecvPacket{ std::vector<uint8_t>((uint8_t*)buf + 6, (uint8_t*)buf + n) });
                }
            }
            m_state = ConnState::Connected;
            continue;
        }
    }
    resendPending(nowMs());
}

void UdpTransport::receive(std::vector<RecvPacket>& out) {
    out.insert(out.end(), std::make_move_iterator(m_inbox.begin()), std::make_move_iterator(m_inbox.end()));
    m_inbox.clear();
}

} // namespace crc
