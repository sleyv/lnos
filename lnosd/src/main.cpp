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
#include <set>
#include <fstream>
#include <filesystem>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sodium.h>
#include <sstream>
#include <sys/stat.h>
#include <chrono>
#include <random>

#include <lnos/crypto.h>
#include "registry.h"
#include <lnos/protocol.h>
#include <lnos/config.h>

class FdGuard {
    int fd_;
public:
    explicit FdGuard(int fd = -1) : fd_(fd) {}
    ~FdGuard() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;

    FdGuard(FdGuard&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }
    FdGuard& operator=(FdGuard&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }
    int release() noexcept {
        int temp = fd_;
        fd_ = -1;
        return temp;
    }
    operator int() const noexcept { return fd_; }
};

constexpr std::size_t RECV_BUFFER_SIZE = 4096;
constexpr int ANNOUNCE_INTERVAL_SECONDS = 2;
constexpr int NODE_TTL_SECONDS = 15;
constexpr int QUERY_WAIT_MS = 400;
constexpr int MAX_ACTIVE_QUERIES = 64;

struct InterfaceInfo {
    std::string name;
    unsigned int index = 0;
    std::string ipv4;
    std::string ipv6;
    bool has_ipv4 = false;
    bool has_ipv6 = false;
};

// Traits for IPv4 and IPv6 deduplication
struct IPv4Traits {
    using AddrType = sockaddr_in;
    using TtlType = unsigned char;
    static constexpr int Family = AF_INET;
    static constexpr int Proto = IPPROTO_IP;
    static constexpr int MulticastTtlOpt = IP_MULTICAST_TTL;
    static constexpr int MulticastLoopOpt = IP_MULTICAST_LOOP;
    static constexpr int MulticastIfOpt = IP_MULTICAST_IF;
    static constexpr int AddMembershipOpt = IP_ADD_MEMBERSHIP;

    static std::string getMcastGroup(const lnos::Config& cfg) { return cfg.mcastGroup; }

    static void setIf(int sock, const std::string& myIp, unsigned int ifindex) {
        in_addr localIf{};
        if (inet_pton(AF_INET, myIp.c_str(), &localIf) == 1) {
            setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &localIf, sizeof(localIf));
        }
    }

    static bool joinGroup(int sock, const std::string& mcastGroup, const std::string& myIp, unsigned int ifindex) {
        ip_mreq mreq{};
        if (inet_pton(AF_INET, mcastGroup.c_str(), &mreq.imr_multiaddr) != 1) return false;
        if (inet_pton(AF_INET, myIp.c_str(), &mreq.imr_interface) != 1) return false;
        return setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) >= 0;
    }

    static bool parseAddr(const std::string& ipStr, uint16_t port, sockaddr_in& addr) {
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        return inet_pton(AF_INET, ipStr.c_str(), &addr.sin_addr) == 1;
    }

    static std::string ntop(const sockaddr_in& addr) {
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip)) == nullptr) return {};
        return ip;
    }

    static constexpr size_t AddrSize = sizeof(sockaddr_in);
};

struct IPv6Traits {
    using AddrType = sockaddr_in6;
    using TtlType = int;
    static constexpr int Family = AF_INET6;
    static constexpr int Proto = IPPROTO_IPV6;
    static constexpr int MulticastTtlOpt = IPV6_MULTICAST_HOPS;
    static constexpr int MulticastLoopOpt = IPV6_MULTICAST_LOOP;
    static constexpr int MulticastIfOpt = IPV6_MULTICAST_IF;
    static constexpr int AddMembershipOpt = IPV6_ADD_MEMBERSHIP;

    static std::string getMcastGroup(const lnos::Config& cfg) { return cfg.mcastGroupV6; }

    static void setIf(int sock, const std::string& myIp, unsigned int ifindex) {
        setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex));
    }

    static bool joinGroup(int sock, const std::string& mcastGroup, const std::string& myIp, unsigned int ifindex) {
        ipv6_mreq mreq{};
        if (inet_pton(AF_INET6, mcastGroup.c_str(), &mreq.ipv6mr_multiaddr) != 1) return false;
        mreq.ipv6mr_interface = ifindex;
        return setsockopt(sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) >= 0;
    }

    static bool parseAddr(const std::string& ipStr, uint16_t port, sockaddr_in6& addr) {
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);
        return inet_pton(AF_INET6, ipStr.c_str(), &addr.sin6_addr) == 1;
    }

    static std::string ntop(const sockaddr_in6& addr) {
        char ip[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &addr.sin6_addr, ip, sizeof(ip)) == nullptr) return {};
        return ip;
    }

    static constexpr size_t AddrSize = sizeof(sockaddr_in6);
};

class Daemon {
public:
    std::atomic<bool> running{true};
    std::atomic<int> activeQueries{0};

    // Atomic Metrics
    std::atomic<uint64_t> queriesResolved{0};
    std::atomic<uint64_t> queriesFailed{0};
    std::atomic<uint64_t> packetsReceived{0};
    std::atomic<uint64_t> packetsDropped{0};
    std::atomic<uint64_t> packetsRejectedSig{0};

    lnos::Config cfg;
    InterfaceInfo interfaceInfo;

    std::array<uint8_t, PRIVATE_KEY_SIZE> privateKey;
    std::array<uint8_t, PUBLIC_KEY_SIZE> publicKey;

    std::shared_mutex nodesMutex;
    std::mutex coutMutex;

    Daemon() {
        if (sodium_init() < 0) {
            throw std::runtime_error("Failed to initialize libsodium");
        }
    }

    void loadKeys() {
        privateKey = lnos::loadPrivateKey();
        publicKey = lnos::loadPublicKey();
    }

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

    void updateOwnersDB() {
        std::set<std::string> owners;
        {
            std::shared_lock<std::shared_mutex> lock(nodesMutex);
            for (const auto& n : nodes) {
                auto dot = n.first.find_last_of('.');
                if (dot != std::string::npos && dot + 1 < n.first.size()) {
                    owners.insert(n.first.substr(dot + 1));
                }
            }
        }

        std::string path = lnos::getConfigDir() + "/owners.db";
        std::string tmp = path + ".tmp";

        std::ofstream f(tmp);
        if (!f.is_open()) return;
        for (const auto& o : owners) {
            f << o << "\n";
        }
        f.close();

        std::error_code ec;
        std::filesystem::permissions(tmp,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read | std::filesystem::perms::others_read,
            std::filesystem::perm_options::replace, ec);
        std::filesystem::rename(tmp, path, ec);
    }

    void sendMulticastQuery(const std::string& target_name) {
        lnos::Packet p(target_name, {});
        p.type = lnos::PacketType::Query;
        p.publicKey = publicKey;

        if (!lnos::signPacket(p, privateKey)) {
            return;
        }

        // Encrypt the payload as multicast (which will use crypto_secretbox symmetrically based on our own publicKey)
        if (!lnos::encryptPacketPayload(p, privateKey, publicKey, true)) {
            return;
        }

        lnos::Blob msg = lnos::encode(p, true);

        // Send IPv4
        if (interfaceInfo.has_ipv4) {
            FdGuard sock4(socket(AF_INET, SOCK_DGRAM, 0));
            if (sock4 >= 0) {
                unsigned char ttl = 1;
                setsockopt(sock4, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
                in_addr localIf{};
                if (inet_pton(AF_INET, interfaceInfo.ipv4.c_str(), &localIf) == 1) {
                    setsockopt(sock4, IPPROTO_IP, IP_MULTICAST_IF, &localIf, sizeof(localIf));
                }
                sockaddr_in addr4{};
                addr4.sin_family = AF_INET;
                addr4.sin_port = htons(cfg.port);
                if (inet_pton(AF_INET, cfg.mcastGroup.c_str(), &addr4.sin_addr) == 1) {
                    sendto(sock4, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&addr4), sizeof(addr4));
                }
            }
        }

        // Send IPv6
        if (interfaceInfo.has_ipv6) {
            FdGuard sock6(socket(AF_INET6, SOCK_DGRAM, 0));
            if (sock6 >= 0) {
                int hops = 1;
                setsockopt(sock6, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));
                setsockopt(sock6, IPPROTO_IPV6, IPV6_MULTICAST_IF, &interfaceInfo.index, sizeof(interfaceInfo.index));
                sockaddr_in6 addr6{};
                addr6.sin6_family = AF_INET6;
                addr6.sin6_port = htons(cfg.port);
                if (inet_pton(AF_INET6, cfg.mcastGroupV6.c_str(), &addr6.sin6_addr) == 1) {
                    sendto(sock6, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&addr6), sizeof(addr6));
                }
            }
        }
    }

    void handle_client(int client) {
        FdGuard clientGuard(client);
        char buf[256];
        ssize_t n = read(client, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::string qname(buf);
            while (!qname.empty() && (qname.back() == '\n' || qname.back() == '\r')) {
                qname.pop_back();
            }

            // Stats request
            if (qname == "__STATS__") {
                std::stringstream ss;
                ss << "queriesResolved=" << queriesResolved.load() << "\n"
                   << "queriesFailed=" << queriesFailed.load() << "\n"
                   << "packetsReceived=" << packetsReceived.load() << "\n"
                   << "packetsDropped=" << packetsDropped.load() << "\n"
                   << "packetsRejectedSig=" << packetsRejectedSig.load() << "\n";
                {
                    std::shared_lock<std::shared_mutex> lock(nodesMutex);
                    ss << "nodeCount=" << nodes.size() << "\n";
                }
                std::string resp = ss.str();
                write(client, resp.data(), resp.length());
                return;
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
                std::this_thread::sleep_for(std::chrono::milliseconds(QUERY_WAIT_MS));
                {
                    std::shared_lock<std::shared_mutex> lock(nodesMutex);
                    auto it = nodes.find(qname);
                    if (it != nodes.end() && it->second.status == NodeStatus::Online) {
                        resolved_ip = it->second.ip;
                    }
                }
            }

            if (resolved_ip != "NOT_FOUND") {
                queriesResolved++;
            } else {
                queriesFailed++;
            }

            std::string resp = resolved_ip + "\n";
            write(client, resp.data(), resp.length());
        }
    }

    void runQueryServer() {
        FdGuard sock(socket(AF_UNIX, SOCK_STREAM, 0));
        if (sock < 0) {
            stopWithError("query_server: socket failed");
            return;
        }

        struct sockaddr_un un{};
        un.sun_family = AF_UNIX;
        std::string socket_path = lnos::getConfigDir() + "/lnosd.sock";
        std::strncpy(un.sun_path, socket_path.c_str(), sizeof(un.sun_path) - 1);
        un.sun_path[sizeof(un.sun_path) - 1] = '\0';
        unlink(un.sun_path);

        if (bind(sock, reinterpret_cast<sockaddr*>(&un), sizeof(un)) < 0) {
            stopWithError("query_server: bind failed to " + socket_path);
            return;
        }

        // Give socket proper permissions so other users can resolve names
        chmod(un.sun_path, 0666);

        if (listen(sock, 128) < 0) {
            stopWithError("query_server: listen failed");
            unlink(un.sun_path);
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

            if (activeQueries >= MAX_ACTIVE_QUERIES) {
                close(client);
                continue;
            }
            activeQueries++;
            std::thread t([this, client]() {
                handle_client(client);
                activeQueries--;
            });
            t.detach();
        }

        unlink(un.sun_path);
    }

    template <typename Traits>
    void runSender(std::string myIp, unsigned int ifindex) {
        FdGuard sock(socket(Traits::Family, SOCK_DGRAM, 0));
        if (sock < 0) {
            stopAfterSystemError("sender socket");
            return;
        }

        typename Traits::TtlType ttl = 1;
        if (setsockopt(sock, Traits::Proto, Traits::MulticastTtlOpt, &ttl, sizeof(ttl)) < 0) {
            stopAfterSystemError("sender TTL");
            return;
        }

        int loop = 1;
        if (setsockopt(sock, Traits::Proto, Traits::MulticastLoopOpt, &loop, sizeof(loop)) < 0) {
            stopAfterSystemError("sender loopback");
            return;
        }

        Traits::setIf(sock, myIp, ifindex);

        typename Traits::AddrType addr{};
        if (!Traits::parseAddr(Traits::getMcastGroup(cfg), cfg.port, addr)) {
            stopWithError("sender: Invalid multicast address");
            return;
        }

        while (running) {
            lnos::Packet p(cfg.name, cfg.services);
            p.type = lnos::PacketType::Announce;
            p.publicKey = publicKey;

            if (!lnos::signPacket(p, privateKey)) {
                stopWithError("sender: Packet signing failed");
                break;
            }

            // Encrypt the payload as multicast (symmetrically using sender's public key)
            if (!lnos::encryptPacketPayload(p, privateKey, publicKey, true)) {
                stopWithError("sender: Packet encryption failed");
                break;
            }

            lnos::Blob msg = lnos::encode(p, true);

#ifndef NDEBUG
            {
                std::lock_guard<std::mutex> lock(coutMutex);
                std::cout << "[debug] sender: sending " << msg.size() << " bytes\n";
            }
#endif

            if (sendto(sock, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                stopAfterSystemError("sender sendto");
                break;
            }

            std::this_thread::sleep_for(std::chrono::seconds(ANNOUNCE_INTERVAL_SECONDS));
        }
    }

    template <typename Traits>
    void runReceiver(std::string myIp, unsigned int ifindex) {
        FdGuard sock(socket(Traits::Family, SOCK_DGRAM, 0));
        if (sock < 0) {
            stopAfterSystemError("receiver socket");
            return;
        }

        int reuse = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            stopAfterSystemError("receiver SO_REUSEADDR");
            return;
        }

        // Dual-stack protection for IPv6
        if constexpr (Traits::Family == AF_INET6) {
            int v6only = 1;
            if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0) {
                stopAfterSystemError("receiver IPV6_V6ONLY");
                return;
            }
        }

        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            stopAfterSystemError("receiver SO_RCVTIMEO");
            return;
        }

        typename Traits::AddrType addr{};
        // Bind to ANY to receive multicast
        if constexpr (Traits::Family == AF_INET) {
            addr.sin_family = AF_INET;
            addr.sin_port = htons(cfg.port);
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(cfg.port);
            addr.sin6_addr = in6addr_any;
        }

        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            stopAfterSystemError("receiver bind");
            return;
        }

        if (!Traits::joinGroup(sock, Traits::getMcastGroup(cfg), myIp, ifindex)) {
            stopAfterSystemError("receiver join multicast group");
            return;
        }

        char buffer[RECV_BUFFER_SIZE];
        std::unordered_map<std::string, int> perSource;
        auto lastReset = std::chrono::steady_clock::now();

        while (running) {
            typename Traits::AddrType senderAddr{};
            socklen_t senderLen = sizeof(senderAddr);

            ssize_t len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);
            if (len < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    stopAfterSystemError("receiver recvfrom");
                    break;
                }
                continue;
            }

            if (len == 0) continue;

            buffer[len] = 0;
            packetsReceived++;

            std::string srcIp = Traits::ntop(senderAddr);
            if (srcIp.empty()) continue;

#ifndef NDEBUG
            {
                std::lock_guard<std::mutex> lock(coutMutex);
                std::cout << "[debug] receiver: received " << len << " bytes from " << srcIp << "\n";
            }
#endif

            // Per-source rate limiting: max 50 packets/sec per source IP
            auto now = std::chrono::steady_clock::now();
            if (now - lastReset > std::chrono::seconds(1)) {
                perSource.clear();
                lastReset = now;
            }
            if (++perSource[srcIp] > 50) {
                packetsDropped++;
                continue;
            }

            lnos::EncodedPacket encoded((uint8_t *) buffer, len);
            lnos::Packet p;
            if (lnos::decode(encoded, p)) {
                // Decrypt multicast packet if encrypted (type is Announce/Response/Query)
                bool isMulticast = (p.type == lnos::PacketType::Announce || p.type == lnos::PacketType::Response || p.type == lnos::PacketType::Query);
                if (p.isEncrypted) {
                    if (!lnos::decryptPacketPayload(p, privateKey, p.publicKey, isMulticast)) {
                        std::cerr << "[error] receiver: decryption failed for source IP: " << srcIp << "\n";
                        continue;
                    }
                }

                if (!lnos::verifyPacket(p)) {
                    std::cerr << "[error] receiver: packet signature verification failed\n";
                    packetsRejectedSig++;
                    continue;
                }

                if (p.type == lnos::PacketType::Query) {
                    if (p.announce.name == cfg.name) {
                        try {
                            lnos::Packet resp(cfg.name, cfg.services);
                            resp.type = lnos::PacketType::Response;
                            resp.publicKey = publicKey;
                            if (lnos::signPacket(resp, privateKey)) {
                                // Multicast responses are encrypted symmetrically
                                if (lnos::encryptPacketPayload(resp, privateKey, publicKey, true)) {
                                    lnos::Blob resp_msg = lnos::encode(resp, true);
                                    sendto(sock, resp_msg.data(), resp_msg.size(), 0, reinterpret_cast<sockaddr*>(&senderAddr), senderLen);
                                }
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "[error] receiver: response failed: " << e.what() << "\n";
                        }
                    }
                } else if (p.type == lnos::PacketType::Announce || p.type == lnos::PacketType::Response) {
                    bool updated = false;
                    {
                        std::unique_lock<std::shared_mutex> lock(nodesMutex);
                        const auto& announce = p.announce;
                        auto it = nodes.find(announce.name);
                        if (it != nodes.end()) {
                            if (it->second.publicKey != p.publicKey) {
                                std::cerr << "[error] receiver: public key mismatch for node '" << announce.name << "', rejecting (C-2 prevention)\n";
                                continue;
                            }
                        } else if (nodes.size() >= 1000) {
                            std::cerr << "[warning] receiver: registry full (1000 limit), ignoring node '" << announce.name << "'\n";
                            continue;
                        }
                        // Prefer IPv4 over IPv6: keep existing IPv4 if we have one
                        bool isNewIpV4 = srcIp.find(':') == std::string::npos;
                        bool keepIp = (it != nodes.end() && it->second.ip.find(':') == std::string::npos && !isNewIpV4);
                        nodes[announce.name] = {
                            announce.name,
                            keepIp ? it->second.ip : srcIp,
                            announce.services,
                            std::chrono::steady_clock::now(),
                            NodeStatus::Online,
                            p.publicKey
                        };
                        updated = true;
                    }
                    if (updated) {
                        updateOwnersDB();
                    }
                } else if (p.type == lnos::PacketType::GossipRequest) {
                    // GossipRequest is unicast (isMulticast = false)
                    // Let's reply with a GossipResponse
                    try {
                        lnos::Packet resp;
                        resp.type = lnos::PacketType::GossipResponse;
                        resp.publicKey = publicKey;

                        // Fill in our known nodes list (only Online nodes, max 100 to fit in limits)
                        {
                            std::shared_lock<std::shared_mutex> lock(nodesMutex);
                            int limit = 0;
                            for (const auto& pair : nodes) {
                                if (pair.second.status == NodeStatus::Online) {
                                    lnos::GossipNode gnode{
                                        pair.second.name,
                                        pair.second.ip,
                                        pair.second.services,
                                        pair.second.publicKey
                                    };
                                    resp.gossipNodes.push_back(gnode);
                                    if (++limit >= 100) break;
                                }
                            }
                        }

                        if (lnos::signPacket(resp, privateKey)) {
                            // Encrypt with recipient's public key (asymmetric)
                            if (lnos::encryptPacketPayload(resp, privateKey, p.publicKey, false)) {
                                lnos::Blob resp_msg = lnos::encode(resp, true);
                                sendto(sock, resp_msg.data(), resp_msg.size(), 0, reinterpret_cast<sockaddr*>(&senderAddr), senderLen);
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[error] receiver: GossipResponse failed: " << e.what() << "\n";
                    }
                } else if (p.type == lnos::PacketType::GossipResponse) {
                    // Merge Gossip nodes back into registry
                    bool updated = false;
                    {
                        std::unique_lock<std::shared_mutex> lock(nodesMutex);
                        for (const auto& gnode : p.gossipNodes) {
                            if (gnode.name == cfg.name) continue; // Don't merge ourselves
                            auto it = nodes.find(gnode.name);
                            if (it != nodes.end()) {
                                if (it->second.publicKey != gnode.publicKey) {
                                    continue; // Key mismatch TOFU
                                }
                            } else if (nodes.size() >= 1000) {
                                continue;
                            }
                            nodes[gnode.name] = {
                                gnode.name,
                                gnode.ip,
                                gnode.services,
                                std::chrono::steady_clock::now(),
                                NodeStatus::Online,
                                gnode.publicKey
                            };
                            updated = true;
                        }
                    }
                    if (updated) {
                        updateOwnersDB();
                    }
                }
            } else {
                std::cerr << "[error] receiver: received invalid packet\n";
            }
        }
    }

    void runGossip() {
        // Run every 30 seconds
        std::default_random_engine generator(std::chrono::system_clock::now().time_since_epoch().count());
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!running) break;

            Node peer;
            bool found = false;

            {
                std::shared_lock<std::shared_mutex> lock(nodesMutex);
                std::vector<Node> candidates;
                for (const auto& pair : nodes) {
                    if (pair.second.name != cfg.name && pair.second.status == NodeStatus::Online) {
                        candidates.push_back(pair.second);
                    }
                }
                if (!candidates.empty()) {
                    std::uniform_int_distribution<size_t> distribution(0, candidates.size() - 1);
                    peer = candidates[distribution(generator)];
                    found = true;
                }
            }

            if (found) {
                // Send GossipRequest unicast to peer
                lnos::Packet p;
                p.type = lnos::PacketType::GossipRequest;
                p.publicKey = publicKey;

                // Fill our registry into gossipNodes
                {
                    std::shared_lock<std::shared_mutex> lock(nodesMutex);
                    int count = 0;
                    for (const auto& pair : nodes) {
                        if (pair.second.status == NodeStatus::Online) {
                            lnos::GossipNode gn{
                                pair.second.name,
                                pair.second.ip,
                                pair.second.services,
                                pair.second.publicKey
                            };
                            p.gossipNodes.push_back(gn);
                            if (++count >= 100) break;
                        }
                    }
                }

                if (lnos::signPacket(p, privateKey)) {
                    // Encrypt as unicast
                    if (lnos::encryptPacketPayload(p, privateKey, peer.publicKey, false)) {
                        lnos::Blob msg = lnos::encode(p, true);

                        // Is it IPv4 or IPv6?
                        if (peer.ip.find(':') != std::string::npos) {
                            // IPv6 unicast
                            FdGuard s6(socket(AF_INET6, SOCK_DGRAM, 0));
                            if (s6 >= 0) {
                                sockaddr_in6 addr6{};
                                addr6.sin6_family = AF_INET6;
                                addr6.sin6_port = htons(cfg.port);
                                if (inet_pton(AF_INET6, peer.ip.c_str(), &addr6.sin6_addr) == 1) {
                                    sendto(s6, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&addr6), sizeof(addr6));
                                }
                            }
                        } else {
                            // IPv4 unicast
                            FdGuard s4(socket(AF_INET, SOCK_DGRAM, 0));
                            if (s4 >= 0) {
                                sockaddr_in addr4{};
                                addr4.sin_family = AF_INET;
                                addr4.sin_port = htons(cfg.port);
                                if (inet_pton(AF_INET, peer.ip.c_str(), &addr4.sin_addr) == 1) {
                                    sendto(s4, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&addr4), sizeof(addr4));
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void runHttpServer() {
        FdGuard server_sock(socket(AF_INET, SOCK_STREAM, 0));
        if (server_sock < 0) {
            std::cerr << "[error] HTTP server socket creation failed\n";
            return;
        }

        int reuse = 1;
        setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(9999);

        if (bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
            std::cerr << "[warning] HTTP server bind failed on port 9999. Running HTTP server is disabled.\n";
            return;
        }

        if (listen(server_sock, 10) < 0) {
            std::cerr << "[error] HTTP server listen failed\n";
            return;
        }

        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while (running) {
            int client = accept(server_sock, nullptr, nullptr);
            if (client < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                break;
            }

            std::thread([this, client]() {
                FdGuard client_sock(client);
                char request_buf[2048];
                ssize_t bytes_received = recv(client, request_buf, sizeof(request_buf) - 1, 0);
                if (bytes_received <= 0) return;

                request_buf[bytes_received] = '\0';
                std::string req(request_buf);
                std::string path;
                if (req.rfind("GET ", 0) == 0) {
                    size_t end_of_path = req.find(' ', 4);
                    if (end_of_path != std::string::npos) {
                        path = req.substr(4, end_of_path - 4);
                    }
                }

                std::string response_body;
                std::string content_type = "text/plain";

                if (path == "/nodes") {
                    content_type = "application/json";
                    std::stringstream json;
                    json << "[\n";
                    {
                        std::shared_lock<std::shared_mutex> lock(nodesMutex);
                        bool first = true;
                        for (const auto& pair : nodes) {
                            if (!first) json << ",\n";
                            first = false;
                            json << "  {\n"
                                 << "    \"name\": \"" << pair.second.name << "\",\n"
                                 << "    \"ip\": \"" << pair.second.ip << "\",\n"
                                 << "    \"status\": \"" << (pair.second.status == NodeStatus::Online ? "Online" : "Offline") << "\",\n"
                                 << "    \"services\": [";
                            bool s_first = true;
                            for (const auto& s : pair.second.services) {
                                if (!s_first) json << ", ";
                                s_first = false;
                                json << "{\"name\":\"" << s.name << "\", \"port\":" << s.port << "}";
                            }
                            json << "]\n"
                                 << "  }";
                        }
                    }
                    json << "\n]";
                    response_body = json.str();
                } else if (path == "/stats") {
                    content_type = "application/json";
                    std::stringstream json;
                    json << "{\n"
                         << "  \"queriesResolved\": " << queriesResolved.load() << ",\n"
                         << "  \"queriesFailed\": " << queriesFailed.load() << ",\n"
                         << "  \"packetsReceived\": " << packetsReceived.load() << ",\n"
                         << "  \"packetsDropped\": " << packetsDropped.load() << ",\n"
                         << "  \"packetsRejectedSig\": " << packetsRejectedSig.load() << "\n"
                         << "}";
                    response_body = json.str();
                } else if (path == "/" || path.empty()) {
                    content_type = "text/html; charset=utf-8";
                     std::stringstream html;
                     html << "<!DOCTYPE html>\n"
                          << "<html>\n<head>\n"
                          << "<title>LNOS</title>\n"
                          << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
                          << "<style>\n"
                          << "  * { margin: 0; padding: 0; box-sizing: border-box; }\n"
                          << "  body { font-family: 'Segoe UI', system-ui, -apple-system, sans-serif; background: #0d1117; color: #c9d1d9; min-height: 100vh; }\n"
                          << "  .header { background: #161b22; border-bottom: 1px solid #30363d; padding: 24px 32px; display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 12px; }\n"
                          << "  .header h1 { font-size: 20px; font-weight: 600; color: #f0f6fc; letter-spacing: -0.3px; }\n"
                          << "  .header p { color: #8b949e; font-size: 14px; }\n"
                          << "  .header .node-name { color: #58a6ff; font-weight: 500; }\n"
                          << "  .header-right { display: flex; align-items: center; gap: 12px; }\n"
                          << "  .container { max-width: 1200px; margin: 0 auto; padding: 24px 32px; }\n"
                          << "  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 16px; margin-bottom: 32px; }\n"
                          << "  .card { background: #161b22; border: 1px solid #30363d; border-radius: 8px; padding: 20px; text-align: center; transition: border-color .2s; }\n"
                          << "  .card:hover { border-color: #58a6ff40; }\n"
                          << "  .card h3 { color: #8b949e; font-size: 12px; text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 8px; }\n"
                          << "  .card .value { font-size: 28px; font-weight: 700; color: #f0f6fc; font-variant-numeric: tabular-nums; }\n"
                          << "  .toolbar { display: flex; align-items: center; justify-content: space-between; margin-bottom: 16px; flex-wrap: wrap; gap: 8px; }\n"
                          << "  .toolbar h2 { font-size: 16px; font-weight: 600; color: #f0f6fc; }\n"
                          << "  .node-count { background: #21262d; color: #8b949e; padding: 2px 10px; border-radius: 12px; font-size: 12px; font-weight: 600; margin-left: 8px; }\n"
                          << "  .btn { background: #21262d; border: 1px solid #30363d; color: #c9d1d9; padding: 6px 16px; border-radius: 6px; cursor: pointer; font-size: 13px; transition: background .15s; display: inline-flex; align-items: center; gap: 6px; }\n"
                          << "  .btn:hover { background: #30363d; }\n"
                          << "  .btn:active { background: #3d444d; }\n"
                          << "  .btn .spinner { display: none; width: 14px; height: 14px; border: 2px solid #8b949e; border-top-color: #f0f6fc; border-radius: 50%; animation: spin .6s linear infinite; }\n"
                          << "  .btn.loading .spinner { display: inline-block; }\n"
                          << "  .btn.loading .label { display: none; }\n"
                          << "  @keyframes spin { to { transform: rotate(360deg); } }\n"
                          << "  .live-dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: #3fb950; margin-right: 6px; animation: pulse 2s ease-in-out infinite; }\n"
                          << "  @keyframes pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.4; } }\n"
                          << "  table { width: 100%; border-collapse: collapse; background: #161b22; border: 1px solid #30363d; border-radius: 8px; overflow: hidden; }\n"
                          << "  th, td { padding: 12px 16px; text-align: left; border-bottom: 1px solid #30363d; font-size: 14px; }\n"
                          << "  th { background: #21262d; color: #8b949e; font-weight: 600; text-transform: uppercase; font-size: 12px; letter-spacing: 0.5px; }\n"
                          << "  tr:last-child td { border-bottom: none; }\n"
                          << "  tr:hover td { background: #1c2128; }\n"
                          << "  .status-badge { padding: 3px 10px; border-radius: 12px; font-size: 12px; font-weight: 600; }\n"
                          << "  .status-online { background: #1b4a2b; color: #3fb950; border: 1px solid #2ea043; }\n"
                          << "  .status-offline { background: #4a1b1b; color: #f85149; border: 1px solid #da3633; }\n"
                          << "  .svc-tag { display: inline-block; background: #21262d; padding: 2px 8px; border-radius: 4px; font-size: 12px; margin: 1px; color: #8b949e; font-family: 'JetBrains Mono', 'Fira Code', monospace; }\n"
                          << "  .footer { text-align: center; padding: 32px; color: #484f58; font-size: 12px; }\n"
                          << "  @media (max-width: 640px) { .header { padding: 16px; } .container { padding: 16px; } .grid { grid-template-columns: 1fr 1fr; } th, td { padding: 8px 10px; font-size: 13px; } }\n"
                          << "</style>\n"
                          << "<script>\n"
                          << "  async function refresh() {\n"
                          << "    const btn = document.getElementById('refresh-btn');\n"
                          << "    if (btn) btn.classList.add('loading');\n"
                          << "    try {\n"
                          << "      const [statsRes, nodesRes] = await Promise.all([\n"
                          << "        fetch('/stats').then(r => r.json()),\n"
                          << "        fetch('/nodes').then(r => r.json())\n"
                          << "      ]);\n"
                          << "      document.getElementById('resolved').innerText = statsRes.queriesResolved;\n"
                          << "      document.getElementById('failed').innerText = statsRes.queriesFailed;\n"
                          << "      document.getElementById('received').innerText = statsRes.packetsReceived;\n"
                          << "      document.getElementById('dropped').innerText = statsRes.packetsDropped;\n"
                          << "      document.getElementById('node-count').innerText = nodesRes.length;\n"
                          << "      let rows = '';\n"
                          << "      nodesRes.forEach(node => {\n"
                          << "        const svcs = node.services.map(s => `<span class=\"svc-tag\">${s.name}:${s.port}</span>`).join('') || '<span style=\"color:#484f58;font-size:12px;\">None</span>';\n"
                          << "        const badge = node.status === 'Online' ? 'status-online' : 'status-offline';\n"
                          << "        rows += `<tr><td style=\"font-weight:500;\">${node.name}</td><td style=\"color:#8b949e;font-family:monospace;font-size:13px;\">${node.ip}</td><td><span class=\"status-badge ${badge}\">${node.status}</span></td><td>${svcs}</td></tr>`;\n"
                          << "      });\n"
                          << "      document.getElementById('peer-rows').innerHTML = rows || '<tr><td colspan=\"4\" style=\"text-align:center;color:#484f58;padding:24px;\">No peers discovered yet</td></tr>';\n"
                          << "      document.getElementById('last-updated').innerText = new Date().toLocaleTimeString();\n"
                          << "    } catch(e) { console.error('refresh failed', e); }\n"
                          << "    finally { if (btn) btn.classList.remove('loading'); }\n"
                          << "  }\n"
                          << "  setInterval(refresh, 2000);\n"
                          << "</script>\n"
                          << "</head>\n"
                          << "<body onload=\"refresh()\">\n"
                          << "  <div class=\"header\">\n"
                          << "    <div>\n"
                          << "      <h1>LNOS</h1>\n"
                          << "      <p>Node: <span class=\"node-name\">" << cfg.name << "</span></p>\n"
                          << "    </div>\n"
                          << "    <div class=\"header-right\">\n"
                          << "      <span style=\"color:#484f58;font-size:13px;\"><span class=\"live-dot\"></span><span id=\"last-updated\">—</span></span>\n"
                          << "      <button id=\"refresh-btn\" class=\"btn\" onclick=\"refresh()\"><span class=\"spinner\"></span><span class=\"label\">Refresh</span></button>\n"
                          << "    </div>\n"
                          << "  </div>\n"
                          << "  <div class=\"container\">\n"
                          << "    <div class=\"grid\">\n"
                          << "      <div class=\"card\"><h3>Resolved</h3><div class=\"value\" id=\"resolved\">0</div></div>\n"
                          << "      <div class=\"card\"><h3>Failed</h3><div class=\"value\" id=\"failed\">0</div></div>\n"
                          << "      <div class=\"card\"><h3>Received</h3><div class=\"value\" id=\"received\">0</div></div>\n"
                          << "      <div class=\"card\"><h3>Dropped</h3><div class=\"value\" id=\"dropped\">0</div></div>\n"
                          << "    </div>\n"
                          << "    <div class=\"toolbar\">\n"
                          << "      <h2>Connected Nodes <span class=\"node-count\" id=\"node-count\">0</span></h2>\n"
                          << "      <span style=\"color:#484f58;font-size:12px;\">Updated: <span id=\"last-updated2\">—</span></span>\n"
                          << "    </div>\n"
                          << "    <table>\n"
                          << "      <thead><tr><th>Name</th><th>IP</th><th>Status</th><th>Services</th></tr></thead>\n"
                          << "      <tbody id=\"peer-rows\"></tbody>\n"
                          << "    </table>\n"
                          << "  </div>\n"
                          << "  <div class=\"footer\">LNOS &mdash; Local Network Overlay System</div>\n"
                          << "</body>\n</html>\n";
                     response_body = html.str();
                } else {
                    response_body = "Not Found";
                }

                std::stringstream resp_stream;
                resp_stream << "HTTP/1.1 200 OK\r\n"
                            << "Content-Length: " << response_body.length() << "\r\n"
                            << "Content-Type: " << content_type << "\r\n"
                            << "Connection: close\r\n\r\n"
                            << response_body;
                std::string full_response = resp_stream.str();
                send(client, full_response.data(), full_response.length(), 0);
            }).detach();
        }
    }

    void runCleanup() {
        while (running) {
            bool changed = false;
            {
                std::unique_lock<std::shared_mutex> lock(nodesMutex);
                for (auto it = nodes.begin(); it != nodes.end(); ) {
                    auto age = std::chrono::steady_clock::now() - it->second.lastSeen;
                    if (age > std::chrono::seconds(NODE_TTL_SECONDS * 4)) {
                        it = nodes.erase(it);
                        changed = true;
                    } else if (age > std::chrono::seconds(NODE_TTL_SECONDS)) {
                        it->second.status = NodeStatus::Offline;
                        ++it;
                    } else {
                        ++it;
                    }
                }
            }
            if (changed) {
                updateOwnersDB();
            }
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }
};

Daemon* g_daemon = nullptr;

void handleSigint(int) {
    if (g_daemon) {
        g_daemon->running = false;
    }
}

int main() {
    std::signal(SIGINT, handleSigint);
    std::signal(SIGTERM, handleSigint);

    try {
        Daemon daemon;
        g_daemon = &daemon;

        lnos::createConfig();
        daemon.cfg = lnos::loadConfig();

        std::cout << "My name: " << daemon.cfg.name << "\n";
        std::cout << "Multicast group: " << daemon.cfg.mcastGroup << " (IPv4), " << daemon.cfg.mcastGroupV6 << " (IPv6)\n";
        std::cout << "Port: " << daemon.cfg.port << "\n";
        std::cout << "---\n";
        std::cout << "Tip: all nodes in the network must use the same multicast group and port.\n";
        std::cout << "Files are in: " << lnos::getConfigDir() << "\n";
        std::cout << "---\n";

        daemon.loadKeys();

        daemon.interfaceInfo = daemon.detectInterface();
        std::cout << "Detected interface: " << daemon.interfaceInfo.name << " (index: " << daemon.interfaceInfo.index << ")\n";
        if (daemon.interfaceInfo.has_ipv4) {
            std::cout << "  IPv4: " << daemon.interfaceInfo.ipv4 << "\n";
        }
        if (daemon.interfaceInfo.has_ipv6) {
            std::cout << "  IPv6: " << daemon.interfaceInfo.ipv6 << "\n";
        }

        if (!daemon.interfaceInfo.has_ipv4 && !daemon.interfaceInfo.has_ipv6) {
            daemon.stopWithError("No active network interfaces with IPv4 or IPv6 found.");
            return 1;
        }

        // Seed owners.db with our own owner
        {
            std::string path = lnos::getConfigDir() + "/owners.db";
            if (!std::filesystem::exists(path)) {
                auto dot = daemon.cfg.name.find_last_of('.');
                if (dot != std::string::npos && dot + 1 < daemon.cfg.name.size()) {
                    std::string tmp = path + ".tmp";
                    std::ofstream f(tmp);
                    if (f.is_open()) {
                        f << daemon.cfg.name.substr(dot + 1) << "\n";
                        f.close();
                        std::error_code ec;
                        std::filesystem::permissions(tmp,
                            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                            std::filesystem::perms::group_read | std::filesystem::perms::others_read,
                            std::filesystem::perm_options::replace, ec);
                        std::filesystem::rename(tmp, path, ec);
                    }
                }
            }
        }

        // Self-register
        {
            std::unique_lock<std::shared_mutex> lock(daemon.nodesMutex);
            nodes[daemon.cfg.name] = {
                daemon.cfg.name,
                daemon.interfaceInfo.has_ipv4 ? daemon.interfaceInfo.ipv4 : daemon.interfaceInfo.ipv6,
                daemon.cfg.services,
                std::chrono::steady_clock::now(),
                NodeStatus::Online,
                daemon.publicKey
            };
        }

        std::vector<std::thread> threads;
        if (daemon.interfaceInfo.has_ipv4) {
            threads.emplace_back(&Daemon::runSender<IPv4Traits>, &daemon, daemon.interfaceInfo.ipv4, 0);
            threads.emplace_back(&Daemon::runReceiver<IPv4Traits>, &daemon, daemon.interfaceInfo.ipv4, 0);
        }
        if (daemon.interfaceInfo.has_ipv6) {
            threads.emplace_back(&Daemon::runSender<IPv6Traits>, &daemon, "", daemon.interfaceInfo.index);
            threads.emplace_back(&Daemon::runReceiver<IPv6Traits>, &daemon, "", daemon.interfaceInfo.index);
        }
        threads.emplace_back(&Daemon::runQueryServer, &daemon);
        threads.emplace_back(&Daemon::runGossip, &daemon);
        threads.emplace_back(&Daemon::runHttpServer, &daemon);
        threads.emplace_back(&Daemon::runCleanup, &daemon);

        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[fatal] Daemon exception: " << e.what() << "\n";
        return 1;
    }

    std::cout << "LNOS is stopped." << std::endl;
    return 0;
}
