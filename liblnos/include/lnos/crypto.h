#pragma once

#include <array>
#include <cstdint>

#include "protocol.h"

namespace lnos {

    bool generateKeys();

    std::array<uint8_t, PUBLIC_KEY_SIZE> loadPublicKey();

    std::array<uint8_t, PRIVATE_KEY_SIZE> loadPrivateKey();

    bool signPacket(
        Packet& packet,
        const std::array<uint8_t, PRIVATE_KEY_SIZE>& privateKey
    );

    bool verifyPacket(const Packet& packet);

}