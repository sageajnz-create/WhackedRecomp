#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class NetLobbyMode : int {
    OfflineOrLan = 0, // Local LAN broadcast/multicast (zero setup) + loopback
    HostRoom     = 1, // Host a Virtual System Link room over UDP (Windows & Linux peers)
    JoinRoom     = 2  // Join a friend's room by WHK-... room code or IP:Port
};

struct NetPeerInfo {
    uint32_t virtual_ip_nbo = 0;
    std::string virtual_ip_str;
    std::string player_name;
    int ping_ms = 0;
    bool is_self = false;
    bool is_host = false;
};

struct NetLobbyStatus {
    NetLobbyMode mode = NetLobbyMode::OfflineOrLan;
    bool active = false;
    bool upnp_mapped = false;
    std::string local_lan_ip;
    std::string public_ip;
    std::string room_code;
    std::string host_endpoint;
    std::string self_virtual_ip;
    std::string status_text;
    uint64_t tx_packets = 0;
    uint64_t rx_packets = 0;
};

bool net_start_offline_lan(const std::string& player_name);
bool net_start_host(uint16_t tunnel_port, bool try_upnp, const std::string& player_name);
bool net_join_room(const std::string& room_code_or_addr, const std::string& player_name);
void net_stop_tunnel();
void net_shutdown();

NetLobbyStatus net_get_status();
std::vector<NetPeerInfo> net_list_peers();

bool net_encode_room_code(const std::string& ip_str, uint16_t port, std::string& out_code);
bool net_decode_room_code(const std::string& code_or_addr, std::string& out_ip, uint16_t& out_port);
