// Tests run against real OpenSSL Ed25519 keys and real signatures -- no
// mocking of the crypto itself is possible or desirable here; the whole
// point is proving the actual cryptographic primitive behaves correctly.

#include "ridgeline/ota_manifest.h"

#include <gtest/gtest.h>

namespace {

ridgeline::OtaManifest SampleManifest() {
  ridgeline::OtaManifest m;
  m.version = 42;
  m.binary_sha256_hex = "d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2";
  m.url = "https://updates.example.com/ridgeline-agent-v42.bin";
  m.issued_unix_ns = 1735689600000000000LL;
  return m;
}

}  // namespace

TEST(OtaManifest, ValidSignatureVerifiesAgainstMatchingPublicKey) {
  const auto keys = ridgeline::GenerateOtaKeyPair();
  const auto manifest = SampleManifest();
  const std::string sig = ridgeline::SignManifest(manifest, keys.private_key_pem);
  EXPECT_TRUE(ridgeline::VerifyManifestSignature(manifest, sig, keys.public_key_pem));
}

TEST(OtaManifest, SignatureIsExactly64BytesForEd25519) {
  const auto keys = ridgeline::GenerateOtaKeyPair();
  const std::string sig = ridgeline::SignManifest(SampleManifest(), keys.private_key_pem);
  EXPECT_EQ(sig.size(), 64u);
}

TEST(OtaManifest, WrongPublicKeyFailsVerification) {
  const auto keys_a = ridgeline::GenerateOtaKeyPair();
  const auto keys_b = ridgeline::GenerateOtaKeyPair();  // A completely different keypair.
  const auto manifest = SampleManifest();
  const std::string sig = ridgeline::SignManifest(manifest, keys_a.private_key_pem);
  EXPECT_FALSE(ridgeline::VerifyManifestSignature(manifest, sig, keys_b.public_key_pem))
      << "a signature must only verify against the public key matching the private key that made it";
}

// ---------------------------------------------------------------------------
// Mutation tests: tamper with exactly one field after signing, confirm the
// signature no longer verifies. Each of these, individually, is what would
// let an attacker swap in a different binary/URL/version undetected if it
// failed -- testing all four is what proves CanonicalBytes() actually
// includes every field, not just some of them.
// ---------------------------------------------------------------------------

TEST(OtaManifest, TamperedVersionInvalidatesSignature) {
  const auto keys = ridgeline::GenerateOtaKeyPair();
  auto manifest = SampleManifest();
  const std::string sig = ridgeline::SignManifest(manifest, keys.private_key_pem);
  manifest.version = 43;  // Attacker claims a newer version than was actually signed.
  EXPECT_FALSE(ridgeline::VerifyManifestSignature(manifest, sig, keys.public_key_pem));
}

TEST(OtaManifest, TamperedBinaryHashInvalidatesSignature) {
  const auto keys = ridgeline::GenerateOtaKeyPair();
  auto manifest = SampleManifest();
  const std::string sig = ridgeline::SignManifest(manifest, keys.private_key_pem);
  manifest.binary_sha256_hex[0] = (manifest.binary_sha256_hex[0] == 'd') ? 'e' : 'd';
  EXPECT_FALSE(ridgeline::VerifyManifestSignature(manifest, sig, keys.public_key_pem))
      << "this is the field that would let an attacker point a legitimately-signed manifest at a "
         "DIFFERENT (malicious) binary if it weren't covered by the signature";
}

TEST(OtaManifest, TamperedUrlInvalidatesSignature) {
  const auto keys = ridgeline::GenerateOtaKeyPair();
  auto manifest = SampleManifest();
  const std::string sig = ridgeline::SignManifest(manifest, keys.private_key_pem);
  manifest.url = "https://attacker.example.com/malicious.bin";
  EXPECT_FALSE(ridgeline::VerifyManifestSignature(manifest, sig, keys.public_key_pem));
}

TEST(OtaManifest, TamperedTimestampInvalidatesSignature) {
  const auto keys = ridgeline::GenerateOtaKeyPair();
  auto manifest = SampleManifest();
  const std::string sig = ridgeline::SignManifest(manifest, keys.private_key_pem);
  manifest.issued_unix_ns += 1;
  EXPECT_FALSE(ridgeline::VerifyManifestSignature(manifest, sig, keys.public_key_pem))
      << "without this covered, a captured old manifest+signature could be replayed indefinitely "
         "with a forged fresher timestamp";
}

TEST(OtaManifest, TamperedSignatureBytesFailVerification) {
  const auto keys = ridgeline::GenerateOtaKeyPair();
  const auto manifest = SampleManifest();
  std::string sig = ridgeline::SignManifest(manifest, keys.private_key_pem);
  sig[0] ^= 0x01;  // Flip one bit in the signature itself, manifest untouched.
  EXPECT_FALSE(ridgeline::VerifyManifestSignature(manifest, sig, keys.public_key_pem));
}

TEST(OtaManifest, TruncatedSignatureFailsVerificationRatherThanCrashing) {
  const auto keys = ridgeline::GenerateOtaKeyPair();
  const auto manifest = SampleManifest();
  std::string sig = ridgeline::SignManifest(manifest, keys.private_key_pem);
  sig.resize(10);  // Malformed/truncated input -- must fail cleanly, not crash or throw.
  EXPECT_FALSE(ridgeline::VerifyManifestSignature(manifest, sig, keys.public_key_pem));
}

TEST(OtaManifest, EmptySignatureFailsVerification) {
  const auto keys = ridgeline::GenerateOtaKeyPair();
  EXPECT_FALSE(ridgeline::VerifyManifestSignature(SampleManifest(), "", keys.public_key_pem));
}

TEST(OtaManifest, CanonicalBytesAreDeterministic) {
  const auto manifest = SampleManifest();
  EXPECT_EQ(manifest.CanonicalBytes(), manifest.CanonicalBytes());
}

TEST(OtaManifest, DifferentManifestsProduceDifferentCanonicalBytes) {
  auto a = SampleManifest();
  auto b = SampleManifest();
  b.version = a.version + 1;
  EXPECT_NE(a.CanonicalBytes(), b.CanonicalBytes());
}

TEST(OtaManifest, GeneratedKeyPairProducesValidPemBlocks) {
  const auto keys = ridgeline::GenerateOtaKeyPair();
  EXPECT_NE(keys.private_key_pem.find("PRIVATE KEY"), std::string::npos);
  EXPECT_NE(keys.public_key_pem.find("PUBLIC KEY"), std::string::npos);
}
