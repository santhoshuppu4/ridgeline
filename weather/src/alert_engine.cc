#include "ridgeline/alert_engine.h"

namespace ridgeline {

std::string ToString(AlertSeverity severity) {
  switch (severity) {
    case AlertSeverity::kNone: return "none";
    case AlertSeverity::kLow: return "low";
    case AlertSeverity::kMedium: return "medium";
    case AlertSeverity::kHigh: return "high";
    case AlertSeverity::kCritical: return "critical";
  }
  return "unknown";  // Unreachable for a valid enum value; defensive against a future enumerator added without a case here.
}

namespace {
AlertSeverity Escalate(AlertSeverity severity) {
  switch (severity) {
    case AlertSeverity::kNone: return AlertSeverity::kLow;   // A weather escalation shouldn't apply to "no signal," but
                                                              // handled defensively rather than left as undefined behavior.
    case AlertSeverity::kLow: return AlertSeverity::kMedium;
    case AlertSeverity::kMedium: return AlertSeverity::kHigh;
    case AlertSeverity::kHigh: return AlertSeverity::kCritical;
    case AlertSeverity::kCritical: return AlertSeverity::kCritical;  // Already at the top; escalating further is a no-op.
  }
  return severity;
}
}  // namespace

AlertSeverity ComputeAlertSeverity(float confidence, const std::optional<WeatherConditions>& weather,
                                  const FireWeatherThresholds& thresholds) {
  if (confidence < thresholds.low_confidence_cutoff) return AlertSeverity::kNone;

  AlertSeverity severity = AlertSeverity::kLow;
  if (confidence >= thresholds.high_confidence_cutoff) severity = AlertSeverity::kHigh;
  else if (confidence >= thresholds.medium_confidence_cutoff) severity = AlertSeverity::kMedium;

  if (weather.has_value()) {
    // Extreme wind alone overrides the confidence-based tier entirely --
    // any confirmed detection under these conditions is a critical
    // situation regardless of how confident the detector itself was.
    if (weather->wind_speed_kmh >= thresholds.extreme_wind_kmh) return AlertSeverity::kCritical;

    // The classic fire-weather danger combination: escalate exactly one
    // level, not to an arbitrary fixed severity, so a low-confidence
    // detection under dangerous weather still reads as more urgent than
    // the same detection on a calm day, without pretending a low-confidence
    // detection is automatically as serious as a high-confidence one.
    if (weather->wind_speed_kmh >= thresholds.high_wind_kmh && weather->relative_humidity_pct <= thresholds.low_humidity_pct) {
      severity = Escalate(severity);
    }
  }

  return severity;
}

}  // namespace ridgeline
