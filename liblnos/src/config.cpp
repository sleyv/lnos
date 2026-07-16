#include <fstream>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <lnos/config.h>
#include <unistd.h>

namespace lnos {

    std::string getConfigDir() {
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg) {
            return std::string(xdg) + "/lnos";
        }
        if (geteuid() == 0) {
            return "/etc/lnos";
        }
        const char* home = std::getenv("HOME");
        if (home && *home) {
            return std::string(home) + "/.config/lnos";
        }
        return "/etc/lnos";
    }

    std::string readFile(const std::string& path, const std::string& fallback) {
        std::ifstream f(path);
        if (!f.is_open()) return fallback;
        std::string line;
        std::getline(f, line);
        if (line.empty()) return fallback;
        auto pos = line.find_last_not_of(" \t\n\r");
        if (pos != std::string::npos) line.erase(pos + 1);
        return line;
    }

    Config loadConfig()
    {
        Config cfg;
        std::string dir = getConfigDir();

        std::ifstream nameFile(dir + "/name");
        if (nameFile.is_open()) {
            nameFile >> cfg.name;
        } else {
            char host[256];
            gethostname(host, sizeof(host));
            cfg.name = std::string(host);
        }

        std::ifstream servicesFile(dir + "/services");
        if (servicesFile.is_open()) {
            std::string line;
            while (std::getline(servicesFile, line)) {
                std::stringstream ss(line);
                Service service;
                ss >> service.name;
                ss >> service.port;
                cfg.services.push_back(service);
            }
        }

        cfg.mcastGroup = readFile(dir + "/mcast_group", "239.255.42.99");
        cfg.mcastGroupV6 = readFile(dir + "/mcast_group_v6", "ff02::4299");

        std::string portStr = readFile(dir + "/port", "4545");
        try {
            int p = std::stoi(portStr);
            if (p > 0 && p <= 65535) cfg.port = static_cast<uint16_t>(p);
        } catch (...) {}

        return cfg;
    }


    bool setConfig(const std::string& key, const std::string& value)
    {
        createConfig();
        std::string dir = getConfigDir();

        if (key == "name") {
            std::ofstream file(dir + "/name");
            if (!file.is_open()) return false;
            file << value << std::endl;
            return true;
        }
        if (key == "mcast_group") {
            std::ofstream file(dir + "/mcast_group");
            if (!file.is_open()) return false;
            file << value << std::endl;
            return true;
        }
        if (key == "mcast_group_v6") {
            std::ofstream file(dir + "/mcast_group_v6");
            if (!file.is_open()) return false;
            file << value << std::endl;
            return true;
        }
        if (key == "domain") {
            std::ofstream file(dir + "/domain");
            if (!file.is_open()) return false;
            file << value << std::endl;
            return true;
        }
        if (key == "port") {
            std::ofstream file(dir + "/port");
            if (!file.is_open()) return false;
            file << value << std::endl;
            return true;
        }
        return false;
    }

    bool createConfig()
    {
        std::string dir = getConfigDir();
        try {
            std::filesystem::create_directories(dir);
            std::filesystem::permissions(dir, std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec | std::filesystem::perms::others_read | std::filesystem::perms::others_exec);
        } catch (...) {
            return false;
        }

        std::string namePath = dir + "/name";
        if (!std::filesystem::exists(namePath)) {
            std::ofstream nameFile(namePath);
            if (!nameFile.is_open()) return false;
            nameFile << "default.node\n";
            std::filesystem::permissions(namePath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read);
        }

        std::string servicesPath = dir + "/services";
        if (!std::filesystem::exists(servicesPath)) {
            std::ofstream servicesFile(servicesPath);
            if (!servicesFile.is_open()) return false;
            std::filesystem::permissions(servicesPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read);
        }

        return true;
    }

}
