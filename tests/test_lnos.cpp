#include <gtest/gtest.h>
#include <lnos/protocol.h>
#include <lnos/crypto.h>
#include <lnos/config.h>
#include <sodium.h>
#include <cstdlib>
#include <filesystem>

TEST(LnosProtocolTest, EncodeDecodeAnnouncePacket) {
    std::vector<lnos::Service> services = {
        {"ssh", 22},
        {"http", 80}
    };
    lnos::Packet original("my.test.node", services);
    original.publicKey.fill(0xAA);
    original.signature.fill(0xBB);

    lnos::Blob blob = lnos::encode(original, true);

    lnos::EncodedPacket encoded{blob.data(), blob.size()};
    lnos::Packet decoded;

    ASSERT_TRUE(lnos::decode(encoded, decoded));
    EXPECT_EQ(decoded.version, original.version);
    EXPECT_EQ(decoded.announce.name, "my.test.node");
    ASSERT_EQ(decoded.announce.services.size(), 2);
    EXPECT_EQ(decoded.announce.services[0].name, "ssh");
    EXPECT_EQ(decoded.announce.services[0].port, 22);
    EXPECT_EQ(decoded.announce.services[1].name, "http");
    EXPECT_EQ(decoded.announce.services[1].port, 80);
    EXPECT_EQ(decoded.publicKey, original.publicKey);
    EXPECT_EQ(decoded.signature, original.signature);
}

TEST(LnosProtocolTest, QueryPacketEncodeDecode) {
    lnos::Packet query("target.device.gervaty", {});
    query.type = lnos::PacketType::Query;

    lnos::Blob blob = lnos::encode(query, true);

    lnos::EncodedPacket encoded{blob.data(), blob.size()};
    lnos::Packet decoded;

    ASSERT_TRUE(lnos::decode(encoded, decoded));
    EXPECT_EQ(decoded.type, lnos::PacketType::Query);
    EXPECT_EQ(decoded.announce.name, "target.device.gervaty");
    EXPECT_TRUE(decoded.announce.services.empty());
}

TEST(LnosProtocolTest, ResponsePacketEncodeDecode) {
    lnos::Packet response("target.device.gervaty", {{"ssh", 22}});
    response.type = lnos::PacketType::Response;

    lnos::Blob blob = lnos::encode(response, true);

    lnos::EncodedPacket encoded{blob.data(), blob.size()};
    lnos::Packet decoded;

    ASSERT_TRUE(lnos::decode(encoded, decoded));
    EXPECT_EQ(decoded.type, lnos::PacketType::Response);
    EXPECT_EQ(decoded.announce.name, "target.device.gervaty");
    ASSERT_EQ(decoded.announce.services.size(), 1);
    EXPECT_EQ(decoded.announce.services[0].name, "ssh");
    EXPECT_EQ(decoded.announce.services[0].port, 22);
}

TEST(LnosProtocolTest, DecodeInvalidPacketTypeReturnsFalse) {
    lnos::Blob blob = { 0, 0, 0, 0, 99, 99 }; // Unknown type 9999
    lnos::EncodedPacket encoded{blob.data(), blob.size()};
    lnos::Packet decoded;
    ASSERT_FALSE(lnos::decode(encoded, decoded));
}

TEST(LnosProtocolTest, EmptyNamePacket) {
    lnos::Packet original("", {});
    lnos::Blob blob = lnos::encode(original, true);

    lnos::EncodedPacket encoded{blob.data(), blob.size()};
    lnos::Packet decoded;

    ASSERT_TRUE(lnos::decode(encoded, decoded));
    EXPECT_EQ(decoded.announce.name, "");
    EXPECT_TRUE(decoded.announce.services.empty());
}

TEST(LnosProtocolTest, DuplicateServicePorts) {
    std::vector<lnos::Service> services = {
        {"web1", 80},
        {"web2", 80}
    };
    lnos::Packet original("dup.ports.node", services);
    lnos::Blob blob = lnos::encode(original, true);

    lnos::EncodedPacket encoded{blob.data(), blob.size()};
    lnos::Packet decoded;

    ASSERT_TRUE(lnos::decode(encoded, decoded));
    ASSERT_EQ(decoded.announce.services.size(), 2);
    EXPECT_EQ(decoded.announce.services[0].name, "web1");
    EXPECT_EQ(decoded.announce.services[0].port, 80);
    EXPECT_EQ(decoded.announce.services[1].name, "web2");
    EXPECT_EQ(decoded.announce.services[1].port, 80);
}

TEST(LnosProtocolTest, NullOrTinyBufferDecode) {
    lnos::Blob tinyBlob = { 1, 2 };
    lnos::EncodedPacket encoded{tinyBlob.data(), tinyBlob.size()};
    lnos::Packet decoded;
    EXPECT_FALSE(lnos::decode(encoded, decoded));
}

TEST(LnosProtocolTest, ExceedServiceLimit) {
    // Generate a payload with service count set to 300 (exceeding 256 limit)
    lnos::Packet p("test", {});
    lnos::Blob blob = lnos::encode(p, true);

    // Overwrite service count in the raw encoded blob (which is placed after version and type and name)
    // For simplicity, we can craft an EncodedPacket with an artificially large service count:
    // format: [version (std::string)] [type (uint16_t)] [name (std::string)] [services len (uint64_t)]
    // Let's create a custom blob:
    lnos::Blob custom;
    lnos::blobPush(custom, std::string("3")); // version
    lnos::blobPush(custom, (uint16_t)0); // type
    lnos::blobPush(custom, std::string("node")); // name
    lnos::blobPush(custom, (uint64_t)300); // 300 services (limit is 256)
    
    // Add public key and signature
    std::array<uint8_t, PUBLIC_KEY_SIZE> pub{};
    std::array<uint8_t, SIGNATURE_SIZE> sig{};
    lnos::blobPush(custom, pub);
    lnos::blobPush(custom, sig);

    lnos::EncodedPacket encoded{custom.data(), custom.size()};
    lnos::Packet decoded;
    EXPECT_FALSE(lnos::decode(encoded, decoded)); // Must be rejected by service limit check
}

TEST(LnosProtocolTest, ExceedStringLengthLimit) {
    lnos::Blob custom;
    lnos::blobPush(custom, std::string("3")); // version
    lnos::blobPush(custom, (uint16_t)0); // type
    lnos::blobPush(custom, (uint64_t)2000); // string length of 2000 (limit is 1024)

    lnos::EncodedPacket encoded{custom.data(), custom.size()};
    lnos::Packet decoded;
    EXPECT_FALSE(lnos::decode(encoded, decoded)); // Must be rejected by string limit check
}

TEST(LnosCryptoTest, SignAndVerifyPacket) {
    ASSERT_GE(sodium_init(), 0);

    unsigned char pub[crypto_sign_PUBLICKEYBYTES];
    unsigned char priv[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pub, priv);

    std::array<uint8_t, PUBLIC_KEY_SIZE> publicKey;
    std::memcpy(publicKey.data(), pub, PUBLIC_KEY_SIZE);
    std::array<uint8_t, PRIVATE_KEY_SIZE> privateKey;
    std::memcpy(privateKey.data(), priv, PRIVATE_KEY_SIZE);

    lnos::Packet packet("secure.node", {{"dns", 53}});
    packet.publicKey = publicKey;

    ASSERT_TRUE(lnos::signPacket(packet, privateKey));

    // Valid packet signature verification
    EXPECT_TRUE(lnos::verifyPacket(packet));

    // Tampering with name must fail verification
    lnos::Packet tampered = packet;
    tampered.announce.name = "unsecure.node";
    EXPECT_FALSE(lnos::verifyPacket(tampered));

    // Tampering with signature must fail verification
    lnos::Packet tamperedSig = packet;
    tamperedSig.signature[0] ^= 0xFF;
    EXPECT_FALSE(lnos::verifyPacket(tamperedSig));
}

TEST(LnosConfigTest, ConfigDirResolution) {
    std::string dir = lnos::getConfigDir();
    EXPECT_FALSE(dir.empty());
}

TEST(LnosConfigTest, CustomXdgEnvResolution) {
    const char* prev_xdg = std::getenv("XDG_CONFIG_HOME");
    std::string prev_xdg_str = prev_xdg ? prev_xdg : "";

    setenv("XDG_CONFIG_HOME", "/tmp/xdg_test", 1);
    std::string dir = lnos::getConfigDir();
    EXPECT_EQ(dir, "/tmp/xdg_test/lnos");

    if (prev_xdg) {
        setenv("XDG_CONFIG_HOME", prev_xdg_str.c_str(), 1);
    } else {
        unsetenv("XDG_CONFIG_HOME");
    }
}

TEST(LnosConfigTest, SetAndLoadConfig) {
    const char* prev_xdg = std::getenv("XDG_CONFIG_HOME");
    std::string prev = prev_xdg ? prev_xdg : "";

    std::string tmp = "/tmp/lnos_test_config_" + std::to_string(getpid());
    setenv("XDG_CONFIG_HOME", tmp.c_str(), 1);

    lnos::createConfig();
    EXPECT_TRUE(lnos::setConfig("name", "test.node.unit"));
    EXPECT_TRUE(lnos::setConfig("domain", ".test"));
    EXPECT_TRUE(lnos::setConfig("mcast_group", "224.1.1.1"));
    EXPECT_TRUE(lnos::setConfig("mcast_group_v6", "ff02::1"));
    EXPECT_TRUE(lnos::setConfig("port", "9999"));

    lnos::Config cfg = lnos::loadConfig();
    EXPECT_EQ(cfg.name, "test.node.unit");
    EXPECT_EQ(cfg.mcastGroup, "224.1.1.1");
    EXPECT_EQ(cfg.mcastGroupV6, "ff02::1");
    EXPECT_EQ(cfg.port, 9999);

    EXPECT_EQ(lnos::readFile(tmp + "/lnos/domain", ""), ".test");

    std::filesystem::remove_all(tmp);
    if (prev.empty()) {
        unsetenv("XDG_CONFIG_HOME");
    } else {
        setenv("XDG_CONFIG_HOME", prev.c_str(), 1);
    }
}

TEST(LnosConfigTest, SetConfigInvalidKey) {
    EXPECT_FALSE(lnos::setConfig("nonexistent", "value"));
}

TEST(LnosConfigTest, ReadFileFallback) {
    EXPECT_EQ(lnos::readFile("/nonexistent/path", "default"), "default");
    EXPECT_EQ(lnos::readFile("/nonexistent/path", ""), "");
}

TEST(LnosProtocolTest, BigEndianEncode) {
    std::vector<lnos::Service> svc = {{"test", 1234}};
    lnos::Packet p("node", svc);
    p.publicKey.fill(0);
    p.signature.fill(0);

    lnos::Blob blob = lnos::encode(p, true);

    // Service port 1234 = 0x04D2, should be big endian: 0x04, 0xD2
    // Layout: version(9) + type(2) + isEncrypted(1) + name(12) + svc_count(8) + svc_name(12) + port(2) + pubkey(32) + sig(64)
    ASSERT_EQ(blob.size(), 142);
    EXPECT_EQ(blob[blob.size() - SIGNATURE_SIZE - PUBLIC_KEY_SIZE - 2], 0x04) << "port high byte";
    EXPECT_EQ(blob[blob.size() - SIGNATURE_SIZE - PUBLIC_KEY_SIZE - 1], 0xD2) << "port low byte";
}

TEST(LnosProtocolTest, EncodeDecodeMaxServices) {
    std::vector<lnos::Service> services;
    for (int i = 0; i < 256; ++i) {
        services.push_back({"s" + std::to_string(i), static_cast<uint16_t>(i)});
    }
    lnos::Packet original("max.services.node", services);
    original.publicKey.fill(0x42);
    original.signature.fill(0x24);

    lnos::Blob blob = lnos::encode(original, true);
    lnos::EncodedPacket encoded{blob.data(), blob.size()};
    lnos::Packet decoded;
    ASSERT_TRUE(lnos::decode(encoded, decoded));
    EXPECT_EQ(decoded.announce.services.size(), 256);
    EXPECT_EQ(decoded.announce.services[0].name, "s0");
    EXPECT_EQ(decoded.announce.services[0].port, 0);
    EXPECT_EQ(decoded.announce.services[255].name, "s255");
    EXPECT_EQ(decoded.announce.services[255].port, 255);
    EXPECT_EQ(decoded.publicKey, original.publicKey);
}

TEST(LnosProtocolTest, EncodeDecodeMaxNameLength) {
    std::string longName(1024, 'x');
    lnos::Packet original(longName, {});
    original.publicKey.fill(0);
    original.signature.fill(0);

    lnos::Blob blob = lnos::encode(original, true);
    lnos::EncodedPacket encoded{blob.data(), blob.size()};
    lnos::Packet decoded;
    ASSERT_TRUE(lnos::decode(encoded, decoded));
    EXPECT_EQ(decoded.announce.name, longName);
}

TEST(LnosProtocolTest, ExceedNameLengthLimit) {
    // Craft packet with name length 1025 (limit is 1024)
    lnos::Blob custom;
    lnos::blobPush(custom, std::string("3")); // version
    lnos::blobPush(custom, (uint16_t)0); // type
    lnos::blobPush(custom, (uint64_t)1025); // name length 1025
    // Skip actually writing 1025 bytes — decode should fail at length check

    lnos::EncodedPacket encoded{custom.data(), custom.size()};
    lnos::Packet decoded;
    EXPECT_FALSE(lnos::decode(encoded, decoded));
}

TEST(LnosProtocolTest, TamperServicesFailsVerify) {
    unsigned char pub[crypto_sign_PUBLICKEYBYTES];
    unsigned char priv[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pub, priv);

    std::array<uint8_t, PUBLIC_KEY_SIZE> publicKey;
    std::memcpy(publicKey.data(), pub, PUBLIC_KEY_SIZE);
    std::array<uint8_t, PRIVATE_KEY_SIZE> privateKey;
    std::memcpy(privateKey.data(), priv, PRIVATE_KEY_SIZE);

    lnos::Packet packet("tamper.service.node", {{"ssh", 22}});
    packet.publicKey = publicKey;
    ASSERT_TRUE(lnos::signPacket(packet, privateKey));

    // Tamper with services
    lnos::Packet tampered = packet;
    tampered.announce.services[0].port = 2222;
    EXPECT_FALSE(lnos::verifyPacket(tampered));
}

TEST(LnosProtocolTest, TamperPublicKeyFailsVerify) {
    unsigned char pub[crypto_sign_PUBLICKEYBYTES];
    unsigned char priv[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pub, priv);

    std::array<uint8_t, PUBLIC_KEY_SIZE> publicKey;
    std::memcpy(publicKey.data(), pub, PUBLIC_KEY_SIZE);
    std::array<uint8_t, PRIVATE_KEY_SIZE> privateKey;
    std::memcpy(privateKey.data(), priv, PRIVATE_KEY_SIZE);

    lnos::Packet packet("tamper.pubkey.node", {});
    packet.publicKey = publicKey;
    ASSERT_TRUE(lnos::signPacket(packet, privateKey));

    // Tamper with public key
    lnos::Packet tampered = packet;
    tampered.publicKey[0] ^= 0xFF;
    EXPECT_FALSE(lnos::verifyPacket(tampered));
}

TEST(LnosProtocolTest, EncodeDecodeManyServices) {
    std::vector<lnos::Service> services;
    for (int i = 0; i < 100; ++i) {
        services.push_back({"service." + std::to_string(i), static_cast<uint16_t>(i * 100)});
    }
    lnos::Packet original("multi.service.node", services);
    original.publicKey.fill(0x11);
    original.signature.fill(0x22);

    lnos::Blob blob = lnos::encode(original, true);
    lnos::EncodedPacket encoded{blob.data(), blob.size()};
    lnos::Packet decoded;
    ASSERT_TRUE(lnos::decode(encoded, decoded));
    ASSERT_EQ(decoded.announce.services.size(), 100);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(decoded.announce.services[i].name, "service." + std::to_string(i));
        EXPECT_EQ(decoded.announce.services[i].port, i * 100);
    }
}

TEST(LnosProtocolTest, EncodeDecodeWithoutSignature) {
    lnos::Packet p("nosig.node", {{"http", 80}});
    p.publicKey.fill(0x33);
    p.signature.fill(0xAA);

    // Encode without signature — should be smaller than with signature
    lnos::Blob blobNoSig = lnos::encode(p, false);
    lnos::Blob blobWithSig = lnos::encode(p, true);
    EXPECT_EQ(blobWithSig.size(), blobNoSig.size() + SIGNATURE_SIZE);

    // Decode expects signature always present — must fail without it
    lnos::EncodedPacket encoded{blobNoSig.data(), blobNoSig.size()};
    lnos::Packet decoded;
    EXPECT_FALSE(lnos::decode(encoded, decoded));

    // With signature, decode succeeds
    lnos::EncodedPacket encoded2{blobWithSig.data(), blobWithSig.size()};
    EXPECT_TRUE(lnos::decode(encoded2, decoded));
    EXPECT_EQ(decoded.announce.name, "nosig.node");
}

TEST(LnosProtocolTest, ZeroLengthBuffer) {
    lnos::Blob empty;
    lnos::EncodedPacket encoded{empty.data(), empty.size()};
    lnos::Packet decoded;
    EXPECT_FALSE(lnos::decode(encoded, decoded));
}

TEST(LnosProtocolTest, BlobPushUint64Roundtrip) {
    lnos::Blob blob;
    lnos::blobPush(blob, (uint64_t)0x1234567890ABCDEFULL);
    ASSERT_EQ(blob.size(), 8);
    // Big endian: first byte should be 0x12
    EXPECT_EQ(blob[0], 0x12);
    EXPECT_EQ(blob[7], 0xEF);
}

TEST(LnosProtocolTest, BlobPushUint16Roundtrip) {
    lnos::Blob blob;
    lnos::blobPush(blob, (uint16_t)0xABCD);
    ASSERT_EQ(blob.size(), 2);
    EXPECT_EQ(blob[0], 0xAB);
    EXPECT_EQ(blob[1], 0xCD);
}

TEST(LnosProtocolTest, ServiceWithSpecialChars) {
    std::vector<lnos::Service> services = {
        {"service-name_with.underscores", 8080},
        {"https", 443}
    };
    lnos::Packet original("special.chars.node", services);
    original.publicKey.fill(0);
    original.signature.fill(0);

    lnos::Blob blob = lnos::encode(original, true);
    lnos::EncodedPacket encoded{blob.data(), blob.size()};
    lnos::Packet decoded;
    ASSERT_TRUE(lnos::decode(encoded, decoded));
    EXPECT_EQ(decoded.announce.services[0].name, "service-name_with.underscores");
    EXPECT_EQ(decoded.announce.services[0].port, 8080);
    EXPECT_EQ(decoded.announce.services[1].name, "https");
    EXPECT_EQ(decoded.announce.services[1].port, 443);
}

TEST(LnosProtocolTest, GossipSerialization) {
    lnos::Packet original;
    original.type = lnos::PacketType::GossipRequest;

    lnos::GossipNode node1;
    node1.name = "gossip.node.one";
    node1.ip = "192.168.1.10";
    node1.services = {{"ssh", 22}, {"web", 80}};
    node1.publicKey.fill(0xAA);

    lnos::GossipNode node2;
    node2.name = "gossip.node.two";
    node2.ip = "ff02::1";
    node2.services = {{"custom", 9999}};
    node2.publicKey.fill(0xBB);

    original.gossipNodes.push_back(node1);
    original.gossipNodes.push_back(node2);
    original.publicKey.fill(0x12);
    original.signature.fill(0x34);

    lnos::Blob blob = lnos::encode(original, true);

    lnos::EncodedPacket encoded{blob.data(), blob.size()};
    lnos::Packet decoded;

    ASSERT_TRUE(lnos::decode(encoded, decoded));
    EXPECT_EQ(decoded.type, lnos::PacketType::GossipRequest);
    ASSERT_EQ(decoded.gossipNodes.size(), 2);

    EXPECT_EQ(decoded.gossipNodes[0].name, "gossip.node.one");
    EXPECT_EQ(decoded.gossipNodes[0].ip, "192.168.1.10");
    ASSERT_EQ(decoded.gossipNodes[0].services.size(), 2);
    EXPECT_EQ(decoded.gossipNodes[0].services[0].name, "ssh");
    EXPECT_EQ(decoded.gossipNodes[0].services[0].port, 22);
    EXPECT_EQ(decoded.gossipNodes[0].publicKey, node1.publicKey);

    EXPECT_EQ(decoded.gossipNodes[1].name, "gossip.node.two");
    EXPECT_EQ(decoded.gossipNodes[1].ip, "ff02::1");
    ASSERT_EQ(decoded.gossipNodes[1].services.size(), 1);
    EXPECT_EQ(decoded.gossipNodes[1].services[0].name, "custom");
    EXPECT_EQ(decoded.gossipNodes[1].services[0].port, 9999);
    EXPECT_EQ(decoded.gossipNodes[1].publicKey, node2.publicKey);
}

TEST(LnosCryptoTest, PayloadEncryption) {
    ASSERT_GE(sodium_init(), 0);

    unsigned char pub1[crypto_sign_PUBLICKEYBYTES];
    unsigned char priv1[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pub1, priv1);

    unsigned char pub2[crypto_sign_PUBLICKEYBYTES];
    unsigned char priv2[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pub2, priv2);

    std::array<uint8_t, PUBLIC_KEY_SIZE> publicKey1, publicKey2;
    std::memcpy(publicKey1.data(), pub1, PUBLIC_KEY_SIZE);
    std::memcpy(publicKey2.data(), pub2, PUBLIC_KEY_SIZE);

    std::array<uint8_t, PRIVATE_KEY_SIZE> privateKey1, privateKey2;
    std::memcpy(privateKey1.data(), priv1, PRIVATE_KEY_SIZE);
    std::memcpy(privateKey2.data(), priv2, PRIVATE_KEY_SIZE);

    // Test Multicast Encryption/Decryption
    {
        lnos::Packet original("mcast.encrypted.node", {{"ssh", 22}});
        original.type = lnos::PacketType::Announce;
        original.publicKey = publicKey1;
        ASSERT_TRUE(lnos::signPacket(original, privateKey1));

        // Encrypt as multicast (uses symmetric key derived from own publicKey)
        ASSERT_TRUE(lnos::encryptPacketPayload(original, privateKey1, publicKey1, true));
        EXPECT_EQ(original.isEncrypted, 1);
        EXPECT_FALSE(original.encryptedPayload.empty());

        // Decode must work but the payload is encrypted
        lnos::Blob blob = lnos::encode(original, true);
        lnos::EncodedPacket encoded{blob.data(), blob.size()};
        lnos::Packet decoded;
        ASSERT_TRUE(lnos::decode(encoded, decoded));
        EXPECT_EQ(decoded.isEncrypted, 1);
        EXPECT_TRUE(decoded.announce.name.empty()); // Name is inside encrypted payload

        // Decrypt multicast
        ASSERT_TRUE(lnos::decryptPacketPayload(decoded, privateKey1, publicKey1, true));
        EXPECT_EQ(decoded.isEncrypted, 0);
        EXPECT_EQ(decoded.announce.name, "mcast.encrypted.node");
        ASSERT_EQ(decoded.announce.services.size(), 1);
        EXPECT_EQ(decoded.announce.services[0].name, "ssh");
        EXPECT_EQ(decoded.announce.services[0].port, 22);
        EXPECT_TRUE(lnos::verifyPacket(decoded));
    }

    // Test Unicast Encryption/Decryption
    {
        lnos::Packet original;
        original.type = lnos::PacketType::GossipRequest;
        original.publicKey = publicKey1;

        lnos::GossipNode gnode;
        gnode.name = "unicast.node";
        gnode.ip = "127.0.0.1";
        original.gossipNodes.push_back(gnode);

        ASSERT_TRUE(lnos::signPacket(original, privateKey1));

        // Encrypt as unicast (uses sender's private key and recipient's public key)
        ASSERT_TRUE(lnos::encryptPacketPayload(original, privateKey1, publicKey2, false));
        EXPECT_EQ(original.isEncrypted, 2);

        // Decode
        lnos::Blob blob = lnos::encode(original, true);
        lnos::EncodedPacket encoded{blob.data(), blob.size()};
        lnos::Packet decoded;
        ASSERT_TRUE(lnos::decode(encoded, decoded));
        EXPECT_EQ(decoded.isEncrypted, 2);
        EXPECT_TRUE(decoded.gossipNodes.empty());

        // Decrypt unicast (recipient uses recipient's private key and sender's public key)
        ASSERT_TRUE(lnos::decryptPacketPayload(decoded, privateKey2, publicKey1, false));
        EXPECT_EQ(decoded.isEncrypted, 0);
        ASSERT_EQ(decoded.gossipNodes.size(), 1);
        EXPECT_EQ(decoded.gossipNodes[0].name, "unicast.node");
        EXPECT_EQ(decoded.gossipNodes[0].ip, "127.0.0.1");
    }
}
