#include "fel/ids.hpp"

#include <array>
#include <cstdio>

namespace fel {

bool is_valid_identifier_text(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxIdentifierLength) {
        return false;
    }
    const auto is_alnum = [](char c) noexcept {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    const auto is_body = [&](char c) noexcept {
        return is_alnum(c) || c == '.' || c == '_' || c == ':' || c == '-';
    };
    if (!is_alnum(text.front()) || !is_alnum(text.back())) {
        return false;
    }
    for (char c : text) {
        if (!is_body(c)) {
            return false;
        }
    }
    return true;
}

std::string mint_identifier_text(std::string_view prefix, std::uint64_t counter) {
    std::array<char, 32> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.*s-%016llx",
                                      static_cast<int>(prefix.size()), prefix.data(),
                                      static_cast<unsigned long long>(counter));
    if (written <= 0) {
        return std::string{prefix};
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

}  // namespace fel
