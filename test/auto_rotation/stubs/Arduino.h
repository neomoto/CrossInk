#pragma once

#include <cstdint>

namespace TestClock {
inline unsigned long nowMs = 0;
}

inline unsigned long millis() { return TestClock::nowMs; }
