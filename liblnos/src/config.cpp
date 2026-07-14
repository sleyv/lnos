#include <fstream>
#include <sstream>
#include <lnos/config.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>

#include <lnos/crypto.h>

namespace lnos {

    Config loadConfig()
    {
        Config cfg;

        std::ifstream nameFile("/etc/lnos/name");

        if (nameFile.is_open()) {
            nameFile >> cfg.name;
        }

        std::ifstream servicesFile("/etc/lnos/services");

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
        if (geteuid() != 0)
            return false;

        createConfig();

        if (key == "name") {

            std::ofstream file("/etc/lnos/name");

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
        if (geteuid() != 0) {
            return "";
        }

        if (key == "name") {

            std::ifstream file("/etc/lnos/name");

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
        if (access("/etc/lnos", F_OK) != 0)
        {
            if (mkdir("/etc/lnos", 0755) != 0)
                return false;
            chmod("/etc/lnos", 0755);
        }

        if (access("/etc/lnos/name", F_OK) != 0)
        {
            std::ofstream nameFile("/etc/lnos/name");

            if (!nameFile.is_open())
                return false;

            nameFile << "default.node\n";
            chmod("/etc/lnos/name", 0644);
        }

        if (access("/etc/lnos/services", F_OK) != 0)
        {
            std::ofstream servicesFile("/etc/lnos/services");

            if (!servicesFile.is_open())
                return false;

            chmod("/etc/lnos/services", 0644);
        }

        return true;
    }

}