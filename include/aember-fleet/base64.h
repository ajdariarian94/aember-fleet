/**
 * @file base64.h
 * @brief Base64 (RFC 4648, with padding) for keys and signatures in JSON.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aember::fleet {

std::string Base64Encode(std::span<const std::uint8_t> data);

/// nullopt for anything that isn't valid padded base64.
std::optional<std::vector<std::uint8_t>> Base64Decode(std::string_view text);

}  // namespace aember::fleet
