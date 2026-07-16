#include <sodium.h>
#include <fstream>
#include <stdexcept>

#include <lnos/crypto.h>
#include <lnos/config.h>


namespace lnos {


    bool signPacket(
        Packet& packet,
        const std::array<uint8_t, PRIVATE_KEY_SIZE>& privateKey)
    {
        Blob data = encode(packet, false);

        unsigned long long len;

        crypto_sign_detached(
            packet.signature.data(),
            &len,
            data.data(),
            data.size(),
            privateKey.data()
        );

        return len == SIGNATURE_SIZE;
    }

    bool verifyPacket(const Packet& packet) {
        Blob data = encode(packet, false);

        return crypto_sign_verify_detached(
            packet.signature.data(),
            data.data(),
            data.size(),
            packet.publicKey.data()
        ) == 0;
    }

    std::array<std::uint8_t, PUBLIC_KEY_SIZE> loadPublicKey() {
        std::array<std::uint8_t, PUBLIC_KEY_SIZE> publicKey{};

        std::string path = getConfigDir() + "/public.key";
        std::ifstream publicKeyFile(path, std::ios::binary);
        if (!publicKeyFile.is_open())
            throw std::runtime_error("Failed to open " + path);

        publicKeyFile.read(reinterpret_cast<char*>(publicKey.data()),
                           publicKey.size());

        if (publicKeyFile.gcount() != static_cast<std::streamsize>(publicKey.size()))
            throw std::runtime_error("Invalid public key file: " + path);

        return publicKey;
    }

    std::array<std::uint8_t, PRIVATE_KEY_SIZE> loadPrivateKey() {
        std::array<std::uint8_t, PRIVATE_KEY_SIZE> privateKey{};

        std::string path = getConfigDir() + "/private.key";
        std::ifstream file(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("Failed to open " + path);

        file.read(reinterpret_cast<char*>(privateKey.data()), privateKey.size());

        if (file.gcount() != static_cast<std::streamsize>(privateKey.size()))
            throw std::runtime_error("Invalid private.key: " + path);

        return privateKey;
    }


}
