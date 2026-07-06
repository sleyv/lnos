#include <fstream>
#include <iostream>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mutex>
#include "ip.h"
#include "protocol.h"
#include "main.h"

#include <map>

#include "registry.h"

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

    while (true) {
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

    int reuse = 1;

    setsockopt(sock,
               SOL_SOCKET,
               SO_REUSEADDR,
               &reuse,
               sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(sock, (sockaddr*)&addr, sizeof(addr));

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

    while (true) {
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

        std::lock_guard<std::mutex> unlock(nodesMutex);
    }
}

void printer() {
    while (true) {
        std::vector<std::string> toDelete;
        std::cout << "\033[2J\033[H";
        std::cout << "=== LNOS NODES ===" << std::endl;

        std::lock_guard<std::mutex> lock(nodesMutex);

        for (const auto& n : nodes) {
            if (n.second.name == myName) continue;

            if (std::chrono::steady_clock::now() - n.second.lastSeen > std::chrono::seconds(15)) {
                toDelete.push_back(n.second.name);
            }

            std::cout << n.second.name
                      << " - " << n.second.ip << std::endl;
        }

        if (!toDelete.empty()) {
            for (const auto& name : toDelete) {
                nodes.erase(name);
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

int main() {
    std::thread t1(sender);
    std::thread t2(receiver);
    std::thread t3(printer);

    t1.join();
    t2.join();
    t3.join();
}