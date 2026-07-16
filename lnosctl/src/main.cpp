#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sodium.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <lnos/config.h>


bool writeKey(const std::string& path, const unsigned char* key, std::size_t size)
{
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        std::cerr << "Cannot open " << path << " for writing securely\n";
        return false;
    }

    ssize_t bytes_written = write(fd, key, size);
    if (bytes_written < 0 || static_cast<size_t>(bytes_written) != size) {
        std::cerr << "Failed to write key to " << path << "\n";
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

bool generateKeys() {
    if (sodium_init() < 0) {
        std::cerr << "Failed to initialize libsodium!" << std::endl;
        return false;
    }

    lnos::createConfig();

    unsigned char publicKey[crypto_sign_PUBLICKEYBYTES];
    unsigned char privateKey[crypto_sign_SECRETKEYBYTES];

    crypto_sign_keypair(publicKey, privateKey);

    std::string pubPath = lnos::getConfigDir() + "/public.key";
    std::string privPath = lnos::getConfigDir() + "/private.key";

    if (!writeKey(pubPath, publicKey, crypto_sign_PUBLICKEYBYTES)) {
        std::cerr << "Failed to write public key to " << pubPath << std::endl;
        return false;
    }
    if (!writeKey(privPath, privateKey, crypto_sign_SECRETKEYBYTES)) {
        std::cerr << "Failed to write private key to " << privPath << std::endl;
        return false;
    }
    return true;
}

void printUsage(char *program_name) {
    std::cerr << "Usage: " << program_name << " <command>\n";
    std::cerr << "Available commands:\n";
    std::cerr << "    generatekeys          generate private and public keys for LNOS node\n";
    std::cerr << "    init                  create LNOS config\n";
    std::cerr << "    config                print LNOS config\n";
    std::cerr << "    set                   override LNOS config property\n";
    std::cerr << "    get                   print LNOS config property\n";
}

int main(int argc, char** argv)
{
    auto cfg = lnos::loadConfig();

    if (argc < 2)
    {
        printUsage(argv[0]);
        return 1;
    }

    std::string command = argv[1];
    if (command == "generatekeys") {
        generateKeys();
    } else if (command == "init") {
        lnos::createConfig();
        return 0;
    } else if (command == "config") {
        std::cout << "Node Name: " << cfg.name << std::endl;
        return 0;
    } else if (command == "set") {
        if (argc < 4) {
            std::cerr << "Not enough arguments" << std::endl;
            return 1;
        }

        std::string key = argv[2];
        std::string value = argv[3];

        lnos::setConfig(key, value);

        return 0;
    } else if (command == "get") {
        if (argc < 3) {
            std::cerr << "Not enough arguments" << std::endl;
            return 1;
        }

        std::string key = argv[2];

        if (key == "name") {
            std::cout << "Node Name: " << cfg.name << std::endl;
        }
        return 0;
    } else {
        printUsage(argv[0]);
        std::cerr << "Unknown subcommand " << command << std::endl;
    }
}
