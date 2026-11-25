#include <nlohmann/json.hpp>

#include <cstring>
#include <format>

#include <aember-fleet/base64.h>
#include <aember-fleet/manifest.h>

extern "C" {
#include <monocypher-ed25519.h>
}

namespace aember::fleet {

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
  // Ed25519 (RFC 8032) through Monocypher (third_party/, BSD-2-Clause).
  if (crypto_ed25519_check(
          sig->data(), key.data(),
          reinterpret_cast<const std::uint8_t*>(manifest.data()),
          manifest.size()) != 0) {
    return std::unexpected("release signature doesn't verify");
  }

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

}  // namespace aember::fleet
