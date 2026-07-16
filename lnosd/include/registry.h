#pragma once

#include <string>
#include <unordered_map>
#include <chrono>

#include "lnos/protocol.h"

enum class NodeStatus {
    Online,
    Offline
};

struct Node {
    std::string name;
    std::string ip;

    std::vector<lnos::Service> services;

    std::chrono::steady_clock::time_point lastSeen;
    NodeStatus status;
    std::array<std::uint8_t, PUBLIC_KEY_SIZE> publicKey;
};

extern std::unordered_map<std::string, Node> nodes;