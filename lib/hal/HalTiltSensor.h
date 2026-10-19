#pragma once

#include <Arduino.h>
#include <Imu.h>

// TODO: Move enums into new header and share with CrossPointSettings.h
namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

namespace CrossPointTiltPageTurn {
enum Value : uint8_t { TILT_OFF = 0, TILT_ON = 1 };
}

namespace CrossPointTiltPageTurnDirection {
enum Value : uint8_t {
  TILT_LEFT_RIGHT = 0,
  TILT_LEFT_RIGHT_INVERTED = 1,
  TILT_FORWARD_BACK = 2,
  TILT_FORWARD_BACK_INVERTED = 3
};
}

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;  // Singleton

class HalTiltSensor {
  bool _available = false;
  Imu _sdkImu;

  // Tilt gesture state machine
  bool _tiltForwardEvent = false;  // Consumed by wasTiltedForward()
  bool _tiltBackEvent = false;     // Consumed by wasTiltedBack()
  bool _hadActivity = false;       // Non-consuming flag for sleep timer
  bool _inTilt = false;            // Currently tilted past threshold
  bool _isAwake = false;           // Tracks power state
  unsigned long _initMs = 0;       // Timestamp of sensor init
  unsigned long _lastTiltMs = 0;   // Debounce / cooldown
  unsigned long _wakeMs = 0;       // Timestamp of last wake() for stabilization
  bool _tiltWasEnabled = false;    // Preserve gesture cooldown when auto-rotation keeps the IMU awake

  // The same poll supplies both consumers; acceleration is consumed once.
  bool _accelerationAvailable = false;
  float _ax = 0.0f;
  float _ay = 0.0f;
  float _az = 0.0f;

  // Tuning constants
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  static constexpr float RATE_THRESHOLD_DPS = 200.0f;  // Stay below the Sticky IMU's ±245 dps range
#else
  static constexpr float RATE_THRESHOLD_DPS = 270.0f;  // Deg/sec speed to trigger flick
#endif
  static constexpr float NEUTRAL_RATE_DPS = 50.0f;         // Must stop moving below this rate before next trigger
  static constexpr unsigned long COOLDOWN_MS = 600;        // Minimum ms between triggers
  static constexpr unsigned long POLL_INTERVAL_MS = 50;    // 20 Hz polling
  static constexpr unsigned long WAKE_STABILIZE_MS = 300;  // Ignore readings after wake

  unsigned long _lastPollMs = 0;
  static constexpr unsigned long ACCELERATION_MAX_AGE_MS = 150;

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // Enables motion polling state.
  bool wake();

  // Puts motion polling state to sleep.
  bool deepSleep();

  // True if an IMU is present on this device
  bool isAvailable() const { return _available; }

  // Poll one IMU sample for tilt gestures and/or reader auto-rotation.
  void update(uint8_t enabled, uint8_t direction, uint8_t orientation, bool inReader, bool autoRotateEnabled = false);

  // Consume the latest fresh acceleration sample in g. No additional sensor read.
  // False while auto-rotation is disabled, asleep, or after a failed/stale poll.
  bool takeAcceleration(float& ax, float& ay, float& az);

  // Returns true once per tilt-forward gesture (next page direction).
  // Consumed on read — subsequent calls return false until next gesture.
  bool wasTiltedForward();

  // Returns true once per tilt-back gesture (previous page direction).
  // Consumed on read.
  bool wasTiltedBack();

  // Non-consuming: true if any tilt activity occurred since last call.
  // Used to reset the auto-sleep inactivity timer.
  bool hadActivity();

  // Discard any pending tilt events (call when leaving reader or disabling tilt).
  void clearPendingEvents();
};
