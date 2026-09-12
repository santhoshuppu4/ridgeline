#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ridgeline {

// ---------------------------------------------------------------------------
// STUDY NOTES.
//
// SCOPE, STATED PLAINLY: this is the CRYPTOGRAPHIC TRUST PRIMITIVE of signed
// OTA -- proving a manifest describing a firmware/binary update genuinely
// came from whoever holds the signing private key, and hasn't been altered.
// It does NOT implement: downloading the binary, atomic A/B partition
// swapping, or watchdog-triggered rollback if the new binary crashes on
// boot. Those need real device/OS-level infrastructure (a bootloader with
// two partition slots, a watchdog timer) that a portfolio project can't
// meaningfully fake without misrepresenting what's real. See ADR-0012 for
// why the split is drawn here, same reasoning as the Kafka mock-vs-real
// split in ADR-0007: build the piece that can be fully, honestly verified
// now, and scope the rest as explicit follow-up rather than half-build it.
//
// WHY Ed25519, NOT RSA: smaller keys and signatures (32-byte public key,
// 64-byte signature, vs. RSA's kilobit-sized everything), and no padding
// scheme to get subtly wrong (a classic RSA signature vulnerability
// class). It's also what the project's own spec named explicitly
// ("Ed25519-signed A/B OTA updates").
//
// WHAT'S SIGNED: the manifest's CANONICAL byte representation -- a fixed
// field order, not JSON (whose key order and whitespace an attacker could
// vary while keeping "the same" semantic content, which would either break
// verification unpredictably or, worse, allow a signature computed over
// one serialization to verify against a maliciously reformatted one if the
// canonicalization isn't airtight). `Manifest::CanonicalBytes()` is the
// single source of truth both the signer and verifier call -- there is no
// second implementation of "how to serialize a manifest" to drift out of
// sync with it.
//
// VERIFICATION WITHOUT REAL DEVICE HARDWARE: this is fully testable with
// nothing but OpenSSL's Ed25519 implementation -- generate a real keypair,
// sign a real manifest, verify it, and confirm tampering with any single
// field (version, binary hash, URL) invalidates the signature. See
// tests/ota/ota_manifest_test.cc and its mutation tests.
// ---------------------------------------------------------------------------

struct OtaManifest {
  std::uint64_t version = 0;
  std::string binary_sha256_hex;  // Lowercase hex SHA-256 of the update binary.
  std::string url;                // Where a real device would fetch the binary from.
  std::int64_t issued_unix_ns = 0;

  // Fixed field order, fixed separators -- deliberately not JSON. See the
  // file-level note on why canonical, unambiguous bytes matter for what
  // gets signed.
  std::string CanonicalBytes() const;
};

struct OtaKeyPair {
  std::string public_key_pem;
  std::string private_key_pem;
};

// Generates a fresh Ed25519 keypair. In a real deployment the private key
// would live in an HSM or a tightly-access-controlled signing service, not
// be generated ad hoc by whatever process needs to sign something -- this
// function exists for tooling/tests, not as a template for production key
// custody.
OtaKeyPair GenerateOtaKeyPair();

// Returns the raw Ed25519 signature bytes (64 bytes) over
// manifest.CanonicalBytes(), or throws std::runtime_error on any OpenSSL
// failure (a malformed private key, for instance).
std::string SignManifest(const OtaManifest& manifest, const std::string& private_key_pem);

// Verifies `signature` against manifest.CanonicalBytes() using the given
// public key. Returns true only if the signature is valid for exactly this
// manifest's exact canonical bytes -- any mutation to any field changes
// CanonicalBytes() and invalidates every previously-valid signature.
bool VerifyManifestSignature(const OtaManifest& manifest, const std::string& signature,
                             const std::string& public_key_pem);

}  // namespace ridgeline
