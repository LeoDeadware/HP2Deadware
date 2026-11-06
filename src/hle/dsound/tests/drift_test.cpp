// Host test: QueueController keeps the device queue bounded for hours with a
// device clock off by up to +-0.5%, without underruns or drops.
//
//   tests/drift_test.sh
//
// Model: the mixer pushes 5 ms of frames per tick on the wall clock (as
// mixer_thread does); the device consumes in 512-frame bursts at
// 48 kHz * (1 + drift) * ratio; the controller runs every 100 ms on the
// queue level, as in the mixer.

#include <cmath>
#include <cstdio>

#include "hle/dsound/drift.h"

int main() {
  const double rate = 48000, hours = 4;
  const double drifts[] = {-0.005, -0.001, 0, 0.001, 0.005};
  int fails = 0;
  // Control: without the controller a 0.5% device must under/overrun (the test
  // can see the failure it guards against).
  for (int control = 1; control >= 0; --control)
  for (double drift : drifts) {
    if (control && std::fabs(drift) != 0.005) continue;
    hp2ds::QueueController qc;
    double queued = 0.040 * rate;  // frames
    double dev_acc = 0, ratio = 1, t_ctl = 0;
    double qmin = 1e9, qmax = 0, settled_dev = 0;
    long under = 0, drops = 0;
    const double dt = 0.005, cap = 0.100 * rate;
    for (double t = 0; t < hours * 3600; t += dt) {
      queued += rate * dt;  // producer: wall clock
      if (queued > cap) {   // mixer's safety net: drop what does not fit
        drops += long(queued - cap);
        queued = cap;
      }
      dev_acc += rate * (1 + drift) * ratio * dt;  // device: own clock
      while (dev_acc >= 512) {
        dev_acc -= 512;
        if (queued >= 512) queued -= 512;
        else { under += long(512 - queued); queued = 0; }
      }
      if (t - t_ctl >= 0.1) {
        ratio = control ? 1.0 : qc.update(queued / rate, t - t_ctl);
        t_ctl = t;
      }
      if (t > 600) {  // after 10 min: steady state
        qmin = std::fmin(qmin, queued / rate);
        qmax = std::fmax(qmax, queued / rate);
        settled_dev = std::fmax(settled_dev, std::fabs(ratio * (1 + drift) - 1));
      }
    }
    bool ok = under == 0 && drops == 0 && qmin > 0.015 && qmax < 0.070;
    if (control) {
      printf("control (no controller) drift %+.1f%%: underrun %ld, dropped %ld -> %s\n",
             drift * 100, under, drops, ok ? "UNEXPECTED ok" : "fails as expected");
      fails += ok;
      continue;
    }
    fails += !ok;
    printf("drift %+.1f%%: queue %.1f..%.1f ms, ratio %.5f (rate error %.4f%%), underrun %ld, "
           "dropped %ld frames over %.0f h -> %s\n",
           drift * 100, qmin * 1000, qmax * 1000, ratio, settled_dev * 100, under, drops, hours,
           ok ? "ok" : "FAIL");
  }
  printf("drift_test: %s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
