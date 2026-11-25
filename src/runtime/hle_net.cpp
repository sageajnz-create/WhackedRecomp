#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "hle_common.h"
#include "net_lobby.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

#ifdef _WIN32
using NativeSock = SOCKET;
constexpr NativeSock kInvalidSock = INVALID_SOCKET;
inline int close_native_sock(NativeSock s) { return ::closesocket(s); }
inline int sock_last_err() { return ::WSAGetLastError(); }
inline void set_nonblocking(NativeSock s, bool nb) {
    u_long mode = nb ? 1ul : 0ul;
    ::ioctlsocket(s, FIONBIO, &mode);
}
#else
using NativeSock = int;
constexpr NativeSock kInvalidSock = -1;
inline int close_native_sock(NativeSock s) { return ::close(s); }
inline int sock_last_err() {
    switch (errno) {
        case EWOULDBLOCK:
#if defined(EAGAIN) && EAGAIN != EWOULDBLOCK
        case EAGAIN:
#endif
            return 10035; // WSAEWOULDBLOCK
        case EINPROGRESS: return 10036;
        case EBADF:       return 10038; // WSAENOTSOCK
        case EMSGSIZE:    return 10040;
        case EADDRINUSE:  return 10048;
        case ECONNRESET:  return 10054;
        case ETIMEDOUT:   return 10060;
        case ECONNREFUSED:return 10061;
        default:          return 10022;
    }
}
inline void set_nonblocking(NativeSock s, bool nb) {
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) flags = 0;
    ::fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
#endif

constexpr uint32_t kWsaWouldBlock = 10035u;
constexpr uint32_t kWsaNotSock    = 10038u;
constexpr uint32_t kWsaConnReset  = 10054u;

inline uint16_t bswap16(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}
inline uint32_t bswap32(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8) |
           ((v & 0x0000FF00u) << 8)  | ((v & 0x000000FFu) << 24);
}

inline uint32_t make_virtual_ip_nbo(uint8_t id) {
    uint32_t host = (10u << 24) | (69u << 16) | (static_cast<uint32_t>(id) << 8) | 1u;
    return htonl(host);
}

inline std::string ip_nbo_to_str(uint32_t ip_nbo) {
    in_addr ia{};
    ia.s_addr = ip_nbo;
    char buf[32]{};
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&ia.s_addr);
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return std::string(buf);
}

inline uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Tunnel Wire Protocol ("WHK1")
constexpr uint32_t kTunnelMagic = 0x314B4857u; // 'WHK1' LE
enum : uint8_t {
    MSG_HELLO     = 1,
    MSG_WELCOME   = 2,
    MSG_KEEPALIVE = 3,
    MSG_PEERLIST  = 4,
    MSG_DATA      = 5,
    MSG_BYE       = 6
};

struct QueuedDatagram {
    uint32_t src_ip_nbo = 0;
    uint16_t src_port_nbo = 0;
    std::vector<uint8_t> payload;
};

struct VirtualUdpSock {
    uint32_t guest_fd = 0;
    bool bound = false;
    uint16_t bound_port = 0; // host byte order
    uint32_t bound_ip_nbo = 0;
    bool broadcast = false;
    bool nonblocking = false;
    std::deque<QueuedDatagram> rx_queue;
};

struct RemotePeerState {
    uint8_t id = 0;
    sockaddr_in ep{};
    std::string player_name;
    uint64_t last_seen_ms = 0;
    int ping_ms = 0;
    uint64_t last_ping_sent_ms = 0;
};

std::mutex g_net_mtx;
std::condition_variable g_net_cv;
std::atomic<bool> g_wsa_inited{false};

uint32_t g_next_sock = 0x00050001u;
std::unordered_map<uint32_t, VirtualUdpSock> g_vsocks;
std::unordered_map<uint32_t, NativeSock> g_raw_socks;
uint32_t g_last_wsa_err = 0;
uint16_t g_next_ephem_port = 49152u;

// Lobby & tunnel state
NetLobbyMode g_lobby_mode = NetLobbyMode::OfflineOrLan;
std::atomic<bool> g_tunnel_stop{false};
std::atomic<bool> g_tunnel_running{false};
bool g_tunnel_connected = false;
bool g_upnp_ok = false;
uint8_t g_self_id = 17;
uint16_t g_host_tunnel_port = 30740;
std::string g_player_name = "Player1";
std::string g_lan_ip;
std::string g_public_ip;
std::string g_room_code;
std::string g_host_endpoint_str;
std::string g_status_text = "LAN / Offline Ready";
std::atomic<uint64_t> g_tx_pkts{0};
std::atomic<uint64_t> g_rx_pkts{0};

NativeSock g_tunnel_sock = kInvalidSock;
std::mutex g_tunnel_send_mtx;
sockaddr_in g_join_host_ep{};
sockaddr_in g_lan_mcast_ep{};
bool g_have_lan_mcast = false;

std::vector<RemotePeerState> g_host_peers; // Used by host
std::vector<NetPeerInfo> g_cached_roster;  // Used by both host & joiners
std::thread g_net_thread;
std::thread g_probe_thread;

uint8_t random_lan_id() {
    uint64_t t = now_ms() ^ std::random_device{}();
    return static_cast<uint8_t>(17u + (t % 230u));
}

void ensure_wsa() {
#ifdef _WIN32
    if (!g_wsa_inited.exchange(true)) {
        WSADATA wd{};
        ::WSAStartup(MAKEWORD(2, 2), &wd);
    }
#else
    g_wsa_inited.store(true);
#endif
}

std::string query_local_lan_ip() {
    ensure_wsa();
    NativeSock s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSock) return "127.0.0.1";
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    ::inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    ::connect(s, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    std::string res = "127.0.0.1";
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0 && local.sin_addr.s_addr != 0) {
        res = ip_nbo_to_str(local.sin_addr.s_addr);
    }
    close_native_sock(s);
    return res;
}

uint8_t crc8_room(const uint8_t* data, size_t n) {
    uint8_t c = 0xA5u;
    for (size_t i = 0; i < n; ++i) {
        c ^= data[i];
        for (int b = 0; b < 8; ++b) {
            c = (c & 0x80u) ? static_cast<uint8_t>((c << 1) ^ 0x07u) : static_cast<uint8_t>(c << 1);
        }
    }
    return c;
}

const char kBase32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int b32_index(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if (ch == 'I' || ch == 'L') ch = '1';
    if (ch == 'O') ch = '0';
    for (int i = 0; i < 32; ++i) {
        if (kBase32[i] == ch) return i;
    }
    return -1;
}

void write_xnaddr(uint32_t pxna_va, uint32_t ip_nbo) {
    if (!pxna_va) return;
    std::memset(mem_ptr(pxna_va), 0, 36);
    wr32(pxna_va + 0, ip_nbo);
    wr32(pxna_va + 4, ip_nbo);
    wr16(pxna_va + 8, bswap16(3074u));
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip_nbo);
    uint8_t enet[6] = {0x00, 0x50, 0xF2, b[1], b[2], b[3]};
    std::memcpy(mem_ptr(pxna_va + 10), enet, 6);
    std::memcpy(mem_ptr(pxna_va + 16), "WHACKED_RECOMP_SYSLINK", 20);
}

void rebuild_roster_locked() {
    g_cached_roster.clear();
    NetPeerInfo self{};
    self.virtual_ip_nbo = make_virtual_ip_nbo(g_self_id);
    self.virtual_ip_str = ip_nbo_to_str(self.virtual_ip_nbo);
    self.player_name = g_player_name;
    self.ping_ms = 0;
    self.is_self = true;
    self.is_host = (g_lobby_mode == NetLobbyMode::HostRoom && g_self_id == 1);
    g_cached_roster.push_back(self);

    if (g_lobby_mode == NetLobbyMode::HostRoom) {
        for (const auto& rp : g_host_peers) {
            NetPeerInfo p{};
            p.virtual_ip_nbo = make_virtual_ip_nbo(rp.id);
            p.virtual_ip_str = ip_nbo_to_str(p.virtual_ip_nbo);
            p.player_name = rp.player_name;
            p.ping_ms = rp.ping_ms;
            p.is_self = false;
            p.is_host = false;
            g_cached_roster.push_back(p);
        }
    }
}

void tunnel_raw_send(const void* data, size_t len, const sockaddr_in& to) {
    std::lock_guard<std::mutex> lk(g_tunnel_send_mtx);
    if (g_tunnel_sock != kInvalidSock) {
        ::sendto(g_tunnel_sock, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                 reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        g_tx_pkts.fetch_add(1ull);
    }
}

// Deliver datagram into local guest VirtualUdpSockets bound to matching port
void deliver_to_local_vsocks_locked(uint32_t src_ip_nbo, uint16_t src_port_nbo,
                                    uint16_t dst_port_hbo, const uint8_t* payload, size_t len) {
    if (src_ip_nbo == make_virtual_ip_nbo(g_self_id)) return; // never echo self packets unless explicit self unicast
    bool delivered = false;
    for (auto& kv : g_vsocks) {
        auto& vs = kv.second;
        if (!vs.bound || vs.bound_port != dst_port_hbo) continue;
        if (vs.rx_queue.size() >= 256) vs.rx_queue.pop_front();
        QueuedDatagram d{};
        d.src_ip_nbo = src_ip_nbo;
        d.src_port_nbo = src_port_nbo;
        d.payload.assign(payload, payload + len);
        vs.rx_queue.push_back(std::move(d));
        delivered = true;
    }
    if (delivered) {
        g_rx_pkts.fetch_add(1ull);
        g_net_cv.notify_all();
    }
}

std::vector<uint8_t> build_msg_data(uint32_t src_ip_nbo, uint16_t src_port_nbo,
                                    uint32_t dst_ip_nbo, uint16_t dst_port_nbo,
                                    const uint8_t* payload, uint16_t plen) {
    std::vector<uint8_t> pkt(18 + plen);
    std::memcpy(&pkt[0], &kTunnelMagic, 4);
    pkt[4] = MSG_DATA;
    pkt[5] = g_self_id;
    std::memcpy(&pkt[6], &src_ip_nbo, 4);
    std::memcpy(&pkt[10], &src_port_nbo, 2);
    std::memcpy(&pkt[12], &dst_ip_nbo, 4);
    std::memcpy(&pkt[16], &dst_port_nbo, 2); // note overlap fix below -> let's use exact layout
    pkt.resize(20 + plen);
    std::memcpy(&pkt[0], &kTunnelMagic, 4);
    pkt[4] = MSG_DATA;
    pkt[5] = g_self_id;
    std::memcpy(&pkt[6], &src_ip_nbo, 4);
    std::memcpy(&pkt[10], &src_port_nbo, 2);
    std::memcpy(&pkt[12], &dst_ip_nbo, 4);
    std::memcpy(&pkt[16], &dst_port_nbo, 2);
    std::memcpy(&pkt[18], &plen, 2);
    if (plen && payload) std::memcpy(&pkt[20], payload, plen);
    return pkt;
}

void send_stun_probe(NativeSock s) {
    // Minimal RFC 5389 STUN Binding Request to Google STUN servers (20 bytes)
    uint8_t req[20]{};
    req[0] = 0x00; req[1] = 0x01; // Binding Request
    req[2] = 0x00; req[3] = 0x00;
    req[4] = 0x21; req[5] = 0x12; req[6] = 0xA4; req[7] = 0x42; // Magic cookie
    uint64_t tid = now_ms() ^ 0x57484B31u;
    std::memcpy(req + 8, &tid, 8);
    req[16] = 0x57; req[17] = 0x48; req[18] = 0x4B; req[19] = 0x21;

    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (::getaddrinfo("stun.l.google.com", "19302", &hints, &res) == 0 && res) {
        std::lock_guard<std::mutex> lk(g_tunnel_send_mtx);
        ::sendto(s, reinterpret_cast<const char*>(req), sizeof(req), 0, res->ai_addr, static_cast<int>(res->ai_addrlen));
        ::freeaddrinfo(res);
    }
}

void parse_stun_response(const uint8_t* buf, int len) {
    if (len < 20 || buf[0] != 0x01 || buf[1] != 0x01) return;
    if (buf[4] != 0x21 || buf[5] != 0x12 || buf[6] != 0xA4 || buf[7] != 0x42) return;
    int pos = 20;
    while (pos + 4 <= len) {
        uint16_t atype = static_cast<uint16_t>((buf[pos] << 8) | buf[pos + 1]);
        uint16_t alen  = static_cast<uint16_t>((buf[pos + 2] << 8) | buf[pos + 3]);
        pos += 4;
        if (pos + alen > len) break;
        if (atype == 0x0020 && alen >= 8 && buf[pos + 1] == 0x01) { // XOR-MAPPED-ADDRESS IPv4
            uint16_t xport = static_cast<uint16_t>(((buf[pos + 2] ^ 0x21) << 8) | (buf[pos + 3] ^ 0x12));
            uint32_t xip_nbo = 0;
            uint8_t ipb[4] = {
                static_cast<uint8_t>(buf[pos + 4] ^ 0x21),
                static_cast<uint8_t>(buf[pos + 5] ^ 0x12),
                static_cast<uint8_t>(buf[pos + 6] ^ 0xA4),
                static_cast<uint8_t>(buf[pos + 7] ^ 0x42)
            };
            std::memcpy(&xip_nbo, ipb, 4);
            std::string pub = ip_nbo_to_str(xip_nbo);
            std::lock_guard<std::mutex> lk(g_net_mtx);
            g_public_ip = pub;
            if (g_lobby_mode == NetLobbyMode::HostRoom) {
                std::string code;
                if (net_encode_room_code(pub, xport ? xport : g_host_tunnel_port, code)) {
                    g_room_code = code;
                }
            }
            return;
        }
        pos += (alen + 3) & ~3;
    }
}

void try_upnp_mapping_thread(uint16_t port, std::string lan_ip) {
    if (g_tunnel_stop.load()) return;
    ensure_wsa();
    NativeSock s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSock) return;

#ifdef _WIN32
    DWORD tv = 450;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{0, 450000};
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    const char* msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";

    sockaddr_in ssdp{};
    ssdp.sin_family = AF_INET;
    ssdp.sin_port = htons(1900);
    ::inet_pton(AF_INET, "239.255.255.250", &ssdp.sin_addr);
    ::sendto(s, msearch, static_cast<int>(std::strlen(msearch)), 0,
             reinterpret_cast<const sockaddr*>(&ssdp), sizeof(ssdp));

    char buf[2048]{};
    sockaddr_in from{};
    socklen_t flen = sizeof(from);
    int r = ::recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &flen);
    close_native_sock(s);
    if (r <= 0 || g_tunnel_stop.load()) return;

    std::string resp(buf, r);
    std::string loc;
    size_t lpos = resp.find("LOCATION:");
    if (lpos == std::string::npos) lpos = resp.find("Location:");
    if (lpos == std::string::npos) lpos = resp.find("location:");
    if (lpos != std::string::npos) {
        size_t start = resp.find("http://", lpos);
        if (start != std::string::npos) {
            size_t end = resp.find_first_of("\r\n ", start);
            loc = resp.substr(start + 7, end == std::string::npos ? std::string::npos : (end - (start + 7)));
        }
    }
    if (loc.empty()) return;

    size_t slash = loc.find('/');
    std::string hostport = (slash == std::string::npos) ? loc : loc.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : loc.substr(slash);
    size_t colon = hostport.find(':');
    std::string host = (colon == std::string::npos) ? hostport : hostport.substr(0, colon);
    int hport = (colon == std::string::npos) ? 80 : std::atoi(hostport.substr(colon + 1).c_str());

    auto http_req = [&](const std::string& method, const std::string& p,
                        const std::string& extra_hdrs, const std::string& body) -> std::string {
        NativeSock ts = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ts == kInvalidSock) return "";
#ifdef _WIN32
        DWORD htv = 600;
        ::setsockopt(ts, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&htv), sizeof(htv));
        ::setsockopt(ts, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&htv), sizeof(htv));
#else
        timeval htv{0, 600000};
        ::setsockopt(ts, SOL_SOCKET, SO_RCVTIMEO, &htv, sizeof(htv));
        ::setsockopt(ts, SOL_SOCKET, SO_SNDTIMEO, &htv, sizeof(htv));
#endif
        sockaddr_in ha{};
        ha.sin_family = AF_INET;
        ha.sin_port = htons(static_cast<uint16_t>(hport));
        if (::inet_pton(AF_INET, host.c_str(), &ha.sin_addr) != 1) {
            close_native_sock(ts);
            return "";
        }
        if (::connect(ts, reinterpret_cast<const sockaddr*>(&ha), sizeof(ha)) != 0) {
            close_native_sock(ts);
            return "";
        }
        std::string req = method + " " + p + " HTTP/1.1\r\nHost: " + hostport + "\r\nConnection: close\r\n" +
                          extra_hdrs + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        ::send(ts, req.data(), static_cast<int>(req.size()), 0);
        std::string out;
        char chunk[2048];
        int nr;
        while ((nr = ::recv(ts, chunk, sizeof(chunk), 0)) > 0) {
            out.append(chunk, nr);
            if (out.size() > 65536) break;
        }
        close_native_sock(ts);
        return out;
    };

    std::string desc = http_req("GET", path, "", "");
    if (desc.empty() || g_tunnel_stop.load()) return;

    std::string service_type = "urn:schemas-upnp-org:service:WANIPConnection:1";
    size_t stp = desc.find(service_type);
    if (stp == std::string::npos) {
        service_type = "urn:schemas-upnp-org:service:WANPPPConnection:1";
        stp = desc.find(service_type);
    }
    if (stp == std::string::npos) return;

    size_t cu0 = desc.find("<controlURL>", stp);
    size_t cu1 = (cu0 == std::string::npos) ? std::string::npos : desc.find("</controlURL>", cu0);
    if (cu0 == std::string::npos || cu1 == std::string::npos) return;
    std::string ctrl = desc.substr(cu0 + 12, cu1 - (cu0 + 12));
    if (!ctrl.empty() && ctrl[0] != '/') ctrl = "/" + ctrl;

    std::string soap =
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n<s:Body>\r\n"
        "<u:AddPortMapping xmlns:u=\"" + service_type + "\">\r\n"
        "<NewRemoteHost></NewRemoteHost>\r\n"
        "<NewExternalPort>" + std::to_string(port) + "</NewExternalPort>\r\n"
        "<NewProtocol>UDP</NewProtocol>\r\n"
        "<NewInternalPort>" + std::to_string(port) + "</NewInternalPort>\r\n"
        "<NewInternalClient>" + lan_ip + "</NewInternalClient>\r\n"
        "<NewEnabled>1</NewEnabled>\r\n"
        "<NewPortMappingDescription>WhackedRecomp</NewPortMappingDescription>\r\n"
        "<NewLeaseDuration>3600</NewLeaseDuration>\r\n"
        "</u:AddPortMapping>\r\n</s:Body>\r\n</s:Envelope>\r\n";

    std::string hdr = "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                      "SOAPACTION: \"" + service_type + "#AddPortMapping\"\r\n";
    std::string map_res = http_req("POST", ctrl, hdr, soap);
    if (map_res.find("200 OK") != std::string::npos) {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        g_upnp_ok = true;
    }
}

void tunnel_worker_loop() {
    uint64_t last_ka_ms = 0;
    uint64_t last_roster_ms = 0;
    uint64_t last_hello_ms = 0;
    bool stun_sent = false;

    uint8_t buf[8192];
    while (!g_tunnel_stop.load()) {
        if (!stun_sent && g_lobby_mode == NetLobbyMode::HostRoom && g_tunnel_sock != kInvalidSock) {
            stun_sent = true;
            send_stun_probe(g_tunnel_sock);
        }

        pollfd pfd{};
        pfd.fd = g_tunnel_sock;
        pfd.events = POLLIN;
#ifdef _WIN32
        int pr = ::WSAPoll(&pfd, 1, 20);
#else
        int pr = ::poll(&pfd, 1, 20);
#endif
        if (pr > 0 && (pfd.revents & POLLIN)) {
            for (int iter = 0; iter < 64; ++iter) {
                sockaddr_in from{};
                socklen_t flen = sizeof(from);
                int n = ::recvfrom(g_tunnel_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&from), &flen);
                if (n <= 0) break;

                // STUN Binding Response check
                if (n >= 20 && buf[0] == 0x01 && buf[1] == 0x01) {
                    parse_stun_response(buf, n);
                    continue;
                }

                if (n < 6) continue;
                uint32_t magic = 0;
                std::memcpy(&magic, buf, 4);
                if (magic != kTunnelMagic) continue;
                uint8_t msg_type = buf[4];
                uint8_t sender_id = buf[5];

                if (msg_type == MSG_DATA && n >= 20) {
                    uint32_t src_ip = 0, dst_ip = 0;
                    uint16_t src_port = 0, dst_port = 0, plen = 0;
                    std::memcpy(&src_ip,   buf + 6,  4);
                    std::memcpy(&src_port, buf + 10, 2);
                    std::memcpy(&dst_ip,   buf + 12, 4);
                    std::memcpy(&dst_port, buf + 16, 2);
                    std::memcpy(&plen,     buf + 18, 2);
                    if (20 + plen > n) continue;
                    const uint8_t* payload = buf + 20;

                    if (g_lobby_mode == NetLobbyMode::HostRoom) {
                        // Forward from client to destination peers + inject locally if matching
                        bool is_bcast = (dst_ip == 0xFFFFFFFFu) || ((ntohl(dst_ip) & 0xFFu) == 0xFFu);
                        std::vector<sockaddr_in> forward_targets;
                        {
                            std::lock_guard<std::mutex> lk(g_net_mtx);
                            for (auto& rp : g_host_peers) {
                                if (rp.ep.sin_addr.s_addr == from.sin_addr.s_addr && rp.ep.sin_port == from.sin_port) {
                                    rp.last_seen_ms = now_ms();
                                    continue;
                                }
                                if (is_bcast || make_virtual_ip_nbo(rp.id) == dst_ip) {
                                    forward_targets.push_back(rp.ep);
                                }
                            }
                            if (is_bcast || dst_ip == make_virtual_ip_nbo(g_self_id)) {
                                deliver_to_local_vsocks_locked(src_ip, src_port, ntohs(dst_port), payload, plen);
                            }
                        }
                        for (const auto& ep : forward_targets) {
                            tunnel_raw_send(buf, static_cast<size_t>(n), ep);
                        }
                    } else {
                        std::lock_guard<std::mutex> lk(g_net_mtx);
                        deliver_to_local_vsocks_locked(src_ip, src_port, ntohs(dst_port), payload, plen);
                    }
                } else if (msg_type == MSG_HELLO && g_lobby_mode == NetLobbyMode::HostRoom && n >= 40) {
                    char pname[33]{};
                    std::memcpy(pname, buf + 8, 32);
                    uint8_t assigned_id = 0;
                    std::vector<uint8_t> welcome(8, 0);
                    {
                        std::lock_guard<std::mutex> lk(g_net_mtx);
                        for (auto& rp : g_host_peers) {
                            if (rp.ep.sin_addr.s_addr == from.sin_addr.s_addr && rp.ep.sin_port == from.sin_port) {
                                assigned_id = rp.id;
                                rp.player_name = pname[0] ? pname : "Player";
                                rp.last_seen_ms = now_ms();
                                break;
                            }
                        }
                        if (!assigned_id) {
                            for (uint8_t cand = 2; cand <= 16; ++cand) {
                                bool used = false;
                                for (const auto& rp : g_host_peers) if (rp.id == cand) used = true;
                                if (!used) { assigned_id = cand; break; }
                            }
                            if (assigned_id) {
                                RemotePeerState rp{};
                                rp.id = assigned_id;
                                rp.ep = from;
                                rp.player_name = pname[0] ? pname : ("Player" + std::to_string(assigned_id));
                                rp.last_seen_ms = now_ms();
                                g_host_peers.push_back(rp);
                            }
                        }
                        rebuild_roster_locked();
                        g_status_text = "Hosting (" + std::to_string(g_host_peers.size() + 1) + " players)";
                    }
                    if (assigned_id) {
                        std::memcpy(&welcome[0], &kTunnelMagic, 4);
                        welcome[4] = MSG_WELCOME;
                        welcome[5] = 1; // host id
                        welcome[6] = assigned_id;
                        welcome[7] = 0;
                        tunnel_raw_send(welcome.data(), welcome.size(), from);
                    }
                } else if (msg_type == MSG_WELCOME && g_lobby_mode == NetLobbyMode::JoinRoom && n >= 8) {
                    uint8_t my_id = buf[6];
                    if (my_id >= 2 && my_id <= 16) {
                        std::lock_guard<std::mutex> lk(g_net_mtx);
                        g_self_id = my_id;
                        g_tunnel_connected = true;
                        g_status_text = "Connected (" + ip_nbo_to_str(make_virtual_ip_nbo(my_id)) + ")";
                        rebuild_roster_locked();
                    }
                } else if (msg_type == MSG_KEEPALIVE && n >= 16) {
                    uint64_t ts = 0;
                    std::memcpy(&ts, buf + 8, 8);
                    if (g_lobby_mode == NetLobbyMode::HostRoom) {
                        std::lock_guard<std::mutex> lk(g_net_mtx);
                        for (auto& rp : g_host_peers) {
                            if (rp.id == sender_id) {
                                rp.last_seen_ms = now_ms();
                                if (buf[6] == 1 && ts > 0 && now_ms() >= ts) { // Pong from client
                                    rp.ping_ms = static_cast<int>(std::min<uint64_t>(999, now_ms() - ts));
                                }
                                break;
                            }
                        }
                        if (buf[6] == 0) { // Client Ping -> echo Pong back
                            buf[6] = 1;
                            tunnel_raw_send(buf, 16, from);
                        }
                    } else if (g_lobby_mode == NetLobbyMode::JoinRoom) {
                        if (buf[6] == 0) { // Host Ping -> reply Pong
                            buf[5] = g_self_id;
                            buf[6] = 1;
                            tunnel_raw_send(buf, 16, g_join_host_ep);
                        }
                    }
                } else if (msg_type == MSG_PEERLIST && g_lobby_mode == NetLobbyMode::JoinRoom && n >= 8) {
                    uint8_t cnt = buf[6];
                    if (8 + cnt * 36 <= n) {
                        std::lock_guard<std::mutex> lk(g_net_mtx);
                        g_cached_roster.clear();
                        for (uint8_t i = 0; i < cnt; ++i) {
                            const uint8_t* rec = buf + 8 + i * 36;
                            uint8_t pid = rec[0];
                            uint16_t pms = 0;
                            std::memcpy(&pms, rec + 2, 2);
                            char nm[33]{};
                            std::memcpy(nm, rec + 4, 32);
                            NetPeerInfo info{};
                            info.virtual_ip_nbo = make_virtual_ip_nbo(pid);
                            info.virtual_ip_str = ip_nbo_to_str(info.virtual_ip_nbo);
                            info.player_name = nm;
                            info.ping_ms = pms;
                            info.is_self = (pid == g_self_id);
                            info.is_host = (pid == 1);
                            g_cached_roster.push_back(info);
                        }
                    }
                } else if (msg_type == MSG_BYE && g_lobby_mode == NetLobbyMode::HostRoom) {
                    std::lock_guard<std::mutex> lk(g_net_mtx);
                    g_host_peers.erase(std::remove_if(g_host_peers.begin(), g_host_peers.end(),
                        [&](const RemotePeerState& r) { return r.id == sender_id; }), g_host_peers.end());
                    rebuild_roster_locked();
                }
            }
        }

        uint64_t t = now_ms();
        if (g_lobby_mode == NetLobbyMode::JoinRoom) {
            if (!g_tunnel_connected && t - last_hello_ms >= 350) {
                last_hello_ms = t;
                uint8_t hello[40]{};
                std::memcpy(&hello[0], &kTunnelMagic, 4);
                hello[4] = MSG_HELLO;
                hello[5] = 0;
                std::lock_guard<std::mutex> lk(g_net_mtx);
                std::snprintf(reinterpret_cast<char*>(hello + 8), 32, "%s", g_player_name.c_str());
                tunnel_raw_send(hello, sizeof(hello), g_join_host_ep);
            } else if (g_tunnel_connected && t - last_ka_ms >= 1000) {
                last_ka_ms = t;
                uint8_t ka[16]{};
                std::memcpy(&ka[0], &kTunnelMagic, 4);
                ka[4] = MSG_KEEPALIVE;
                ka[5] = g_self_id;
                ka[6] = 0; // Ping
                std::memcpy(&ka[8], &t, 8);
                tunnel_raw_send(ka, sizeof(ka), g_join_host_ep);
            }
        } else if (g_lobby_mode == NetLobbyMode::HostRoom && t - last_roster_ms >= 800) {
            last_roster_ms = t;
            std::vector<sockaddr_in> targets;
            std::vector<uint8_t> roster_pkt;
            std::vector<uint8_t> ping_pkt(16, 0);
            {
                std::lock_guard<std::mutex> lk(g_net_mtx);
                size_t before = g_host_peers.size();
                g_host_peers.erase(std::remove_if(g_host_peers.begin(), g_host_peers.end(),
                    [&](const RemotePeerState& r) { return (t - r.last_seen_ms) > 10000; }), g_host_peers.end());
                if (g_host_peers.size() != before) {
                    rebuild_roster_locked();
                    g_status_text = "Hosting (" + std::to_string(g_host_peers.size() + 1) + " players)";
                } else {
                    rebuild_roster_locked();
                }
                uint8_t cnt = static_cast<uint8_t>(std::min<size_t>(16, g_cached_roster.size()));
                roster_pkt.assign(8 + cnt * 36, 0);
                std::memcpy(&roster_pkt[0], &kTunnelMagic, 4);
                roster_pkt[4] = MSG_PEERLIST;
                roster_pkt[5] = 1;
                roster_pkt[6] = cnt;
                for (uint8_t i = 0; i < cnt; ++i) {
                    uint8_t* rec = &roster_pkt[8 + i * 36];
                    rec[0] = g_cached_roster[i].is_host ? 1 : static_cast<uint8_t>((ntohl(g_cached_roster[i].virtual_ip_nbo) >> 8) & 0xFFu);
                    uint16_t pms = static_cast<uint16_t>(std::clamp(g_cached_roster[i].ping_ms, 0, 999));
                    std::memcpy(rec + 2, &pms, 2);
                    std::snprintf(reinterpret_cast<char*>(rec + 4), 32, "%s", g_cached_roster[i].player_name.c_str());
                }
                for (const auto& rp : g_host_peers) targets.push_back(rp.ep);
            }
            std::memcpy(&ping_pkt[0], &kTunnelMagic, 4);
            ping_pkt[4] = MSG_KEEPALIVE;
            ping_pkt[5] = 1;
            ping_pkt[6] = 0;
            std::memcpy(&ping_pkt[8], &t, 8);
            for (const auto& ep : targets) {
                tunnel_raw_send(roster_pkt.data(), roster_pkt.size(), ep);
                tunnel_raw_send(ping_pkt.data(), ping_pkt.size(), ep);
            }
        }
    }
}

void stop_worker_threads() {
    if (g_lobby_mode == NetLobbyMode::JoinRoom && g_tunnel_connected && g_tunnel_sock != kInvalidSock) {
        uint8_t bye[8]{};
        std::memcpy(&bye[0], &kTunnelMagic, 4);
        bye[4] = MSG_BYE;
        bye[5] = g_self_id;
        tunnel_raw_send(bye, sizeof(bye), g_join_host_ep);
    }
    g_tunnel_stop.store(true);
    if (g_net_thread.joinable()) g_net_thread.join();
    if (g_probe_thread.joinable()) g_probe_thread.join();
    {
        std::lock_guard<std::mutex> lk(g_tunnel_send_mtx);
        if (g_tunnel_sock != kInvalidSock) {
            close_native_sock(g_tunnel_sock);
            g_tunnel_sock = kInvalidSock;
        }
    }
    g_tunnel_running.store(false);
    g_tunnel_stop.store(false);
}

// =========================================================================
// Guest XNet & Winsock2 Virtualized HLE Thunks
// =========================================================================

void hle_XNetStartup(CpuContext& ctx) {
    ensure_wsa();
    if (!g_tunnel_running.load()) {
        net_start_offline_lan(g_player_name);
    }
    hle_ret_u32(ctx, 0, 4);
}

void hle_WSAStartup(CpuContext& ctx) {
    ensure_wsa();
    uint32_t wsa_va = hle_arg(ctx, 1);
    if (wsa_va) {
        std::memset(mem_ptr(wsa_va), 0, 400);
        wr16(wsa_va + 0, 0x0202);
        wr16(wsa_va + 2, 0x0202);
    }
    hle_ret_u32(ctx, 0, 8);
}

void hle_WSACleanup(CpuContext& ctx) { hle_ret_u32(ctx, 0, 0); }
void hle_WSAGetLastError(CpuContext& ctx) { hle_ret_u32(ctx, g_last_wsa_err, 0); }

void hle_htonl_xb(CpuContext& ctx) {
    uint32_t v = hle_arg(ctx, 0);
    hle_ret_u32(ctx, bswap32(v), 4);
}
void hle_htons_xb(CpuContext& ctx) {
    uint16_t v = static_cast<uint16_t>(hle_arg(ctx, 0));
    hle_ret_u32(ctx, bswap16(v), 4);
}

void fill_random(uint32_t va, uint32_t cb) {
    if (!va || !cb) return;
    uint64_t seed = now_ms() ^ std::random_device{}() ^ 0x9E3779B97F4A7C15ull;
    uint8_t* dst = mem_ptr(va);
    for (uint32_t i = 0; i < cb; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        dst[i] = static_cast<uint8_t>(seed >> 56);
    }
}

void hle_XNetRandom(CpuContext& ctx) {
    fill_random(hle_arg(ctx, 0), hle_arg(ctx, 1));
    hle_ret_u32(ctx, 0, 8);
}

void hle_XNetCreateKey(CpuContext& ctx) {
    uint32_t pkid = hle_arg(ctx, 0);
    uint32_t pkey = hle_arg(ctx, 1);
    if (pkid) {
        fill_random(pkid, 8);
        wr8(pkid, rd8(pkid) & 0x0Fu);
    }
    if (pkey) fill_random(pkey, 16);
    hle_ret_u32(ctx, 0, 8);
}

void hle_XNetGetTitleXnAddr(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_net_mtx);
    write_xnaddr(hle_arg(ctx, 0), make_virtual_ip_nbo(g_self_id));
    hle_ret_u32(ctx, 0x00000004u, 4); // XNET_GET_XNADDR_STATIC
}

void hle_XNetXnAddrToInAddr(CpuContext& ctx) {
    uint32_t pxna = hle_arg(ctx, 0);
    uint32_t pina = hle_arg(ctx, 2);
    uint32_t ip = pxna ? rd32(pxna) : 0u;
    if (!ip) {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        ip = make_virtual_ip_nbo(g_self_id);
    }
    if (pina) wr32(pina, ip);
    hle_ret_u32(ctx, 0, 12);
}

void hle_XNetInAddrToXnAddr(CpuContext& ctx) {
    uint32_t in_addr_nbo = hle_arg(ctx, 0);
    uint32_t pxna = hle_arg(ctx, 1);
    uint32_t pkid = hle_arg(ctx, 2);
    uint32_t ip = in_addr_nbo;
    if (!ip) {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        ip = make_virtual_ip_nbo(g_self_id);
    }
    write_xnaddr(pxna, ip);
    if (pkid) std::memset(mem_ptr(pkid), 0, 8);
    hle_ret_u32(ctx, 0, 12);
}

void hle_socket(CpuContext& ctx) {
    ensure_wsa();
    int af = static_cast<int>(hle_arg(ctx, 0));
    int type = static_cast<int>(hle_arg(ctx, 1));
    int proto = static_cast<int>(hle_arg(ctx, 2));

    if (af == AF_INET && (type == SOCK_DGRAM || proto == 0xFE)) {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        uint32_t g_fd = g_next_sock++;
        VirtualUdpSock vs{};
        vs.guest_fd = g_fd;
        g_vsocks[g_fd] = std::move(vs);
        g_last_wsa_err = 0;
        hle_ret_u32(ctx, g_fd, 12);
        return;
    }

    if (proto == 0xFE || proto == 0) proto = (type == SOCK_DGRAM) ? IPPROTO_UDP : IPPROTO_TCP;
    NativeSock s = ::socket(af, type, proto);
    if (s == kInvalidSock) {
        g_last_wsa_err = static_cast<uint32_t>(sock_last_err());
        hle_ret_u32(ctx, 0xFFFFFFFFu, 12);
        return;
    }
    std::lock_guard<std::mutex> lk(g_net_mtx);
    uint32_t g_fd = g_next_sock++;
    g_raw_socks[g_fd] = s;
    g_last_wsa_err = 0;
    hle_ret_u32(ctx, g_fd, 12);
}

void hle_closesocket(CpuContext& ctx) {
    uint32_t g_fd = hle_arg(ctx, 0);
    NativeSock raw = kInvalidSock;
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        if (g_vsocks.erase(g_fd)) {
            g_last_wsa_err = 0;
            g_net_cv.notify_all();
            hle_ret_u32(ctx, 0, 4);
            return;
        }
        auto it = g_raw_socks.find(g_fd);
        if (it != g_raw_socks.end()) {
            raw = it->second;
            g_raw_socks.erase(it);
        }
    }
    if (raw != kInvalidSock) close_native_sock(raw);
    g_last_wsa_err = 0;
    hle_ret_u32(ctx, 0, 4);
}

void hle_setsockopt(CpuContext& ctx) {
    uint32_t g_fd = hle_arg(ctx, 0);
    int level = static_cast<int>(hle_arg(ctx, 1));
    int opt = static_cast<int>(hle_arg(ctx, 2));
    uint32_t val_va = hle_arg(ctx, 3);
    int len = static_cast<int>(hle_arg(ctx, 4));
    NativeSock raw = kInvalidSock;
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        auto vit = g_vsocks.find(g_fd);
        if (vit != g_vsocks.end()) {
            if ((level == 0xFFFF || level == SOL_SOCKET) && (opt == 0x0020 || opt == SO_BROADCAST) && val_va && len >= 4) {
                vit->second.broadcast = (rd32(val_va) != 0);
            }
            g_last_wsa_err = 0;
            hle_ret_u32(ctx, 0, 20);
            return;
        }
        auto rit = g_raw_socks.find(g_fd);
        if (rit != g_raw_socks.end()) raw = rit->second;
    }
    if (raw == kInvalidSock) {
        g_last_wsa_err = kWsaNotSock;
        hle_ret_u32(ctx, 0xFFFFFFFFu, 20);
        return;
    }
    ::setsockopt(raw, level, opt, val_va ? reinterpret_cast<const char*>(mem_ptr(val_va)) : nullptr, len);
    g_last_wsa_err = 0;
    hle_ret_u32(ctx, 0, 20);
}

void hle_getsockopt(CpuContext& ctx) {
    uint32_t g_fd = hle_arg(ctx, 0);
    int level = static_cast<int>(hle_arg(ctx, 1));
    int opt = static_cast<int>(hle_arg(ctx, 2));
    uint32_t val_va = hle_arg(ctx, 3);
    uint32_t len_va = hle_arg(ctx, 4);
    NativeSock raw = kInvalidSock;
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        if (g_vsocks.count(g_fd)) {
            if (val_va) wr32(val_va, 0);
            if (len_va) wr32(len_va, 4);
            g_last_wsa_err = 0;
            hle_ret_u32(ctx, 0, 20);
            return;
        }
        auto rit = g_raw_socks.find(g_fd);
        if (rit != g_raw_socks.end()) raw = rit->second;
    }
    if (raw == kInvalidSock) {
        g_last_wsa_err = kWsaNotSock;
        hle_ret_u32(ctx, 0xFFFFFFFFu, 20);
        return;
    }
    socklen_t len = len_va ? static_cast<socklen_t>(rd32(len_va)) : 0;
    int r = ::getsockopt(raw, level, opt, val_va ? reinterpret_cast<char*>(mem_ptr(val_va)) : nullptr, len_va ? &len : nullptr);
    if (len_va) wr32(len_va, static_cast<uint32_t>(len));
    g_last_wsa_err = (r < 0) ? static_cast<uint32_t>(sock_last_err()) : 0;
    hle_ret_u32(ctx, static_cast<uint32_t>(r < 0 ? -1 : 0), 20);
}

void hle_ioctlsocket(CpuContext& ctx) {
    uint32_t g_fd = hle_arg(ctx, 0);
    uint32_t cmd = hle_arg(ctx, 1);
    uint32_t argp_va = hle_arg(ctx, 2);
    NativeSock raw = kInvalidSock;
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        auto vit = g_vsocks.find(g_fd);
        if (vit != g_vsocks.end()) {
            if (cmd == 0x8004667Eu && argp_va) { // FIONBIO
                vit->second.nonblocking = (rd32(argp_va) != 0);
            } else if (cmd == 0x4004667Fu && argp_va) { // FIONREAD
                uint32_t avail = vit->second.rx_queue.empty() ? 0u : static_cast<uint32_t>(vit->second.rx_queue.front().payload.size());
                wr32(argp_va, avail);
            }
            g_last_wsa_err = 0;
            hle_ret_u32(ctx, 0, 12);
            return;
        }
        auto rit = g_raw_socks.find(g_fd);
        if (rit != g_raw_socks.end()) raw = rit->second;
    }
    if (raw == kInvalidSock) {
        g_last_wsa_err = kWsaNotSock;
        hle_ret_u32(ctx, 0xFFFFFFFFu, 12);
        return;
    }
#ifdef _WIN32
    u_long val = argp_va ? rd32(argp_va) : 0;
    int r = ::ioctlsocket(raw, static_cast<long>(cmd), &val);
    if (argp_va) wr32(argp_va, static_cast<uint32_t>(val));
#else
    int r = 0;
    if (cmd == 0x8004667Eu && argp_va) set_nonblocking(raw, rd32(argp_va) != 0);
#endif
    g_last_wsa_err = (r < 0) ? static_cast<uint32_t>(sock_last_err()) : 0;
    hle_ret_u32(ctx, static_cast<uint32_t>(r < 0 ? -1 : 0), 12);
}

void hle_bind(CpuContext& ctx) {
    uint32_t g_fd = hle_arg(ctx, 0);
    uint32_t name_va = hle_arg(ctx, 1);
    int namelen = static_cast<int>(hle_arg(ctx, 2));
    if (!name_va || namelen < 8) {
        g_last_wsa_err = kWsaNotSock;
        hle_ret_u32(ctx, 0xFFFFFFFFu, 12);
        return;
    }
    uint16_t port_nbo = rd16(name_va + 2);
    uint32_t ip_nbo = rd32(name_va + 4);
    uint16_t port_hbo = bswap16(port_nbo);

    NativeSock raw = kInvalidSock;
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        auto vit = g_vsocks.find(g_fd);
        if (vit != g_vsocks.end()) {
            vit->second.bound = true;
            vit->second.bound_port = port_hbo ? port_hbo : (g_next_ephem_port++);
            vit->second.bound_ip_nbo = ip_nbo;
            g_last_wsa_err = 0;
            if (g_hle_trace) {
                std::printf("[NET] virtual bind(port=%u -> vsock 0x%X)\n", vit->second.bound_port, g_fd);
            }
            hle_ret_u32(ctx, 0, 12);
            return;
        }
        auto rit = g_raw_socks.find(g_fd);
        if (rit != g_raw_socks.end()) raw = rit->second;
    }
    if (raw == kInvalidSock) {
        g_last_wsa_err = kWsaNotSock;
        hle_ret_u32(ctx, 0xFFFFFFFFu, 12);
        return;
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = port_nbo;
    sa.sin_addr.s_addr = ip_nbo;
    int r = ::bind(raw, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
    g_last_wsa_err = (r < 0) ? static_cast<uint32_t>(sock_last_err()) : 0;
    hle_ret_u32(ctx, static_cast<uint32_t>(r < 0 ? -1 : 0), 12);
}

void hle_connect(CpuContext& ctx) {
    uint32_t g_fd = hle_arg(ctx, 0);
    uint32_t name_va = hle_arg(ctx, 1);
    int namelen = static_cast<int>(hle_arg(ctx, 2));
    if (!name_va || namelen < 8) {
        g_last_wsa_err = kWsaNotSock;
        hle_ret_u32(ctx, 0xFFFFFFFFu, 12);
        return;
    }
    NativeSock raw = kInvalidSock;
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        if (g_vsocks.count(g_fd)) {
            g_last_wsa_err = 0;
            hle_ret_u32(ctx, 0, 12);
            return;
        }
        auto rit = g_raw_socks.find(g_fd);
        if (rit != g_raw_socks.end()) raw = rit->second;
    }
    if (raw == kInvalidSock) {
        g_last_wsa_err = kWsaNotSock;
        hle_ret_u32(ctx, 0xFFFFFFFFu, 12);
        return;
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = rd16(name_va + 2);
    sa.sin_addr.s_addr = rd32(name_va + 4);
    int r = ::connect(raw, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
    g_last_wsa_err = (r < 0) ? static_cast<uint32_t>(sock_last_err()) : 0;
    hle_ret_u32(ctx, static_cast<uint32_t>(r < 0 ? -1 : 0), 12);
}

void hle_select(CpuContext& ctx) {
    uint32_t rfds_va = hle_arg(ctx, 1);
    uint32_t wfds_va = hle_arg(ctx, 2);
    uint32_t efds_va = hle_arg(ctx, 3);
    uint32_t to_va = hle_arg(ctx, 4);

    auto load_ids = [](uint32_t va) -> std::vector<uint32_t> {
        std::vector<uint32_t> v;
        if (!va) return v;
        uint32_t n = std::min<uint32_t>(rd32(va), 64u);
        for (uint32_t i = 0; i < n; ++i) v.push_back(rd32(va + 4 + i * 4));
        return v;
    };
    std::vector<uint32_t> rids = load_ids(rfds_va);
    std::vector<uint32_t> wids = load_ids(wfds_va);
    std::vector<uint32_t> eids = load_ids(efds_va);

    int64_t wait_us = -1;
    if (to_va) {
        uint32_t sec = rd32(to_va + 0);
        uint32_t usec = rd32(to_va + 4);
        wait_us = static_cast<int64_t>(sec) * 1000000LL + static_cast<int64_t>(usec);
    }

    auto has_raw = [&]() -> bool {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        for (uint32_t id : rids) if (g_raw_socks.count(id)) return true;
        for (uint32_t id : wids) if (g_raw_socks.count(id)) return true;
        for (uint32_t id : eids) if (g_raw_socks.count(id)) return true;
        return false;
    };

    if (!has_raw()) {
        // Pure virtual UDP socket select
        auto check_ready = [&](std::vector<uint32_t>& r_out, std::vector<uint32_t>& w_out) -> int {
            r_out.clear();
            w_out.clear();
            for (uint32_t id : rids) {
                auto it = g_vsocks.find(id);
                if (it != g_vsocks.end() && !it->second.rx_queue.empty()) r_out.push_back(id);
            }
            for (uint32_t id : wids) {
                if (g_vsocks.count(id)) w_out.push_back(id);
            }
            return static_cast<int>(r_out.size() + w_out.size());
        };

        std::unique_lock<std::mutex> lk(g_net_mtx);
        std::vector<uint32_t> rr, wr;
        int ready = check_ready(rr, wr);
        if (ready == 0 && wait_us != 0 && !rids.empty()) {
            if (wait_us < 0) {
                g_net_cv.wait(lk, [&]() { return check_ready(rr, wr) > 0 || g_tunnel_stop.load(); });
            } else {
                g_net_cv.wait_for(lk, std::chrono::microseconds(std::min<int64_t>(wait_us, 50000)),
                                  [&]() { return check_ready(rr, wr) > 0; });
            }
            ready = check_ready(rr, wr);
        }
        auto write_set = [](uint32_t va, const std::vector<uint32_t>& ids) {
            if (!va) return;
            uint32_t cnt = static_cast<uint32_t>(std::min<size_t>(64, ids.size()));
            for (uint32_t i = 0; i < cnt; ++i) wr32(va + 4 + i * 4, ids[i]);
            wr32(va, cnt);
        };
        write_set(rfds_va, rr);
        write_set(wfds_va, wr);
        if (efds_va) wr32(efds_va, 0);
        g_last_wsa_err = 0;
        hle_ret_u32(ctx, static_cast<uint32_t>(ready), 20);
        return;
    }

    // Raw OS select fallback
    fd_set rh, wh, eh;
    FD_ZERO(&rh); FD_ZERO(&wh); FD_ZERO(&eh);
    NativeSock maxfd = 0;
    std::vector<std::pair<uint32_t, NativeSock>> rp, wp, ep;
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        for (uint32_t id : rids) { auto it = g_raw_socks.find(id); if (it != g_raw_socks.end()) { FD_SET(it->second, &rh); rp.push_back(*it); maxfd = std::max(maxfd, it->second); } }
        for (uint32_t id : wids) { auto it = g_raw_socks.find(id); if (it != g_raw_socks.end()) { FD_SET(it->second, &wh); wp.push_back(*it); maxfd = std::max(maxfd, it->second); } }
        for (uint32_t id : eids) { auto it = g_raw_socks.find(id); if (it != g_raw_socks.end()) { FD_SET(it->second, &eh); ep.push_back(*it); maxfd = std::max(maxfd, it->second); } }
    }
    timeval tv{};
    timeval* ptv = nullptr;
    if (wait_us >= 0) {
        tv.tv_sec = static_cast<long>(wait_us / 1000000LL);
        tv.tv_usec = static_cast<long>(wait_us % 1000000LL);
        ptv = &tv;
    }
    int r = ::select(static_cast<int>(maxfd) + 1, rp.empty() ? nullptr : &rh, wp.empty() ? nullptr : &wh, ep.empty() ? nullptr : &eh, ptv);
    auto write_raw = [](uint32_t va, fd_set* s, const std::vector<std::pair<uint32_t, NativeSock>>& ps) {
        if (!va) return;
        uint32_t cnt = 0;
        for (const auto& p : ps) {
            if (FD_ISSET(p.second, s) && cnt < 64u) wr32(va + 4 + (cnt++) * 4, p.first);
        }
        wr32(va, cnt);
    };
    if (r >= 0) {
        write_raw(rfds_va, &rh, rp);
        write_raw(wfds_va, &wh, wp);
        write_raw(efds_va, &eh, ep);
        g_last_wsa_err = 0;
    } else {
        g_last_wsa_err = static_cast<uint32_t>(sock_last_err());
    }
    hle_ret_u32(ctx, static_cast<uint32_t>(r < 0 ? -1 : r), 20);
}

void hle_sendto(CpuContext& ctx) {
    uint32_t g_fd = hle_arg(ctx, 0);
    uint32_t buf_va = hle_arg(ctx, 1);
    int len = static_cast<int>(hle_arg(ctx, 2));
    int flags = static_cast<int>(hle_arg(ctx, 3));
    uint32_t to_va = hle_arg(ctx, 4);
    int tolen = static_cast<int>(hle_arg(ctx, 5));

    NativeSock raw = kInvalidSock;
    bool is_virtual = false;
    bool handled = false;
    uint32_t handled_ret = static_cast<uint32_t>(len);
    std::vector<uint8_t> send_pkt;
    std::vector<sockaddr_in> fanout_eps;

    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        auto vit = g_vsocks.find(g_fd);
        if (vit != g_vsocks.end()) {
            is_virtual = true;
            if (!vit->second.bound) {
                vit->second.bound = true;
                vit->second.bound_port = g_next_ephem_port++;
            }
            uint32_t src_ip_nbo = make_virtual_ip_nbo(g_self_id);
            uint16_t src_port_nbo = bswap16(vit->second.bound_port);

            uint32_t dst_ip_nbo = (to_va && tolen >= 8) ? rd32(to_va + 4) : 0xFFFFFFFFu;
            uint16_t dst_port_nbo = (to_va && tolen >= 8) ? rd16(to_va + 2) : src_port_nbo;
            uint16_t dst_port_hbo = bswap16(dst_port_nbo);

            const uint8_t* payload = buf_va ? mem_ptr(buf_va) : nullptr;
            uint16_t plen = static_cast<uint16_t>(std::clamp(len, 0, 4096));

            uint32_t dst_host_ip = ntohl(dst_ip_nbo);
            bool is_bcast = (dst_ip_nbo == 0xFFFFFFFFu) || ((dst_host_ip & 0xFFu) == 0xFFu) || (dst_ip_nbo == 0);
            bool is_self  = (dst_ip_nbo == src_ip_nbo) || (dst_host_ip == 0x7F000001u);

            if (is_self) {
                for (auto& kv : g_vsocks) {
                    if (kv.second.bound && kv.second.bound_port == dst_port_hbo) {
                        if (kv.second.rx_queue.size() >= 256) kv.second.rx_queue.pop_front();
                        kv.second.rx_queue.push_back({src_ip_nbo, src_port_nbo, std::vector<uint8_t>(payload, payload + plen)});
                    }
                }
                g_net_cv.notify_all();
                g_last_wsa_err = 0;
                handled = true;
            } else if (g_lobby_mode == NetLobbyMode::HostRoom) {
                send_pkt = build_msg_data(src_ip_nbo, src_port_nbo, dst_ip_nbo, dst_port_nbo, payload, plen);
                for (const auto& rp : g_host_peers) {
                    if (is_bcast || make_virtual_ip_nbo(rp.id) == dst_ip_nbo) {
                        fanout_eps.push_back(rp.ep);
                    }
                }
                g_last_wsa_err = 0;
                handled = true;
            } else if (g_lobby_mode == NetLobbyMode::JoinRoom) {
                send_pkt = build_msg_data(src_ip_nbo, src_port_nbo, dst_ip_nbo, dst_port_nbo, payload, plen);
                fanout_eps.push_back(g_join_host_ep);
                g_last_wsa_err = 0;
                handled = true;
            } else {
                if (g_have_lan_mcast) {
                    send_pkt = build_msg_data(src_ip_nbo, src_port_nbo, dst_ip_nbo, dst_port_nbo, payload, plen);
                    fanout_eps.push_back(g_lan_mcast_ep);
                }
                g_last_wsa_err = 0;
                handled = true;
            }
        } else {
            auto rit = g_raw_socks.find(g_fd);
            if (rit != g_raw_socks.end()) raw = rit->second;
        }
    }

    if (is_virtual && handled) {
        if (!send_pkt.empty()) {
            for (const auto& ep : fanout_eps) tunnel_raw_send(send_pkt.data(), send_pkt.size(), ep);
        }
        hle_ret_u32(ctx, handled_ret, 24);
        return;
    }

    if (raw == kInvalidSock) {
        g_last_wsa_err = kWsaNotSock;
        hle_ret_u32(ctx, 0xFFFFFFFFu, 24);
        return;
    }
    sockaddr_in sa{};
    if (to_va && tolen >= 8) {
        sa.sin_family = AF_INET;
        sa.sin_port = rd16(to_va + 2);
        sa.sin_addr.s_addr = rd32(to_va + 4);
    }
    int r = ::sendto(raw, reinterpret_cast<const char*>(mem_ptr(buf_va)), len, flags,
                     (to_va && tolen >= 8) ? reinterpret_cast<const sockaddr*>(&sa) : nullptr,
                     (to_va && tolen >= 8) ? sizeof(sa) : 0);
    g_last_wsa_err = (r < 0) ? static_cast<uint32_t>(sock_last_err()) : 0;
    hle_ret_u32(ctx, static_cast<uint32_t>(r < 0 ? -1 : r), 24);
}

void hle_send(CpuContext& ctx) {
    uint32_t g_fd = hle_arg(ctx, 0);
    uint32_t buf_va = hle_arg(ctx, 1);
    int len = static_cast<int>(hle_arg(ctx, 2));
    int flags = static_cast<int>(hle_arg(ctx, 3));
    NativeSock raw = kInvalidSock;
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        if (g_vsocks.count(g_fd)) {
            g_last_wsa_err = 0;
            hle_ret_u32(ctx, static_cast<uint32_t>(len), 16);
            return;
        }
        auto rit = g_raw_socks.find(g_fd);
        if (rit != g_raw_socks.end()) raw = rit->second;
    }
    if (raw == kInvalidSock) {
        g_last_wsa_err = kWsaNotSock;
        hle_ret_u32(ctx, 0xFFFFFFFFu, 16);
        return;
    }
    int r = ::send(raw, reinterpret_cast<const char*>(mem_ptr(buf_va)), len, flags);
    g_last_wsa_err = (r < 0) ? static_cast<uint32_t>(sock_last_err()) : 0;
    hle_ret_u32(ctx, static_cast<uint32_t>(r < 0 ? -1 : r), 16);
}

void hle_recvfrom(CpuContext& ctx) {
    uint32_t g_fd = hle_arg(ctx, 0);
    uint32_t buf_va = hle_arg(ctx, 1);
    int len = static_cast<int>(hle_arg(ctx, 2));
    int flags = static_cast<int>(hle_arg(ctx, 3));
    uint32_t from_va = hle_arg(ctx, 4);
    uint32_t flen_va = hle_arg(ctx, 5);

    NativeSock raw = kInvalidSock;
    {
        std::unique_lock<std::mutex> lk(g_net_mtx);
        auto vit = g_vsocks.find(g_fd);
        if (vit != g_vsocks.end()) {
            if (vit->second.rx_queue.empty()) {
                if (vit->second.nonblocking) {
                    g_last_wsa_err = kWsaWouldBlock;
                    hle_ret_u32(ctx, 0xFFFFFFFFu, 24);
                    return;
                }
                g_net_cv.wait_for(lk, std::chrono::milliseconds(50), [&]() {
                    auto it2 = g_vsocks.find(g_fd);
                    return it2 == g_vsocks.end() || !it2->second.rx_queue.empty() || g_tunnel_stop.load();
                });
                vit = g_vsocks.find(g_fd);
                if (vit == g_vsocks.end() || vit->second.rx_queue.empty()) {
                    g_last_wsa_err = kWsaWouldBlock;
                    hle_ret_u32(ctx, 0xFFFFFFFFu, 24);
                    return;
                }
            }
            QueuedDatagram d = std::move(vit->second.rx_queue.front());
            vit->second.rx_queue.pop_front();
            int copy_n = std::min<int>(len, static_cast<int>(d.payload.size()));
            if (buf_va && copy_n > 0) std::memcpy(mem_ptr(buf_va), d.payload.data(), static_cast<size_t>(copy_n));
            if (from_va) {
                std::memset(mem_ptr(from_va), 0, 16);
                wr16(from_va + 0, 2); // AF_INET
                wr16(from_va + 2, d.src_port_nbo);
                wr32(from_va + 4, d.src_ip_nbo);
            }
            if (flen_va) wr32(flen_va, 16u);
            g_last_wsa_err = 0;
            hle_ret_u32(ctx, static_cast<uint32_t>(copy_n), 24);
            return;
        }
        auto rit = g_raw_socks.find(g_fd);
        if (rit != g_raw_socks.end()) raw = rit->second;
    }

    if (raw == kInvalidSock) {
        g_last_wsa_err = kWsaNotSock;
        hle_ret_u32(ctx, 0xFFFFFFFFu, 24);
        return;
    }
    sockaddr_in sa{};
    socklen_t slen = sizeof(sa);
    int r = ::recvfrom(raw, reinterpret_cast<char*>(mem_ptr(buf_va)), len, flags,
                       from_va ? reinterpret_cast<sockaddr*>(&sa) : nullptr, from_va ? &slen : nullptr);
    if (r >= 0 && from_va) {
        std::memset(mem_ptr(from_va), 0, 16);
        wr16(from_va + 0, 2);
        wr16(from_va + 2, sa.sin_port);
        wr32(from_va + 4, sa.sin_addr.s_addr);
    }
    if (flen_va) wr32(flen_va, 16u);
    g_last_wsa_err = (r < 0) ? static_cast<uint32_t>(sock_last_err()) : 0;
    hle_ret_u32(ctx, static_cast<uint32_t>(r < 0 ? -1 : r), 24);
}

void hle_recv(CpuContext& ctx) {
    uint32_t g_fd = hle_arg(ctx, 0);
    uint32_t buf_va = hle_arg(ctx, 1);
    int len = static_cast<int>(hle_arg(ctx, 2));
    int flags = static_cast<int>(hle_arg(ctx, 3));
    NativeSock raw = kInvalidSock;
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        auto vit = g_vsocks.find(g_fd);
        if (vit != g_vsocks.end()) {
            if (vit->second.rx_queue.empty()) {
                g_last_wsa_err = kWsaWouldBlock;
                hle_ret_u32(ctx, 0xFFFFFFFFu, 16);
                return;
            }
            QueuedDatagram d = std::move(vit->second.rx_queue.front());
            vit->second.rx_queue.pop_front();
            int copy_n = std::min<int>(len, static_cast<int>(d.payload.size()));
            if (buf_va && copy_n > 0) std::memcpy(mem_ptr(buf_va), d.payload.data(), static_cast<size_t>(copy_n));
            g_last_wsa_err = 0;
            hle_ret_u32(ctx, static_cast<uint32_t>(copy_n), 16);
            return;
        }
        auto rit = g_raw_socks.find(g_fd);
        if (rit != g_raw_socks.end()) raw = rit->second;
    }
    if (raw == kInvalidSock) {
        g_last_wsa_err = kWsaNotSock;
        hle_ret_u32(ctx, 0xFFFFFFFFu, 16);
        return;
    }
    int r = ::recv(raw, reinterpret_cast<char*>(mem_ptr(buf_va)), len, flags);
    g_last_wsa_err = (r < 0) ? static_cast<uint32_t>(sock_last_err()) : 0;
    hle_ret_u32(ctx, static_cast<uint32_t>(r < 0 ? -1 : r), 16);
}

void hle_WSAFDIsSet(CpuContext& ctx) {
    uint32_t g_fd = hle_arg(ctx, 0);
    uint32_t set_va = hle_arg(ctx, 1);
    uint32_t hit = 0;
    if (set_va) {
        uint32_t n = std::min<uint32_t>(rd32(set_va), 64u);
        for (uint32_t i = 0; i < n; ++i) {
            if (rd32(set_va + 4 + i * 4) == g_fd) { hit = 1; break; }
        }
    }
    hle_ret_u32(ctx, hit, 8);
}

template <uint32_t RetVal, uint32_t PopBytes>
void hle_stub_ret(CpuContext& ctx) {
    hle_ret_u32(ctx, RetVal, PopBytes);
}

} // namespace

// =========================================================================
// Public Lobby & Room Code API (for Launcher / Overlay / CLI)
// =========================================================================

bool net_encode_room_code(const std::string& ip_str, uint16_t port, std::string& out_code) {
    in_addr ia{};
    if (::inet_pton(AF_INET, ip_str.c_str(), &ia) != 1) return false;
    const uint8_t* ipb = reinterpret_cast<const uint8_t*>(&ia.s_addr);
    uint8_t bytes[7]{
        ipb[0], ipb[1], ipb[2], ipb[3],
        static_cast<uint8_t>(port >> 8),
        static_cast<uint8_t>(port & 0xFFu),
        0
    };
    bytes[6] = crc8_room(bytes, 6);
    uint64_t val = (static_cast<uint64_t>(bytes[0]) << 48) |
                   (static_cast<uint64_t>(bytes[1]) << 40) |
                   (static_cast<uint64_t>(bytes[2]) << 32) |
                   (static_cast<uint64_t>(bytes[3]) << 24) |
                   (static_cast<uint64_t>(bytes[4]) << 16) |
                   (static_cast<uint64_t>(bytes[5]) << 8)  |
                   static_cast<uint64_t>(bytes[6]);
    std::string raw(12, '0');
    for (int i = 0; i < 12; ++i) {
        raw[i] = kBase32[(val >> (55 - i * 5)) & 31u];
    }
    out_code = "WHK-" + raw.substr(0, 4) + "-" + raw.substr(4, 4) + "-" + raw.substr(8, 4);
    return true;
}

bool net_decode_room_code(const std::string& code_or_addr, std::string& out_ip, uint16_t& out_port) {
    ensure_wsa();
    std::string s = code_or_addr;
    // Trim whitespace
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s.empty()) return false;

    // Check WHK-XXXX-XXXX-XXXX form (or 12-char Base32 code)
    std::string upper;
    for (char c : s) {
        if (c != '-' && c != ' ') upper.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? (c - 'a' + 'A') : c));
    }
    if (upper.rfind("WHK", 0) == 0 && upper.size() == 15) upper = upper.substr(3);
    if (upper.size() == 12) {
        bool valid_b32 = true;
        uint64_t val = 0;
        for (char c : upper) {
            int idx = b32_index(c);
            if (idx < 0) { valid_b32 = false; break; }
            val = (val << 5) | static_cast<uint64_t>(idx);
        }
        if (valid_b32 && (val >> 56) == 0) {
            uint8_t bytes[7]{
                static_cast<uint8_t>((val >> 48) & 0xFFu),
                static_cast<uint8_t>((val >> 40) & 0xFFu),
                static_cast<uint8_t>((val >> 32) & 0xFFu),
                static_cast<uint8_t>((val >> 24) & 0xFFu),
                static_cast<uint8_t>((val >> 16) & 0xFFu),
                static_cast<uint8_t>((val >> 8) & 0xFFu),
                static_cast<uint8_t>(val & 0xFFu)
            };
            if (crc8_room(bytes, 6) == bytes[6]) {
                char ipbuf[32]{};
                std::snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
                out_ip = ipbuf;
                out_port = static_cast<uint16_t>((bytes[4] << 8) | bytes[5]);
                return out_port != 0;
            }
        }
    }

    // Parse host[:port]
    std::string host = s;
    uint16_t port = 30740;
    size_t colon = s.rfind(':');
    if (colon != std::string::npos) {
        host = s.substr(0, colon);
        int p = std::atoi(s.substr(colon + 1).c_str());
        if (p <= 0 || p > 65535) return false;
        port = static_cast<uint16_t>(p);
    }
    in_addr ia{};
    if (::inet_pton(AF_INET, host.c_str(), &ia) == 1) {
        out_ip = host;
        out_port = port;
        return true;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
        auto* sa = reinterpret_cast<sockaddr_in*>(res->ai_addr);
        out_ip = ip_nbo_to_str(sa->sin_addr.s_addr);
        out_port = port;
        ::freeaddrinfo(res);
        return true;
    }
    return false;
}

bool net_start_offline_lan(const std::string& player_name) {
    stop_worker_threads();
    ensure_wsa();

    std::string lan = query_local_lan_ip();
    NativeSock s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    bool have_mc = false;
    if (s != kInvalidSock) {
        int yes = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifdef SO_REUSEPORT
        ::setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif
        sockaddr_in bind_sa{};
        bind_sa.sin_family = AF_INET;
        bind_sa.sin_port = htons(30741u);
        bind_sa.sin_addr.s_addr = INADDR_ANY;
        if (::bind(s, reinterpret_cast<const sockaddr*>(&bind_sa), sizeof(bind_sa)) == 0) {
            ip_mreq mreq{};
            ::inet_pton(AF_INET, "239.69.69.1", &mreq.imr_multiaddr);
            mreq.imr_interface.s_addr = INADDR_ANY;
            ::setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));
            unsigned char loop = 1;
            ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));
            set_nonblocking(s, true);
            have_mc = true;
        } else {
            close_native_sock(s);
            s = kInvalidSock;
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        g_lobby_mode = NetLobbyMode::OfflineOrLan;
        g_self_id = random_lan_id();
        g_player_name = player_name.empty() ? "Player1" : player_name;
        g_lan_ip = lan;
        g_public_ip.clear();
        g_upnp_ok = false;
        g_tunnel_connected = false;
        g_host_peers.clear();
        g_room_code.clear();
        g_host_endpoint_str.clear();
        g_tunnel_sock = s;
        g_have_lan_mcast = have_mc;
        if (have_mc) {
            std::memset(&g_lan_mcast_ep, 0, sizeof(g_lan_mcast_ep));
            g_lan_mcast_ep.sin_family = AF_INET;
            g_lan_mcast_ep.sin_port = htons(30741u);
            ::inet_pton(AF_INET, "239.69.69.1", &g_lan_mcast_ep.sin_addr);
        }
        g_status_text = have_mc ? "LAN System Link Ready (Multicast)" : "Localhost Loopback Ready";
        rebuild_roster_locked();
    }

    g_tunnel_stop.store(false);
    g_tunnel_running.store(true);
    if (have_mc) g_net_thread = std::thread(tunnel_worker_loop);
    return true;
}

bool net_start_host(uint16_t tunnel_port, bool try_upnp, const std::string& player_name) {
    stop_worker_threads();
    ensure_wsa();
    if (!tunnel_port) tunnel_port = 30740;

    NativeSock s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSock) return false;

    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in bind_sa{};
    bind_sa.sin_family = AF_INET;
    bind_sa.sin_port = htons(tunnel_port);
    bind_sa.sin_addr.s_addr = INADDR_ANY;
    if (::bind(s, reinterpret_cast<const sockaddr*>(&bind_sa), sizeof(bind_sa)) != 0) {
        close_native_sock(s);
        std::lock_guard<std::mutex> lk(g_net_mtx);
        g_status_text = "Failed to bind UDP port " + std::to_string(tunnel_port);
        return false;
    }
    set_nonblocking(s, true);

    std::string lan = query_local_lan_ip();
    std::string initial_code;
    net_encode_room_code(lan, tunnel_port, initial_code);

    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        g_lobby_mode = NetLobbyMode::HostRoom;
        g_self_id = 1;
        g_host_tunnel_port = tunnel_port;
        g_player_name = player_name.empty() ? "Host" : player_name;
        g_lan_ip = lan;
        g_public_ip.clear();
        g_upnp_ok = false;
        g_tunnel_connected = true;
        g_host_peers.clear();
        g_room_code = initial_code;
        g_host_endpoint_str = lan + ":" + std::to_string(tunnel_port);
        g_tunnel_sock = s;
        g_have_lan_mcast = false;
        g_status_text = "Hosting Room (1 player)";
        rebuild_roster_locked();
    }

    g_tunnel_stop.store(false);
    g_tunnel_running.store(true);
    g_net_thread = std::thread(tunnel_worker_loop);
    if (try_upnp) {
        g_probe_thread = std::thread(try_upnp_mapping_thread, tunnel_port, lan);
    }
    return true;
}

bool net_join_room(const std::string& room_code_or_addr, const std::string& player_name) {
    stop_worker_threads();
    ensure_wsa();

    std::string host_ip;
    uint16_t host_port = 0;
    if (!net_decode_room_code(room_code_or_addr, host_ip, host_port)) {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        g_lobby_mode = NetLobbyMode::OfflineOrLan;
        g_status_text = "Invalid room code or IP:Port";
        return false;
    }

    NativeSock s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSock) return false;
    sockaddr_in bind_sa{};
    bind_sa.sin_family = AF_INET;
    bind_sa.sin_port = 0;
    bind_sa.sin_addr.s_addr = INADDR_ANY;
    if (::bind(s, reinterpret_cast<const sockaddr*>(&bind_sa), sizeof(bind_sa)) != 0) {
        close_native_sock(s);
        return false;
    }
    set_nonblocking(s, true);

    std::string canonical_code;
    net_encode_room_code(host_ip, host_port, canonical_code);
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        g_lobby_mode = NetLobbyMode::JoinRoom;
        g_self_id = 2; // Provisional until MSG_WELCOME
        g_player_name = player_name.empty() ? "Player" : player_name;
        g_lan_ip = query_local_lan_ip();
        g_public_ip.clear();
        g_upnp_ok = false;
        g_tunnel_connected = false;
        g_host_peers.clear();
        g_room_code = canonical_code;
        g_host_endpoint_str = host_ip + ":" + std::to_string(host_port);
        g_tunnel_sock = s;
        g_have_lan_mcast = false;
        std::memset(&g_join_host_ep, 0, sizeof(g_join_host_ep));
        g_join_host_ep.sin_family = AF_INET;
        g_join_host_ep.sin_port = htons(host_port);
        ::inet_pton(AF_INET, host_ip.c_str(), &g_join_host_ep.sin_addr);
        g_status_text = "Connecting to " + g_host_endpoint_str + "...";
        rebuild_roster_locked();
    }

    g_tunnel_stop.store(false);
    g_tunnel_running.store(true);
    g_net_thread = std::thread(tunnel_worker_loop);
    return true;
}

void net_stop_tunnel() {
    net_start_offline_lan(g_player_name);
}

void net_shutdown() {
    stop_worker_threads();
    std::lock_guard<std::mutex> lk(g_net_mtx);
    g_vsocks.clear();
    for (auto& kv : g_raw_socks) close_native_sock(kv.second);
    g_raw_socks.clear();
    g_cached_roster.clear();
}

NetLobbyStatus net_get_status() {
    std::lock_guard<std::mutex> lk(g_net_mtx);
    NetLobbyStatus st{};
    st.mode = g_lobby_mode;
    st.active = g_tunnel_running.load();
    st.upnp_mapped = g_upnp_ok;
    st.local_lan_ip = g_lan_ip;
    st.public_ip = g_public_ip;
    st.room_code = g_room_code;
    st.host_endpoint = g_host_endpoint_str;
    st.self_virtual_ip = ip_nbo_to_str(make_virtual_ip_nbo(g_self_id));
    st.status_text = g_status_text;
    st.tx_packets = g_tx_pkts.load();
    st.rx_packets = g_rx_pkts.load();
    return st;
}

std::vector<NetPeerInfo> net_list_peers() {
    std::lock_guard<std::mutex> lk(g_net_mtx);
    return g_cached_roster;
}

void hle_register_net() {
    // Verified XNET & Winsock2 targets with exact stack pops
    recomp_register_fn(0x0022160Eu, hle_XNetRandom);
    recomp_register_fn(0x00221619u, hle_XNetCreateKey);
    recomp_register_fn(0x00221624u, hle_stub_ret<0, 8>);            // XNetRegisterKey -> 0
    recomp_register_fn(0x0022162Fu, hle_stub_ret<0, 4>);            // XNetUnregisterKey -> 0
    recomp_register_fn(0x0022163Au, hle_XNetXnAddrToInAddr);
    recomp_register_fn(0x0022165Cu, hle_XNetInAddrToXnAddr);
    recomp_register_fn(0x0022167Eu, hle_stub_ret<0, 20>);
    recomp_register_fn(0x0022168Du, hle_stub_ret<0, 48>);
    recomp_register_fn(0x002216A9u, hle_stub_ret<0, 4>);            // XNetUnregisterInAddr -> 0
    recomp_register_fn(0x002216B4u, hle_XNetGetTitleXnAddr);
    recomp_register_fn(0x002216BFu, hle_XNetGetTitleXnAddr);        // XNetGetDebugXnAddr
    recomp_register_fn(0x002216CAu, hle_socket);
    recomp_register_fn(0x002216D5u, hle_closesocket);
    recomp_register_fn(0x002216F6u, hle_setsockopt);
    recomp_register_fn(0x00221705u, hle_getsockopt);
    recomp_register_fn(0x00221714u, hle_ioctlsocket);
    recomp_register_fn(0x0022172Au, hle_bind);
    recomp_register_fn(0x00221735u, hle_connect);
    recomp_register_fn(0x00221756u, hle_select);
    recomp_register_fn(0x00221799u, hle_sendto);
    recomp_register_fn(0x002217B7u, hle_send);
    recomp_register_fn(0x002217D1u, hle_recvfrom);
    recomp_register_fn(0x002217E0u, hle_recv);
    recomp_register_fn(0x00221951u, hle_stub_ret<0xFFFFFFFFu, 12>);
    recomp_register_fn(0x00221D13u, hle_XNetStartup);
    recomp_register_fn(0x00221D2Au, hle_WSAStartup);
    recomp_register_fn(0x00221E25u, hle_WSACleanup);
    recomp_register_fn(0x00221F05u, hle_WSAGetLastError);
    recomp_register_fn(0x00221F34u, hle_WSAFDIsSet);
    recomp_register_fn(0x00223014u, hle_htonl_xb);
    recomp_register_fn(0x0022303Eu, hle_htons_xb);
    recomp_register_fn(0x0022307Cu, hle_htons_xb);

    // 37 XONLINE targets (clean offline S_OK / E_FAIL with exact stack pops)
    constexpr uint32_t E_XONLINE_OFFLINE = 0x80150001u;
    recomp_register_fn(0x001F8F00u, hle_stub_ret<0x00150001u, 4>);  // XOnlineTaskContinue -> completed
    recomp_register_fn(0x001F8F0Bu, hle_stub_ret<0, 4>);            // XOnlineTaskClose -> S_OK
    recomp_register_fn(0x001F8F16u, hle_stub_ret<0, 8>);
    recomp_register_fn(0x001F8F21u, hle_stub_ret<E_XONLINE_OFFLINE, 20>);
    recomp_register_fn(0x001F8F30u, hle_stub_ret<0, 4>);
    recomp_register_fn(0x001F8F51u, hle_stub_ret<E_XONLINE_OFFLINE, 0>);
    recomp_register_fn(0x001F8F5Cu, hle_stub_ret<E_XONLINE_OFFLINE, 8>);
    recomp_register_fn(0x001F8F88u, hle_stub_ret<0, 4>);            // XOnlineCleanup
    recomp_register_fn(0x001F90EAu, hle_stub_ret<E_XONLINE_OFFLINE, 32>);
    recomp_register_fn(0x001F90F9u, hle_stub_ret<E_XONLINE_OFFLINE, 40>);
    recomp_register_fn(0x001F9129u, hle_stub_ret<E_XONLINE_OFFLINE, 12>);
    recomp_register_fn(0x001F9134u, hle_stub_ret<E_XONLINE_OFFLINE, 16>);
    recomp_register_fn(0x001F9152u, hle_stub_ret<E_XONLINE_OFFLINE, 16>);
    recomp_register_fn(0x001F9170u, hle_stub_ret<E_XONLINE_OFFLINE, 28>);
    recomp_register_fn(0x001F917Fu, hle_stub_ret<0, 12>);
    recomp_register_fn(0x001F91A5u, hle_stub_ret<0, 16>);
    recomp_register_fn(0x001F91B0u, hle_stub_ret<0, 12>);
    recomp_register_fn(0x001F91FDu, hle_stub_ret<E_XONLINE_OFFLINE, 24>);
    recomp_register_fn(0x001F9230u, hle_stub_ret<E_XONLINE_OFFLINE, 8>);
    recomp_register_fn(0x001F923Bu, hle_stub_ret<E_XONLINE_OFFLINE, 8>);
    recomp_register_fn(0x001F9246u, hle_stub_ret<E_XONLINE_OFFLINE, 12>);
    recomp_register_fn(0x001F9251u, hle_stub_ret<E_XONLINE_OFFLINE, 4>);
    recomp_register_fn(0x001F925Cu, hle_stub_ret<E_XONLINE_OFFLINE, 12>);
    recomp_register_fn(0x001F92A9u, hle_stub_ret<E_XONLINE_OFFLINE, 16>);
    recomp_register_fn(0x001F92B4u, hle_stub_ret<E_XONLINE_OFFLINE, 8>);
    recomp_register_fn(0x001F92BFu, hle_stub_ret<E_XONLINE_OFFLINE, 16>);
    recomp_register_fn(0x001F92ECu, hle_stub_ret<E_XONLINE_OFFLINE, 20>);
    recomp_register_fn(0x001F930Du, hle_stub_ret<E_XONLINE_OFFLINE, 20>);
    recomp_register_fn(0x001F932Eu, hle_stub_ret<E_XONLINE_OFFLINE, 12>);
    recomp_register_fn(0x001F9339u, hle_stub_ret<E_XONLINE_OFFLINE, 12>);
    recomp_register_fn(0x001F934Fu, hle_stub_ret<E_XONLINE_OFFLINE, 4>);
    recomp_register_fn(0x001F935Au, hle_stub_ret<E_XONLINE_OFFLINE, 24>);
    recomp_register_fn(0x001F9369u, hle_stub_ret<E_XONLINE_OFFLINE, 8>);
    recomp_register_fn(0x001F9374u, hle_stub_ret<E_XONLINE_OFFLINE, 16>);
    recomp_register_fn(0x001F9396u, hle_stub_ret<E_XONLINE_OFFLINE, 16>);
    recomp_register_fn(0x001F93B8u, hle_stub_ret<E_XONLINE_OFFLINE, 32>);
    recomp_register_fn(0x001F9F72u, hle_stub_ret<0, 4>);
    recomp_register_fn(0x001FABF1u, hle_stub_ret<0, 0>); // XOnline static/launch init
    recomp_register_fn(0x001FABDFu, hle_stub_ret<0, 8>); // XOnline XLaunchNewImage / title info helper
}
