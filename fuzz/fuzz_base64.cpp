// Base64Decode on any text; what decodes must encode and decode back.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

#include <aember-fleet/base64.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  using namespace aember::fleet;
  const std::string_view text{reinterpret_cast<const char*>(data), size};
  if (const auto raw = Base64Decode(text)) {
    if (Base64Decode(Base64Encode(*raw)) != raw) std::abort();
  }
  const std::span<const std::uint8_t> bytes{data, size};
  const auto back = Base64Decode(Base64Encode(bytes));
  if (!back || !std::ranges::equal(*back, bytes)) std::abort();
  return 0;
}
