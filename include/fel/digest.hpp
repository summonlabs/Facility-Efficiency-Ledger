#pragma once

// Content digests used for evidence identity and journal integrity.
//
//  * SHA-256 provides evidence identity and the journal hash chain.
//  * CRC32C provides per-record transmission integrity with cheap validation.
//
// Both are implemented in-tree so that no network fetch or platform crypto
// provider is required and so that digests are byte-identical everywhere.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "fel/status.hpp"

namespace fel {

inline constexpr std::size_t kDigestBytes = 32;

class Digest {
public:
    Digest() noexcept = default;
    explicit Digest(const std::array<std::uint8_t, kDigestBytes>& bytes) noexcept : bytes_(bytes) {}

    static std::optional<Digest> from_hex(std::string_view hex) noexcept;
    static Result<Digest> parse_hex(std::string_view hex);

    const std::array<std::uint8_t, kDigestBytes>& bytes() const noexcept { return bytes_; }
    bool is_zero() const noexcept;
    std::string to_hex() const;

    friend bool operator==(const Digest& a, const Digest& b) noexcept { return a.bytes_ == b.bytes_; }
    friend bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }
    friend bool operator<(const Digest& a, const Digest& b) noexcept { return a.bytes_ < b.bytes_; }

private:
    std::array<std::uint8_t, kDigestBytes> bytes_{};
};

class Sha256 {
public:
    Sha256() noexcept;

    void update(std::span<const std::uint8_t> data) noexcept;
    void update(std::string_view text) noexcept;
    void update_byte(std::uint8_t value) noexcept;

    Digest finish() noexcept;

    static Digest hash(std::span<const std::uint8_t> data) noexcept;
    static Digest hash(std::string_view text) noexcept;

private:
    void transform(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::uint64_t bit_length_{0};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_{0};
    bool finished_{false};
};

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept;
std::uint32_t crc32c(std::string_view text) noexcept;
std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept;

}  // namespace fel

namespace std {
template <>
struct hash<fel::Digest> {
    std::size_t operator()(const fel::Digest& digest) const noexcept {
        std::size_t value = 1469598103934665603ULL;
        for (std::uint8_t byte : digest.bytes()) {
            value = (value ^ byte) * 1099511628211ULL;
        }
        return value;
    }
};
}  // namespace std
