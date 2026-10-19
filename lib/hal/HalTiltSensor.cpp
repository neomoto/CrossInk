#include "HalTiltSensor.h"

#include <Logging.h>

#include <cmath>

HalTiltSensor halTiltSensor;  // Singleton instance

void HalTiltSensor::begin() {
  _isAwake = false;
  _accelerationAvailable = false;
  _inTilt = false;
  clearPendingEvents();
  _tiltWasEnabled = false;
#ifdef FORCE_TILT_SENSOR_AVAILABLE
  _available = true;
  _isAwake = false;
  _initMs = millis();
  _lastPollMs = millis();
  LOG_INF("GYR", "Tilt sensor override active via build flag");
  return;
#endif

  _available = _sdkImu.begin();
  if (_available) {
    _initMs = millis();
    _lastPollMs = millis();
    // begin() leaves the sensors sampling; stand them by until a reader needs
    // tilt page turns or auto-rotation, so a disabled IMU doesn't drain the battery.
    if (!_sdkImu.sleep()) {
      LOG_ERR("GYR", "IMU standby failed");
    }
    LOG_INF("GYR", "SDK IMU initialized");
    return;
  }
  LOG_ERR("GYR", "SDK IMU not found");
}

bool HalTiltSensor::wake() {
  _accelerationAvailable = false;
  if (!_available) {
    return false;
  }

  if (!_sdkImu.wake()) {
    LOG_ERR("GYR", "IMU wake failed");
    return false;
  }

  _lastPollMs = millis();
  _lastTiltMs = millis();
  _wakeMs = millis();
  _isAwake = true;
  return true;
}

bool HalTiltSensor::deepSleep() {
  _accelerationAvailable = false;
  clearPendingEvents();
  _inTilt = false;
  _tiltWasEnabled = false;
  if (!_available) {
    return false;
  }

  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU sleep failed");
    return false;
  }

  _isAwake = false;
  return true;
}

void HalTiltSensor::update(const uint8_t enabled, const uint8_t direction, const uint8_t orientation,
                           const bool inReader, const bool autoRotateEnabled) {
  const bool tiltEnabled = (enabled != CrossPointTiltPageTurn::TILT_OFF) && inReader;
  if (tiltEnabled && !_tiltWasEnabled) {
    _lastTiltMs = millis();
  }
  _tiltWasEnabled = tiltEnabled;
  if (!tiltEnabled) {
    clearPendingEvents();
    _inTilt = false;
  }
  if (!autoRotateEnabled || !inReader) {
    _accelerationAvailable = false;
  }
  if (!_available) {
    _accelerationAvailable = false;
    return;
  }

  const bool shouldBeAwake = inReader && (tiltEnabled || autoRotateEnabled);

  // Share the existing power state and polling cadence between both features.
  if (shouldBeAwake && !_isAwake) {
    _isAwake = wake();
    return;
  } else if (!shouldBeAwake && _isAwake) {
    _isAwake = !deepSleep();
    return;
  }

  // If disabled or outside a reader, skip polling and avoid unnecessary I2C traffic.
  if (!shouldBeAwake) {
    return;
  }

  const unsigned long now = millis();
  // Stabilization: discard readings during gyro startup transient
  if ((now - _wakeMs) < WAKE_STABILIZE_MS) {
    return;
  }

  if ((now - _lastPollMs) < POLL_INTERVAL_MS) {
    return;
  }
  _lastPollMs = now;
  _accelerationAvailable = false;

  Imu::Sample sample;
  if (!_sdkImu.read(sample)) {
    return;
  }
  if (autoRotateEnabled) {
    _ax = sample.ax;
    _ay = sample.ay;
    _az = sample.az;
    _accelerationAvailable = true;
  }
  if (!tiltEnabled) {
    return;
  }
  const float gx = sample.gx;
  const float gy = sample.gy;

  // Map the gyro axis to the selected gesture based on reader orientation.
  // On the X3 PCB: X axis = left/right in portrait, Y axis = left/right in landscape.
  const bool forwardBack = direction == CrossPointTiltPageTurnDirection::TILT_FORWARD_BACK ||
                           direction == CrossPointTiltPageTurnDirection::TILT_FORWARD_BACK_INVERTED;
  const bool inverted = direction == CrossPointTiltPageTurnDirection::TILT_LEFT_RIGHT ||
                        direction == CrossPointTiltPageTurnDirection::TILT_FORWARD_BACK_INVERTED;
  float tiltAxis;
  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      tiltAxis = forwardBack ? gy : gx;
      break;
    case CrossPointOrientation::INVERTED:
      tiltAxis = forwardBack ? -gy : -gx;
      break;
    case CrossPointOrientation::LANDSCAPE_CW:
      tiltAxis = forwardBack ? gx : -gy;
      break;
    case CrossPointOrientation::LANDSCAPE_CCW:
      tiltAxis = forwardBack ? -gx : gy;
      break;
    default:
      tiltAxis = forwardBack ? gy : gx;
      break;
  }
  if (inverted) {
    tiltAxis = -tiltAxis;
  }

  if (_inTilt) {
    // Wait for device to return to neutral before allowing next trigger
    if (fabsf(tiltAxis) < NEUTRAL_RATE_DPS) {
      _inTilt = false;
    }
  } else {
    // Check for new tilt gesture (with cooldown)
    if ((now - _lastTiltMs) >= COOLDOWN_MS) {
      if (tiltAxis > RATE_THRESHOLD_DPS) {
        _tiltForwardEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Forward Trigger=(%.1f) dps", tiltAxis);
      } else if (tiltAxis < -RATE_THRESHOLD_DPS) {
        _tiltBackEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Backward Trigger=(%.1f) dps", tiltAxis);
      }
    }
  }
}

bool HalTiltSensor::takeAcceleration(float& ax, float& ay, float& az) {
  if (!_accelerationAvailable) {
    return false;
  }
  _accelerationAvailable = false;
  if (!_available || !_isAwake || (millis() - _lastPollMs) > ACCELERATION_MAX_AGE_MS) {
    return false;
  }
  ax = _ax;
  ay = _ay;
  az = _az;
  return true;
}

bool HalTiltSensor::wasTiltedForward() {
  const bool val = _tiltForwardEvent;
  _tiltForwardEvent = false;
  return val;
}

bool HalTiltSensor::wasTiltedBack() {
  const bool val = _tiltBackEvent;
  _tiltBackEvent = false;
  return val;
}

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}

void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _hadActivity = false;
  // Intentionally preserve _inTilt so a held tilt doesn't retrigger on next poll
}
