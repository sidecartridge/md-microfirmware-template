#!/usr/bin/env python3
"""Exercise every tools/dev command against the running device and say, check
by check, whether it behaves.

Needs a debug build on the RP (the mailbox commands), the Debug Probe, and
`tools/dev/console.py watch` running: this reads the console *log*
(tools/dev/logs/console.log) and never opens the UART. A released SELECT
restarts the RP on the way.

    python3 tools/dev/tools_harness.py            # device checks
    python3 tools/dev/tools_harness.py --build    # also build both types and check them
    python3 tools/dev/tools_harness.py --flash    # also flash the debug build first
    python3 tools/dev/tools_harness.py --reset    # also restart the RP at the end

Leaves the setup menu on screen. Exits 0 when every check passes. Writes
tools/dev/logs/tools-harness-<time>.json.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

DEV = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(DEV, "..", ".."))
LOG = os.path.join(DEV, "logs", "console.log")
sys.path.insert(0, DEV)
import swd  # noqa: E402

results = []


def check(name, ok, detail=""):
    results.append({"check": name, "ok": bool(ok), "detail": detail})
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))
    return ok


def run(*args, timeout=600):
    p = subprocess.run(args, cwd=REPO, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def swdpy(*args, timeout=120):
    return run(sys.executable, os.path.join(DEV, "swd.py"), *args, timeout=timeout)


class LogCursor:
    """What the console log gained since the cursor was placed."""

    def __init__(self):
        self.pos = os.path.getsize(LOG) if os.path.exists(LOG) else 0

    def new(self):
        if not os.path.exists(LOG):
            return ""
        size = os.path.getsize(LOG)
        start = self.pos if size >= self.pos else 0  # rotated
        with open(LOG, "rb") as f:
            f.seek(start)
            return f.read().decode("utf-8", "replace")

    def wait(self, pattern, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.new()):
                return True
            time.sleep(0.2)
        return False


def text():
    rc, out = swdpy("text")
    return out if rc == 0 else ""


def keys(s, pause=0.4):
    for ch in s:
        swdpy("key", ch)
        time.sleep(pause)


def to_menu():
    keys("\nm\n")
    time.sleep(1.0)
    return "Select an option" in text()


def heap_in_use_kb():
    rc, out = swdpy("heap")
    m = re.search(r"in use ([0-9.]+) KB", out)
    return float(m.group(1)) if m else None


# ---------------------------------------------------------------------------
def build_checks():
    print("build")
    for t in ("release", "debug"):
        rc, out = run(os.path.join(DEV, "flash.sh"), t, "--build-only", timeout=900)
        check(f"{t} builds", rc == 0, out.strip().splitlines()[-1] if out.strip() else "")
    flags = {t: open(os.path.join(DEV, "builds", t, "CMakeFiles", "rp.dir", "flags.make")).read()
             for t in ("release", "debug")}
    check("release compiles with _DEBUG=0", "-D_DEBUG=0" in flags["release"])
    check("debug compiles with _DEBUG=1", "-D_DEBUG=1" in flags["debug"])
    check("921,600 baud in debug only",
          "PICO_DEFAULT_UART_BAUD_RATE=921600" in flags["debug"]
          and "PICO_DEFAULT_UART_BAUD_RATE" not in flags["release"])
    for t, want in (("release", False), ("debug", True)):
        syms = swd.elf_symbols(os.path.join(DEV, "builds", t, "rp.elf"),
                               "devhooksMailbox", "release_build_id",
                               "__rom_in_ram_start__")
        check(f"{t} ELF keeps symbols", "__rom_in_ram_start__" in syms)
        check(f"{t} ELF carries release_build_id", "release_build_id" in syms)
        check(f"{t} {'has' if want else 'has no'} devhooks mailbox",
              ("devhooksMailbox" in syms) == want)
    a = open(os.path.join(DEV, "builds", "release", "rp.uf2"), "rb").read()
    b = open(os.path.join(DEV, "builds", "debug", "rp.uf2"), "rb").read()
    check("release and debug images differ", a != b)


def device_checks(do_reset):
    elf = os.path.join(DEV, "builds", "debug", "rp.elf")
    print("device")
    rc, out = swdpy("running", elf, "--timeout", "10")
    check("RP runs the debug ELF", rc == 0, out.strip())
    rc, out = swdpy("verify", elf)
    check("flash matches the ELF", rc == 0, out.strip())
    built = open(os.path.join(DEV, "builds", "debug", "generated", "build_id",
                              "build_id.h")).read()
    want = re.search(r'RELEASE_BUILD_ID "(.*)"', built).group(1)
    rc, out = swdpy("build-id")
    check("build ID on the device is the one built", out.strip() == want,
          f"device {out.strip()}, built {want}")

    print("console log (read-only)")
    rc, out = run(sys.executable, os.path.join(DEV, "console.py"), "since-boot")
    check("boot reached the main loop", "Start the app loop" in out)
    check("no panic or hard fault since boot",
          not re.search(r"PANIC|HardFault|\*\*\* ", out))
    tail = out.splitlines()[-15:]
    printable = sum(ch.isprintable() or ch in "\t" for ln in tail for ch in ln)
    total = sum(len(ln) for ln in tail) or 1
    check("console text is clean (baud rate matches)", printable / total > 0.98,
          f"{100 * printable / total:.1f}% printable in the last 15 lines")

    print("screen, text, shared")
    ok = to_menu()
    check("setup menu is on screen", ok)
    png = os.path.join(DEV, "logs", "tools-harness-screen.png")
    rc, out = swdpy("screen", png)
    lit = re.search(r"(\d+) pixels lit", out)
    check("screen renders the framebuffer", rc == 0 and lit and int(lit.group(1)) > 1000,
          out.strip())
    rc, out = swdpy("shared")
    check("shared reads sentinel, token and seed",
          rc == 0 and all(s in out for s in ("command sentinel", "random token", "seed")))

    print("mailbox: key, inject, app")
    cur = LogCursor()
    keys("h\n")
    time.sleep(0.8)
    check("key: 'h' + Enter shows the help", "Available commands" in text())
    check("key: the firmware logged both keystrokes",
          cur.wait(r"Keystroke: h\.") and cur.wait(r"Keystroke: 10\."))
    check("key: back to the menu", to_menu())
    cur = LogCursor()
    rc, out = swdpy("inject", "0x0001", "0x0068", "0")
    rc2, _ = swdpy("inject", "0x0001", "0x000A", "0")
    time.sleep(0.8)
    check("inject: raw keystroke frames reach the terminal",
          rc == 0 and rc2 == 0 and cur.wait(r"Injected command 0001") and
          "Available commands" in text())
    to_menu()
    before = heap_in_use_kb()
    rc, out = swdpy("app", "heap_hold", "16")
    held = heap_in_use_kb()
    rc2, _ = swdpy("app", "heap_hold", "0")
    after = heap_in_use_kb()
    check("app heap_hold 16 holds 16 KB, 0 releases it",
          rc == 0 and rc2 == 0 and None not in (before, held, after)
          and abs(held - before - 16.0) < 0.6 and abs(after - before) < 0.6,
          f"in use {before} -> {held} -> {after} KB")

    print("counters, ring (no halt)")
    rc, out = swdpy("counters")
    check("counters reads the command channel's counters",
          rc == 0 and "commands answered" in out, out.strip().splitlines()[0] if out.strip() else "")
    rc, out = swdpy("ring")
    check("ring decodes the capture ring", rc == 0 and re.search(r"\d+ frames, \d+ with a bad", out),
          out.strip().splitlines()[-1] if out.strip() else "")
    rc, out = swdpy("ring", "--mark")
    rc2, out2 = swdpy("ring", "--since-mark")
    check("ring --mark, then --since-mark", rc == 0 and rc2 == 0 and "marked at sample" in out
          and "frames" in out2, out2.strip().splitlines()[-1] if out2.strip() else "")

    print("select (one OpenOCD session: press, look, release)")
    select_cur = LogCursor()
    defs = swd.include_defines()
    gpio = defs["SELECT_GPIO"]
    ctrl = swd.IO_BANK0 + 4 + 8 * gpio
    normal = swd.read_word(ctrl) & ~(3 << swd.INOVER_SHIFT)
    pressed = normal | (swd.INOVER_HIGH << swd.INOVER_SHIFT)
    addr, size = swd.elf_symbols(elf, "screen")["screen"]
    out = swd.openocd(f"mww 0x{ctrl:08x} 0x{pressed:08x}", "sleep 2500",
                      "mdw 0xd0000004", f"mdb 0x{addr:08x} {size}",
                      f"mww 0x{ctrl:08x} 0x{normal:08x}", check=False)
    m = re.search(r"0xd0000004: ([0-9a-f]+)", out)
    check("select: the pin reads pressed while held",
          m and (int(m.group(1), 16) >> gpio) & 1 == 1)
    data = bytes(int(x, 16) for ln in
                 re.findall(r"0x[0-9a-f]+: ((?:[0-9a-f]{2} ?)+)", out)[1:]
                 for x in ln.split())
    check("select: the menu shows it pressed", re.search(rb"SELECT +: Pressed", data) is not None)
    check("select: override cleared afterwards",
          (swd.read_word(ctrl) >> swd.INOVER_SHIFT) & 3 == 0)

    # A released SELECT is a short press, which restarts the RP (since SELECT
    # was wired up): wait until the app loop is back before attaching GDB, or
    # its flash probe lands in the middle of boot2 setting up XIP.
    booted = select_cur.wait(r"Start the app loop", 60)
    check("select: the release restarts the RP into its app loop", booted)
    print("crash, postmortem")
    rc, out = swdpy("crash")
    check("crash reads the watchdog reason", rc == 0 and "watchdog reason" in out)
    rc, out = swdpy("postmortem", timeout=180)
    check("postmortem: backtrace reaches emul_start and both cores resume",
          rc == 0 and "emul_start" in out and "both cores released" in out)

    if do_reset:
        print("reset")
        cur = LogCursor()
        rc, out = swdpy("reset")
        booted = cur.wait(r"Start the app loop", timeout=30)
        rc2, _ = swdpy("running", elf, "--timeout", "10")
        check("reset: the RP reboots into the same firmware", rc == 0 and booted and rc2 == 0)
        time.sleep(1.5)
        check("reset: the menu is back", "Select an option" in text())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--flash", action="store_true")
    ap.add_argument("--reset", action="store_true")
    args = ap.parse_args()
    started = time.strftime("%Y%m%d-%H%M%S")
    if args.build:
        build_checks()
    if args.flash:
        print("flash")
        rc, out = run(os.path.join(DEV, "flash.sh"), "debug", timeout=900)
        check("flash.sh debug builds, flashes and verifies", rc == 0,
              out.strip().splitlines()[-1] if out.strip() else "")
        time.sleep(6)  # let the boot reach the main loop (Wi-Fi connect)
    device_checks(args.reset)
    failed = [r for r in results if not r["ok"]]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    report = os.path.join(DEV, "logs", f"tools-harness-{started}.json")
    with open(report, "w") as f:
        json.dump({"started": started, "results": results}, f, indent=2)
    print(f"report: {report}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
