// Sha256Hasher through wolfCrypt (GPL-2.0-only): kept until the
// aember-resources SDK carries mbedTLS (see sha256-mbedtls.cpp).
#include <format>

#include <wolfssl/options.h>

#include <aember-fleet/manifest.h>
#include <wolfssl/wolfcrypt/sha256.h>

namespace aember::fleet {

static_assert(sizeof(wc_Sha256) <= 512, "Sha256Hasher::state_ too small");

Sha256Hasher::Sha256Hasher() {
  wc_InitSha256(reinterpret_cast<wc_Sha256*>(state_.data()));
}
Sha256Hasher::~Sha256Hasher() {
  wc_Sha256Free(reinterpret_cast<wc_Sha256*>(state_.data()));
}

void Sha256Hasher::Update(std::string_view data) {
  wc_Sha256Update(reinterpret_cast<wc_Sha256*>(state_.data()),
                  reinterpret_cast<const byte*>(data.data()),
                  static_cast<word32>(data.size()));
}

std::string Sha256Hasher::HexDigest() {
  std::array<std::uint8_t, 32> d{};
  wc_Sha256Final(reinterpret_cast<wc_Sha256*>(state_.data()), d.data());
  std::string hex;
  for (const auto b : d) hex += std::format("{:02x}", b);
  return hex;
}

}  // namespace aember::fleet
