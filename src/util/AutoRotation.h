#pragma once

#include <cmath>
#include <cstdint>

// Foreground-reader policy only. Samples are acceleration in g from HalTiltSensor;
// this class neither reads hardware nor records activity for the sleep timer.
class AutoRotation {
  static constexpr uint8_t NO_CANDIDATE = 0xFF;
  static constexpr uint32_t STABLE_MS = 500;
  static constexpr uint32_t MAX_SAMPLE_GAP_MS = 150;
  static constexpr float MIN_GRAVITY_SQUARED = 0.8f * 0.8f;
  static constexpr float MAX_GRAVITY_SQUARED = 1.2f * 1.2f;
  static constexpr float MIN_UPRIGHT_G = 0.65f;
  static constexpr float MAX_FACE_G = 0.75f;
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
    // The SDK's board frame defines gyro X as the portrait left/right rotation
    // axis, suggesting acceleration X runs along the portrait vertical axis.
    // Static signs have NOT been measured on an X3. Keep this assumed mapping
    // isolated for physical four-way validation: +X portrait, -Y clockwise,
    // -X inverted, +Y counterclockwise. The SDK's mount comment proves no signs.
    if (std::fabs(ax) > std::fabs(ay)) {
      return ax > 0.0f ? 0 : 2;
    }
    return ay < 0.0f ? 1 : 3;
  }

 public:
  void reset() {
    candidate_ = NO_CANDIDATE;
    haveSample_ = false;
    emitted_ = false;
  }

  // Orientation IDs match the renderer: portrait=0, CW=1, inverted=2, CCW=3.
  // Only a continuous, quiet 500 ms hold can produce a target. Invalid samples,
  // motion, or a gap over 150 ms discard the hold; uint32_t subtraction handles
  // millis() rollover. targetOrientation is unchanged unless true is returned.
  bool update(float ax, float ay, float az, uint32_t now, uint8_t currentOrientation, uint8_t& targetOrientation) {
    if (currentOrientation > 3 || !std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az)) {
      reset();
      return false;
    }

    const float gravitySquared = ax * ax + ay * ay + az * az;
    const float absX = std::fabs(ax);
    const float absY = std::fabs(ay);
    const float dominant = absX > absY ? absX : absY;
    const float other = absX > absY ? absY : absX;
    if (gravitySquared < MIN_GRAVITY_SQUARED || gravitySquared > MAX_GRAVITY_SQUARED || dominant < MIN_UPRIGHT_G ||
        std::fabs(az) > MAX_FACE_G || dominant < AXIS_DOMINANCE * other) {
      reset();
      return false;
    }

    if (haveSample_) {
      const float dx = ax - lastAx_;
      const float dy = ay - lastAy_;
      const float dz = az - lastAz_;
      if (static_cast<uint32_t>(now - lastSampleMs_) > MAX_SAMPLE_GAP_MS ||
          dx * dx + dy * dy + dz * dz > MAX_SAMPLE_STEP_SQUARED) {
        candidate_ = NO_CANDIDATE;
        emitted_ = false;
      }
    }
    lastAx_ = ax;
    lastAy_ = ay;
    lastAz_ = az;
    lastSampleMs_ = now;
    haveSample_ = true;

    const uint8_t orientation = orientationFor(ax, ay);
    if (orientation == currentOrientation) {
      candidate_ = NO_CANDIDATE;
      emitted_ = false;
      return false;
    }
    if (orientation != candidate_) {
      candidate_ = orientation;
      candidateSince_ = now;
      emitted_ = false;
      return false;
    }
    if (!emitted_ && static_cast<uint32_t>(now - candidateSince_) >= STABLE_MS) {
      targetOrientation = candidate_;
      emitted_ = true;
      return true;
    }
    return false;
  }
};
