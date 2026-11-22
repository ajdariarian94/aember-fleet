#include <nlohmann/json.hpp>

#include <cstring>
#include <format>

#include <wolfssl/options.h>

#include <aember-fleet/base64.h>
#include <aember-fleet/manifest.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/sha256.h>

namespace aember::fleet {

static_assert(sizeof(wc_Sha256) <= 512, "Sha256Hasher::state_ too small");

std::expected<PublicKey, std::string> ParsePublicKey(std::string_view b64) {
  const auto raw = Base64Decode(b64);
  if (!raw || raw->size() != 32) {
    return std::unexpected("release_public_key: base64 of 32 bytes");
  }
  PublicKey k{};
  std::copy(raw->begin(), raw->end(), k.begin());
  return k;
}

std::expected<Manifest, std::string> VerifyManifest(
    const PublicKey& key, std::string_view manifest,
    std::string_view signature_b64) {
  const auto sig = Base64Decode(signature_b64);
  if (!sig || sig->size() != 64)
    return std::unexpected("signature: base64 of 64 bytes");
  ed25519_key k{};
  if (wc_ed25519_init(&k) != 0) return std::unexpected("Ed25519 setup failed");
  int verified = 0;
  const bool ok =
      wc_ed25519_import_public(key.data(), key.size(), &k) == 0 &&
      wc_ed25519_verify_msg(sig->data(),
                            sig->size(),
                            reinterpret_cast<const byte*>(manifest.data()),
                            manifest.size(),
                            &verified,
                            &k) == 0 &&
      verified == 1;
  wc_ed25519_free(&k);
  if (!ok) return std::unexpected("release signature doesn't verify");

  const auto j = nlohmann::json::parse(manifest, nullptr, false);
  Manifest m;
  try {
    m.container = j.at("container").get<std::string>();
    m.version = j.at("version").get<std::string>();
    m.arch = j.at("arch").get<std::string>();
    m.sha256 = j.at("sha256").get<std::string>();
    m.size = j.at("size").get<std::uint64_t>();
  } catch (const nlohmann::json::exception& e) {
    return std::unexpected(std::format("manifest: {}", e.what()));
  }
  if (m.sha256.size() != 64 ||
      m.sha256.find_first_not_of("0123456789abcdef") != std::string::npos) {
    return std::unexpected("manifest: sha256 is not 64 hex digits");
  }
  if (m.container.empty() || m.container.find('/') != std::string::npos ||
      m.container.find("..") != std::string::npos) {
    return std::unexpected("manifest: bad container name");
  }
  return m;
}

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
