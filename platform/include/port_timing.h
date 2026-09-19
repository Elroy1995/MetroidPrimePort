#pragma once

#include <algorithm>
#include <cmath>

namespace PortTiming {
class FixedStepClock {
public:
  static constexpr double kPeriod = 1.0 / 60.0;
  unsigned Advance(double elapsed, bool oneTick = false, bool cappedCadence = false) {
    if (oneTick) {
      mRemainder = 0.0;
      return 1;
    }
    if (!std::isfinite(elapsed) || elapsed < 0.0) return 0;
    // Bound long stalls, but retain fractional time through ordinary jitter.
    mRemainder = std::min(mRemainder + elapsed, 0.25);
    // A precise 60 Hz deadline still jitters by microseconds. Without a little
    // scheduling leeway it can alternate zero/two ticks at 60 rendered FPS.
    // Keep the borrowed time as debt; unlike the old heuristic, no time is lost.
    const double leeway = cappedCadence ? 0.00025 : 0.0;
    const unsigned steps = static_cast<unsigned>(std::max(0.0, mRemainder + leeway) / kPeriod);
    mRemainder -= steps * kPeriod;
    return steps;
  }
  float Interpolation() const { return static_cast<float>(std::clamp(mRemainder / kPeriod, 0.0, 1.0)); }
private:
  double mRemainder = 0.0;
};
} // namespace PortTiming
