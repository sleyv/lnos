#include <iostream>
#include <thread>
#include <csignal>
#include <cerrno>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mutex>
#include <atomic>
#include <array>
#include <sodium.h>

#include <lnos/crypto.h>
#include "registry.h"
#include <lnos/protocol.h>
#include <lnos/config.h>

#define MCAST_GROUP "239.255.42.99"
#define PORT 4545


std::atomic<bool> running = true;

void handleSigint(int) {
    std::cout << "CTRL+C received\n";
    running = false;
}

std::array<std::uint8_t, PUBLIC_KEY_SIZE> publicKey;
std::mutex nodesMutex;
std::mutex coutMutex;

lnos::Config cfg;
std::string myIp;

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

bool signPacket(lnos::Packet& packet,
                const std::array<uint8_t, PRIVATE_KEY_SIZE>& privateKey)
{
    lnos::Blob data = lnos::encode(packet, true);

    // Важно: кодируем без signature

    unsigned long long signatureLength;

    crypto_sign_detached(
        packet.signature.data(),
        &signatureLength,
        data.data(),
        data.size(),
        privateKey.data()
    );

    return signatureLength == crypto_sign_BYTES;
}

void sender() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        stopAfterSystemError("sender socket");
        return;
    }

    unsigned char ttl = 1;

    if (setsockopt(sock,
                   IPPROTO_IP,
                   IP_MULTICAST_TTL,
                   &ttl,
                   sizeof(ttl)) < 0) {
        stopAfterSystemError("IP_MULTICAST_TTL");
        close(sock);
        return;
    }

    // Разрешаем получать свои же multicast-пакеты
    int loop = 1;
    if (setsockopt(sock,
                   IPPROTO_IP,
                   IP_MULTICAST_LOOP,
                   &loop,
                   sizeof(loop)) < 0) {
        stopAfterSystemError("IP_MULTICAST_LOOP");
        close(sock);
        return;
    }

    // Указываем интерфейс для multicast
    in_addr localInterface{};

    if (inet_pton(AF_INET,
                  myIp.c_str(),
                  &localInterface) != 1) {
        stopWithError("Invalid local IPv4 address: '" + myIp + "'");
        close(sock);
        return;
    }

    if (setsockopt(sock,
                   IPPROTO_IP,
                   IP_MULTICAST_IF,
                   &localInterface,
                   sizeof(localInterface)) < 0) {
        stopAfterSystemError("IP_MULTICAST_IF");
        close(sock);
        return;
    }


    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, MCAST_GROUP, &addr.sin_addr) != 1) {
        stopWithError("Invalid multicast IPv4 address: '" MCAST_GROUP "'");
        close(sock);
        return;
    }

    auto privateKey = lnos::loadPrivateKey();
    auto publicKey = lnos::loadPublicKey();

    while (running) {

        lnos::Packet p(cfg.name, cfg.services);

        p.publicKey = publicKey;

        if (!lnos::signPacket(p, privateKey))
        {
            stopWithError("Packet signing failed");
            break;
        }


        lnos::Blob msg = lnos::encode(p, true);

        std::cout << "[debug] sending "
                  << msg.size()
                  << " bytes\n";


        if (sendto(sock,
                   msg.data(),
                   msg.size(),
                   0,
                   reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            stopAfterSystemError("sendto");
            break;
        }


        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    close(sock);
}

void receiver() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        stopAfterSystemError("receiver socket");
        return;
    }

    int reuse = 1;

    if (setsockopt(sock,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &reuse,
                   sizeof(reuse)) < 0) {
        stopAfterSystemError("SO_REUSEADDR");
        close(sock);
        return;
    }


    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    if (setsockopt(sock,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &tv,
                   sizeof(tv)) < 0) {
        stopAfterSystemError("SO_RCVTIMEO");
        close(sock);
        return;
    }


    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;


    if (bind(sock,
             reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {

        stopAfterSystemError("bind");
        close(sock);
        return;
    }


    ip_mreq mreq{};


    // multicast адрес
    if (inet_pton(AF_INET,
                  MCAST_GROUP,
                  &mreq.imr_multiaddr) != 1) {
        stopWithError("Invalid multicast IPv4 address: '" MCAST_GROUP "'");
        close(sock);
        return;
    }

    // интерфейс
    if (inet_pton(AF_INET, myIp.c_str(), &mreq.imr_interface) != 1) {
        stopWithError("Invalid local IPv4 address: '" + myIp + "'");
        close(sock);
        return;
    }


    if (setsockopt(sock,
                   IPPROTO_IP,
                   IP_ADD_MEMBERSHIP,
                   &mreq,
                   sizeof(mreq)) < 0) {

        stopAfterSystemError("IP_ADD_MEMBERSHIP");
        close(sock);
        return;
    }


    char buffer[1024];

    while (running) {

        sockaddr_in senderAddr{};
        socklen_t senderLen = sizeof(senderAddr);


        ssize_t len = recvfrom(sock,
                               buffer,
                               sizeof(buffer) - 1,
                               0,
                               reinterpret_cast<sockaddr*>(&senderAddr),
                               &senderLen);

        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                stopAfterSystemError("recvfrom");
                break;
            }
            continue;
        }

        if (len == 0)
            continue;


        buffer[len] = 0;


        char ip[INET_ADDRSTRLEN];

        if (inet_ntop(AF_INET,
                      &senderAddr.sin_addr,
                      ip,
                      sizeof(ip)) == nullptr) {
            stopAfterSystemError("inet_ntop");
            break;
        }


        std::cout << "[debug] received "
                  << len
                  << " bytes from "
                  << ip
                  << "\n";


        lnos::EncodedPacket encoded((uint8_t *) buffer, len);
        lnos::Packet p;
        if (lnos::decode(encoded, p)) {
            std::lock_guard<std::mutex> lock(nodesMutex);
            if (p.type == lnos::PacketType::Announce) {
                nodes[p.as.announce.name] = {
                    p.as.announce.name,
                    ip,
                    p.as.announce.services,
                    std::chrono::steady_clock::now(),
                    NodeStatus::Online
                };
            }
        } else {
            std::cerr << "[error] received invalid packet\n";
        }
    }


    close(sock);
}

void printer() {
    while (running) {

        {
            std::lock_guard<std::mutex> lock(nodesMutex);

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
            std::lock_guard<std::mutex> lock(nodesMutex);

            for (auto& n : nodes) {
                if (std::chrono::steady_clock::now() - n.second.lastSeen
                    > std::chrono::seconds(15)) {
                    n.second.status = NodeStatus::Offline;
                    }
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

int main() {
    std::signal(SIGINT, handleSigint);

    lnos::createConfig();

    cfg = lnos::loadConfig();
    std::cout << "My name: " << cfg.name << "\n";

    std::thread t1(sender);
    std::thread t2(receiver);
    std::thread t3(printer);
    std::thread t4(cleanup);

    t1.join();
    t2.join();
    t3.join();
    t4.join();
    std::cout << "LNOS is stopped." << std::endl;
}
