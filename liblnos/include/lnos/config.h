#pragma once

#include <string>
#include <vector>

#include "protocol.h"

namespace lnos {

    struct Config {
        std::string name;
        std::vector<Service> services;
        std::string mcastGroup = "239.255.42.99";
        std::string mcastGroupV6 = "ff02::4299";
        uint16_t port = 4545;
        uint16_t httpPort = 9999;
    };

    std::string getConfigDir();

    Config loadConfig();

    bool setConfig(const std::string& key, const std::string& value);

    bool createConfig();

    std::string readFile(const std::string& path, const std::string& fallback);
}
