# ADR-0014: Weather fusion and alert engine

- **Status:** Accepted
- **Date:** 2026-09-12

## Context
The original design spec called for weather fusion: combining a
detection's own confidence with live weather conditions into a severity
level, since the same confirmed smoke detection means something very
different on a calm day versus during high wind and low humidity -- the
exact conditions under which fire spreads fastest.

## A real sandbox constraint, confirmed directly, not assumed

This project's development sandbox has an allowlisted set of network
domains for testing; a direct `curl` to `api.open-meteo.com` from it
returns `Host not in allowlist`, not a timeout or a real response --
confirmed before writing any code, the same discipline applied to the
DynamoDB Local and real-Kafka-broker splits earlier in this project. This
shaped the whole design: **the logic had to be verifiable without live
network access, and the live round trip had to be handed off explicitly**
rather than silently assumed to work.

## Decisions

**`WeatherClient` depends on an injectable `HttpGetTransport`**, exactly
the "depend on an interface, not libcurl directly" pattern from
`DeviceShadowStore`. `FetchCurrent` returns `nullopt` on ANY failure --
unreachable, non-200, malformed JSON, or a response missing an expected
field -- rather than a partially-populated reading that looks identical to
a real one from the caller's side. Tested with a fake transport: valid
parse, non-200, connection failure, malformed JSON, missing `current` key,
missing individual fields, and a field with the wrong type -- 8 tests, all
without any network.

**`ComputeAlertSeverity` is a pure function**, no I/O, taking confidence
and an `optional<WeatherConditions>`. Escalation logic is modeled on the
U.S. National Weather Service's Red Flag Warning criteria (high wind AND
low humidity together, not either alone) -- the specific thresholds are
illustrative defaults, not a certified meteorological standard, and are
configurable via `FireWeatherThresholds`. Extreme wind alone overrides to
critical regardless of confidence tier. 14 tests, including a mutation
test proving the escalation condition is actually load-bearing (disabling
it makes the exact test designed to catch that fail, as it should).

**Missing weather data never suppresses an alert.** This is the single
most important correctness property here, tested explicitly
(`MissingWeatherNeverSuppressesAnAlert`) and verified in the real running
gateway, not just in a unit test: with `--weather-lat`/`--weather-lon` set
and the live API genuinely unreachable from this sandbox, every confirmed
detection still produced a correctly-tiered `ALERT` log line from
confidence alone, with `(no weather data yet)` noted honestly rather than
either crashing or silently dropping the alert.

**`WeatherCache` keeps the last successful reading on a failed refresh**,
rather than clearing to `nullopt` -- a transient API hiccup shouldn't
blank out weather context between refreshes. Deliberately NOT tracking a
max-age/staleness cutoff on that cached reading in this phase; worth
revisiting if this ever needs to be trusted through a long outage.

## What's verified here vs. what needs real infrastructure

Verified directly, in this sandbox: the parsing logic (8 tests), the
fusion logic (14 tests, mutation-tested), and the graceful-degradation
path in the real running gateway (confirmed: weather genuinely
unreachable, detections still processed and alerted correctly).

NOT verified here, and explicitly not claimed to be:
`scripts/weather_test.sh` exercises a real live round trip against
Open-Meteo and was written to run wherever real internet access exists
(GitHub Actions runners, a developer's own machine) -- run here, it fails
exactly as expected with the same "Host not in allowlist" limitation,
which is itself the correct, honest outcome for this environment. This is
the same split as ADR-0007's Kafka mock-vs-real-broker and ADR-0008's
DynamoDB fake-transport-vs-live-endpoint: the untestable-here piece is
named specifically, not glossed over.

## Consequences
- `RIDGELINE_WITH_WEATHER` excluded from TSan, same libcurl-is-
  uninstrumented reasoning as `RIDGELINE_WITH_DYNAMODB`.
- Single global weather location per gateway process (`--weather-lat`/
  `--weather-lon`), not per-device or per-tenant. A fleet spread across
  meaningfully different climates would need per-region weather, which
  this phase doesn't build.
- `scripts/weather_test.sh` is wired into CI without a sandbox-specific
  skip condition, since GitHub Actions runners have real internet access
  unlike this development sandbox -- it should genuinely pass there.
