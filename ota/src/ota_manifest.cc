#include "ridgeline/ota_manifest.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <memory>
#include <sstream>
#include <stdexcept>

namespace ridgeline {

std::string OtaManifest::CanonicalBytes() const {
  // Fixed order, fixed separator, no field ever omitted (even if empty) --
  // so "field present with a different value" and "field absent" can never
  // collide into the same byte string for two semantically different
  // manifests.
  std::ostringstream out;
  out << "v1|version=" << version << "|binary_sha256=" << binary_sha256_hex << "|url=" << url
      << "|issued_unix_ns=" << issued_unix_ns;
  return out.str();
}

namespace {

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

[[noreturn]] void ThrowOpenSslError(const std::string& what) {
  throw std::runtime_error(what + " (OpenSSL error, see stderr for details)");
}

EvpPkeyPtr LoadPrivateKeyFromPem(const std::string& pem) {
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  if (!bio) ThrowOpenSslError("BIO_new_mem_buf failed");
  EVP_PKEY* raw = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
  if (!raw) ThrowOpenSslError("failed to parse Ed25519 private key PEM");
  return EvpPkeyPtr(raw, EVP_PKEY_free);
}

EvpPkeyPtr LoadPublicKeyFromPem(const std::string& pem) {
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  if (!bio) ThrowOpenSslError("BIO_new_mem_buf failed");
  EVP_PKEY* raw = PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
  if (!raw) ThrowOpenSslError("failed to parse Ed25519 public key PEM");
  return EvpPkeyPtr(raw, EVP_PKEY_free);
}

std::string PemFromPrivateKey(EVP_PKEY* key) {
  BioPtr bio(BIO_new(BIO_s_mem()), BIO_free);
  if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
    ThrowOpenSslError("failed to serialize private key to PEM");
  }
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio.get(), &data);
  return std::string(data, static_cast<std::size_t>(len));
}

std::string PemFromPublicKey(EVP_PKEY* key) {
  BioPtr bio(BIO_new(BIO_s_mem()), BIO_free);
  if (!bio || PEM_write_bio_PUBKEY(bio.get(), key) != 1) {
    ThrowOpenSslError("failed to serialize public key to PEM");
  }
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio.get(), &data);
  return std::string(data, static_cast<std::size_t>(len));
}

}  // namespace

OtaKeyPair GenerateOtaKeyPair() {
  EVP_PKEY* raw = nullptr;
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr),
                                                                  EVP_PKEY_CTX_free);
  if (!ctx || EVP_PKEY_keygen_init(ctx.get()) != 1 || EVP_PKEY_keygen(ctx.get(), &raw) != 1) {
    ThrowOpenSslError("Ed25519 key generation failed");
  }
  EvpPkeyPtr key(raw, EVP_PKEY_free);

  OtaKeyPair out;
  out.private_key_pem = PemFromPrivateKey(key.get());
  out.public_key_pem = PemFromPublicKey(key.get());
  return out;
}

std::string SignManifest(const OtaManifest& manifest, const std::string& private_key_pem) {
  EvpPkeyPtr key = LoadPrivateKeyFromPem(private_key_pem);
  const std::string canonical = manifest.CanonicalBytes();

  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md_ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!md_ctx) ThrowOpenSslError("EVP_MD_CTX_new failed");
  // Ed25519 is a "one-shot" scheme in OpenSSL's API: no separate
  // DigestSignUpdate calls, and the digest algorithm parameter is null
  // (Ed25519 hashes internally as part of the signature scheme itself).
  if (EVP_DigestSignInit(md_ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
    ThrowOpenSslError("EVP_DigestSignInit failed");
  }
  std::size_t sig_len = 0;
  if (EVP_DigestSign(md_ctx.get(), nullptr, &sig_len,
                     reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size()) != 1) {
    ThrowOpenSslError("EVP_DigestSign (length query) failed");
  }
  std::string signature(sig_len, '\0');
  if (EVP_DigestSign(md_ctx.get(), reinterpret_cast<unsigned char*>(signature.data()), &sig_len,
                     reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size()) != 1) {
    ThrowOpenSslError("EVP_DigestSign failed");
  }
  signature.resize(sig_len);
  return signature;
}

bool VerifyManifestSignature(const OtaManifest& manifest, const std::string& signature,
                             const std::string& public_key_pem) {
  EvpPkeyPtr key = LoadPublicKeyFromPem(public_key_pem);
  const std::string canonical = manifest.CanonicalBytes();

  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md_ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!md_ctx) return false;
  if (EVP_DigestVerifyInit(md_ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) return false;
  // Returns 1 (valid), 0 (invalid signature -- NOT an error), or <0 (a real
  // error, e.g. malformed input). Only 1 means "trust this manifest";
  // everything else, including "real error," is treated as untrusted --
  // the safe default for a security check is to fail closed.
  const int result = EVP_DigestVerify(md_ctx.get(), reinterpret_cast<const unsigned char*>(signature.data()),
                                      signature.size(), reinterpret_cast<const unsigned char*>(canonical.data()),
                                      canonical.size());
  return result == 1;
}

}  // namespace ridgeline
