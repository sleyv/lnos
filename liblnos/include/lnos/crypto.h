#pragma once

#include <array>
#include <cstdint>

#include "protocol.h"

namespace lnos {

    std::array<uint8_t, PUBLIC_KEY_SIZE> loadPublicKey();

    std::array<uint8_t, PRIVATE_KEY_SIZE> loadPrivateKey();

    bool signPacket(
        Packet& packet,
        const std::array<uint8_t, PRIVATE_KEY_SIZE>& privateKey
    );

    bool verifyPacket(const Packet& packet);

    bool encryptPacketPayload(
        Packet& packet,
        const std::array<uint8_t, PRIVATE_KEY_SIZE>& myPrivateKey,
        const std::array<uint8_t, PUBLIC_KEY_SIZE>& recipientPublicKey,
        bool isMulticast
    );

    bool decryptPacketPayload(
        Packet& packet,
        const std::array<uint8_t, PRIVATE_KEY_SIZE>& myPrivateKey,
        const std::array<uint8_t, PUBLIC_KEY_SIZE>& senderPublicKey,
        bool isMulticast
    );

}