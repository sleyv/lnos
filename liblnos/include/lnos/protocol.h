#pragma once
#include <string>

/*{
  "version": "1",
  "type": "announce",
  "name": "teskum.pc.gervaty",
  "services": [
    {
      "name": "SSH",
      "port": 22
    },
    {
      "name": "HTTP",
      "port": 80
    },
    {
      "name": "Minecraft",
      "port": 25565
    }
  ]
}*/

namespace lnos {

    enum PacketType {
        PacketTypeAnnounce = 0,
    };

    struct Service {
        std::string name;
        uint16_t port;
    };

    struct Packet {
        std::string version;
        PacketType type;
        std::string name;
        std::vector<Service> services;
    };

    using Blob = std::vector<uint8_t>;

    struct EncodedPacket {
        uint8_t *data;
        uint64_t len;
    };

    inline void blobPush(Blob& blob, uint64_t data) {
        uint8_t *ptr = (uint8_t *) &data;
        for (uint64_t i = 0; i < sizeof(data); ++i)
            blob.push_back(ptr[i]);
    }

    inline void blobPush(Blob& blob, uint16_t data) {
        uint8_t *ptr = (uint8_t *) &data;
        for (uint64_t i = 0; i < sizeof(data); ++i)
            blob.push_back(ptr[i]);
    }

    inline void blobPush(Blob& blob, const std::string& data) {
        uint64_t len = data.length();
        blobPush(blob, len);
        for (uint64_t i = 0; i < len; ++i)
            blob.push_back((uint8_t) data.data()[i]);
    }

    inline bool encodedPacketConsumeImpl(EncodedPacket& packet, uint64_t& data) {
        if (packet.len < sizeof(uint64_t))
            return false;
        data = *(uint64_t *) packet.data;
        packet.data += sizeof(uint64_t);
        packet.len -= sizeof(uint64_t);
        return true;
    }

    inline bool encodedPacketConsumeImpl(EncodedPacket& packet, uint16_t& data) {
        if (packet.len < sizeof(uint16_t))
            return false;
        data = *(uint16_t *) packet.data;
        packet.data += sizeof(uint16_t);
        packet.len -= sizeof(uint16_t);
        return true;
    }

    inline bool encodedPacketConsumeImpl(EncodedPacket& packet, std::string& data) {
        uint64_t len = 0;
        if (!encodedPacketConsumeImpl(packet, len))
            return false;
        if (packet.len < len)
            return false;
        data = std::string((char *) packet.data, len);
        packet.data += len;
        packet.len -= len;
        return true;
    }

    inline Blob encode(const Packet& p) {
        Blob blob;

        blobPush(blob, p.version);
        blobPush(blob, (uint16_t) p.type);
        blobPush(blob, p.name);

        uint64_t len = p.services.size();
        blobPush(blob, len);
        for (const auto& s : p.services)
        {
            blobPush(blob, s.name);
            blobPush(blob, s.port);
        }

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
        encodedPacketConsume(packet, result.name);

        uint64_t len = 0;
        encodedPacketConsume(packet, len);
        result.services.reserve(len);

        for (uint64_t i = 0; i < len; ++i)
        {
            Service service;
            encodedPacketConsume(packet, service.name);
            encodedPacketConsume(packet, service.port);
            result.services.push_back(service);
        }

        return true;
    }

#undef encodedPacketConsume

}
