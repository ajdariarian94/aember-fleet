
#include "check.h"
#include "release_key.h"
#include <aember-fleet/base64.h>
#include <aember-fleet/manifest.h>

#include <span>
#include <string_view>

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
  {  // The two-block FIPS 180-2 message, fed in uneven pieces.
    Sha256Hasher h2;
    const std::string msg =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    for (std::size_t i = 0; i < msg.size(); i += 5) {
      h2.Update(std::string_view{msg}.substr(i, 5));
    }
    CHECK_EQ(h2.HexDigest(),
             std::string{"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6"
                         "ecedd419db06c1"});
  }
  {  // RFC 8032 test 2: a signature made elsewhere (like the server's
     // Python `cryptography`) verifies; the message isn't JSON, so the
     // manifest is refused after the signature check, not by it.
    const auto pub = ParsePublicKey(Base64Encode(std::array<std::uint8_t, 32>{
        0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a, 0x92, 0xb7, 0x0a,
        0xa7, 0x4d, 0x1b, 0x7e, 0xbc, 0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4,
        0x96, 0x8c, 0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c}));
    const std::array<std::uint8_t, 64> sig{
        0x92, 0xa0, 0x09, 0xa9, 0xf0, 0xd4, 0xca, 0xb8, 0x72, 0x0e, 0x82,
        0x0b, 0x5f, 0x64, 0x25, 0x40, 0xa2, 0xb2, 0x7b, 0x54, 0x16, 0x50,
        0x3f, 0x8f, 0xb3, 0x76, 0x22, 0x23, 0xeb, 0xdb, 0x69, 0xda, 0x08,
        0x5a, 0xc1, 0xe4, 0x3e, 0x15, 0x99, 0x6e, 0x45, 0x8f, 0x36, 0x13,
        0xd0, 0xf1, 0x1d, 0x8c, 0x38, 0x7b, 0x2e, 0xae, 0xb4, 0x30, 0x2a,
        0xee, 0xb0, 0x0d, 0x29, 0x16, 0x12, 0xbb, 0x0c, 0x00};
    CHECK(pub.has_value());
    if (pub) {
      auto r = VerifyManifest(*pub, "r", Base64Encode(sig));
      CHECK(!r.has_value());
      if (!r) CHECK(r.error() != "release signature doesn't verify");
      auto bad = VerifyManifest(*pub, "s", Base64Encode(sig));
      CHECK(!bad.has_value());
      if (!bad) CHECK_EQ(bad.error(), std::string{"release signature doesn't verify"});
    }
  }
  return Failures();
}
