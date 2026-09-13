#pragma once

#include <optional>
#include <string>

#include "ridgeline/weather_client.h"

namespace ridgeline {

// ---------------------------------------------------------------------------
// STUDY NOTES.
//
// WHAT "FUSION" MEANS HERE: a confirmed smoke detection's OWN confidence
// score says something about how likely the detection itself is real, but
// says nothing about how DANGEROUS that detection is right now. The same
// confidence-0.7 smoke detection is a very different situation on a calm,
// humid day versus during high wind and low humidity -- the exact
// conditions under which a fire spreads fastest and is hardest to contain.
// Fusing the two signals (detection confidence + live weather) into one
// severity is what lets a downstream alerting system prioritize correctly,
// instead of every confirmed detection paging someone at the same urgency.
//
// WHERE THE THRESHOLDS COME FROM: the wind/humidity escalation condition
// (high wind AND low humidity together) is modeled on the U.S. National
// Weather Service's Red Flag Warning criteria for critical fire weather --
// but the specific numbers here (`FireWeatherThresholds`'s defaults) are
// illustrative and configurable, not a certified meteorological standard;
// treat them as a reasonable starting point to tune against real
// operational data, not as authoritative fire-danger science.
//
// WHY THIS RETURNS THE SAME SEVERITY REGARDLESS OF WEATHER DATA AVAILABILITY,
// JUST WITHOUT ESCALATION: `ComputeAlertSeverity` takes an
// `optional<WeatherConditions>` and degrades gracefully when it's
// `nullopt` (e.g. WeatherClient::FetchCurrent failed, or hasn't completed
// its first fetch yet) -- the base severity from confidence alone is still
// returned, just without the wind/humidity escalation. A detection must
// never go UNREPORTED just because the weather API happened to be
// unreachable at that moment; that would make an external, unrelated
// dependency capable of silently suppressing a real alert.
// ---------------------------------------------------------------------------

enum class AlertSeverity { kNone, kLow, kMedium, kHigh, kCritical };

std::string ToString(AlertSeverity severity);

struct FireWeatherThresholds {
  float low_confidence_cutoff = 0.30f;    // Below this: no real detection signal, severity is kNone.
  float medium_confidence_cutoff = 0.60f;
  float high_confidence_cutoff = 0.85f;
  double high_wind_kmh = 40.0;    // ~25 mph -- roughly NWS Red Flag Warning's wind criterion.
  double low_humidity_pct = 25.0;  // Roughly NWS Red Flag Warning's humidity criterion.
  double extreme_wind_kmh = 65.0;  // ~40 mph -- treated as critical regardless of confidence tier.
};

// Combines a detection's confidence with (optionally available) weather
// conditions into one severity level. Pure function: no I/O, no state,
// fully deterministic given its inputs -- everything about this is
// testable with plain values, no fake transports or mocks needed.
AlertSeverity ComputeAlertSeverity(float confidence, const std::optional<WeatherConditions>& weather,
                                  const FireWeatherThresholds& thresholds = {});

}  // namespace ridgeline
