#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "util/AutoRotation.h"

namespace {
struct Gravity {
  float ax;
  float ay;
  float az;
};

// These encode the documented, not yet hardware-measured X3 sign assumption.
constexpr std::array<Gravity, 4> ORIENTATION_SAMPLES = {
    {{1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}};

bool sample(AutoRotation& policy, const Gravity& gravity, uint32_t now, uint8_t current, uint8_t& target) {
  return policy.update(gravity.ax, gravity.ay, gravity.az, now, current, target);
}

Gravity readingPose(uint8_t orientation, float elevationDegrees, float faceSign = 1.0f, float scale = 1.0f) {
  constexpr float RADIANS_PER_DEGREE = 0.017453292519943295f;
  const float elevation = elevationDegrees * RADIANS_PER_DEGREE;
  const float planeGravity = scale * std::sin(elevation);
  const auto& direction = ORIENTATION_SAMPLES[orientation];
  return {direction.ax * planeGravity, direction.ay * planeGravity, faceSign * scale * std::cos(elevation)};
}

void expectHoldBefore(AutoRotation& policy, const Gravity& gravity, uint32_t start, uint8_t current, uint8_t& target) {
  for (uint32_t elapsed = 0; elapsed < 500; elapsed += 50) {
    EXPECT_FALSE(sample(policy, gravity, start + elapsed, current, target)) << elapsed;
  }
}
}  // namespace

TEST(AutoRotation, ConfirmsEachOfTheFourOrientationsOnlyAfterHalfASecond) {
  for (uint8_t orientation = 0; orientation < ORIENTATION_SAMPLES.size(); ++orientation) {
    SCOPED_TRACE(orientation);
    AutoRotation policy;
    uint8_t target = 0xFF;
    const uint8_t current = (orientation + 1) % 4;

    expectHoldBefore(policy, ORIENTATION_SAMPLES[orientation], 1000, current, target);
    EXPECT_EQ(target, 0xFF);
    ASSERT_TRUE(sample(policy, ORIENTATION_SAMPLES[orientation], 1500, current, target));
    EXPECT_EQ(target, orientation);
  }
}

TEST(AutoRotation, DoesNotEmitTheCurrentOrientation) {
  for (uint8_t orientation = 0; orientation < ORIENTATION_SAMPLES.size(); ++orientation) {
    AutoRotation policy;
    uint8_t target = 0xFF;
    for (uint32_t now = 0; now <= 1500; now += 50) {
      EXPECT_FALSE(sample(policy, ORIENTATION_SAMPLES[orientation], now, orientation, target));
    }
    EXPECT_EQ(target, 0xFF);
  }
}

TEST(AutoRotation, EmitsOnlyOnceForAHeldCandidateEvenBeforeTheCallerAppliesIt) {
  AutoRotation policy;
  uint8_t target = 0xFF;
  expectHoldBefore(policy, ORIENTATION_SAMPLES[1], 0, 0, target);
  ASSERT_TRUE(sample(policy, ORIENTATION_SAMPLES[1], 500, 0, target));
  ASSERT_EQ(target, 1);

  for (uint32_t now = 550; now <= 1500; now += 50) {
    EXPECT_FALSE(sample(policy, ORIENTATION_SAMPLES[1], now, 0, target));
  }
  for (uint32_t now = 1550; now <= 2500; now += 50) {
    EXPECT_FALSE(sample(policy, ORIENTATION_SAMPLES[1], now, 1, target));
  }
}

TEST(AutoRotation, CanRotateThroughAllFourOrientationsAsTheCallerAppliesEachTarget) {
  AutoRotation policy;
  uint8_t current = 0;
  uint8_t target = 0xFF;
  uint32_t start = 0;
  for (uint8_t orientation : {1, 2, 3, 0}) {
    expectHoldBefore(policy, ORIENTATION_SAMPLES[orientation], start, current, target);
    ASSERT_TRUE(sample(policy, ORIENTATION_SAMPLES[orientation], start + 500, current, target));
    EXPECT_EQ(target, orientation);
    current = target;
    start += 550;
  }
}

TEST(AutoRotation, ConfirmsAllFourOrientationsAtThirtyDegreeReadingElevation) {
  // At 30 degrees above horizontal, half the gravity lies in the display plane
  // and sqrt(0.75) lies along its normal. Either face-normal sign is supported.
  for (uint8_t orientation = 0; orientation < ORIENTATION_SAMPLES.size(); ++orientation) {
    for (const float faceSign : {-1.0f, 1.0f}) {
      SCOPED_TRACE(orientation);
      SCOPED_TRACE(faceSign);
      const Gravity gravity = readingPose(orientation, 30.0f, faceSign);
      AutoRotation policy;
      uint8_t target = 0xFF;
      const uint8_t current = (orientation + 1) % 4;

      expectHoldBefore(policy, gravity, 1000, current, target);
      EXPECT_EQ(target, 0xFF);
      ASSERT_TRUE(sample(policy, gravity, 1500, current, target));
      EXPECT_EQ(target, orientation);
      for (uint32_t now = 1550; now <= 2000; now += 50) {
        EXPECT_FALSE(sample(policy, gravity, now, current, target));
      }
      for (uint32_t now = 2050; now <= 3000; now += 50) {
        EXPECT_FALSE(sample(policy, gravity, now, orientation, target));
      }
      EXPECT_EQ(target, orientation);
    }
  }
}

TEST(AutoRotation, AllowsSmallPitchWiggleAroundThirtyDegreeReadingElevation) {
  for (uint8_t orientation = 0; orientation < ORIENTATION_SAMPLES.size(); ++orientation) {
    for (const float faceSign : {-1.0f, 1.0f}) {
      SCOPED_TRACE(orientation);
      SCOPED_TRACE(faceSign);
      AutoRotation policy;
      uint8_t target = 0xFF;
      const uint8_t current = (orientation + 1) % 4;
      for (uint32_t now = 0; now < 500; now += 50) {
        const float elevation = (now / 50) % 2 ? 32.0f : 28.0f;
        EXPECT_FALSE(sample(policy, readingPose(orientation, elevation, faceSign), now, current, target));
      }
      EXPECT_EQ(target, 0xFF);
      ASSERT_TRUE(sample(policy, readingPose(orientation, 30.0f, faceSign), 500, current, target));
      EXPECT_EQ(target, orientation);
    }
  }
}

TEST(AutoRotation, ReadingPostureDoesNotDependOnPlausibleGravityScale) {
  for (const float elevation : {30.0f, 45.0f}) {
    for (const float scale : {0.85f, 1.0f, 1.15f}) {
      for (const float faceSign : {-1.0f, 1.0f}) {
        SCOPED_TRACE(elevation);
        SCOPED_TRACE(scale);
        SCOPED_TRACE(faceSign);
        AutoRotation policy;
        uint8_t target = 0xFF;
        const Gravity gravity = readingPose(1, elevation, faceSign, scale);
        expectHoldBefore(policy, gravity, 0, 0, target);
        EXPECT_EQ(target, 0xFF);
        ASSERT_TRUE(sample(policy, gravity, 500, 0, target));
        EXPECT_EQ(target, 1);
      }
    }
  }
}

TEST(AutoRotation, RefusesTableFlatAndNearFlatJitter) {
  for (const float elevation : {0.0f, 15.0f}) {
    for (const float faceSign : {-1.0f, 1.0f}) {
      SCOPED_TRACE(elevation);
      SCOPED_TRACE(faceSign);
      AutoRotation policy;
      uint8_t target = 0xFF;
      for (uint32_t now = 0; now <= 1500; now += 50) {
        const float pitchJitter = (now / 50) % 2 ? 2.0f : -2.0f;
        EXPECT_FALSE(sample(policy, readingPose(1, elevation + pitchJitter, faceSign), now, 0, target));
      }
      EXPECT_EQ(target, 0xFF);
    }
  }
}

TEST(AutoRotation, RejectsAmbiguousDiagonalSamplesInEveryQuadrant) {
  for (float ax : {-0.707f, 0.707f}) {
    for (float ay : {-0.707f, 0.707f}) {
      AutoRotation policy;
      uint8_t target = 0xFF;
      for (uint32_t now = 0; now <= 1500; now += 50) {
        EXPECT_FALSE(policy.update(ax, ay, 0.0f, now, 0, target));
      }
      EXPECT_EQ(target, 0xFF);
    }
  }
}

TEST(AutoRotation, AcceptsSmallRealisticNoiseAndAnAngledReadingPosture) {
  AutoRotation policy;
  uint8_t target = 0xFF;
  for (uint32_t now = 0; now < 500; now += 50) {
    const float noise = (now / 50) % 2 ? 0.02f : -0.02f;
    EXPECT_FALSE(policy.update(noise, -0.9f + noise, 0.4f, now, 0, target));
  }
  ASSERT_TRUE(policy.update(0.0f, -0.9f, 0.4f, 500, 0, target));
  EXPECT_EQ(target, 1);
}

TEST(AutoRotation, RejectsRapidNoiseEvenWhenEverySampleHasPlausibleGravity) {
  AutoRotation policy;
  uint8_t target = 0xFF;
  for (uint32_t now = 0; now <= 2000; now += 50) {
    const float noise = (now / 50) % 2 ? 0.18f : -0.18f;
    EXPECT_FALSE(policy.update(noise, -0.98f, 0.0f, now, 0, target));
  }
  EXPECT_EQ(target, 0xFF);
}

TEST(AutoRotation, RejectsFreefallAndShakingMagnitude) {
  constexpr std::array<Gravity, 5> moving = {
      {{0.0f, 0.0f, 0.0f}, {0.0f, -0.3f, 0.0f}, {0.0f, -1.21f, 0.0f}, {0.0f, -1.8f, 0.0f}, {1.0f, -1.0f, 1.0f}}};
  for (const auto& gravity : moving) {
    AutoRotation policy;
    uint8_t target = 0xFF;
    for (uint32_t now = 0; now <= 1500; now += 50) {
      EXPECT_FALSE(sample(policy, gravity, now, 0, target));
    }
    EXPECT_EQ(target, 0xFF);
  }
}

TEST(AutoRotation, RejectsNonfiniteValuesOnAnyAxisAndDiscardsTheEarlierHold) {
  const std::array<float, 3> invalid = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                        -std::numeric_limits<float>::infinity()};
  for (const float value : invalid) {
    for (int axis = 0; axis < 3; ++axis) {
      AutoRotation policy;
      uint8_t target = 0xFF;
      expectHoldBefore(policy, ORIENTATION_SAMPLES[1], 0, 0, target);
      Gravity bad = ORIENTATION_SAMPLES[1];
      if (axis == 0) bad.ax = value;
      if (axis == 1) bad.ay = value;
      if (axis == 2) bad.az = value;
      EXPECT_FALSE(sample(policy, bad, 500, 0, target));
      expectHoldBefore(policy, ORIENTATION_SAMPLES[1], 550, 0, target);
      EXPECT_EQ(target, 0xFF);
      ASSERT_TRUE(sample(policy, ORIENTATION_SAMPLES[1], 1050, 0, target));
      EXPECT_EQ(target, 1);
    }
  }
}

TEST(AutoRotation, InvalidFlatOrDiagonalSamplesRestartTheHold) {
  constexpr std::array<Gravity, 3> interruptions = {{{0.0f, 0.0f, 1.0f}, {0.707f, -0.707f, 0.0f}, {0.0f, -1.5f, 0.0f}}};
  for (const auto& interruption : interruptions) {
    AutoRotation policy;
    uint8_t target = 0xFF;
    expectHoldBefore(policy, ORIENTATION_SAMPLES[1], 0, 0, target);
    EXPECT_FALSE(sample(policy, interruption, 500, 0, target));
    expectHoldBefore(policy, ORIENTATION_SAMPLES[1], 550, 0, target);
    EXPECT_EQ(target, 0xFF);
    EXPECT_TRUE(sample(policy, ORIENTATION_SAMPLES[1], 1050, 0, target));
  }
}

TEST(AutoRotation, BounceToAnotherOrientationRestartsTheHold) {
  AutoRotation policy;
  uint8_t target = 0xFF;
  expectHoldBefore(policy, ORIENTATION_SAMPLES[1], 0, 0, target);
  EXPECT_FALSE(sample(policy, ORIENTATION_SAMPLES[3], 500, 0, target));
  expectHoldBefore(policy, ORIENTATION_SAMPLES[1], 550, 0, target);
  EXPECT_EQ(target, 0xFF);
  EXPECT_TRUE(sample(policy, ORIENTATION_SAMPLES[1], 1050, 0, target));
}

TEST(AutoRotation, ResetDiscardsCandidateAndPreviouslyEmittedState) {
  AutoRotation policy;
  uint8_t target = 0xFF;
  expectHoldBefore(policy, ORIENTATION_SAMPLES[1], 0, 0, target);
  policy.reset();
  expectHoldBefore(policy, ORIENTATION_SAMPLES[1], 500, 0, target);
  ASSERT_TRUE(sample(policy, ORIENTATION_SAMPLES[1], 1000, 0, target));
  policy.reset();
  expectHoldBefore(policy, ORIENTATION_SAMPLES[1], 1050, 0, target);
  EXPECT_TRUE(sample(policy, ORIENTATION_SAMPLES[1], 1550, 0, target));
}

TEST(AutoRotation, MissingSamplesLongerThan150msCannotFinishAnOldHold) {
  AutoRotation policy;
  uint8_t target = 0xFF;
  expectHoldBefore(policy, ORIENTATION_SAMPLES[1], 0, 0, target);
  expectHoldBefore(policy, ORIENTATION_SAMPLES[1], 601, 0, target);
  EXPECT_EQ(target, 0xFF);
  EXPECT_TRUE(sample(policy, ORIENTATION_SAMPLES[1], 1101, 0, target));
}

TEST(AutoRotation, AllowsA150msGapWithoutDiscardingAValidHold) {
  AutoRotation policy;
  uint8_t target = 0xFF;
  for (uint32_t now : {0U, 150U, 300U, 450U}) {
    EXPECT_FALSE(sample(policy, ORIENTATION_SAMPLES[1], now, 0, target));
  }
  EXPECT_TRUE(sample(policy, ORIENTATION_SAMPLES[1], 500, 0, target));
  EXPECT_EQ(target, 1);
}

TEST(AutoRotation, RepeatedTimestampsCannotAdvanceTheStabilityTimer) {
  AutoRotation policy;
  uint8_t target = 0xFF;
  for (int i = 0; i < 30; ++i) {
    EXPECT_FALSE(sample(policy, ORIENTATION_SAMPLES[1], 1000, 0, target));
  }
  EXPECT_EQ(target, 0xFF);
}

TEST(AutoRotation, MaintainsTheHoldAcrossUint32MillisRollover) {
  AutoRotation policy;
  uint8_t target = 0xFF;
  constexpr uint32_t start = std::numeric_limits<uint32_t>::max() - 249;
  expectHoldBefore(policy, ORIENTATION_SAMPLES[1], start, 0, target);
  EXPECT_TRUE(sample(policy, ORIENTATION_SAMPLES[1], start + uint32_t{500}, 0, target));
  EXPECT_EQ(target, 1);
}

TEST(AutoRotation, DetectsMissingSampleGapsAcrossUint32MillisRollover) {
  AutoRotation policy;
  uint8_t target = 0xFF;
  constexpr uint32_t start = std::numeric_limits<uint32_t>::max() - 449;
  expectHoldBefore(policy, ORIENTATION_SAMPLES[1], start, 0, target);
  const uint32_t afterGap = start + uint32_t{601};
  expectHoldBefore(policy, ORIENTATION_SAMPLES[1], afterGap, 0, target);
  EXPECT_TRUE(sample(policy, ORIENTATION_SAMPLES[1], afterGap + uint32_t{500}, 0, target));
  EXPECT_EQ(target, 1);
}
