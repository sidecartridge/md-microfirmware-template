#!/usr/bin/env python3
"""Count power-cycled boots from the debug console.

Run it, then power-cycle the ST (which powers the RP too), waiting for the
setup menu or GEM each time. For every RP boot it prints how long after the
banner the cartridge went live and whether the ST said hello (it booted
through the cartridge: the setup menu) or not (it did not find the cartridge
in time: GEM). Three boots are the default: power cycles wear a machine this
old, and anything that does not need a cold start is tested with a reset
through the sentinel instead.

    python3 tools/dev/power_cycles.py [--boots 3]

Needs a debug build and `tools/dev/console.py watch` running.
"""
import argparse
import datetime
import os
import re
import sys
import time

LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs", "console.log")
HELLO_WINDOW_S = 20.0


def stamp(line):
    return datetime.datetime.strptime(line[:23], "%Y-%m-%d %H:%M:%S.%f")


def follow(path):
    with open(path, "r", errors="replace") as f:
        f.seek(0, os.SEEK_END)
        while True:
            line = f.readline()
            if line:
                yield line.rstrip("\n")
            else:
                yield None
                time.sleep(0.05)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boots", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=1800, help="give up after this many seconds")
    args = ap.parse_args()

    results = []
    boot = None
    end = time.time() + args.timeout
    print(f"watching {LOG}; power-cycle the ST {args.boots} times")
    for line in follow(LOG):
        now = datetime.datetime.now()
        if boot and boot.get("hello") is None and (now - boot["banner"]).total_seconds() > HELLO_WINDOW_S:
            boot["hello"] = False
            results.append(boot)
            print(f"boot {len(results)}: cartridge live +{boot.get('live', float('nan')):.0f} ms, "
                  f"NO hello within {HELLO_WINDOW_S:.0f} s (GEM?)")
            boot = None
        if len(results) >= args.boots or time.time() > end:
            break
        if line is None or len(line) < 24:
            continue
        if re.search(r"App\. v\S+ \(", line):
            if boot and boot.get("hello") is None:
                boot["hello"] = False
                results.append(boot)
                print(f"boot {len(results)}: NO hello before the next boot")
            boot = {"banner": stamp(line), "hello": None}
        elif boot and "ROM emulator initialized" in line:
            boot["live"] = (stamp(line) - boot["banner"]).total_seconds() * 1000
        elif boot and boot.get("hello") is None and "The ST has booted" in line:
            boot["hello"] = (stamp(line) - boot["banner"]).total_seconds() * 1000
            results.append(boot)
            print(f"boot {len(results)}: cartridge live +{boot.get('live', float('nan')):.0f} ms, "
                  f"hello +{boot['hello']:.0f} ms: setup menu")
            boot = None

    menus = sum(1 for b in results if b["hello"] is not False)
    print(f"\n{menus} of {len(results)} power-cycled boots reached the setup menu")
    return 0 if results and menus == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
