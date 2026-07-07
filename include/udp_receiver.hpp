#pragma once

#include <string>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace itch50 {
namespace net {

// ============================================================================
// Zero-Copy UDP Multicast Receiver
// ============================================================================
class UdpMulticastReceiver {
#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif

public:
    UdpMulticastReceiver(const std::string& multicast_ip, uint16_t port, const std::string& interface_ip = "0.0.0.0") {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#endif

        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
        if (sock_ == INVALID_SOCKET) {
#else
        if (sock_ < 0) {
#endif
            throw std::runtime_error("Failed to create socket");
        }

        // Allow multiple listeners (SO_REUSEADDR)
        int reuse = 1;
        if (setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse)) < 0) {
            std::cerr << "[Warning] Failed to set SO_REUSEADDR\n";
        }

        // Bind to port
        sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(port);
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(sock_, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
            throw std::runtime_error("Failed to bind socket");
        }

        // Join multicast group
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(multicast_ip.c_str());
        mreq.imr_interface.s_addr = inet_addr(interface_ip.c_str());

        if (setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq)) < 0) {
            throw std::runtime_error("Failed to join multicast group (IP_ADD_MEMBERSHIP)");
        }

        // Increase receive buffer for HFT (e.g., 32MB) to prevent drops on bursts
        int rcvbuf = 32 * 1024 * 1024;
        if (setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf)) < 0) {
            std::cerr << "[Warning] Failed to set 32MB SO_RCVBUF. Packet drops may occur on bursts.\n";
        }
    }

    ~UdpMulticastReceiver() {
#ifdef _WIN32
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            WSACleanup();
        }
#else
        if (sock_ >= 0) {
            close(sock_);
        }
#endif
    }

    // Blocking receive.
    // In a kernel-bypass architecture (io_uring, AF_XDP, EF_VI, Solarflare),
    // this would poll a ring buffer instead of entering the kernel.
    inline int receive(char* buffer, std::size_t max_length) noexcept {
        return recv(sock_, buffer, static_cast<int>(max_length), 0);
    }
};

} // namespace net
} // namespace itch50
