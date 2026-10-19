#pragma once

class Imu {
 public:
  struct Sample {
    float ax, ay, az;
    float gx, gy, gz;
  };

  inline static bool beginResult = true;
  inline static bool wakeResult = true;
  inline static bool sleepResult = true;
  inline static bool readResult = true;
  inline static unsigned beginCalls = 0;
  inline static unsigned wakeCalls = 0;
  inline static unsigned sleepCalls = 0;
  inline static unsigned readCalls = 0;
  inline static Sample nextSample = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

  static void reset() {
    beginResult = wakeResult = sleepResult = readResult = true;
    beginCalls = wakeCalls = sleepCalls = readCalls = 0;
    nextSample = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  }

  bool begin() {
    ++beginCalls;
    return beginResult;
  }
  bool wake() {
    ++wakeCalls;
    return wakeResult;
  }
  bool sleep() {
    ++sleepCalls;
    return sleepResult;
  }
  bool read(Sample& out) {
    ++readCalls;
    if (!readResult) return false;
    out = nextSample;
    return true;
  }
};
