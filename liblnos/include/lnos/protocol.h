#pragma once
#include <array>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <variant>
#include <endian.h>

constexpr std::size_t PUBLIC_KEY_SIZE = 32;
constexpr std::size_t PRIVATE_KEY_SIZE = 64;
constexpr std::size_t SIGNATURE_SIZE = 64;
constexpr int PROTOCOL_VERSION = 3;

/*
+----------------+
| version        |
+----------------+
| type           |
+----------------+
| public key     |  <-- who
+----------------+
| name           |
+----------------+
| services       |
+----------------+
| signature      |  <-- proof (r.i.p)
+----------------+ */

namespace lnos {

    enum class PacketType {
        Announce = 0,
        Query = 1,
        Response = 2
    };

    struct Service {
        std::string name;
        uint16_t port;
    };

    struct PacketAnnounce {
        std::string name;
        std::vector<Service> services;

        PacketAnnounce(std::string name, std::vector<Service> services)
            : name(name), services(services)
        {}

        ~PacketAnnounce()
        {}
    };

    struct Packet {
        std::string version;
        PacketType type;
        PacketAnnounce announce;
        std::array<std::uint8_t, PUBLIC_KEY_SIZE> publicKey;
        std::array<std::uint8_t, SIGNATURE_SIZE> signature;

        Packet()
            : version(std::to_string(PROTOCOL_VERSION)),
              type(PacketType::Announce),
              announce({}, {})
        {}

        Packet(std::string name, std::vector<Service> services)
            : version(std::to_string(PROTOCOL_VERSION)),
              type(PacketType::Announce),
              announce(name, services)
        {}
    };

    using Blob = std::vector<uint8_t>;

    struct EncodedPacket {
        uint8_t *data;
        uint64_t len;
    };

    inline void blobPush(Blob& blob, uint64_t data) {
        uint64_t be_data = htobe64(data);
        const uint8_t *ptr = reinterpret_cast<const uint8_t*>(&be_data);
        for (uint64_t i = 0; i < sizeof(be_data); ++i)
            blob.push_back(ptr[i]);
    }

    inline void blobPush(Blob& blob, uint16_t data) {
        uint16_t be_data = htobe16(data);
        const uint8_t *ptr = reinterpret_cast<const uint8_t*>(&be_data);
        for (uint64_t i = 0; i < sizeof(be_data); ++i)
            blob.push_back(ptr[i]);
    }

    inline void blobPush(Blob& blob, const std::string& data) {
        uint64_t len = data.length();
        blobPush(blob, len);
        for (uint64_t i = 0; i < len; ++i)
            blob.push_back((uint8_t) data.data()[i]);
    }

    template <std::size_t N>
    inline void blobPush(Blob& blob, const std::array<uint8_t, N>& data) {
        for (uint8_t b : data)
            blob.push_back(b);
    }

    inline bool encodedPacketConsumeImpl(EncodedPacket& packet, uint64_t& data) {
        if (packet.len < sizeof(uint64_t))
            return false;
        uint64_t raw;
        std::memcpy(&raw, packet.data, sizeof(raw));
        data = be64toh(raw);
        packet.data += sizeof(uint64_t);
        packet.len -= sizeof(uint64_t);
        return true;
    }

    inline bool encodedPacketConsumeImpl(EncodedPacket& packet, uint16_t& data) {
        if (packet.len < sizeof(uint16_t))
            return false;
        uint16_t raw;
        std::memcpy(&raw, packet.data, sizeof(raw));
        data = be16toh(raw);
        packet.data += sizeof(uint16_t);
        packet.len -= sizeof(uint16_t);
        return true;
    }

    inline bool encodedPacketConsumeImpl(EncodedPacket& packet, std::string& data) {
        uint64_t len = 0;
        if (!encodedPacketConsumeImpl(packet, len))
            return false;
        // Limit maximum string length to prevent OOM/large-alloc DoS (H-4 protection)
        if (len > 1024)
            return false;
        if (packet.len < len)
            return false;
        data = std::string((char *) packet.data, len);
        packet.data += len;
        packet.len -= len;
        return true;
    }

    template <std::size_t N>
    inline bool encodedPacketConsumeImpl(EncodedPacket& packet,
                            std::array<uint8_t, N>& data) {
        if (packet.len < N)
            return false;

        std::memcpy(data.data(), packet.data, N);

        packet.data += N;
        packet.len -= N;

        return true;
    }

    inline Blob encode(const Packet& p, bool includeSignature) {
        Blob blob;

        blobPush(blob, p.version);
        blobPush(blob, (uint16_t) p.type);

        switch (p.type) {
        case PacketType::Announce:
        case PacketType::Query:
        case PacketType::Response: {
          const auto& announce = p.announce;
          blobPush(blob, announce.name);

          uint64_t len = announce.services.size();
          blobPush(blob, len);

          for (const auto& s : announce.services)
          {
            blobPush(blob, s.name);
            blobPush(blob, s.port);
          }
        } break;
        }

        blobPush(blob, p.publicKey);
        if (includeSignature)
          blobPush(blob, p.signature);

        return blob;
    }

#define encodedPacketConsume(blob, data)       \
  do {                                         \
    if (!encodedPacketConsumeImpl(blob, data)) \
      return false;                            \
  } while (0)

    inline bool decode(EncodedPacket& packet, Packet& result) {
        encodedPacketConsume(packet, result.version);
        uint16_t type = 0;
        encodedPacketConsume(packet, type);
        result.type = (PacketType) type;

        switch (result.type) {
        case PacketType::Announce:
        case PacketType::Query:
        case PacketType::Response: {
          auto& announce = result.announce;
          encodedPacketConsume(packet, announce.name);

          uint64_t len = 0;
          encodedPacketConsume(packet, len);
          // Limit maximum number of services to prevent OOM reserve DoS (H-3 protection)
          if (len > 256)
              return false;
          announce.services.clear();
          announce.services.reserve(len);

          for (uint64_t i = 0; i < len; ++i)
          {
            Service service;
            encodedPacketConsume(packet, service.name);
            encodedPacketConsume(packet, service.port);
            announce.services.push_back(service);
          }
        } break;
        default:
          return false;
        }

        encodedPacketConsume(packet, result.publicKey);
        encodedPacketConsume(packet, result.signature);

        return true;
    }

#undef encodedPacketConsume

}
