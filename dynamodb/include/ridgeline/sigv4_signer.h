#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace ridgeline {

// ---------------------------------------------------------------------------
// STUDY NOTES.
//
// WHAT THIS IS: AWS Signature Version 4 -- the request-signing scheme every
// AWS API (including DynamoDB, and DynamoDB Local for compatibility) expects.
// It is a well-specified, entirely deterministic algorithm, which is exactly
// why it's split out into its own class: it can be tested completely on its
// own, with no network, no DynamoDB, no server of any kind. Given the same
// inputs it always produces the same signature -- if it doesn't, either this
// implementation has a bug or something upstream (system clock, config)
// changed the inputs.
//
// THE FOUR STAGES (this maps directly onto Sign()'s internal steps; useful
// to be able to name these from memory):
//   1. Canonical request: a normalized string representation of the HTTP
//      request (method, path, sorted+trimmed headers, payload hash) so two
//      semantically-identical requests always hash to the same bytes
//      regardless of header ordering or incidental whitespace.
//   2. String to sign: the canonical request's SHA-256 hash, wrapped with a
//      timestamp and a "credential scope" (date/region/service) that ties
//      the signature to a specific place and time -- this is what makes a
//      captured signature useless for a replay attack against a different
//      region or a different day.
//   3. Signing key: derived through a CHAIN of HMAC-SHA256 calls
//      (date -> region -> service -> "aws4_request"), each keyed by the
//      previous result, starting from the secret key prefixed with
//      "AWS4". Deliberately not just "HMAC the secret key once" -- the
//      chain scopes the derived key to that exact date/region/service, so a
//      leaked derived key (much more likely to leak than the root secret,
//      since it's used to sign specific requests) is far less valuable.
//   4. Final signature: HMAC-SHA256 of the "string to sign" using the
//      derived signing key from step 3, hex-encoded.
//
// VERIFICATION WITHOUT A LIVE AWS ACCOUNT: this class has no network
// dependency and needs no real AWS credentials to test -- SigV4 is validated
// entirely by checking that this implementation is internally consistent
// (same input always produces the same signature; changing ANY part of the
// input changes the signature) AND by cross-checking one fixed example
// against an independent second implementation of the same published
// algorithm (a short Python script using hashlib/hmac directly, not this
// codebase) -- see tests/dynamodb/sigv4_signer_test.cc and
// context/adr/0008 for exactly how that cross-check was done and why two
// independently-written implementations agreeing is real evidence, whereas
// a single implementation "testing itself" against its own logic would not
// be.
// ---------------------------------------------------------------------------

struct SigV4Credentials {
  std::string access_key_id;
  std::string secret_access_key;
  std::string region;
  std::string service;  // "dynamodb"
};

struct SigV4SignedRequest {
  std::string authorization_header;  // Full "AWS4-HMAC-SHA256 Credential=..., SignedHeaders=..., Signature=..." value.
  std::string amz_date;              // "X-Amz-Date" header value used in signing; caller must send this exact header.
};

class SigV4Signer {
 public:
  explicit SigV4Signer(SigV4Credentials credentials) : credentials_(std::move(credentials)) {}

  // Signs an HTTP request for a given host/path/method/body at a given
  // instant (unix seconds -- passed explicitly, not read from the system
  // clock internally, specifically so tests can pin a fixed timestamp and
  // get a fully deterministic, reproducible signature).
  //
  // `extra_headers` are headers the caller will ALSO send and that must be
  // included in the signature (SigV4 requires signing every header the
  // request actually sends, at minimum host/content-type/x-amz-target for
  // DynamoDB's JSON protocol). Keys are expected already-lowercased.
  SigV4SignedRequest Sign(const std::string& method, const std::string& host, const std::string& path,
                          const std::string& body, const std::map<std::string, std::string>& extra_headers,
                          std::int64_t unix_seconds) const;

 private:
  SigV4Credentials credentials_;
};

}  // namespace ridgeline
