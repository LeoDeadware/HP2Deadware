#!/usr/bin/env python3
"""Resource guard: is it safe to start more work on this Mac (16 GB RAM, shared disk)?

  tools/guard.py              print state + verdict (exit 0 ok, 1 wait, 2 stop)
  tools/guard.py --jobs       print a safe -j for compiling (memory-aware, 1..8)
  tools/guard.py --wait       block until the verdict is ok (used by lock.py heavy)

Limits (see CLAUDE.md -> Resources):
  RAM:  memory_pressure free >= 25 %        else WAIT
  swap: used <= 4 GB                         else WAIT; > 8 GB STOP
  disk: free >= 40 GB                        else WAIT (run tools/cleanup.py); < 25 GB STOP
"""
import re
import shutil
import subprocess
import sys
import time

GB = 1 << 30
JOB_MEM = 1.5 * GB  # one clang on a generated TU, worst case seen


def state():
    mp = subprocess.run(["memory_pressure"], capture_output=True, text=True).stdout
    free_pct = int(re.search(r"free percentage: (\d+)%", mp).group(1))
    sw = subprocess.run(["sysctl", "-n", "vm.swapusage"], capture_output=True, text=True).stdout
    used = re.search(r"used = ([\d.]+)M", sw)
    swap_used = float(used.group(1)) * (1 << 20) if used else 0.0
    mem = int(subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True).stdout)
    disk_free = shutil.disk_usage("/System/Volumes/Data").free
    return {"free_pct": free_pct, "swap_used": swap_used, "mem": mem, "disk_free": disk_free}


def verdict(s):
    if s["swap_used"] > 8 * GB or s["disk_free"] < 25 * GB:
        return 2, "STOP"
    if s["free_pct"] < 25 or s["swap_used"] > 4 * GB or s["disk_free"] < 40 * GB:
        return 1, "WAIT"
    return 0, "OK"


def jobs(s):
    avail = s["mem"] * s["free_pct"] / 100 - 3 * GB  # keep 3 GB for the apps
    return max(1, min(8, int(avail // JOB_MEM)))


def main():
    s = state()
    if "--jobs" in sys.argv:
        print(jobs(s))
        return
    if "--wait" in sys.argv:
        while True:
            code, word = verdict(s)
            if code == 0:
                return
            if code == 2:
                print(f"[guard] STOP: {describe(s)} — free resources first (tools/cleanup.py)", file=sys.stderr)
                sys.exit(2)
            print(f"[guard] WAIT: {describe(s)}", file=sys.stderr)
            time.sleep(20)
            s = state()
    code, word = verdict(s)
    print(f"{word}: {describe(s)}; safe -j{jobs(s)}")
    sys.exit(code)


def describe(s):
    return (f"RAM free {s['free_pct']}%, swap used {s['swap_used'] / GB:.1f} GB, "
            f"disk free {s['disk_free'] / GB:.0f} GB")


if __name__ == "__main__":
    main()
