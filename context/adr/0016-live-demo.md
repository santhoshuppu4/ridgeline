# ADR-0016: Live demo

- **Status:** Accepted
- **Date:** 2026-09-13

## Context
Every phase of this project has been verified with tests, real hardware
runs, or (where infrastructure couldn't be reached from this sandbox)
explicit hand-offs to real environments -- but all of it lives in a repo
and a set of documents. This phase makes a real slice of the system
actually run continuously, publicly reachable, at zero guaranteed cost.

## What's live vs. what's scoped out, and why

**Live**: `ComputeAlertSeverity` and `WeatherClient` (ADR-0014) -- the real
compiled code, not a reimplementation -- running in AWS Lambda, fed by a
scheduled synthetic feed (EventBridge, every 5 minutes) and a visitor-
triggered "try it yourself" button.

**Deliberately not live**: the ONNX detection pipeline, Kafka, mTLS, the
whole gateway. Running real-time inference in a public-facing serverless
function at guaranteed-zero cost isn't a realistic combination; scoping
the demo to the fusion logic is what makes "runs forever for free" an
honest claim rather than an optimistic one.

## Reusing already-verified code instead of a new AWS SDK dependency

`DynamoDemoWriter` (`demo/lambda/`) reuses `ridgeline::SigV4Signer`
(ADR-0008) rather than pulling in the full AWS SDK for C++. The one real
extension needed: Lambda's execution role provides *temporary* credentials
(access key, secret key, AND a session token) via environment variables,
which the original signer (built for DynamoDB Local's static fake
credentials) never needed. No change to `SigV4Signer` itself was
required -- the session token is passed as an ordinary extra header
(`x-amz-security-token`), since the signer already signs whatever headers
it's given. Verified: `SessionTokenIsIncludedWhenProvided` test passes.

## The split that keeps this testable

`demo_handler_core.h/.cc` contains the entire actual behavior (fetch
weather, fuse, record, list) with zero dependency on the Lambda Runtime
API. `handler.cc` -- the real `aws-lambda-cpp` entry point -- is a thin
~90-line wrapper that parses the incoming event and calls into the core.
9 tests cover the core directly, including the same "missing weather never
suppresses an alert" property from ADR-0014, now verified through this
entire new path, and a mutation-style check that a DynamoDB write failure
never changes the severity a visitor actually sees.

## What was and wasn't verified in this sandbox

**Verified directly:**
- All 9 core-logic tests pass (fake weather transport, fake DynamoDB
  transport -- the same pattern as every other injectable-transport
  component in this project).
- `handler.cc` -- the actual Lambda entry point -- **compiles and links
  successfully against the real `aws-lambda-cpp` library**, producing a
  genuine, complete executable. This is meaningfully stronger evidence
  than "the code looks right": every symbol the Lambda Runtime API
  actually needs resolved correctly.
- The frontend's HTML is well-formed (parsed with Python's `html.parser`,
  zero tag mismatches) and its embedded JavaScript is syntactically valid
  (`node --check`).
- All six Terraform-related resources in `terraform/demo/main.tf` pass
  `terraform-config-inspect` with zero diagnostics.

**NOT verified in this sandbox, and explicitly not claimed to be:**
- `Dockerfile.demo-lambda` was never actually built -- no Docker daemon
  here, and the `amazonlinux:2023`/`public.ecr.aws` base images aren't
  reachable from this sandbox's network allowlist either. What WAS
  verified is the thing this Dockerfile packages: the handler binary
  itself, already proven to compile and link.
- No real Lambda invocation, no real API Gateway request, no real
  DynamoDB round trip from inside an actual AWS Lambda execution
  environment. Same category of gap as ADR-0015's `terraform apply`, and
  handed off the same way: `demo/README.md`'s deploy steps and smoke test
  are exactly the verification this sandbox couldn't do itself.

## Cost design

Lambda and DynamoDB are AWS's permanent Always Free tier -- not a
12-month trial. API Gateway HTTP API is free for 12 months, then
~$1/million requests; at realistic portfolio-demo traffic this rounds to
$0, but "rounds to" isn't "guaranteed to," which is why
`aws_budgets_budget.cost_alert` exists as a hard backstop, not just an
assurance. Its one real limitation, stated in its own resource comment:
it monitors the whole AWS account's spend, not this stack specifically --
accurately scoping a budget to one resource group needs cost-allocation
tags configured well in advance, which is more setup than a portfolio demo
justifies.

## Consequences
- This is a genuinely separate Terraform root (`terraform/demo/`) from
  the paid infrastructure in `terraform/` -- applying one has no effect on
  the other, and they can be deployed, or not, completely independently.
- `ListRecent`'s DynamoDB Scan (rather than a Query against an index) is a
  real, named scale tradeoff: correct and simple at portfolio-demo volume,
  not the right choice at real production scale.
