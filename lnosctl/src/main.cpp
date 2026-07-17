#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sodium.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <lnos/config.h>
#include <cstring>


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

    if (sodium_mlock(privateKey, sizeof(privateKey)) != 0) {
        std::cerr << "Warning: failed to mlock private key memory\n";
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
    std::cerr << "    stats                 print LNOS daemon statistics\n";
    std::cerr << "    resolve <name>        resolve LNOS node name via running daemon\n";
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
        auto c = lnos::loadConfig();
        std::cout << "Config created at " << lnos::getConfigDir() << "\n";
        std::cout << "  Node name: " << c.name << " (name: device.type.owner)\n";
        std::cout << "  Multicast IPv4: " << c.mcastGroup << " (change with: lnosctl set mcast_group <ip>)\n";
        std::cout << "  Port: " << c.port << " (change with: lnosctl set port <num>)\n";
        std::cout << "Generate keys with: lnosctl generatekeys\n";
        return 0;
    } else if (command == "config") {
        std::cout << "Node Name: " << cfg.name << std::endl;
        std::cout << "Multicast Group (IPv4): " << cfg.mcastGroup << std::endl;
        std::cout << "Multicast Group (IPv6): " << cfg.mcastGroupV6 << std::endl;
        std::cout << "Port: " << cfg.port << std::endl;
        std::cout << "HTTP Port: " << cfg.httpPort << std::endl;
        std::string domain = lnos::readFile(lnos::getConfigDir() + "/domain", "");
        if (!domain.empty()) std::cout << "Domain: " << domain << std::endl;
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
        } else if (key == "mcast_group") {
            std::cout << "Multicast Group (IPv4): " << cfg.mcastGroup << std::endl;
        } else if (key == "mcast_group_v6") {
            std::cout << "Multicast Group (IPv6): " << cfg.mcastGroupV6 << std::endl;
        } else if (key == "port") {
            std::cout << "Port: " << cfg.port << std::endl;
        } else if (key == "http_port") {
            std::cout << "HTTP Port: " << cfg.httpPort << std::endl;
        } else if (key == "domain") {
            std::cout << "Domain: " << lnos::readFile(lnos::getConfigDir() + "/domain", "") << std::endl;
        }
        return 0;
    } else if (command == "stats") {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cerr << "Failed to create UNIX socket\n";
            return 1;
        }
        struct sockaddr_un un{};
        un.sun_family = AF_UNIX;
        std::string socket_path = lnos::getConfigDir() + "/lnosd.sock";
        std::strncpy(un.sun_path, socket_path.c_str(), sizeof(un.sun_path) - 1);
        un.sun_path[sizeof(un.sun_path) - 1] = '\0';

        if (connect(sock, reinterpret_cast<sockaddr*>(&un), sizeof(un)) < 0) {
            std::cerr << "Failed to connect to daemon at " << socket_path << " (is lnosd running?)\n";
            close(sock);
            return 1;
        }

        std::string query = "__STATS__\n";
        if (write(sock, query.data(), query.length()) < 0) {
            std::cerr << "Failed to send request to daemon\n";
            close(sock);
            return 1;
        }

        char buf[1024];
        ssize_t n = read(sock, buf, sizeof(buf) - 1);
        close(sock);

        if (n <= 0) {
            std::cerr << "Failed to read response from daemon\n";
            return 1;
        }

        buf[n] = '\0';
        std::cout << "=== LNOS Daemon Statistics ===\n" << buf;
        return 0;
    } else if (command == "resolve") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " resolve <name>\n";
            return 1;
        }

        std::string name = argv[2];
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cerr << "Failed to create UNIX socket\n";
            return 1;
        }

        struct sockaddr_un un{};
        un.sun_family = AF_UNIX;
        std::string socket_path = lnos::getConfigDir() + "/lnosd.sock";
        std::strncpy(un.sun_path, socket_path.c_str(), sizeof(un.sun_path) - 1);
        un.sun_path[sizeof(un.sun_path) - 1] = '\0';

        if (connect(sock, reinterpret_cast<sockaddr*>(&un), sizeof(un)) < 0) {
            std::cerr << "Failed to connect to daemon at " << socket_path
                      << " (is lnosd running?)\n";
            close(sock);
            return 1;
        }

        std::string query = name + "\n";
        if (write(sock, query.data(), query.length()) < 0) {
            std::cerr << "Failed to send request to daemon\n";
            close(sock);
            return 1;
        }

        char buf[1024];
        ssize_t n = read(sock, buf, sizeof(buf) - 1);
        close(sock);

        if (n <= 0) {
            std::cerr << "Failed to read response from daemon\n";
            return 1;
        }

        buf[n] = '\0';
        std::string resp(buf);
        while (!resp.empty() && (resp.back() == '\n' || resp.back() == '\r'))
            resp.pop_back();

        if (resp == "NOT_FOUND") {
            std::cout << "NOT_FOUND\n";
            return 1;
        }

        std::cout << resp << "\n";
        return 0;
    } else {
        printUsage(argv[0]);
        std::cerr << "Unknown subcommand " << command << std::endl;
    }
}
