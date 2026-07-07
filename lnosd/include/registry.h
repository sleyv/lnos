#pragma once

#include <string>
#include <unordered_map>
#include <chrono>

enum class NodeStatus {
    Online,
    Offline
};

struct Node {
    std::string name;
    std::string ip;

    std::chrono::steady_clock::time_point lastSeen;
    NodeStatus status;
};

extern std::unordered_map<std::string, Node> nodes;