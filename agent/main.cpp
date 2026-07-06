#include <fstream>
#include <iostream>
#include <thread>
#include <csignal>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mutex>
#include "ip.h"
#include "protocol.h"
#include <atomic>
#include <map>

#include "registry.h"

#define SERVER_IP "127.0.0.1"

#define MCAST_GROUP "239.255.42.99"
#define PORT 4545


std::atomic<bool> running = true;

void handleSigint(int) {
    std::cout << "CTRL+C received\n";
    running = false;
}

std::string getName() {
    std::string name;
    std::ifstream nameFile("/etc/lnos/name");

    if (!nameFile.is_open()) {
        std::cerr << "lnos: Error: Unable to open the file" << std::endl;
        return name;
    }
    nameFile >> name;

    nameFile.close();

    return name;
}

std::mutex nodesMutex;

std::string myName = getName();
std::string myIp = getip();

void sender() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    inet_pton(AF_INET, MCAST_GROUP, &addr.sin_addr);

    while (running) {
        lnos::Packet p{myName};

        std::string msg = lnos::encode(p);

        sendto(sock,
               msg.c_str(),
               msg.size(),
               0,
               reinterpret_cast<sockaddr *>(&addr),
               sizeof(addr));

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void receiver() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

    ip_mreq mreq{};

    inet_pton(AF_INET,
              MCAST_GROUP,
              &mreq.imr_multiaddr);

    mreq.imr_interface.s_addr = INADDR_ANY;

    setsockopt(sock,
               IPPROTO_IP,
               IP_ADD_MEMBERSHIP,
               &mreq,
               sizeof(mreq));

    char buffer[1024];

    while (running) {
        sockaddr_in senderAddr{};
        socklen_t senderLen = sizeof(senderAddr);

        int len = recvfrom(sock,
                           buffer,
                           sizeof(buffer) - 1,
                           0,
                           reinterpret_cast<sockaddr *>(&senderAddr),
                           &senderLen);

        if (len <= 0)
            continue;

        buffer[len] = 0;

        char ip[INET_ADDRSTRLEN];

        inet_ntop(AF_INET,
                  &senderAddr.sin_addr,
                  ip,
                  sizeof(ip));

        lnos::Packet p = lnos::decode(buffer);

        std::lock_guard<std::mutex> lock(nodesMutex);

        nodes[p.name] = {
            p.name,
            ip,
            std::chrono::steady_clock::now()
        };
    }
}

void printer() {
    while (running) {
        std::cout << "\033[2J\033[H";
        std::cout << "=== LNOS NODES ===" << std::endl;

        std::lock_guard<std::mutex> lock(nodesMutex);

        for (const auto& n : nodes) {
            if (n.second.name == myName) continue;
            std::cout << n.second.name
                      << " - " << n.second.ip << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

void cleanup() {
    while (running) {

        std::vector<std::string> toDelete;

        {
            std::lock_guard<std::mutex> lock(nodesMutex);

            for (const auto& n : nodes) {
                if (std::chrono::steady_clock::now() - n.second.lastSeen
                    > std::chrono::seconds(15)) {
                    toDelete.push_back(n.first);
                    }
            }

            for (const auto& name : toDelete) {
                nodes.erase(name);
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

int main() {
    std::signal(SIGINT, handleSigint);

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