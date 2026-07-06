#include <iostream>
#include <unordered_map>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../shared/protocol.h"

#define PORT 4545

struct Node {
    std::string name;
    std::string ip;
};

std::unordered_map<std::string, Node> nodes;

void receiver() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(sock, (sockaddr*)&addr, sizeof(addr));

    char buffer[1024];

    while (true) {
        int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (len <= 0) continue;

        buffer[len] = 0;

        lnos::Packet p = lnos::decode(buffer);

        nodes[p.name] = {p.name, p.ip};
    }
}

void printer() {
    while (true) {
        system("clear");
        std::cout << "=== LNOS SERVER ===\n\n";

        for (auto& n : nodes) {
            std::cout << n.second.name << " -> " << n.second.ip << "\n";
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

int main() {
    std::thread t1(receiver);
    std::thread t2(printer);

    t1.join();
    t2.join();
}