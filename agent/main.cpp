#include <fstream>
#include <iostream>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include "ip.h"
#include "../shared/protocol.h"

#define SERVER_IP "127.0.0.1"
#define PORT 4545

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


std::string myName = getName();
std::string myIp = getip();

void sender() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server.sin_addr);

    while (true) {
        lnos::Packet p{myName, myIp};
        std::string msg = lnos::encode(p);

        sendto(sock,
               msg.c_str(),
               msg.size(),
               0,
               reinterpret_cast<sockaddr *>(&server),
               sizeof(server));

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

int main() {
    sender();
}