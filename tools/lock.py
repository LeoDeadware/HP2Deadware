#!/usr/bin/env python3
"""Runs a command under one of the project-wide locks (macOS has no flock(1)).
Blocks until the lock is free; prints who holds it while waiting.

  tools/lock.py heavy -- cmake --build out --target hp2_game -j8
  tools/lock.py install -- tools/install_app.sh
  tools/lock.py status
"""
import fcntl
import os
import subprocess
import sys
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hp2paths  # noqa: E402

LOCK_DIR = os.path.join(hp2paths.CACHE, 'locks')
LOCKS = ("heavy", "install")


def holder(path):
    try:
        return open(path).read().strip() or "?"
    except OSError:
        return "?"


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "status":
        for name in LOCKS:
            path = os.path.join(LOCK_DIR, name + ".lock")
            if not os.path.exists(path):
                print(f"{name}: free")
                continue
            with open(path, "a+") as f:
                try:
                    fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    print(f"{name}: free")
                except BlockingIOError:
                    print(f"{name}: held by {holder(path)}")
        return
    if len(sys.argv) < 4 or sys.argv[1] not in LOCKS or sys.argv[2] != "--":
        sys.exit(__doc__)
    name, cmd = sys.argv[1], sys.argv[3:]
    guard = os.path.join(os.path.dirname(os.path.abspath(__file__)), "guard.py")
    if name == "heavy" and subprocess.call([sys.executable, "-I", guard, "--wait"]) != 0:
        sys.exit("[lock] resources too low; not starting")
    os.makedirs(LOCK_DIR, exist_ok=True)
    path = os.path.join(LOCK_DIR, name + ".lock")
    with open(path, "a+") as f:
        waited = False
        while True:
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if not waited:
                    print(f"[lock] {name} held by {holder(path)}; waiting", file=sys.stderr)
                    waited = True
                time.sleep(2)
        f.seek(0)
        f.truncate()
        f.write(f"{os.getcwd()} pid {os.getpid()} since {time.strftime('%H:%M:%S')}: {' '.join(cmd)}\n")
        f.flush()
        try:
            sys.exit(subprocess.call(cmd))
        finally:
            f.seek(0)
            f.truncate()


if __name__ == "__main__":
    main()
