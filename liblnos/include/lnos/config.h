#pragma once

#include <string>
#include <vector>

#include "protocol.h"

namespace lnos {

    struct Config {
        std::string name;
        std::vector<Service> services;
        std::string domainSuffix = ".gervaty";
        std::string mcastGroup = "239.255.42.99";
        std::string mcastGroupV6 = "ff02::4299";
        uint16_t port = 4545;
    };

    std::string getConfigDir();

    std::string getDomainSuffix();

    Config loadConfig();

    bool setConfig(const std::string& key, const std::string& value);

    bool createConfig();
}
