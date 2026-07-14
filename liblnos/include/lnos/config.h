#pragma once

#include <string>
#include <vector>

#include "protocol.h"

namespace lnos {

    struct Config {
        std::string name;
        std::vector<Service> services;
    };

    Config loadConfig();

    bool setConfig(const std::string& key, const std::string& value);

    bool createConfig();
}
