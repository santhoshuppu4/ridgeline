# ADR-0015: Terraform AWS infrastructure

- **Status:** Accepted
- **Date:** 2026-09-13

## Context
Phase 5-ii: real Terraform for the AWS infrastructure this project's
backing services already assume -- DynamoDB, Redis, Kafka -- plus what's
needed to actually run the gateway (ECS Fargate) and host signed OTA
artifacts (S3).

## A real, unavoidable constraint: no AWS credentials, no billable resources

This project's development sandbox has neither AWS credentials nor a
reason to spin up real, billable cloud infrastructure. This shaped the
whole verification approach the same way the lack of a real DynamoDB
Local endpoint shaped ADR-0008, and the lack of general internet access
shaped ADR-0014's weather client: **write real, correct HCL, verify what
can genuinely be verified without live infrastructure, and name exactly
what still needs a human with real AWS access.**

The real `terraform` binary itself isn't reachable from this sandbox
either (`releases.hashicorp.com` isn't in the sandbox's network
allowlist, confirmed directly rather than assumed). `terraform-config-inspect`
-- a genuine HashiCorp tool, available via `apt` -- IS available, and does
real (if shallow) syntax and internal-reference checking with zero AWS
credentials needed. Every module and the root configuration were run
through it and returned zero diagnostics. Additionally, every variable
passed between the root module and each submodule was manually
cross-checked by hand against each submodule's `variables.tf` for name
mismatches or missing required values -- catching a class of error
`terraform-config-inspect`'s shallow parsing wouldn't.

**What this does NOT prove:** that resource arguments match the AWS
provider's actual schema (attribute names, types, valid value ranges),
that IAM policies are semantically correct, or that `terraform apply`
would actually succeed. That needs the real `terraform` binary
(`terraform validate`, then `terraform plan`) against a real AWS account
-- explicitly handed off in `terraform/README.md`, not silently skipped.

## A real integration gap, found while writing this (not discovered later)

MSK Serverless requires AWS IAM SASL client authentication. The existing
`KafkaProducer` (ADR-0007) was built and tested only against plaintext
broker connections -- a real in-process mock cluster, then a real local
Redpanda broker. **It has no IAM SASL support, so the gateway as built
today cannot actually connect to the MSK Serverless cluster this
Terraform provisions.** Documented explicitly in `terraform/README.md`
rather than left for whoever tries to connect the two first. Two real
paths forward: add IAM SASL support to `KafkaProducer` (librdkafka
supports the mechanism; not implemented here), or provision MSK
differently (unauthenticated, VPC-security-group-restricted) to match
what the gateway actually supports today.

## Other real design decisions, each with a stated tradeoff

- **DynamoDB PAY_PER_REQUEST**, not provisioned capacity: no capacity
  planning for genuinely low, unpredictable portfolio traffic.
- **Single-node ElastiCache, not a replication group with failover**:
  the data it holds (`RedisHotStateStore`) is explicitly, deliberately
  ephemeral (ADR-0008) -- paying for HA on data designed to be safely
  lossy would be protecting the wrong thing.
- **MSK Serverless, not provisioned MSK**: no broker/partition capacity
  planning up front, cost scales with actual (low) throughput. ADR-0009's
  fleet-scale findings would be the right trigger to revisit this once
  real traffic is characterized.
- **Public subnets, no NAT gateway**: a real, avoidable recurring cost
  this portfolio deployment doesn't need to carry, given the gateway
  needs inbound internet access anyway.
- **IAM task role scoped to exactly three actions** (DynamoDB
  Get/PutItem on one table, three MSK actions, S3 GetObject on one
  bucket) rather than an AWS-managed blanket policy -- the same
  least-privilege instinct as ADR-0011's mTLS device identity, applied
  to infrastructure.

## Consequences
- `Dockerfile.gateway` (repo root) was written and reasoned about but
  never actually built in this sandbox -- a full multi-stage build
  compiling gRPC/protobuf/librdkafka from a base Ubuntu image is real,
  meaningful build time this phase's session budget didn't leave room
  for after the Terraform itself. Build it and confirm it works before
  trusting it; package names for the runtime stage's shared libraries
  (e.g. `libgrpc++1.51t64`) are Ubuntu-24.04-plausible but unverified.
- No CI job runs any of this Terraform -- correctly so; doing so would
  need real AWS credentials checked into (or provided to) CI, which is
  its own security decision this project hasn't made and shouldn't make
  implicitly by just wiring up a CI step.
