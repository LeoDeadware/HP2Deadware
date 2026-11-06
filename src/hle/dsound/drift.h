#pragma once
// Device clock drift control (owner: Audio). The mixer produces frames on the
// host wall clock; a real output device consumes them on its own crystal,
// a little fast or slow (typically < 0.1%, worst case ~0.5%). Left alone, the
// device queue would slowly fill (latency, then drops) or drain (underruns)
// over a long session. QueueController keeps the queue near a target level by
// returning a playback ratio for SDL_SetAudioStreamFrequencyRatio (>1 = the
// device consumes our frames faster). PI control on the queue level, ratio
// clamped to +-kMaxAdjust, so the pitch change is inaudible (1% ~ 17 cents
// at the worst clamp; steady state is the drift itself, ~0.1%).
// No dependencies: src/hle/dsound/tests/drift_test.cpp simulates it.

#include <algorithm>

namespace hp2ds {

class QueueController {
 public:
  static constexpr double kTarget = 0.040;     // seconds queued in the device
  static constexpr double kMaxAdjust = 0.01;   // ratio limit, +-1%
  static constexpr double kKp = 0.05;          // per second of level error
  static constexpr double kKi = 0.02;          // per second of error, per second

  // queued: seconds currently waiting in the device queue; dt: seconds since
  // the last call. Returns the playback ratio to apply.
  double update(double queued, double dt) {
    double err = queued - kTarget;
    integral_ = std::clamp(integral_ + err * dt, -kMaxAdjust / kKi, kMaxAdjust / kKi);
    ratio_ = 1.0 + std::clamp(kKp * err + kKi * integral_, -kMaxAdjust, kMaxAdjust);
    return ratio_;
  }
  double ratio() const { return ratio_; }

 private:
  double integral_ = 0;
  double ratio_ = 1.0;
};

}  // namespace hp2ds
