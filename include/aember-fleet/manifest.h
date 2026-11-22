/**
 * @file manifest.h
 * @brief Release manifests: what the fleet server says a container image is,
 *        signed (Ed25519) by the fleet's release key, which never is on the
 *        server. The agent checks the signature over the manifest's exact
 *        bytes, then that it is for this container and architecture, and
 *        later the image's size and SHA-256 against it.
 *
 *   {"arch":"x64-poky-linux","container":"echo","sha256":"<hex>",
 *    "size":123,"version":"2"}
 */
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace aember::fleet {

struct Manifest {
  std::string container;
  std::string version;
  std::string arch;
  std::string sha256;  ///< lower-case hex
  std::uint64_t size = 0;
};

using PublicKey = std::array<std::uint8_t, 32>;

/// The release public key from base64 (fleet.json's "release_public_key").
std::expected<PublicKey, std::string> ParsePublicKey(std::string_view b64);

/// Check @p signature_b64 over @p manifest with @p key, then parse it.
std::expected<Manifest, std::string> VerifyManifest(
    const PublicKey& key, std::string_view manifest,
    std::string_view signature_b64);

/// Streaming SHA-256 (lower-case hex at the end).
class Sha256Hasher {
 public:
  Sha256Hasher();
  ~Sha256Hasher();
  Sha256Hasher(const Sha256Hasher&) = delete;
  Sha256Hasher& operator=(const Sha256Hasher&) = delete;
  void Update(std::string_view data);
  std::string HexDigest();

 private:
  alignas(16) std::array<std::uint8_t, 512> state_{};
};

}  // namespace aember::fleet
