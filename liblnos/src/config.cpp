#include <fstream>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <lnos/config.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lnos/crypto.h>

namespace lnos {

    std::string getConfigDir() {
        if (geteuid() == 0) {
            return "/etc/lnos";
        }
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg) {
            return std::string(xdg) + "/lnos";
        }
        const char* home = std::getenv("HOME");
        if (home && *home) {
            return std::string(home) + "/.config/lnos";
        }
        return "/etc/lnos";
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
            auto hostname = std::string(host);
            cfg.name = hostname;
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

        return cfg;
    }


    bool setConfig(const std::string& key, const std::string& value)
    {
        createConfig();

        std::string dir = getConfigDir();
        if (key == "name") {

            std::ofstream file(dir + "/name");

            if (file.is_open()) {
                file << value << std::endl;
                return true;
            }

            return false;
        }

        return false;
    }

    std::string getConfig(const std::string& key)
    {
        std::string dir = getConfigDir();
        if (key == "name") {

            std::ifstream file(dir + "/name");

            if (file.is_open()) {
                std::string value;
                file >> value;
                return value;
            }

            return "";
        }

        return "";
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

            if (!nameFile.is_open())
                return false;

            nameFile << "default.node\n";
            std::filesystem::permissions(namePath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read);
        }

        std::string servicesPath = dir + "/services";
        if (!std::filesystem::exists(servicesPath)) {
            std::ofstream servicesFile(servicesPath);

            if (!servicesFile.is_open())
                return false;

            std::filesystem::permissions(servicesPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read);
        }

        return true;
    }

}
