#include "fel/digest.hpp"

#include <array>

namespace fel {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256K{{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
}};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
}

constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < 256U; ++index) {
        std::uint32_t crc = index;
        for (int bit = 0; bit < 8; ++bit) {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0x82F63B78U) : (crc >> 1);
        }
        table[index] = crc;
    }
    return table;
}

constexpr auto kCrc32cTable = make_crc32c_table();

}  // namespace

bool Digest::is_zero() const noexcept {
    for (std::uint8_t byte : bytes_) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string Digest::to_hex() const {
    constexpr std::string_view kHexDigits = "0123456789abcdef";
    std::string out;
    out.reserve(kDigestBytes * 2U);
    for (std::uint8_t byte : bytes_) {
        out.push_back(kHexDigits[static_cast<std::size_t>(byte >> 4)]);
        out.push_back(kHexDigits[static_cast<std::size_t>(byte & 0x0FU)]);
    }
    return out;
}

std::optional<Digest> Digest::from_hex(std::string_view hex) noexcept {
    if (hex.size() != kDigestBytes * 2U) {
        return std::nullopt;
    }
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::array<std::uint8_t, kDigestBytes> bytes{};
    for (std::size_t i = 0; i < kDigestBytes; ++i) {
        const int high = nibble(hex[i * 2U]);
        const int low = nibble(hex[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return Digest{bytes};
}

Result<Digest> Digest::parse_hex(std::string_view hex) {
    const auto parsed = from_hex(hex);
    if (!parsed.has_value()) {
        return Reason{ReasonCode::MalformedNumber,
                      "expected " + std::to_string(kDigestBytes * 2U) +
                          " lowercase hexadecimal characters"};
    }
    return *parsed;
}

Sha256::Sha256() noexcept {
    state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
              0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
}

void Sha256::transform(const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16; ++i) {
        schedule[i] = (static_cast<std::uint32_t>(block[i * 4U]) << 24) |
                      (static_cast<std::uint32_t>(block[i * 4U + 1U]) << 16) |
                      (static_cast<std::uint32_t>(block[i * 4U + 2U]) << 8) |
                      static_cast<std::uint32_t>(block[i * 4U + 3U]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(schedule[i - 15], 7) ^ rotr(schedule[i - 15], 18) ^
                                 (schedule[i - 15] >> 3);
        const std::uint32_t s1 =
            rotr(schedule[i - 2], 17) ^ rotr(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + schedule[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
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

void Sha256::update(std::span<const std::uint8_t> data) noexcept {
    if (finished_) {
        return;
    }
    bit_length_ += static_cast<std::uint64_t>(data.size()) * 8ULL;
    std::size_t offset = 0;
    if (buffer_size_ != 0) {
        while (offset < data.size() && buffer_size_ < buffer_.size()) {
            buffer_[buffer_size_++] = data[offset++];
        }
        if (buffer_size_ == buffer_.size()) {
            transform(buffer_.data());
            buffer_size_ = 0;
        }
    }
    while (data.size() - offset >= buffer_.size()) {
        transform(data.data() + offset);
        offset += buffer_.size();
    }
    while (offset < data.size()) {
        buffer_[buffer_size_++] = data[offset++];
    }
}

void Sha256::update(std::string_view text) noexcept {
    update(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()),
                                         text.size()});
}

void Sha256::update_byte(std::uint8_t value) noexcept { update(std::span<const std::uint8_t>{&value, 1}); }

Digest Sha256::finish() noexcept {
    if (!finished_) {
        const std::uint64_t bit_length = bit_length_;
        const std::uint8_t pad = 0x80U;
        update(std::span<const std::uint8_t>{&pad, 1});
        const std::uint8_t zero = 0x00U;
        while (buffer_size_ != 56U) {
            update(std::span<const std::uint8_t>{&zero, 1});
        }
        std::array<std::uint8_t, 8> length_bytes{};
        for (std::size_t i = 0; i < 8; ++i) {
            length_bytes[i] = static_cast<std::uint8_t>((bit_length >> ((7U - i) * 8U)) & 0xFFU);
        }
        update(std::span<const std::uint8_t>{length_bytes.data(), length_bytes.size()});
        finished_ = true;
    }
    std::array<std::uint8_t, kDigestBytes> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 4U] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFU);
        out[i * 4U + 1U] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFU);
        out[i * 4U + 2U] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFU);
        out[i * 4U + 3U] = static_cast<std::uint8_t>(state_[i] & 0xFFU);
    }
    return Digest{out};
}

Digest Sha256::hash(std::span<const std::uint8_t> data) noexcept {
    Sha256 hasher;
    hasher.update(data);
    return hasher.finish();
}

Digest Sha256::hash(std::string_view text) noexcept {
    Sha256 hasher;
    hasher.update(text);
    return hasher.finish();
}

std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept {
    std::uint32_t crc = seed;
    for (std::uint8_t byte : data) {
        crc = kCrc32cTable[(crc ^ byte) & 0xFFU] ^ (crc >> 8);
    }
    return crc;
}

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept {
    return crc32c_extend(0xFFFFFFFFU, data) ^ 0xFFFFFFFFU;
}

std::uint32_t crc32c(std::string_view text) noexcept {
    return crc32c(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()),
                                                text.size()});
}

}  // namespace fel
