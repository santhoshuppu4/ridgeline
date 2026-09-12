// SigV4Signer needs no network and no DynamoDB to test -- see the header's
// top comment for why. Three kinds of test here:
//   1. Cross-check: this EXACT input, signed by an independent Python
//      implementation (hashlib/hmac directly, no Ridgeline code -- see
//      /tmp/sigv4_reference.py in the development session that produced
//      this, and context/adr/0008 for the full value), must produce this
//      EXACT signature. If it doesn't, the algorithm itself is wrong.
//   2. Determinism: same input, called twice, must produce the same output.
//   3. Sensitivity: changing any single input must change the signature --
//      a signer that ignores one of its inputs (e.g. forgets to include the
//      body in the payload hash) would still pass a determinism check but
//      fail this one.

#include "ridgeline/sigv4_signer.h"

#include <gtest/gtest.h>

namespace {

ridgeline::SigV4Credentials FixedCredentials() {
  return {"AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "us-west-2", "dynamodb"};
}

std::map<std::string, std::string> FixedHeaders() {
  return {{"content-type", "application/x-amz-json-1.0"}, {"x-amz-target", "DynamoDB_20120810.GetItem"}};
}

}  // namespace

TEST(SigV4Signer, MatchesIndependentPythonReferenceImplementation) {
  // This exact signature was computed by a from-scratch Python
  // implementation of the AWS SigV4 algorithm (hashlib.sha256 + hmac
  // directly), given the identical inputs below. Two independently written
  // implementations of a deterministic, well-specified algorithm producing
  // byte-identical output is real evidence of correctness -- a single
  // implementation checking its own output against itself would not be.
  ridgeline::SigV4Signer signer(FixedCredentials());
  const auto result = signer.Sign("POST", "dynamodb.us-west-2.amazonaws.com", "/",
                                  R"({"TableName":"device_shadows"})", FixedHeaders(),
                                  /*unix_seconds=*/1735689600 /* 2025-01-01T00:00:00Z */);

  EXPECT_EQ(result.amz_date, "20250101T000000Z");
  EXPECT_EQ(result.authorization_header,
            "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20250101/us-west-2/dynamodb/aws4_request, "
            "SignedHeaders=content-type;host;x-amz-date;x-amz-target, "
            "Signature=3d9ebfd4396270567a5c795dc1bbde1adebd8613fb4de961d8c2dd37199ab0f6");
}

TEST(SigV4Signer, SameInputAlwaysProducesSameSignature) {
  ridgeline::SigV4Signer signer(FixedCredentials());
  const auto r1 = signer.Sign("POST", "host", "/", "body", FixedHeaders(), 1735689600);
  const auto r2 = signer.Sign("POST", "host", "/", "body", FixedHeaders(), 1735689600);
  EXPECT_EQ(r1.authorization_header, r2.authorization_header);
}

TEST(SigV4Signer, ChangingBodyChangesSignature) {
  ridgeline::SigV4Signer signer(FixedCredentials());
  const auto r1 = signer.Sign("POST", "host", "/", "body-a", FixedHeaders(), 1735689600);
  const auto r2 = signer.Sign("POST", "host", "/", "body-b", FixedHeaders(), 1735689600);
  EXPECT_NE(r1.authorization_header, r2.authorization_header)
      << "the payload hash must feed into the signature -- if this fails, a signer that forgot to hash "
         "the body would still 'work' against a fixed test body forever";
}

TEST(SigV4Signer, ChangingTimestampChangesSignature) {
  ridgeline::SigV4Signer signer(FixedCredentials());
  const auto r1 = signer.Sign("POST", "host", "/", "body", FixedHeaders(), 1735689600);
  const auto r2 = signer.Sign("POST", "host", "/", "body", FixedHeaders(), 1735689601);
  EXPECT_NE(r1.authorization_header, r2.authorization_header);
  EXPECT_NE(r1.amz_date, r2.amz_date);
}

TEST(SigV4Signer, ChangingSecretKeyChangesSignatureButNotVisibleCredentialId) {
  ridgeline::SigV4Credentials creds_a = FixedCredentials();
  ridgeline::SigV4Credentials creds_b = FixedCredentials();
  creds_b.secret_access_key = "different-secret-entirely";

  const auto r1 = ridgeline::SigV4Signer(creds_a).Sign("POST", "host", "/", "body", FixedHeaders(), 1735689600);
  const auto r2 = ridgeline::SigV4Signer(creds_b).Sign("POST", "host", "/", "body", FixedHeaders(), 1735689600);

  EXPECT_NE(r1.authorization_header, r2.authorization_header)
      << "the secret key feeds the signing-key derivation chain -- two different secrets must never "
         "produce the same signature";
  // The access key ID is intentionally NOT secret (it's how the server
  // looks up which secret to check against), so it's expected to appear
  // identically in both headers even though the signatures differ.
  EXPECT_NE(r1.authorization_header.find("Credential=AKIDEXAMPLE/"), std::string::npos);
  EXPECT_NE(r2.authorization_header.find("Credential=AKIDEXAMPLE/"), std::string::npos);
}

TEST(SigV4Signer, ChangingExtraHeaderChangesSignature) {
  ridgeline::SigV4Signer signer(FixedCredentials());
  auto headers_a = FixedHeaders();
  auto headers_b = FixedHeaders();
  headers_b["x-amz-target"] = "DynamoDB_20120810.PutItem";  // Different operation, different signed header value.

  const auto r1 = signer.Sign("POST", "host", "/", "body", headers_a, 1735689600);
  const auto r2 = signer.Sign("POST", "host", "/", "body", headers_b, 1735689600);
  EXPECT_NE(r1.authorization_header, r2.authorization_header);
}

TEST(SigV4Signer, SignedHeadersListIsSortedRegardlessOfInputOrder) {
  // std::map already sorts by key, so this mostly documents/locks in that
  // behavior rather than testing something that could plausibly vary --
  // but SignedHeaders order is part of what the server re-derives when
  // verifying, so a regression here would be a real, subtle bug.
  ridgeline::SigV4Signer signer(FixedCredentials());
  const auto result = signer.Sign("POST", "host", "/", "body", FixedHeaders(), 1735689600);
  const auto pos = result.authorization_header.find("SignedHeaders=");
  ASSERT_NE(pos, std::string::npos);
  const auto end = result.authorization_header.find(',', pos);
  const std::string signed_headers = result.authorization_header.substr(pos, end - pos);
  EXPECT_EQ(signed_headers, "SignedHeaders=content-type;host;x-amz-date;x-amz-target");
}
