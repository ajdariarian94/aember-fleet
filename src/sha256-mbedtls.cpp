// Sha256Hasher through mbedTLS's crypto library (Apache-2.0). Builds against
// mbedTLS 2.28 (Ubuntu 24.04, host tests) and 3.6 (Yocto, the SDK).
#include <format>

#include <aember-fleet/manifest.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#if MBEDTLS_VERSION_MAJOR >= 3
#define AEMBER_SHA256(op) mbedtls_sha256_##op
#else
#define AEMBER_SHA256(op) mbedtls_sha256_##op##_ret
#endif

namespace aember::fleet {

namespace {
mbedtls_sha256_context* Ctx(std::array<std::uint8_t, 512>& state) {
  return reinterpret_cast<mbedtls_sha256_context*>(state.data());
}
}  // namespace

static_assert(sizeof(mbedtls_sha256_context) <= 512,
              "Sha256Hasher::state_ too small");

Sha256Hasher::Sha256Hasher() {
  mbedtls_sha256_init(Ctx(state_));
  AEMBER_SHA256(starts)(Ctx(state_), 0);  // 0: SHA-256, not SHA-224
}
Sha256Hasher::~Sha256Hasher() { mbedtls_sha256_free(Ctx(state_)); }

void Sha256Hasher::Update(std::string_view data) {
  AEMBER_SHA256(update)(Ctx(state_),
                        reinterpret_cast<const unsigned char*>(data.data()),
                        data.size());
}

std::string Sha256Hasher::HexDigest() {
  std::array<std::uint8_t, 32> d{};
  AEMBER_SHA256(finish)(Ctx(state_), d.data());
  std::string hex;
  for (const auto b : d) hex += std::format("{:02x}", b);
  return hex;
}

}  // namespace aember::fleet
