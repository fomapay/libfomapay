#include <cstdint>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "fomapay/fomapay.hpp"
#include "internal.hpp"

namespace fomapay::webhook {

namespace {

// ───────────────────────────── SHA-256 (FIPS 180-4) ─────────────────────────────
// Implémenté ici pour ne dépendre d'aucune bibliothèque cryptographique.

const std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

class Sha256 {
public:
    void update(const unsigned char* data, std::size_t length) {
        for (std::size_t i = 0; i < length; ++i) {
            block_[filled_++] = data[i];
            if (filled_ == 64) {
                compress();
                bits_ += 512;
                filled_ = 0;
            }
        }
    }

    void finish(unsigned char out[32]) {
        const std::uint64_t total = bits_ + static_cast<std::uint64_t>(filled_) * 8;
        block_[filled_++] = 0x80;
        if (filled_ > 56) {
            std::memset(block_ + filled_, 0, 64 - filled_);
            compress();
            filled_ = 0;
        }
        std::memset(block_ + filled_, 0, 56 - filled_);
        for (int i = 0; i < 8; ++i) {
            block_[63 - i] = static_cast<unsigned char>(total >> (8 * i));
        }
        compress();
        for (int i = 0; i < 8; ++i) {
            out[i * 4] = static_cast<unsigned char>(state_[i] >> 24);
            out[i * 4 + 1] = static_cast<unsigned char>(state_[i] >> 16);
            out[i * 4 + 2] = static_cast<unsigned char>(state_[i] >> 8);
            out[i * 4 + 3] = static_cast<unsigned char>(state_[i]);
        }
    }

private:
    void compress() {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(block_[i * 4]) << 24) | (std::uint32_t(block_[i * 4 + 1]) << 16) | (std::uint32_t(block_[i * 4 + 2]) << 8) |
                   std::uint32_t(block_[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::uint32_t state_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char block_[64] = {};
    std::size_t filled_ = 0;
    std::uint64_t bits_ = 0;
};

void sha256(const std::string& a, const std::string& b, unsigned char out[32]) {
    Sha256 hash;
    hash.update(reinterpret_cast<const unsigned char*>(a.data()), a.size());
    hash.update(reinterpret_cast<const unsigned char*>(b.data()), b.size());
    hash.finish(out);
}

/// Renvoie "" si la signature est valide, sinon la raison du refus.
std::string check(const std::string& payload, const std::string& header, const std::string& secret, long tolerance, long long now) {
    if (secret.empty()) {
        throw Error("ERROR_WEBHOOK_SECRET_MISSING", "Le secret de signature du webhook (whsec_…) est requis.");
    }

    std::string timestamp, signature;
    std::size_t start = 0;
    while (start <= header.size()) {
        std::size_t end = header.find(',', start);
        if (end == std::string::npos) {
            end = header.size();
        }
        std::string part = header.substr(start, end - start);
        part.erase(0, part.find_first_not_of(" \t"));
        if (part.compare(0, 2, "t=") == 0) {
            timestamp = part.substr(2);
        } else if (part.compare(0, 3, "v1=") == 0) {
            signature = part.substr(3);
        }
        start = end + 1;
    }
    if (timestamp.empty() || timestamp.size() > 18 || timestamp.find_first_not_of("0123456789") != std::string::npos || signature.empty()) {
        return "En-tête de signature absent ou mal formé.";
    }

    for (char& c : signature) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    const std::string expected = hmac_sha256_hex(secret, timestamp + "." + payload);
    // Comparaison en temps constant.
    unsigned diff = static_cast<unsigned>(expected.size() ^ signature.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        diff |= static_cast<unsigned char>(expected[i]) ^ static_cast<unsigned char>(i < signature.size() ? signature[i] : 0);
    }
    if (diff != 0) {
        return "La signature ne correspond pas au contenu reçu.";
    }

    // L'horodatage n'est contrôlé qu'après la signature : il est alors authentique.
    const long long current = now != 0 ? now : static_cast<long long>(std::time(nullptr));
    const long long age = current - std::stoll(timestamp);
    return (age > tolerance || age < -tolerance) ? "Événement trop ancien : rejeu refusé." : "";
}

}  // namespace

std::string hmac_sha256_hex(const std::string& key, const std::string& message) {
    unsigned char block[64] = {};
    if (key.size() > 64) {
        sha256(key, "", block);
    } else {
        std::memcpy(block, key.data(), key.size());
    }
    std::string inner(64, '\0'), outer(64, '\0');
    for (int i = 0; i < 64; ++i) {
        inner[i] = static_cast<char>(block[i] ^ 0x36);
        outer[i] = static_cast<char>(block[i] ^ 0x5c);
    }
    unsigned char digest[32];
    sha256(inner, message, digest);
    sha256(outer, std::string(reinterpret_cast<const char*>(digest), 32), digest);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const unsigned char byte : digest) {
        out += hex[byte >> 4];
        out += hex[byte & 0x0F];
    }
    return out;
}

bool verify(const std::string& payload, const std::string& header, const std::string& secret, long tolerance, long long now) {
    return check(payload, header, secret, tolerance, now).empty();
}

Event construct_event(const std::string& payload, const std::string& header, const std::string& secret, long tolerance, long long now) {
    const std::string failure = check(payload, header, secret, tolerance, now);
    if (!failure.empty()) {
        throw SignatureError("ERROR_INVALID_SIGNATURE", failure, 400);
    }
    try {
        const json::Value root = json::parse(payload);
        if (root.is_object()) {
            Event event;
            event.id = root.str("id");
            event.type = root.str("type");
            event.created = root.str("created");
            event.livemode = root.flag("livemode");
            if (const json::Value* data = root.find("data"); data && data->is_object()) {
                event.data = internal::to_payment(*data);
            }
            return event;
        }
    } catch (const std::invalid_argument&) {
        // traité ci-dessous
    }
    throw SignatureError("ERROR_INVALID_PAYLOAD", "Le corps du webhook n'est pas un événement FomaPay valide.", 400);
}

}  // namespace fomapay::webhook
