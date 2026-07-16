#include <gtest/gtest.h>
#include <lnos/protocol.h>
#include <lnos/crypto.h>
#include <lnos/config.h>
#include <sodium.h>
#include <cstdlib>

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
