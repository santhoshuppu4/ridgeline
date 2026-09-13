#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ridgeline/device_shadow_store.h"  // Reuses HttpTransport/HttpResponse -- see file comment below.
#include "ridgeline/sigv4_signer.h"

namespace ridgeline::demo {

// ---------------------------------------------------------------------------
// STUDY NOTES.
//
// WHY THIS REUSES ridgeline::SigV4Signer RATHER THAN PULLING IN THE FULL AWS
// SDK FOR C++: the signer is already built, already independently
// cross-checked against a from-scratch Python implementation (ADR-0008),
// and already has 7 tests covering exactly the kind of tampering an
// incorrect Lambda integration could introduce. Adding the full aws-sdk-cpp
// (a genuinely large dependency, with its own DynamoDB client) to save
// writing ~80 lines of JSON construction would be trading a small, already-
// verified piece of code for a much larger, unverified one.
//
// THE ONE REAL EXTENSION THIS NEEDED: Lambda's execution role provides
// TEMPORARY credentials (access key, secret key, AND a session token) via
// environment variables at runtime -- SigV4Signer's original design (built
// for a DynamoDB Local dev setup with static fake credentials) never needed
// a session token. Rather than modifying SigV4Signer itself, the session
// token is passed as an ordinary extra_header
// ("x-amz-security-token") -- SigV4Signer already signs whatever headers
// it's given, so no core code change was needed at all. This is exactly
// the kind of thing the class's own header comment anticipates: "the
// caller will ALSO send" headers beyond the fixed set.
//
// WHAT'S VERIFIED HERE VS. WHAT NEEDS A REAL LAMBDA INVOCATION: the request
// construction (this file) reuses SigV4Signer, which is independently
// verified. The actual round trip against a real DynamoDB table from
// inside a real Lambda execution environment cannot be tested from this
// sandbox (no AWS Lambda runtime here) -- same honest split as every other
// "needs real infrastructure" piece in this project (ADR-0007, ADR-0008,
// ADR-0014, ADR-0015).
// ---------------------------------------------------------------------------

struct DemoEvent {
  std::string event_id;      // Typically a timestamp-derived string; used as DynamoDB partition key.
  std::int64_t timestamp_unix_ns = 0;
  float confidence = 0.0f;
  std::string severity;      // ridgeline::ToString(AlertSeverity) -- kept as a plain string here to avoid
                              // this demo module depending on the weather module's enum directly.
  double temperature_c = 0.0;
  double relative_humidity_pct = 0.0;
  double wind_speed_kmh = 0.0;
  bool weather_available = false;
  bool user_triggered = false;  // true = visitor-submitted via the "try it yourself" API; false = scheduled synthetic traffic.
};

class DynamoDemoWriter {
 public:
  // `region`/`table_name` are the target DynamoDB table; credentials are
  // read by the caller from Lambda's environment variables
  // (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_SESSION_TOKEN) and
  // passed in explicitly here rather than read from the environment
  // inside this class -- keeps this class testable with fixed,
  // hand-supplied credentials, same reasoning as WeatherClient's injected
  // transport.
  DynamoDemoWriter(std::shared_ptr<HttpTransport> transport, SigV4Credentials credentials, std::string endpoint,
                   std::string table_name, std::string session_token);

  // Returns false on any failure -- this demo's DynamoDB write is
  // best-effort exactly like RedisHotStateStore's writes are: a visitor
  // seeing "your request was processed" (the ComputeAlertSeverity result)
  // must not depend on whether the write-for-posterity to DynamoDB
  // succeeded. The frontend shows the computed result either way; the
  // DynamoDB row is for the "live feed" history, not the immediate response.
  bool PutEvent(const DemoEvent& event);

  // Returns the most recent events (best-effort: an empty vector on any
  // failure, same reasoning as PutEvent -- a broken "history" feed should
  // never look like an error page to a visitor, just an empty feed).
  // Uses a DynamoDB Scan, not a Query: this demo table has no meaningful
  // secondary index to query against, and at the visitor volumes a
  // portfolio demo actually sees, scanning the whole (small) table and
  // sorting client-side by timestamp is simpler and correct. This would
  // NOT be the right choice at real production scale -- named explicitly
  // as a scale tradeoff, not an oversight.
  std::vector<DemoEvent> ListRecent(int limit);

 private:
  std::shared_ptr<HttpTransport> transport_;
  SigV4Signer signer_;
  std::string endpoint_;
  std::string table_name_;
  std::string session_token_;
};

}  // namespace ridgeline::demo
