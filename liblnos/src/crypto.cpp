#include <sodium.h>
#include <fstream>
#include <iostream>
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

    bool encryptPacketPayload(
        Packet& packet,
        const std::array<uint8_t, PRIVATE_KEY_SIZE>& myPrivateKey,
        const std::array<uint8_t, PUBLIC_KEY_SIZE>& recipientPublicKey,
        bool isMulticast)
    {
        // 1. Prepare serialize of raw payload (isEncrypted = 0)
        Packet inner = packet;
        inner.isEncrypted = 0;
        inner.encryptedPayload.clear();
        inner.nonce.fill(0);

        Blob rawPayload = encode(inner, false);

        // 2. Encrypt
        std::array<uint8_t, 24> nonce;
        randombytes_buf(nonce.data(), nonce.size());

        if (isMulticast) {
            // For multicast, we use symmetric crypto_secretbox where the key is derived from sender's own public key.
            // Why: Confidentiality from outsiders (non-LNOS participants who don't have the sender's public key),
            // and fits multicast well.
            // Let's derive a 32-byte key from the sender's public key.
            std::array<uint8_t, 32> symmetricKey{};
            // Since we want to use Curve25519 or just sha256 of Ed25519 pk, let's use sha256 to hash Ed25519 pk into 32-bytes key.
            crypto_hash_sha256(symmetricKey.data(), packet.publicKey.data(), packet.publicKey.size());

            std::vector<uint8_t> encrypted(rawPayload.size() + crypto_secretbox_MACBYTES);
            crypto_secretbox_easy(
                encrypted.data(),
                rawPayload.data(),
                rawPayload.size(),
                nonce.data(),
                symmetricKey.data()
            );

            packet.isEncrypted = 1;
            packet.nonce = nonce;
            packet.encryptedPayload = std::move(encrypted);
        } else {
            // Unicast: convert Ed25519 keys to Curve25519
            std::array<uint8_t, crypto_box_PUBLICKEYBYTES> recPkCurve;
            std::array<uint8_t, crypto_box_SECRETKEYBYTES> mySkCurve;

            if (crypto_sign_ed25519_pk_to_curve25519(recPkCurve.data(), recipientPublicKey.data()) != 0) {
                return false;
            }
            if (crypto_sign_ed25519_sk_to_curve25519(mySkCurve.data(), myPrivateKey.data()) != 0) {
                return false;
            }

            std::vector<uint8_t> encrypted(rawPayload.size() + crypto_box_MACBYTES);
            if (crypto_box_easy(
                encrypted.data(),
                rawPayload.data(),
                rawPayload.size(),
                nonce.data(),
                recPkCurve.data(),
                mySkCurve.data()
            ) != 0) {
                return false;
            }

            packet.isEncrypted = 2;
            packet.nonce = nonce;
            packet.encryptedPayload = std::move(encrypted);
        }

        return true;
    }

    bool decryptPacketPayload(
        Packet& packet,
        const std::array<uint8_t, PRIVATE_KEY_SIZE>& myPrivateKey,
        const std::array<uint8_t, PUBLIC_KEY_SIZE>& senderPublicKey,
        bool isMulticast)
    {
        if (packet.isEncrypted == 0) {
            return true;
        }

        Blob decrypted;

        if (isMulticast && packet.isEncrypted == 1) {
            std::array<uint8_t, 32> symmetricKey{};
            crypto_hash_sha256(symmetricKey.data(), senderPublicKey.data(), senderPublicKey.size());

            if (packet.encryptedPayload.size() < crypto_secretbox_MACBYTES) {
                return false;
            }

            decrypted.resize(packet.encryptedPayload.size() - crypto_secretbox_MACBYTES);
            if (crypto_secretbox_open_easy(
                decrypted.data(),
                packet.encryptedPayload.data(),
                packet.encryptedPayload.size(),
                packet.nonce.data(),
                symmetricKey.data()
            ) != 0) {
                return false;
            }
        } else if (!isMulticast && packet.isEncrypted == 2) {
            std::array<uint8_t, crypto_box_PUBLICKEYBYTES> sendPkCurve;
            std::array<uint8_t, crypto_box_SECRETKEYBYTES> mySkCurve;

            if (crypto_sign_ed25519_pk_to_curve25519(sendPkCurve.data(), senderPublicKey.data()) != 0) {
                return false;
            }
            if (crypto_sign_ed25519_sk_to_curve25519(mySkCurve.data(), myPrivateKey.data()) != 0) {
                return false;
            }

            if (packet.encryptedPayload.size() < crypto_box_MACBYTES) {
                return false;
            }

            decrypted.resize(packet.encryptedPayload.size() - crypto_box_MACBYTES);
            if (crypto_box_open_easy(
                decrypted.data(),
                packet.encryptedPayload.data(),
                packet.encryptedPayload.size(),
                packet.nonce.data(),
                sendPkCurve.data(),
                mySkCurve.data()
            ) != 0) {
                return false;
            }
        } else {
            return false;
        }

        // Decode the decrypted payload back into packet
        EncodedPacket enc{decrypted.data(), decrypted.size()};
        Packet temp;
        // The raw payload was encoded using encode(inner, false) -> it does not have the signature at the end, but decode() expects signature.
        // Wait, let's look at decode(). decode() expects the signature at the end.
        // Let's modify decode() or we can append a dummy signature to the decrypted payload to satisfy decode().
        // Yes, let's just append the original signature to decrypted payload, or write a dedicated decodeInner() that doesn't consume signature?
        // Actually, appending the original signature + sender's public key (32 + 64 bytes) is super simple and robust.
        // Wait, encode(inner, false) encodes everything EXCEPT the signature.
        // Wait, encode(inner, false) encodes: version, type, isEncrypted, inner_fields, publicKey.
        // Ah! encode(inner, false) DOES encode the publicKey, but not the signature.
        // Let's verify:
        // blobPush(blob, p.publicKey);
        // if (includeSignature) blobPush(blob, p.signature);
        // Yes! So encode(inner, false) has publicKey but not signature.
        // But decode expects: encodedPacketConsume(packet, result.publicKey); encodedPacketConsume(packet, result.signature);
        // So we can just append SIGNATURE_SIZE (64) bytes of zero or original signature to decrypted payload, then call decode()!
        size_t orig_size = decrypted.size();
        decrypted.resize(orig_size + SIGNATURE_SIZE);
        std::memcpy(decrypted.data() + orig_size, packet.signature.data(), SIGNATURE_SIZE);

        EncodedPacket encFull{decrypted.data(), decrypted.size()};
        if (!decode(encFull, temp)) {
            return false;
        }

        // Copy decoded fields back to packet
        packet.announce = std::move(temp.announce);
        packet.gossipNodes = std::move(temp.gossipNodes);
        packet.isEncrypted = 0;
        packet.encryptedPayload.clear();

        return true;
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

        if (sodium_mlock(privateKey.data(), privateKey.size()) != 0) {
            std::cerr << "[warning] failed to mlock private key memory\n";
        }

        return privateKey;
    }


}
