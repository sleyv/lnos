#include <iostream>
#include <thread>
#include <csignal>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mutex>
#include <atomic>
#include <map>

#include "registry.h"
#include <lnos/ip.h>
#include <lnos/protocol.h>
#include <lnos/config.h>

#define SERVER_IP "127.0.0.1"

#define MCAST_GROUP "239.255.42.99"
#define PORT 4545


std::atomic<bool> running = true;

void handleSigint(int) {
    std::cout << "CTRL+C received\n";
    running = false;
}


std::mutex nodesMutex;
std::mutex coutMutex;

lnos::Config cfg = lnos::loadConfig();
std::string myName = cfg.name;
std::string myIp = getip(cfg.interface);

void sender() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        perror("sender socket");
        return;
    }

    unsigned char ttl = 1;

    setsockopt(sock,
               IPPROTO_IP,
               IP_MULTICAST_TTL,
               &ttl,
               sizeof(ttl));

    // Разрешаем получать свои же multicast-пакеты
    int loop = 1;
    if (setsockopt(sock,
                   IPPROTO_IP,
                   IP_MULTICAST_LOOP,
                   &loop,
                   sizeof(loop)) < 0) {
        perror("IP_MULTICAST_LOOP");
                   }

    // Указываем интерфейс для multicast
    in_addr localInterface{};

    if (inet_pton(AF_INET,
                  myIp.c_str(),
                  &localInterface) <= 0) {
        perror("sender inet_pton");
        close(sock);
        return;
                  }

    if (setsockopt(sock,
                   IPPROTO_IP,
                   IP_MULTICAST_IF,
                   &localInterface,
                   sizeof(localInterface)) < 0) {
        perror("IP_MULTICAST_IF");
        close(sock);
        return;
                   }


    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    inet_pton(AF_INET,
              MCAST_GROUP,
              &addr.sin_addr);


    while (running) {

        lnos::Packet p{myName};

        std::string msg = lnos::encode(p);

        std::cout << "[debug] sending "
                  << msg.size()
                  << " bytes\n";


        if (sendto(sock,
                   msg.c_str(),
                   msg.size(),
                   0,
                   reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            perror("sendto");
                   }


        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    close(sock);
}

void receiver() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    int reuse = 1;

    if (setsockopt(sock,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &reuse,
                   sizeof(reuse)) < 0) {
        perror("SO_REUSEADDR");
                   }

    if (sock < 0) {
        perror("receiver socket");
        return;
    }


    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    setsockopt(sock,
               SOL_SOCKET,
               SO_RCVTIMEO,
               &tv,
               sizeof(tv));


    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;


    if (bind(sock,
             reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {

        perror("bind");
        close(sock);
        return;
    }


    ip_mreq mreq{};


    // multicast адрес
    if (inet_pton(AF_INET,
                  MCAST_GROUP,
                  &mreq.imr_multiaddr) <= 0) {

        perror("multicast address");
        close(sock);
        return;
    }


    // интерфейс
    if (inet_pton(AF_INET,
                  myIp.c_str(),
                  &mreq.imr_interface) <= 0) {

        perror("interface address");
        close(sock);
        return;
    }


    if (setsockopt(sock,
                   IPPROTO_IP,
                   IP_ADD_MEMBERSHIP,
                   &mreq,
                   sizeof(mreq)) < 0) {

        perror("IP_ADD_MEMBERSHIP");
        close(sock);
        return;
    }


    char buffer[1024];


    while (running) {

        sockaddr_in senderAddr{};
        socklen_t senderLen = sizeof(senderAddr);


        int len = recvfrom(sock,
                           buffer,
                           sizeof(buffer) - 1,
                           0,
                           reinterpret_cast<sockaddr*>(&senderAddr),
                           &senderLen);


        if (len <= 0)
            continue;


        buffer[len] = 0;


        char ip[INET_ADDRSTRLEN];

        inet_ntop(AF_INET,
                  &senderAddr.sin_addr,
                  ip,
                  sizeof(ip));


        std::cout << "[debug] received "
                  << len
                  << " bytes from "
                  << ip
                  << "\n";


        lnos::Packet p = lnos::decode(buffer);


        std::lock_guard<std::mutex> lock(nodesMutex);


        nodes[p.name] = {
            p.name,
            ip,
            std::chrono::steady_clock::now(),
            NodeStatus::Online
        };
    }


    close(sock);
}

void printer() {
    while (running) {

        {
            std::lock_guard<std::mutex> lock(nodesMutex);

            std::cout << "\033[2J\033[H";
            std::cout << "=== LNOS NODES ===" << std::endl;

            for (const auto& n : nodes) {
                std::cout << n.second.name
                          << " - " << n.second.ip
                          << " Status: "
                          << (n.second.status == NodeStatus::Online
                              ? "Online"
                              : "Offline")
                          << std::endl;
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

    std::cout << "My name: " << myName << "\n";
    std::cout << "My IP: " << myIp << "\n";

    std::thread t1(sender);
    std::thread t2(receiver);
    std::thread t3(printer);
    std::thread t4(cleanup);

    t1.join();
    t2.join();
    t3.join();
    t4.join();
    std::cout << "LNOS shutting down..." << std::endl;
}