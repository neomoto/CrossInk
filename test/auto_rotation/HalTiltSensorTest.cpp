#include <gtest/gtest.h>

#include "HalTiltSensor.h"

namespace {
class HalTiltSensorTest : public ::testing::Test {
 protected:
  HalTiltSensor sensor;

  void SetUp() override {
    Imu::reset();
    TestClock::nowMs = 0;
    sensor.begin();
  }

  void poll(unsigned long now, bool tilt, bool autoRotate, bool inReader = true,
            uint8_t direction = CrossPointTiltPageTurnDirection::TILT_LEFT_RIGHT_INVERTED,
            uint8_t orientation = CrossPointOrientation::PORTRAIT) {
    TestClock::nowMs = now;
    sensor.update(tilt ? CrossPointTiltPageTurn::TILT_ON : CrossPointTiltPageTurn::TILT_OFF, direction, orientation,
                  inReader, autoRotate);
  }

  bool takeAcceleration() {
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    return sensor.takeAcceleration(ax, ay, az);
  }
};
}  // namespace

TEST_F(HalTiltSensorTest, StartsInStandbyAndDoesNotPollOutsideAnEnabledReader) {
  ASSERT_TRUE(sensor.isAvailable());
  EXPECT_EQ(Imu::beginCalls, 1U);
  EXPECT_EQ(Imu::sleepCalls, 1U);
  poll(0, false, false);
  poll(1000, false, false);
  poll(2000, true, true, false);
  EXPECT_EQ(Imu::wakeCalls, 0U);
  EXPECT_EQ(Imu::readCalls, 0U);
  EXPECT_FALSE(takeAcceleration());
  EXPECT_FALSE(sensor.hadActivity());
}

TEST_F(HalTiltSensorTest, AutoRotationAloneWakesAndSamplesWithoutPageTurnsOrActivity) {
  Imu::nextSample = {0.0f, -1.0f, 0.0f, 400.0f, -400.0f, 400.0f};
  poll(0, false, true);
  EXPECT_EQ(Imu::wakeCalls, 1U);
  poll(299, false, true);
  EXPECT_EQ(Imu::readCalls, 0U);
  poll(300, false, true);
  EXPECT_EQ(Imu::readCalls, 1U);
  EXPECT_TRUE(takeAcceleration());
  poll(600, false, true);
  EXPECT_TRUE(takeAcceleration());
  EXPECT_FALSE(sensor.wasTiltedForward());
  EXPECT_FALSE(sensor.wasTiltedBack());
  EXPECT_FALSE(sensor.hadActivity());
  EXPECT_EQ(Imu::sleepCalls, 1U);
}

TEST_F(HalTiltSensorTest, StationaryAutoRotationPollingNeverBecomesSleepActivity) {
  poll(0, false, true);
  for (unsigned long now = 300; now <= 3000; now += 50) {
    poll(now, false, true);
    EXPECT_TRUE(takeAcceleration());
    EXPECT_FALSE(sensor.hadActivity());
  }
}

TEST_F(HalTiltSensorTest, TiltAlonePreservesPageTurnsAndDoesNotExposeAcceleration) {
  poll(0, true, false);
  poll(300, true, false);
  EXPECT_FALSE(takeAcceleration());
  Imu::nextSample.gx = 300.0f;
  poll(600, true, false);
  EXPECT_TRUE(sensor.wasTiltedForward());
  EXPECT_FALSE(sensor.wasTiltedForward());
  EXPECT_FALSE(sensor.wasTiltedBack());
  EXPECT_TRUE(sensor.hadActivity());
  EXPECT_FALSE(sensor.hadActivity());
  EXPECT_FALSE(takeAcceleration());

  // A held flick must return to neutral before triggering again.
  poll(1200, true, false);
  EXPECT_FALSE(sensor.wasTiltedForward());
  Imu::nextSample.gx = 0.0f;
  poll(1250, true, false);
  Imu::nextSample.gx = -300.0f;
  poll(1300, true, false);
  EXPECT_TRUE(sensor.wasTiltedBack());
}

TEST_F(HalTiltSensorTest, BothFeaturesShareOneReadAndTheExisting50msCadence) {
  poll(0, true, true);
  poll(299, true, true);
  EXPECT_EQ(Imu::readCalls, 0U);
  poll(300, true, true);
  EXPECT_EQ(Imu::readCalls, 1U);
  poll(349, true, true);
  EXPECT_EQ(Imu::readCalls, 1U);
  EXPECT_TRUE(takeAcceleration());
  poll(350, true, true);
  EXPECT_EQ(Imu::readCalls, 2U);

  Imu::nextSample = {0.1f, -0.98f, 0.05f, 300.0f, 0.0f, 0.0f};
  poll(600, true, true);
  EXPECT_EQ(Imu::readCalls, 3U);
  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  ASSERT_TRUE(sensor.takeAcceleration(ax, ay, az));
  EXPECT_FLOAT_EQ(ax, 0.1f);
  EXPECT_FLOAT_EQ(ay, -0.98f);
  EXPECT_FLOAT_EQ(az, 0.05f);
  EXPECT_FALSE(takeAcceleration());
  EXPECT_EQ(Imu::readCalls, 3U);
  EXPECT_TRUE(sensor.wasTiltedForward());
  EXPECT_TRUE(sensor.hadActivity());
}

TEST_F(HalTiltSensorTest, DisablingTiltClearsGesturesWithoutSleepingAnAutoRotationReader) {
  poll(0, true, true);
  Imu::nextSample.gx = 300.0f;
  poll(600, true, true);
  poll(601, false, true);
  EXPECT_EQ(Imu::wakeCalls, 1U);
  EXPECT_EQ(Imu::sleepCalls, 1U);
  EXPECT_FALSE(sensor.wasTiltedForward());
  EXPECT_FALSE(sensor.wasTiltedBack());
  EXPECT_FALSE(sensor.hadActivity());
  EXPECT_TRUE(takeAcceleration());
  poll(650, false, true);
  EXPECT_FALSE(sensor.wasTiltedForward());
  EXPECT_FALSE(sensor.hadActivity());
}

TEST_F(HalTiltSensorTest, EnablingTiltWhileAutoRotationIsAwakeRetainsTheGestureCooldown) {
  poll(0, false, true);
  poll(300, false, true);
  Imu::nextSample.gx = 300.0f;
  poll(600, true, true);
  EXPECT_FALSE(sensor.wasTiltedForward());
  poll(1150, true, true);
  EXPECT_FALSE(sensor.wasTiltedForward());
  poll(1200, true, true);
  EXPECT_TRUE(sensor.wasTiltedForward());
  EXPECT_EQ(Imu::wakeCalls, 1U);
}

TEST_F(HalTiltSensorTest, DisablingAutoRotationInvalidatesItsCacheButKeepsTiltAwake) {
  poll(0, true, true);
  poll(300, true, true);
  poll(301, true, false);
  EXPECT_FALSE(takeAcceleration());
  EXPECT_EQ(Imu::sleepCalls, 1U);
  EXPECT_EQ(Imu::wakeCalls, 1U);
  poll(350, true, false);
  EXPECT_EQ(Imu::readCalls, 2U);
  EXPECT_FALSE(takeAcceleration());
}

TEST_F(HalTiltSensorTest, LeavingTheReaderSleepsAndDiscardsCachedAndPendingInput) {
  poll(0, true, true);
  Imu::nextSample.gx = 300.0f;
  poll(600, true, true);
  poll(601, true, true, false);
  EXPECT_EQ(Imu::sleepCalls, 2U);
  EXPECT_FALSE(takeAcceleration());
  EXPECT_FALSE(sensor.wasTiltedForward());
  EXPECT_FALSE(sensor.hadActivity());
  poll(1000, true, true, false);
  EXPECT_EQ(Imu::readCalls, 1U);
  poll(1001, false, true);
  EXPECT_EQ(Imu::wakeCalls, 2U);
  poll(1300, false, true);
  EXPECT_EQ(Imu::readCalls, 1U);
  poll(1301, false, true);
  EXPECT_EQ(Imu::readCalls, 2U);
}

TEST_F(HalTiltSensorTest, DisablingBothFeaturesSleepsAndDiscardsCachedAcceleration) {
  poll(0, false, true);
  poll(300, false, true);
  poll(301, false, false);
  EXPECT_EQ(Imu::sleepCalls, 2U);
  EXPECT_FALSE(takeAcceleration());
  poll(1000, false, false);
  EXPECT_EQ(Imu::readCalls, 1U);
}

TEST_F(HalTiltSensorTest, FailedPollCannotReturnThePreviousUntakenSample) {
  poll(0, false, true);
  poll(300, false, true);
  Imu::readResult = false;
  poll(350, false, true);
  float ax = 9.0f, ay = 8.0f, az = 7.0f;
  EXPECT_FALSE(sensor.takeAcceleration(ax, ay, az));
  EXPECT_FLOAT_EQ(ax, 9.0f);
  EXPECT_FLOAT_EQ(ay, 8.0f);
  EXPECT_FLOAT_EQ(az, 7.0f);
  Imu::readResult = true;
  Imu::nextSample = {-1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  poll(400, false, true);
  ASSERT_TRUE(sensor.takeAcceleration(ax, ay, az));
  EXPECT_FLOAT_EQ(ax, -1.0f);
  EXPECT_EQ(Imu::readCalls, 3U);
}

TEST_F(HalTiltSensorTest, UntakenSamplesExpireWithoutAnyExtraHardwareRead) {
  poll(0, false, true);
  poll(300, false, true);
  TestClock::nowMs = 451;
  EXPECT_FALSE(takeAcceleration());
  EXPECT_FALSE(takeAcceleration());
  EXPECT_EQ(Imu::readCalls, 1U);
}

TEST_F(HalTiltSensorTest, ClearingTiltInputDoesNotConsumeTheIndependentAccelerationSample) {
  poll(0, true, true);
  poll(600, true, true);
  sensor.clearPendingEvents();
  EXPECT_TRUE(takeAcceleration());
  EXPECT_FALSE(sensor.hadActivity());
}

TEST_F(HalTiltSensorTest, SleepFailuresStillDiscardCacheAndNeverPollWhileDisabled) {
  poll(0, false, true);
  poll(300, false, true);
  Imu::sleepResult = false;
  poll(301, false, false);
  EXPECT_FALSE(takeAcceleration());
  poll(500, false, false);
  EXPECT_EQ(Imu::readCalls, 1U);
  EXPECT_EQ(Imu::sleepCalls, 3U);
  Imu::sleepResult = true;
  poll(501, false, false);
  EXPECT_EQ(Imu::sleepCalls, 4U);
}

TEST_F(HalTiltSensorTest, WakeFailuresCannotExposeAccelerationAndRetryWithoutPolling) {
  Imu::wakeResult = false;
  poll(0, false, true);
  poll(300, false, true);
  EXPECT_EQ(Imu::wakeCalls, 2U);
  EXPECT_EQ(Imu::readCalls, 0U);
  EXPECT_FALSE(takeAcceleration());
  Imu::wakeResult = true;
  poll(350, false, true);
  poll(649, false, true);
  EXPECT_EQ(Imu::readCalls, 0U);
  poll(650, false, true);
  EXPECT_EQ(Imu::readCalls, 1U);
  EXPECT_TRUE(takeAcceleration());
}

TEST_F(HalTiltSensorTest, UnavailableSensorCannotWakePollOrReturnAcceleration) {
  Imu::beginResult = false;
  sensor.begin();
  EXPECT_FALSE(sensor.isAvailable());
  poll(0, true, true);
  poll(1000, true, true);
  EXPECT_EQ(Imu::wakeCalls, 0U);
  EXPECT_EQ(Imu::readCalls, 0U);
  EXPECT_FALSE(takeAcceleration());
}

TEST(HalTiltSensorMapping, RetainsExistingGyroAxesAndSignsForEveryReaderOrientation) {
  struct Gesture {
    uint8_t orientation;
    uint8_t direction;
    float gx;
    float gy;
  };
  const Gesture gestures[] = {
      {CrossPointOrientation::PORTRAIT, CrossPointTiltPageTurnDirection::TILT_LEFT_RIGHT_INVERTED, 300.0f, 0.0f},
      {CrossPointOrientation::INVERTED, CrossPointTiltPageTurnDirection::TILT_LEFT_RIGHT_INVERTED, -300.0f, 0.0f},
      {CrossPointOrientation::LANDSCAPE_CW, CrossPointTiltPageTurnDirection::TILT_LEFT_RIGHT_INVERTED, 0.0f, -300.0f},
      {CrossPointOrientation::LANDSCAPE_CCW, CrossPointTiltPageTurnDirection::TILT_LEFT_RIGHT_INVERTED, 0.0f, 300.0f},
      {CrossPointOrientation::PORTRAIT, CrossPointTiltPageTurnDirection::TILT_FORWARD_BACK, 0.0f, 300.0f},
      {CrossPointOrientation::INVERTED, CrossPointTiltPageTurnDirection::TILT_FORWARD_BACK, 0.0f, -300.0f},
      {CrossPointOrientation::LANDSCAPE_CW, CrossPointTiltPageTurnDirection::TILT_FORWARD_BACK, 300.0f, 0.0f},
      {CrossPointOrientation::LANDSCAPE_CCW, CrossPointTiltPageTurnDirection::TILT_FORWARD_BACK, -300.0f, 0.0f},
  };
  for (const auto& gesture : gestures) {
    Imu::reset();
    TestClock::nowMs = 0;
    HalTiltSensor sensor;
    sensor.begin();
    sensor.update(CrossPointTiltPageTurn::TILT_ON, gesture.direction, gesture.orientation, true, false);
    Imu::nextSample.gx = gesture.gx;
    Imu::nextSample.gy = gesture.gy;
    TestClock::nowMs = 600;
    sensor.update(CrossPointTiltPageTurn::TILT_ON, gesture.direction, gesture.orientation, true, false);
    EXPECT_TRUE(sensor.wasTiltedForward());
    EXPECT_FALSE(sensor.wasTiltedBack());
  }
}
