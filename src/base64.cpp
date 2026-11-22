#include <array>

#include <aember-fleet/base64.h>

namespace aember::fleet {

namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int Value(char c) {
  const auto pos = kAlphabet.find(c);
  return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
}

}  // namespace

std::string Base64Encode(std::span<const std::uint8_t> data) {
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 3 <= data.size(); i += 3) {
    const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    for (const int shift : {18, 12, 6, 0}) out += kAlphabet[(n >> shift) & 63];
  }
  const std::size_t rest = data.size() - i;
  if (rest > 0) {
    std::uint32_t n = data[i] << 16;
    if (rest == 2) n |= data[i + 1] << 8;
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += rest == 2 ? kAlphabet[(n >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

std::optional<std::vector<std::uint8_t>> Base64Decode(std::string_view text) {
  if (text.size() % 4 != 0) return std::nullopt;
  std::vector<std::uint8_t> out;
  out.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    std::array<int, 4> v{};
    int pad = 0;
    for (int j = 0; j < 4; ++j) {
      const char c = text[i + j];
      if (c == '=' && i + 4 == text.size() && j >= 2) {
        ++pad;
        v[j] = 0;
        continue;
      }
      if (pad > 0) return std::nullopt;  // data after padding
      v[j] = Value(c);
      if (v[j] < 0) return std::nullopt;
    }
    const std::uint32_t n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
    out.push_back(static_cast<std::uint8_t>(n >> 16));
    if (pad < 2) out.push_back(static_cast<std::uint8_t>(n >> 8));
    if (pad < 1) out.push_back(static_cast<std::uint8_t>(n));
  }
  return out;
}

}  // namespace aember::fleet
