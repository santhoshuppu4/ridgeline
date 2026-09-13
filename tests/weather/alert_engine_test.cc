// AlertEngine is a pure function -- no I/O, no mocking needed. Every test
// here is a plain value in, plain value out.

#include "ridgeline/alert_engine.h"

#include <gtest/gtest.h>

using ridgeline::AlertSeverity;
using ridgeline::ComputeAlertSeverity;
using ridgeline::FireWeatherThresholds;
using ridgeline::ToString;
using ridgeline::WeatherConditions;

namespace {
WeatherConditions Calm() {
  WeatherConditions w;
  w.wind_speed_kmh = 5.0;
  w.relative_humidity_pct = 60.0;
  w.temperature_c = 20.0;
  return w;
}
WeatherConditions HighWindLowHumidity() {
  WeatherConditions w;
  w.wind_speed_kmh = 45.0;    // Above the default 40 kmh threshold.
  w.relative_humidity_pct = 15.0;  // Below the default 25% threshold.
  return w;
}
WeatherConditions ExtremeWind() {
  WeatherConditions w;
  w.wind_speed_kmh = 70.0;  // Above the default 65 kmh extreme threshold.
  w.relative_humidity_pct = 50.0;  // Humidity irrelevant once wind alone crosses the extreme line.
  return w;
}
}  // namespace

// --- Confidence tiers alone (no weather data at all) ---

TEST(AlertEngine, BelowLowConfidenceCutoffIsNone) {
  EXPECT_EQ(ComputeAlertSeverity(0.10f, std::nullopt), AlertSeverity::kNone);
}

TEST(AlertEngine, ExactlyAtLowCutoffIsLowNotNone) {
  // Threshold is a >= comparison, not >, so the boundary value itself
  // counts as a real signal -- worth locking in explicitly rather than
  // leaving the boundary behavior implicit.
  EXPECT_EQ(ComputeAlertSeverity(0.30f, std::nullopt), AlertSeverity::kLow);
}

TEST(AlertEngine, MidConfidenceWithNoWeatherIsMedium) {
  EXPECT_EQ(ComputeAlertSeverity(0.70f, std::nullopt), AlertSeverity::kMedium);
}

TEST(AlertEngine, HighConfidenceWithNoWeatherIsHigh) {
  EXPECT_EQ(ComputeAlertSeverity(0.90f, std::nullopt), AlertSeverity::kHigh);
}

TEST(AlertEngine, MissingWeatherNeverSuppressesAnAlert) {
  // The core correctness property from the header's design note: no
  // weather data must never mean "no alert." A confident detection still
  // reports at least its confidence-based severity.
  EXPECT_NE(ComputeAlertSeverity(0.90f, std::nullopt), AlertSeverity::kNone);
}

// --- Weather escalation (the actual fusion behavior) ---

TEST(AlertEngine, CalmWeatherDoesNotEscalate) {
  EXPECT_EQ(ComputeAlertSeverity(0.70f, Calm()), AlertSeverity::kMedium)
      << "calm conditions should match the no-weather-data result exactly";
}

TEST(AlertEngine, HighWindLowHumidityEscalatesExactlyOneLevel) {
  EXPECT_EQ(ComputeAlertSeverity(0.10f, HighWindLowHumidity()), AlertSeverity::kNone)
      << "escalation never applies below the low-confidence cutoff -- there's no real base severity to escalate";
  EXPECT_EQ(ComputeAlertSeverity(0.30f, HighWindLowHumidity()), AlertSeverity::kMedium)
      << "kLow escalates to kMedium";
  EXPECT_EQ(ComputeAlertSeverity(0.70f, HighWindLowHumidity()), AlertSeverity::kHigh)
      << "kMedium escalates to kHigh";
  EXPECT_EQ(ComputeAlertSeverity(0.90f, HighWindLowHumidity()), AlertSeverity::kCritical)
      << "kHigh escalates to kCritical";
}

TEST(AlertEngine, HighWindAloneWithoutLowHumidityDoesNotEscalate) {
  WeatherConditions w = HighWindLowHumidity();
  w.relative_humidity_pct = 70.0;  // Wind alone, without the humidity half of the combination.
  EXPECT_EQ(ComputeAlertSeverity(0.70f, w), AlertSeverity::kMedium)
      << "the escalation condition requires BOTH high wind AND low humidity, not either alone";
}

TEST(AlertEngine, LowHumidityAloneWithoutHighWindDoesNotEscalate) {
  WeatherConditions w = HighWindLowHumidity();
  w.wind_speed_kmh = 5.0;  // Low humidity, without the wind half of the combination.
  EXPECT_EQ(ComputeAlertSeverity(0.70f, w), AlertSeverity::kMedium);
}

TEST(AlertEngine, ExtremeWindOverridesToCriticalRegardlessOfConfidenceTier) {
  // Even a low-confidence detection becomes critical under extreme wind --
  // this is a deliberate override, not just an escalation, so it must
  // apply even where a one-level escalation from kLow would only reach
  // kMedium.
  EXPECT_EQ(ComputeAlertSeverity(0.30f, ExtremeWind()), AlertSeverity::kCritical);
  EXPECT_EQ(ComputeAlertSeverity(0.90f, ExtremeWind()), AlertSeverity::kCritical);
}

TEST(AlertEngine, ExtremeWindDoesNotOverrideBelowLowConfidenceCutoff) {
  // The override is for confirmed detections under dangerous weather, not
  // a way to manufacture an alert with no underlying detection signal at all.
  EXPECT_EQ(ComputeAlertSeverity(0.10f, ExtremeWind()), AlertSeverity::kNone);
}

TEST(AlertEngine, EscalationClampsAtCriticalRatherThanOverflowing) {
  // Already at kCritical via the high-confidence tier; the escalation
  // condition being ALSO true must not do anything undefined or wrap
  // around -- it should simply stay at kCritical.
  FireWeatherThresholds thresholds;
  EXPECT_EQ(ComputeAlertSeverity(0.90f, HighWindLowHumidity(), thresholds), AlertSeverity::kCritical);
}

// --- Custom thresholds ---

TEST(AlertEngine, CustomThresholdsAreHonored) {
  FireWeatherThresholds thresholds;
  thresholds.high_wind_kmh = 100.0;  // Deliberately unreachable by the test fixture's HighWindLowHumidity().
  EXPECT_EQ(ComputeAlertSeverity(0.70f, HighWindLowHumidity(), thresholds), AlertSeverity::kMedium)
      << "with a much higher wind threshold, the same weather data should no longer trigger escalation";
}

// --- ToString ---

TEST(AlertEngine, ToStringCoversEveryEnumerator) {
  EXPECT_EQ(ToString(AlertSeverity::kNone), "none");
  EXPECT_EQ(ToString(AlertSeverity::kLow), "low");
  EXPECT_EQ(ToString(AlertSeverity::kMedium), "medium");
  EXPECT_EQ(ToString(AlertSeverity::kHigh), "high");
  EXPECT_EQ(ToString(AlertSeverity::kCritical), "critical");
}
