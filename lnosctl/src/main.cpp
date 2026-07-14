#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sodium.h>
#include <lnos/config.h>

bool generateKeys() {
    if (sodium_init() < 0) {
        std::cout << "Failed to initialize libsodium!\n";
    } if (geteuid() != 0) return false;

    unsigned char publicKey[crypto_sign_PUBLICKEYBYTES];
    unsigned char privateKey[crypto_sign_SECRETKEYBYTES];

    crypto_sign_keypair(publicKey, privateKey);

    std::ofstream("/etc/lnos/public.key", std::ios::binary)
        .write(reinterpret_cast<char*>(publicKey), crypto_sign_PUBLICKEYBYTES);

    std::ofstream("/etc/lnos/private.key", std::ios::binary)
        .write(reinterpret_cast<char*>(privateKey), crypto_sign_SECRETKEYBYTES);
    return true;
}

int main(int argc, char** argv)
{
    auto cfg = lnos::loadConfig();

    if (argc < 2)
    {
        std::cout << "Usage: lnosctl <command>\n";
        return 1;
    }

    if (argv[1] == "generatekeys") {
        if (geteuid() == 0) generateKeys();
    }
    std::string command = argv[1];

    if (command == "init") {
        if (geteuid() != 0) return 1;

        lnos::createConfig();
        return 0;
    } else if (command == "config") {
        std::cout << "Node Name: " << cfg.name << std::endl;

        return 0;
    } else if (command == "set") {
        if (geteuid() != 0) return 1;
        if (argv[2] == nullptr || argv[3] == nullptr) return 1;

        std::string key = argv[2];
        std::string value = argv[3];

        lnos::setConfig(key, value);

        return 0;
    } else if (command == "get") {
        if (argv[2] == nullptr) return 1;

        std::string key = argv[2];

        if (key == "name") {
            std::cout << "Node Name: " << cfg.name << std::endl;
        }
        return 0;
    }
}
