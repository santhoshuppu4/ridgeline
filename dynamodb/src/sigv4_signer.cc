#include "ridgeline/sigv4_signer.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <sstream>

namespace ridgeline {

namespace {

std::string ToHex(const unsigned char* data, unsigned int len) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(static_cast<std::size_t>(len) * 2);
  for (unsigned int i = 0; i < len; ++i) {
    out[2 * i] = kHex[(data[i] >> 4) & 0xF];
    out[2 * i + 1] = kHex[data[i] & 0xF];
  }
  return out;
}

std::string Sha256Hex(const std::string& data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
  return ToHex(digest, SHA256_DIGEST_LENGTH);
}

// Returns the raw (not hex) HMAC-SHA256 digest, as a std::string of exactly
// 32 bytes -- used both for intermediate signing-key derivation (where the
// RAW bytes feed the next HMAC step) and for the final signature (where the
// caller hex-encodes the result). Conflating these two -- e.g. hex-encoding
// an intermediate key before using it as the next HMAC's key -- is a classic
// SigV4 implementation bug: the signature would be well-formed and
// deterministic, just wrong, which is exactly the kind of mistake a
// same-file self-check wouldn't catch and the independent Python cross-check
// in the test suite is specifically there to catch.
std::string HmacSha256Raw(const std::string& key, const std::string& data) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(data.data()),
       data.size(), digest, &len);
  return std::string(reinterpret_cast<char*>(digest), len);
}

std::string FormatAmzDate(std::int64_t unix_seconds) {
  std::time_t t = static_cast<std::time_t>(unix_seconds);
  std::tm tm_utc{};
  gmtime_r(&t, &tm_utc);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm_utc);
  return buf;
}

std::string FormatDateStamp(std::int64_t unix_seconds) {
  std::time_t t = static_cast<std::time_t>(unix_seconds);
  std::tm tm_utc{};
  gmtime_r(&t, &tm_utc);
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y%m%d", &tm_utc);
  return buf;
}

}  // namespace

SigV4SignedRequest SigV4Signer::Sign(const std::string& method, const std::string& host, const std::string& path,
                                     const std::string& body, const std::map<std::string, std::string>& extra_headers,
                                     std::int64_t unix_seconds) const {
  const std::string amz_date = FormatAmzDate(unix_seconds);
  const std::string date_stamp = FormatDateStamp(unix_seconds);
  const std::string payload_hash = Sha256Hex(body);

  // ---- Stage 1: canonical request ----
  // All headers that will actually be sent must appear here, sorted by
  // lowercased header name -- this is what makes the signature independent
  // of the order the caller happens to build headers in.
  std::map<std::string, std::string> all_headers = extra_headers;
  all_headers["host"] = host;
  all_headers["x-amz-date"] = amz_date;

  std::ostringstream canonical_headers;
  std::ostringstream signed_headers;
  bool first = true;
  for (const auto& [name, value] : all_headers) {  // std::map is already sorted by key.
    canonical_headers << name << ":" << value << "\n";
    if (!first) signed_headers << ";";
    signed_headers << name;
    first = false;
  }

  // No query string in DynamoDB's JSON protocol (POST body carries the
  // request), so the canonical query string is always empty -- documented
  // rather than silently hardcoded, since a future signer reused for a
  // GET-based AWS API would need this filled in.
  const std::string canonical_request = method + "\n" + path + "\n" + "" /* canonical query string */ + "\n" +
                                        canonical_headers.str() + "\n" + signed_headers.str() + "\n" + payload_hash;

  // ---- Stage 2: string to sign ----
  const std::string credential_scope = date_stamp + "/" + credentials_.region + "/" + credentials_.service + "/aws4_request";
  const std::string string_to_sign =
      "AWS4-HMAC-SHA256\n" + amz_date + "\n" + credential_scope + "\n" + Sha256Hex(canonical_request);

  // ---- Stage 3: derive the signing key (HMAC chain, see header comment) ----
  const std::string k_date = HmacSha256Raw("AWS4" + credentials_.secret_access_key, date_stamp);
  const std::string k_region = HmacSha256Raw(k_date, credentials_.region);
  const std::string k_service = HmacSha256Raw(k_region, credentials_.service);
  const std::string k_signing = HmacSha256Raw(k_service, "aws4_request");

  // ---- Stage 4: final signature ----
  const std::string signature_raw = HmacSha256Raw(k_signing, string_to_sign);
  unsigned char sig_bytes[EVP_MAX_MD_SIZE];
  std::copy(signature_raw.begin(), signature_raw.end(), sig_bytes);
  const std::string signature_hex = ToHex(sig_bytes, static_cast<unsigned int>(signature_raw.size()));

  SigV4SignedRequest result;
  result.amz_date = amz_date;
  result.authorization_header = "AWS4-HMAC-SHA256 Credential=" + credentials_.access_key_id + "/" + credential_scope +
                                ", SignedHeaders=" + signed_headers.str() + ", Signature=" + signature_hex;
  return result;
}

}  // namespace ridgeline
