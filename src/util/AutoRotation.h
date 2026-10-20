#pragma once

#include <cmath>
#include <cstdint>

// Foreground-reader policy only. Samples are acceleration in g from HalTiltSensor;
// this class neither reads hardware nor records activity for the sleep timer.
class AutoRotation {
#if defined(CROSSINK_ROTATION_DIAGNOSTICS) && CROSSINK_ROTATION_DIAGNOSTICS
 public:
#endif
  enum class DiagnosticReason : uint8_t {
    Reset,
    InvalidCurrent,
    Nonfinite,
    Gravity,
    Flat,
    Diagonal,
    Current,
    Step,
    Gap,
    Settling,
    Emit,
    Emitted
  };
#if defined(CROSSINK_ROTATION_DIAGNOSTICS) && CROSSINK_ROTATION_DIAGNOSTICS
  struct DiagnosticSnapshot {
    DiagnosticReason reason = DiagnosticReason::Reset;
    uint8_t candidate = 0xFF;
    uint32_t stableMs = 0;
    bool emitted = false;
  };

  const DiagnosticSnapshot& diagnosticSnapshot() const { return diagnostic_; }
  static const char* diagnosticReasonName(const DiagnosticReason reason) {
    switch (reason) {
      case DiagnosticReason::Reset:
        return "reset";
      case DiagnosticReason::InvalidCurrent:
        return "invalid_current";
      case DiagnosticReason::Nonfinite:
        return "nonfinite";
      case DiagnosticReason::Gravity:
        return "gravity";
      case DiagnosticReason::Flat:
        return "flat";
      case DiagnosticReason::Diagonal:
        return "diagonal";
      case DiagnosticReason::Current:
        return "current";
      case DiagnosticReason::Step:
        return "step";
      case DiagnosticReason::Gap:
        return "gap";
      case DiagnosticReason::Settling:
        return "settling";
      case DiagnosticReason::Emit:
        return "emit";
      case DiagnosticReason::Emitted:
        return "emitted";
    }
    return "reset";
  }

 private:
  DiagnosticSnapshot diagnostic_;
#endif

  static constexpr uint8_t NO_CANDIDATE = 0xFF;
  static constexpr uint32_t STABLE_MS = 500;
  static constexpr uint32_t MAX_SAMPLE_GAP_MS = 150;
  static constexpr float MIN_GRAVITY_SQUARED = 0.8f * 0.8f;
  static constexpr float MAX_GRAVITY_SQUARED = 1.2f * 1.2f;
  // Require 40% of gravity in the display plane (~24 degrees above horizontal).
  // A ratio keeps the posture decision independent of plausible gravity scale.
  static constexpr float MIN_PLANE_GRAVITY_RATIO_SQUARED = 0.4f * 0.4f;
  static constexpr float AXIS_DOMINANCE = 1.5f;
  static constexpr float MAX_SAMPLE_STEP_SQUARED = 0.2f * 0.2f;

  uint32_t candidateSince_ = 0;
  uint32_t lastSampleMs_ = 0;
  float lastAx_ = 0.0f;
  float lastAy_ = 0.0f;
  float lastAz_ = 0.0f;
  uint8_t candidate_ = NO_CANDIDATE;
  bool haveSample_ = false;
  bool emitted_ = false;

  static uint8_t orientationFor(float ax, float ay) {
    // Canonical classifier coordinates: +X portrait, -Y clockwise,
    // -X inverted, +Y counterclockwise. ReaderUtils adapts raw X3 samples
    // to this frame; other device profiles keep their existing inputs.
    if (std::fabs(ax) > std::fabs(ay)) {
      return ax > 0.0f ? 0 : 2;
    }
    return ay < 0.0f ? 1 : 3;
  }

  static bool isDiagonal(float ax, float ay) {
    const float absX = std::fabs(ax);
    const float absY = std::fabs(ay);
    if (absX > absY) {
      return absX < AXIS_DOMINANCE * absY;
    }
    return absY < AXIS_DOMINANCE * absX;
  }

  static DiagnosticReason classifySample(float ax, float ay, float az, uint8_t currentOrientation) {
    if (currentOrientation > 3) {
      return DiagnosticReason::InvalidCurrent;
    }
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az)) {
      return DiagnosticReason::Nonfinite;
    }
    const float planeGravitySquared = ax * ax + ay * ay;
    const float gravitySquared = planeGravitySquared + az * az;
    if (gravitySquared < MIN_GRAVITY_SQUARED || gravitySquared > MAX_GRAVITY_SQUARED) {
      return DiagnosticReason::Gravity;
    }
    if (planeGravitySquared < MIN_PLANE_GRAVITY_RATIO_SQUARED * gravitySquared) {
      return DiagnosticReason::Flat;
    }
    if (isDiagonal(ax, ay)) {
      return DiagnosticReason::Diagonal;
    }
    return DiagnosticReason::Settling;
  }

  DiagnosticReason continuityFor(float ax, float ay, float az, uint32_t now) const {
    if (!haveSample_) {
      return DiagnosticReason::Settling;
    }
    if (static_cast<uint32_t>(now - lastSampleMs_) > MAX_SAMPLE_GAP_MS) {
      return DiagnosticReason::Gap;
    }
    const float dx = ax - lastAx_;
    const float dy = ay - lastAy_;
    const float dz = az - lastAz_;
    if (dx * dx + dy * dy + dz * dz > MAX_SAMPLE_STEP_SQUARED) {
      return DiagnosticReason::Step;
    }
    return DiagnosticReason::Settling;
  }

 public:
  void reset() {
    candidate_ = NO_CANDIDATE;
    haveSample_ = false;
    emitted_ = false;
#if defined(CROSSINK_ROTATION_DIAGNOSTICS) && CROSSINK_ROTATION_DIAGNOSTICS
    diagnostic_ = {};
#endif
  }

  // Orientation IDs match the renderer: portrait=0, CW=1, inverted=2, CCW=3.
  // Only a continuous, quiet 500 ms hold can produce a target. Invalid samples,
  // motion, or a gap over 150 ms discard the hold; uint32_t subtraction handles
  // millis() rollover. targetOrientation is unchanged unless true is returned.
  bool update(float ax, float ay, float az, uint32_t now, uint8_t currentOrientation, uint8_t& targetOrientation) {
#if defined(CROSSINK_ROTATION_DIAGNOSTICS) && CROSSINK_ROTATION_DIAGNOSTICS
    diagnostic_ = {DiagnosticReason::Settling, NO_CANDIDATE, 0, false};
#endif
    const auto rejection = classifySample(ax, ay, az, currentOrientation);
    if (rejection != DiagnosticReason::Settling) {
      reset();
#if defined(CROSSINK_ROTATION_DIAGNOSTICS) && CROSSINK_ROTATION_DIAGNOSTICS
      diagnostic_.reason = rejection;
#endif
      return false;
    }

    const auto continuity = continuityFor(ax, ay, az, now);
    if (continuity != DiagnosticReason::Settling) {
      candidate_ = NO_CANDIDATE;
      emitted_ = false;
#if defined(CROSSINK_ROTATION_DIAGNOSTICS) && CROSSINK_ROTATION_DIAGNOSTICS
      diagnostic_.reason = continuity;
#endif
    }
    lastAx_ = ax;
    lastAy_ = ay;
    lastAz_ = az;
    lastSampleMs_ = now;
    haveSample_ = true;

    const uint8_t orientation = orientationFor(ax, ay);
#if defined(CROSSINK_ROTATION_DIAGNOSTICS) && CROSSINK_ROTATION_DIAGNOSTICS
    diagnostic_.candidate = orientation;
#endif
    if (orientation == currentOrientation) {
      candidate_ = NO_CANDIDATE;
      emitted_ = false;
#if defined(CROSSINK_ROTATION_DIAGNOSTICS) && CROSSINK_ROTATION_DIAGNOSTICS
      diagnostic_.reason = DiagnosticReason::Current;
#endif
      return false;
    }
    if (orientation != candidate_) {
      candidate_ = orientation;
      candidateSince_ = now;
      emitted_ = false;
      return false;
    }
    if (emitted_) {
#if defined(CROSSINK_ROTATION_DIAGNOSTICS) && CROSSINK_ROTATION_DIAGNOSTICS
      diagnostic_.stableMs = static_cast<uint32_t>(now - candidateSince_);
      diagnostic_.emitted = true;
      diagnostic_.reason = DiagnosticReason::Emitted;
#endif
      return false;
    }
    const uint32_t stableMs = static_cast<uint32_t>(now - candidateSince_);
#if defined(CROSSINK_ROTATION_DIAGNOSTICS) && CROSSINK_ROTATION_DIAGNOSTICS
    diagnostic_.stableMs = stableMs;
#endif
    if (stableMs >= STABLE_MS) {
      targetOrientation = candidate_;
      emitted_ = true;
#if defined(CROSSINK_ROTATION_DIAGNOSTICS) && CROSSINK_ROTATION_DIAGNOSTICS
      diagnostic_.reason = DiagnosticReason::Emit;
      diagnostic_.emitted = true;
#endif
      return true;
    }
    return false;
  }
};
