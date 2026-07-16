#include <iostream>
#include <thread>
#include <csignal>
#include <cerrno>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <array>
#include <vector>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sodium.h>

#include <lnos/crypto.h>
#include "registry.h"
#include <lnos/protocol.h>
#include <lnos/config.h>

#define MCAST_GROUP "239.255.42.99"
#define MCAST_GROUP_V6 "ff02::4299"
#define PORT 4545

constexpr std::size_t RECV_BUFFER_SIZE = 1024;
constexpr int ANNOUNCE_INTERVAL_SECONDS = 2;
constexpr int NODE_TTL_SECONDS = 15;

std::atomic<bool> running = true;

struct InterfaceInfo {
    std::string name;
    unsigned int index = 0;
    std::string ipv4;
    std::string ipv6;
    bool has_ipv4 = false;
    bool has_ipv6 = false;
};

InterfaceInfo detectInterface() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return {};
    }

    auto extractAddresses = [&](const std::string& name, InterfaceInfo& info) {
        for (struct ifaddrs* ifa2 = ifaddr; ifa2 != nullptr; ifa2 = ifa2->ifa_next) {
            if (!ifa2->ifa_addr || std::string(ifa2->ifa_name) != name) continue;
            if (ifa2->ifa_addr->sa_family == AF_INET) {
                char host[NI_MAXHOST];
                if (getnameinfo(ifa2->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) == 0) {
                    info.ipv4 = host;
                    info.has_ipv4 = true;
                }
            } else if (ifa2->ifa_addr->sa_family == AF_INET6) {
                char host[NI_MAXHOST];
                if (getnameinfo(ifa2->ifa_addr, sizeof(struct sockaddr_in6), host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) == 0) {
                    std::string s(host);
                    auto pos = s.find('%');
                    if (pos != std::string::npos) {
                        s = s.substr(0, pos);
                    }
                    info.ipv6 = s;
                    info.has_ipv6 = true;
                }
            }
        }
    };

    // Pass 1: find non-loopback active interfaces
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_RUNNING)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        InterfaceInfo info;
        info.name = ifa->ifa_name;
        info.index = if_nametoindex(ifa->ifa_name);

        extractAddresses(info.name, info);

        if (info.has_ipv4 || info.has_ipv6) {
            freeifaddrs(ifaddr);
            return info;
        }
    }

    // Pass 2: fallback to loopback
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_RUNNING)) continue;
        if (!(ifa->ifa_flags & IFF_LOOPBACK)) continue;

        InterfaceInfo info;
        info.name = ifa->ifa_name;
        info.index = if_nametoindex(ifa->ifa_name);

        extractAddresses(info.name, info);

        if (info.has_ipv4 || info.has_ipv6) {
            freeifaddrs(ifaddr);
            return info;
        }
    }

    freeifaddrs(ifaddr);
    return {};
}

void handleSigint(int) {
    std::cout << "CTRL+C received\n";
    running = false;
}

std::shared_mutex nodesMutex;
std::mutex coutMutex;

lnos::Config cfg;

void stopWithError(const std::string& message) {
    if (!running.exchange(false))
        return;

    std::cerr << "[fatal] " << message << "\n"
              << "LNOS will now shut down.\n";
}

void stopAfterSystemError(const char* operation) {
    perror(operation);
    stopWithError(std::string("Network operation failed: ") + operation);
}

void sendMulticastQuery(const std::string& target_name) {
    static bool keys_loaded = false;
    static std::array<uint8_t, PRIVATE_KEY_SIZE> privateKey;
    static std::array<uint8_t, PUBLIC_KEY_SIZE> publicKey;

    if (!keys_loaded) {
        try {
            privateKey = lnos::loadPrivateKey();
            publicKey = lnos::loadPublicKey();
            keys_loaded = true;
        } catch (...) {
            return;
        }
    }

    lnos::Packet p(target_name, {});
    p.type = lnos::PacketType::Query;
    p.publicKey = publicKey;

    if (!lnos::signPacket(p, privateKey)) {
        return;
    }

    lnos::Blob msg = lnos::encode(p, true);

    // Send IPv4
    int sock4 = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock4 >= 0) {
        unsigned char ttl = 1;
        setsockopt(sock4, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        sockaddr_in addr4{};
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(PORT);
        if (inet_pton(AF_INET, MCAST_GROUP, &addr4.sin_addr) == 1) {
            sendto(sock4, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&addr4), sizeof(addr4));
        }
        close(sock4);
    }

    // Send IPv6
    int sock6 = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock6 >= 0) {
        int hops = 1;
        setsockopt(sock6, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));
        sockaddr_in6 addr6{};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(PORT);
        if (inet_pton(AF_INET6, MCAST_GROUP_V6, &addr6.sin6_addr) == 1) {
            sendto(sock6, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&addr6), sizeof(addr6));
        }
        close(sock6);
    }
}

void handle_client(int client) {
    char buf[256];
    ssize_t n = read(client, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string qname(buf);
        while (!qname.empty() && (qname.back() == '\n' || qname.back() == '\r')) {
            qname.pop_back();
        }

        std::string resolved_ip = "NOT_FOUND";

        {
            std::shared_lock<std::shared_mutex> lock(nodesMutex);
            auto it = nodes.find(qname);
            if (it != nodes.end() && it->second.status == NodeStatus::Online) {
                resolved_ip = it->second.ip;
            }
        }

        if (resolved_ip == "NOT_FOUND") {
            sendMulticastQuery(qname);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            {
                std::shared_lock<std::shared_mutex> lock(nodesMutex);
                auto it = nodes.find(qname);
                if (it != nodes.end() && it->second.status == NodeStatus::Online) {
                    resolved_ip = it->second.ip;
                }
            }
        }

        std::string resp = resolved_ip + "\n";
        write(client, resp.data(), resp.length());
    }
    close(client);
}

void query_server() {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        stopWithError("query_server: socket failed");
        return;
    }

    struct sockaddr_un un{};
    un.sun_family = AF_UNIX;
    std::string socket_path = lnos::getConfigDir() + "/lnosd.sock";
    std::strncpy(un.sun_path, socket_path.c_str(), sizeof(un.sun_path) - 1);
    unlink(un.sun_path);

    if (bind(sock, reinterpret_cast<sockaddr*>(&un), sizeof(un)) < 0) {
        stopWithError("query_server: bind failed to " + socket_path);
        close(sock);
        return;
    }

    if (listen(sock, 128) < 0) {
        stopWithError("query_server: listen failed");
        unlink(un.sun_path);
        close(sock);
        return;
    }

    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (running) {
        int client = accept(sock, nullptr, nullptr);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            break;
        }

        std::thread t(handle_client, client);
        t.detach();
    }

    unlink(un.sun_path);
    close(sock);
}


void sender_ipv4(std::string myIp) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        stopAfterSystemError("sender_ipv4 socket");
        return;
    }

    unsigned char ttl = 1;
    if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        stopAfterSystemError("sender_ipv4 IP_MULTICAST_TTL");
        close(sock);
        return;
    }

    int loop = 1;
    if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        stopAfterSystemError("sender_ipv4 IP_MULTICAST_LOOP");
        close(sock);
        return;
    }

    in_addr localInterface{};
    if (inet_pton(AF_INET, myIp.c_str(), &localInterface) != 1) {
        stopWithError("sender_ipv4: Invalid local IPv4 address '" + myIp + "'");
        close(sock);
        return;
    }

    if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &localInterface, sizeof(localInterface)) < 0) {
        stopAfterSystemError("sender_ipv4 IP_MULTICAST_IF");
        close(sock);
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, MCAST_GROUP, &addr.sin_addr) != 1) {
        stopWithError("sender_ipv4: Invalid multicast address '" MCAST_GROUP "'");
        close(sock);
        return;
    }

    std::array<uint8_t, PRIVATE_KEY_SIZE> privateKey;
    std::array<uint8_t, PUBLIC_KEY_SIZE> publicKey;
    try {
        privateKey = lnos::loadPrivateKey();
        publicKey = lnos::loadPublicKey();
    } catch (const std::exception& e) {
        stopWithError("sender_ipv4: failed to load keys: " + std::string(e.what()));
        close(sock);
        return;
    }

    while (running) {
        lnos::Packet p(cfg.name, cfg.services);
        p.type = lnos::PacketType::Announce;
        p.publicKey = publicKey;

        if (!lnos::signPacket(p, privateKey)) {
            stopWithError("sender_ipv4: Packet signing failed");
            break;
        }

        lnos::Blob msg = lnos::encode(p, true);

        {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cout << "[debug] sender_ipv4: sending " << msg.size() << " bytes\n";
        }

        if (sendto(sock, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            stopAfterSystemError("sender_ipv4 sendto");
            break;
        }

        std::this_thread::sleep_for(std::chrono::seconds(ANNOUNCE_INTERVAL_SECONDS));
    }

    close(sock);
}

void receiver_ipv4(std::string myIp) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        stopAfterSystemError("receiver_ipv4 socket");
        return;
    }

    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        stopAfterSystemError("receiver_ipv4 SO_REUSEADDR");
        close(sock);
        return;
    }

    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        stopAfterSystemError("receiver_ipv4 SO_RCVTIMEO");
        close(sock);
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        stopAfterSystemError("receiver_ipv4 bind");
        close(sock);
        return;
    }

    ip_mreq mreq{};
    if (inet_pton(AF_INET, MCAST_GROUP, &mreq.imr_multiaddr) != 1) {
        stopWithError("receiver_ipv4: Invalid multicast address");
        close(sock);
        return;
    }

    if (inet_pton(AF_INET, myIp.c_str(), &mreq.imr_interface) != 1) {
        stopWithError("receiver_ipv4: Invalid local address");
        close(sock);
        return;
    }

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        stopAfterSystemError("receiver_ipv4 IP_ADD_MEMBERSHIP");
        close(sock);
        return;
    }

    char buffer[RECV_BUFFER_SIZE];

    while (running) {
        sockaddr_in senderAddr{};
        socklen_t senderLen = sizeof(senderAddr);

        ssize_t len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);
        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                stopAfterSystemError("receiver_ipv4 recvfrom");
                break;
            }
            continue;
        }

        if (len == 0) continue;

        buffer[len] = 0;

        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &senderAddr.sin_addr, ip, sizeof(ip)) == nullptr) {
            stopAfterSystemError("receiver_ipv4 inet_ntop");
            break;
        }

        {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cout << "[debug] receiver_ipv4: received " << len << " bytes from " << ip << "\n";
        }

        lnos::EncodedPacket encoded((uint8_t *) buffer, len);
        lnos::Packet p;
        if (lnos::decode(encoded, p)) {
            if (!lnos::verifyPacket(p)) {
                std::cerr << "[error] receiver_ipv4: packet signature verification failed\n";
                continue;
            }

            if (p.type == lnos::PacketType::Query) {
                if (p.announce.name == cfg.name) {
                    try {
                        auto privateKey = lnos::loadPrivateKey();
                        auto publicKey = lnos::loadPublicKey();
                        lnos::Packet resp(cfg.name, cfg.services);
                        resp.type = lnos::PacketType::Response;
                        resp.publicKey = publicKey;
                        if (lnos::signPacket(resp, privateKey)) {
                            lnos::Blob resp_msg = lnos::encode(resp, true);
                            sendto(sock, resp_msg.data(), resp_msg.size(), 0, reinterpret_cast<sockaddr*>(&senderAddr), senderLen);
                        }
                    } catch (...) {}
                }
            } else if (p.type == lnos::PacketType::Announce || p.type == lnos::PacketType::Response) {
                std::unique_lock<std::shared_mutex> lock(nodesMutex);
                const auto& announce = p.announce;
                auto it = nodes.find(announce.name);
                if (it != nodes.end()) {
                    if (it->second.publicKey != p.publicKey) {
                        std::cerr << "[error] receiver_ipv4: public key mismatch for node '" << announce.name << "', rejecting (C-2 prevention)\n";
                        continue;
                    }
                } else if (nodes.size() >= 1000) {
                    std::cerr << "[warning] receiver_ipv4: registry full (1000 limit), ignoring node '" << announce.name << "'\n";
                    continue;
                }
                nodes[announce.name] = {
                    announce.name,
                    ip,
                    announce.services,
                    std::chrono::steady_clock::now(),
                    NodeStatus::Online,
                    p.publicKey
                };
            }
        } else {
            std::cerr << "[error] receiver_ipv4: received invalid packet\n";
        }
    }

    close(sock);
}

void sender_ipv6(unsigned int ifindex) {
    int sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        stopAfterSystemError("sender_ipv6 socket");
        return;
    }

    int hops = 1;
    if (setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) < 0) {
        stopAfterSystemError("sender_ipv6 IPV6_MULTICAST_HOPS");
        close(sock);
        return;
    }

    int loop = 1;
    if (setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        stopAfterSystemError("sender_ipv6 IPV6_MULTICAST_LOOP");
        close(sock);
        return;
    }

    if (setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex)) < 0) {
        stopAfterSystemError("sender_ipv6 IPV6_MULTICAST_IF");
        close(sock);
        return;
    }

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(PORT);
    if (inet_pton(AF_INET6, MCAST_GROUP_V6, &addr.sin6_addr) != 1) {
        stopWithError("sender_ipv6: Invalid multicast address '" MCAST_GROUP_V6 "'");
        close(sock);
        return;
    }

    std::array<uint8_t, PRIVATE_KEY_SIZE> privateKey;
    std::array<uint8_t, PUBLIC_KEY_SIZE> publicKey;
    try {
        privateKey = lnos::loadPrivateKey();
        publicKey = lnos::loadPublicKey();
    } catch (const std::exception& e) {
        stopWithError("sender_ipv6: failed to load keys: " + std::string(e.what()));
        close(sock);
        return;
    }

    while (running) {
        lnos::Packet p(cfg.name, cfg.services);
        p.type = lnos::PacketType::Announce;
        p.publicKey = publicKey;

        if (!lnos::signPacket(p, privateKey)) {
            stopWithError("sender_ipv6: Packet signing failed");
            break;
        }

        lnos::Blob msg = lnos::encode(p, true);

        {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cout << "[debug] sender_ipv6: sending " << msg.size() << " bytes\n";
        }

        if (sendto(sock, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            stopAfterSystemError("sender_ipv6 sendto");
            break;
        }

        std::this_thread::sleep_for(std::chrono::seconds(ANNOUNCE_INTERVAL_SECONDS));
    }

    close(sock);
}

void receiver_ipv6(unsigned int ifindex) {
    int sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        stopAfterSystemError("receiver_ipv6 socket");
        return;
    }

    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        stopAfterSystemError("receiver_ipv6 SO_REUSEADDR");
        close(sock);
        return;
    }

    int v6only = 1;
    if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0) {
        stopAfterSystemError("receiver_ipv6 IPV6_V6ONLY");
        close(sock);
        return;
    }

    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        stopAfterSystemError("receiver_ipv6 SO_RCVTIMEO");
        close(sock);
        return;
    }

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(PORT);
    addr.sin6_addr = in6addr_any;

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        stopAfterSystemError("receiver_ipv6 bind");
        close(sock);
        return;
    }

    ipv6_mreq mreq{};
    if (inet_pton(AF_INET6, MCAST_GROUP_V6, &mreq.ipv6mr_multiaddr) != 1) {
        stopWithError("receiver_ipv6: Invalid multicast address");
        close(sock);
        return;
    }
    mreq.ipv6mr_interface = ifindex;

    if (setsockopt(sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        stopAfterSystemError("receiver_ipv6 IPV6_ADD_MEMBERSHIP");
        close(sock);
        return;
    }

    char buffer[RECV_BUFFER_SIZE];

    while (running) {
        sockaddr_in6 senderAddr{};
        socklen_t senderLen = sizeof(senderAddr);

        ssize_t len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);
        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                stopAfterSystemError("receiver_ipv6 recvfrom");
                break;
            }
            continue;
        }

        if (len == 0) continue;

        buffer[len] = 0;

        char ip[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &senderAddr.sin6_addr, ip, sizeof(ip)) == nullptr) {
            stopAfterSystemError("receiver_ipv6 inet_ntop");
            break;
        }

        {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cout << "[debug] receiver_ipv6: received " << len << " bytes from " << ip << "\n";
        }

        lnos::EncodedPacket encoded((uint8_t *) buffer, len);
        lnos::Packet p;
        if (lnos::decode(encoded, p)) {
            if (!lnos::verifyPacket(p)) {
                std::cerr << "[error] receiver_ipv6: packet signature verification failed\n";
                continue;
            }

            if (p.type == lnos::PacketType::Query) {
                if (p.announce.name == cfg.name) {
                    try {
                        auto privateKey = lnos::loadPrivateKey();
                        auto publicKey = lnos::loadPublicKey();
                        lnos::Packet resp(cfg.name, cfg.services);
                        resp.type = lnos::PacketType::Response;
                        resp.publicKey = publicKey;
                        if (lnos::signPacket(resp, privateKey)) {
                            lnos::Blob resp_msg = lnos::encode(resp, true);
                            sendto(sock, resp_msg.data(), resp_msg.size(), 0, reinterpret_cast<sockaddr*>(&senderAddr), senderLen);
                        }
                    } catch (...) {}
                }
            } else if (p.type == lnos::PacketType::Announce || p.type == lnos::PacketType::Response) {
                std::unique_lock<std::shared_mutex> lock(nodesMutex);
                const auto& announce = p.announce;
                auto it = nodes.find(announce.name);
                if (it != nodes.end()) {
                    if (it->second.publicKey != p.publicKey) {
                        std::cerr << "[error] receiver_ipv6: public key mismatch for node '" << announce.name << "', rejecting (C-2 prevention)\n";
                        continue;
                    }
                } else if (nodes.size() >= 1000) {
                    std::cerr << "[warning] receiver_ipv6: registry full (1000 limit), ignoring node '" << announce.name << "'\n";
                    continue;
                }
                nodes[announce.name] = {
                    announce.name,
                    ip,
                    announce.services,
                    std::chrono::steady_clock::now(),
                    NodeStatus::Online,
                    p.publicKey
                };
            }
        } else {
            std::cerr << "[error] receiver_ipv6: received invalid packet\n";
        }
    }

    close(sock);
}

void printer() {
    while (running) {

        {
            std::shared_lock<std::shared_mutex> lock(nodesMutex);

            // std::cout << "\033[2J\033[H";
            std::cout << "=== LNOS NODES ===" << std::endl;

                for (const auto& n : nodes) {
                    auto seconds = std::chrono::duration_cast<std::chrono::seconds>
                    (std::chrono::steady_clock::now() - n.second.lastSeen).count();

                    std::cout << n.second.name
                              << " - " << n.second.ip
                              << " Status: "
                              << (n.second.status == NodeStatus::Online
                                  ? "Online"
                                  : "Offline");
                    if (n.second.status == NodeStatus::Offline) {
                        std::cout << "(" << seconds << " seconds ago)";
                    }
                    std::cout << std::endl;
                    std::cout << "Services:\n";

                    for (const auto& s : n.second.services)
                    {
                        std::cout
                            << "  "
                            << s.name
                            << ":"
                            << s.port
                            << '\n';
                    }
                }
        } // mutex освобождён здесь


        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

void cleanup() {
    while (running) {
        {
            std::unique_lock<std::shared_mutex> lock(nodesMutex);
            for (auto it = nodes.begin(); it != nodes.end(); ) {
                auto age = std::chrono::steady_clock::now() - it->second.lastSeen;
                if (age > std::chrono::seconds(NODE_TTL_SECONDS * 4)) {
                    it = nodes.erase(it);
                } else if (age > std::chrono::seconds(NODE_TTL_SECONDS)) {
                    it->second.status = NodeStatus::Offline;
                    ++it;
                } else {
                    ++it;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

int main() {
    std::signal(SIGINT, handleSigint);

    if (sodium_init() < 0) {
        std::cerr << "[fatal] Failed to initialize libsodium\n";
        return 1;
    }

    lnos::createConfig();

    cfg = lnos::loadConfig();
    std::cout << "My name: " << cfg.name << "\n";

    InterfaceInfo info = detectInterface();
    std::cout << "Detected interface: " << info.name << " (index: " << info.index << ")\n";
    if (info.has_ipv4) {
        std::cout << "  IPv4: " << info.ipv4 << "\n";
    }
    if (info.has_ipv6) {
        std::cout << "  IPv6: " << info.ipv6 << "\n";
    }

    if (!info.has_ipv4 && !info.has_ipv6) {
        stopWithError("No active network interfaces with IPv4 or IPv6 found.");
        return 1;
    }

    std::vector<std::thread> threads;
    if (info.has_ipv4) {
        threads.emplace_back(sender_ipv4, info.ipv4);
        threads.emplace_back(receiver_ipv4, info.ipv4);
    }
    if (info.has_ipv6) {
        threads.emplace_back(sender_ipv6, info.index);
        threads.emplace_back(receiver_ipv6, info.index);
    }
    threads.emplace_back(query_server);
    threads.emplace_back(printer);
    threads.emplace_back(cleanup);

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    std::cout << "LNOS is stopped." << std::endl;
}
