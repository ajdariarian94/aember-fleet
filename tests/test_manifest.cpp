#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/random.h>

#include "check.h"
#include "release_key.h"
#include <aember-fleet/base64.h>
#include <aember-fleet/manifest.h>

#include <span>
#include <string_view>
#include <wolfssl/wolfcrypt/ed25519.h>

using namespace aember::fleet;

int main() {
  ReleaseKey key;
  const std::string m =
      R"({"arch":"x64-poky-linux","container":"echo","sha256":")" +
      std::string(64, 'a') + R"(","size":5,"version":"2"})";
  auto ok = VerifyManifest(key.pub, m, key.Sign(m));
  CHECK(ok.has_value());
  if (ok) {
    CHECK_EQ(ok->container, std::string{"echo"});
    CHECK_EQ(ok->version, std::string{"2"});
    CHECK_EQ(ok->size, 5u);
  }
  CHECK(!VerifyManifest(key.pub, m + " ", key.Sign(m)).has_value());
  ReleaseKey other;
  CHECK(!VerifyManifest(other.pub, m, key.Sign(m)).has_value());
  CHECK(!VerifyManifest(key.pub, m, "AAAA").has_value());
  for (
      const std::string& bad :
      {std::string{R"({"container":"echo"})"},
       R"({"arch":"a","container":"../x","sha256":")" + std::string(64, 'a') +
           R"(","size":1,"version":"1"})",
       std::string{
           R"({"arch":"a","container":"x","sha256":"zz","size":1,"version":"1"})"},
       // hex, but one digit short
       R"({"arch":"a","container":"x","sha256":")" + std::string(63, 'a') +
           R"(","size":1,"version":"1"})"}) {
    CHECK(!VerifyManifest(key.pub, bad, key.Sign(bad)).has_value());
  }
  CHECK(ParsePublicKey(Base64Encode(key.pub)).has_value());
  CHECK(!ParsePublicKey("AAAA").has_value());

  // Every padding case of the encoder (1, 2 and 0 bytes left over).
  const auto enc = [](std::string_view s) {
    return Base64Encode(std::span{reinterpret_cast<const std::uint8_t*>(s.data()),
                                  s.size()});
  };
  CHECK_EQ(enc("a"), std::string{"YQ=="});
  CHECK_EQ(enc("ab"), std::string{"YWI="});
  CHECK_EQ(enc("abc"), std::string{"YWJj"});
  CHECK_EQ(enc("abcd"), std::string{"YWJjZA=="});

  Sha256Hasher h;  // "abc" (FIPS 180-2)
  h.Update("a");
  h.Update("bc");
  CHECK_EQ(
      h.HexDigest(),
      std::string{
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
  return Failures();
}
