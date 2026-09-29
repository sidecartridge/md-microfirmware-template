#!/usr/bin/env python3
"""Run the ST-side command-path tests (sttest.s) on the hardware.

Builds a temporary copy of the tree (tools/dev/builds/sttree) with sttest.s in
place of userfw.s, assembles the cartridge image, flashes the debug firmware
from that copy, reboots the ST through the sentinel so it runs the new
cartridge code, starts the tests with [F]irmware, and reads the results from
the console log. Never touches the working tree and never opens the UART:
`tools/dev/console.py watch` must be running. Leaves the test firmware on the
RP; flash the real one afterwards (`tools/dev/flash.sh debug`).

    python3 tools/dev/st_harness.py                 # build, flash, reboot the ST, run
    python3 tools/dev/st_harness.py --oversize      # also send the oversize frame (T5)
    python3 tools/dev/st_harness.py --long          # also run the 3000 x 1 KB burst (T6)
    python3 tools/dev/st_harness.py --no-build      # reuse the last build and flash
    python3 tools/dev/st_harness.py --no-build --no-flash --no-reboot   # run again at once
    python3 tools/dev/st_harness.py --label before  # name the report
    python3 tools/dev/st_harness.py --connect-race  # the ST's boot commands during the RP's Wi-Fi connect
    python3 tools/dev/st_harness.py --mste 16c      # a Mega STE at 16 MHz with the cache for the run

The tests: T0 what the cartridge hands over (the return address into TOS, the
machine), T1 the flags each sender returns with, T2 the registers each send
changes, T3 100 small commands, T4 10 commands of 1 KB, T5 an oversize frame
and then an ordinary command, T6 a long burst of 1 KB commands. On a Mega STE
T0 also reports the speed and cache at the handover, which must be the user's
setting; --mste sets them for the run (8, 16 or 16c), as the user would, and
the ST's closing reboot keeps whatever the reset leaves. Exits 0 when
every check passes. Writes tools/dev/logs/st-harness-<label>-<time>.json.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEV = HERE
LOG = os.path.join(DEV, "logs", "console.log")
TREE = os.path.join(DEV, "builds", "sttree")
sys.path.insert(0, DEV)
import swd  # noqa: E402

CMD_RESET, CMD_NOP = 1, 0


# The temporary tree sits inside the repository, so without this its builds
# would carry the same git build ID as the real firmware, and the ELF cache
# (tools/dev/builds/elf/<type>-<id>.elf) would hand swd.py the wrong program.
STTEST_ENV = dict(os.environ, RELEASE_BUILD_ID="sttest-" + subprocess.run(
    ["git", "rev-parse", "--short=7", "HEAD"], cwd=REPO, capture_output=True,
    text=True).stdout.strip())


def run(*args, cwd=REPO, timeout=900, env=None):
    if env is None and any("flash.sh" in str(a) for a in args):
        env = STTEST_ENV
    p = subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                       timeout=timeout, env=env)
    return p.returncode, p.stdout + p.stderr


class LogCursor:
    def __init__(self):
        self.pos = os.path.getsize(LOG) if os.path.exists(LOG) else 0

    def new(self):
        size = os.path.getsize(LOG)
        with open(LOG, "rb") as f:
            f.seek(self.pos if size >= self.pos else 0)
            return f.read().decode("utf-8", "replace")

    def wait(self, pattern, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.new()):
                return True
            time.sleep(0.25)
        return False


MSTE_SETTINGS = {"8": 0, "16": 2, "16c": 3}   # $FFFF8E21: bit 1 16 MHz, bit 0 the cache


def mste_setting(reg):
    # Only bits 1-0 are the register; the rest read back as whatever the bus held.
    return f"{'16' if reg & 2 else '8'} MHz, cache {'on' if reg & 1 else 'off'}"


def build(oversize, long_burst=False, mste=-1):
    print("build: temporary tree with sttest.s as userfw.s")
    if os.path.exists(TREE):
        shutil.rmtree(TREE)
    # shutil.copy, not copytree's default copy2: fresh modification times, or
    # the incremental build dir keeps objects built from an earlier copy that
    # are newer than the repo's sources (a stale commemul.o was linked once).
    shutil.copytree(os.path.join(REPO, "target", "atarist"),
                    os.path.join(TREE, "target", "atarist"),
                    ignore=shutil.ignore_patterns("build", "dist"),
                    copy_function=shutil.copy)
    shutil.copytree(os.path.join(REPO, "rp", "src"), os.path.join(TREE, "rp", "src"),
                    copy_function=shutil.copy)
    src = open(os.path.join(HERE, "sttest.s")).read()
    with open(os.path.join(TREE, "target", "atarist", "src", "userfw.s"), "w") as f:
        f.write(f"STTEST_OVERSIZE equ {1 if oversize else 0}\n"
                f"STTEST_LONG equ {1 if long_burst else 0}\n"
                f"STTEST_MSTE equ {mste}\n" + src)
    at = os.path.join(TREE, "target", "atarist")
    env = dict(os.environ, STCMD_NO_TTY="1")
    rc, out = run("./build.sh", at, "release", cwd=at, env=env)
    if rc != 0:
        sys.exit("m68k build failed:\n" + out[-3000:])
    print("  " + [l for l in out.splitlines() if "Cartridge code" in l][0])
    rc, out = run(os.path.join(DEV, "flash.sh"), "debug", "--src",
                  os.path.join(TREE, "rp", "src"), "--build-only")
    if rc != 0:
        sys.exit("RP build failed:\n" + out[-3000:])
    print("  " + out.strip().splitlines()[-1])


def elf_path():
    rc, out = run(os.path.join(DEV, "flash.sh"), "debug", "--src",
                  os.path.join(TREE, "rp", "src"), "--build-only")
    m = re.search(r"Built debug \S+: (\S+)/rp\.uf2", out)
    return os.path.join(m.group(1), "rp.elf")


def sentinel_addr(elf):
    base = swd.cartridge_window(elf)
    return base + swd.header_defines(
        os.path.join(REPO, "rp", "src", "include", "chandler.h"))["CHANDLER_CMD_SENTINEL_OFFSET"]


def write_sentinel(addr, value):
    swapped = ((value << 16) | (value >> 16)) & 0xFFFFFFFF
    swd.openocd(f"mww 0x{addr:08x} 0x{swapped:08x}")


def st_in_menu_loop(elf):
    """The ST's menu loop reads the framebuffer every vsync: ROM4 DMA
    channel 1's read address moves inside it."""
    base = swd.cartridge_window(elf)
    defs = swd.include_defines()
    lo = base + defs["DISPLAY_BUFFER_OFFSET"]
    hi = base + 0x10000
    out = swd.openocd(*(["mdw 0x50000040", "sleep 25"] * 6), check=False)
    addrs = [int(v, 16) for v in re.findall(r"0x50000040: ([0-9a-f]+)", out)]
    inside = {a for a in addrs if lo <= a < hi}
    return len(inside) >= 2


def wait_st_menu(elf, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if st_in_menu_loop(elf):
            return True
        time.sleep(1.0)
    return False


def parse_reports(text):
    """{command id: [D3, D4, D5, ...]} from term_loop's trace lines."""
    reports = {}
    current = None
    for line in text.splitlines():
        m = re.search(r"Command ID: (\d+)\. Size: (\d+)", line)
        if m:
            current = int(m.group(1))
            reports.setdefault(current, [])
            reports[current] = []
            continue
        m = re.search(r"Payload D[3-6]: 0x([0-9A-Fa-f]+)", line)
        if m and current is not None:
            reports[current].append(int(m.group(1), 16))
    return reports


REGS = ["d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "a4"]


def regs(mask):
    return ",".join(r for i, r in enumerate(REGS) if mask >> i & 1) or "none"


def stamp(line):
    h, m, rest = line[11:23].split(":")
    return int(h) * 3600 + int(m) * 60 + float(rest)


def connect_race(elf, sent, label):
    """Reboot the ST and then the RP: the ST's boot commands (detect_hw,
    get_tos_version) arrive while the RP is joining Wi-Fi."""
    print("reboot the ST and the RP together")
    cur = LogCursor()
    write_sentinel(sent, CMD_RESET)
    time.sleep(0.3)
    run(sys.executable, os.path.join(DEV, "swd.py"), "reset")
    ok = cur.wait(r"Start the app loop", 200)
    back = wait_st_menu(elf, 60)
    lines = cur.new().splitlines()
    def first(pat):
        for l in lines:
            if re.search(pat, l):
                return stamp(l)
        return None
    t = {"connect_start": first(r"Connecting to SSID"), "connected": first(r"Connected\. Check"),
         "loop": first(r"Start the app loop"),
         "var0": first(r"Setting shared variable 0 "), "var1": first(r"Setting shared variable 1 ")}
    base = t["connect_start"]
    rel = {k: (round(v - base, 3) if v is not None and base is not None else None) for k, v in t.items()}
    print(f"  seconds from the start of the Wi-Fi connect: {rel}")
    during = None not in (t["var0"], t["connected"]) and t["var0"] < t["connected"]
    gap = round(t["var1"] - t["var0"], 3) if None not in (t["var0"], t["var1"]) else None
    print(f"  ST boot commands during the connect: {during}; var0 -> var1: {gap} s; "
          f"RP loop reached: {ok}; ST back in menu: {back}")
    report = os.path.join(DEV, "logs", f"st-harness-{label}-{time.strftime('%Y%m%d-%H%M%S')}.json")
    with open(report, "w") as f:
        json.dump({"label": label, "timeline": rel, "during_connect": during,
                   "var0_to_var1_s": gap, "loop": ok, "st_back": back}, f, indent=2)
    print(f"report: {report}")
    return 0


def reboot_st(elf, sent):
    """Reboot the ST through the sentinel so it runs the new cartridge code, and
    check it said hello; returns the shared variables and the boot trace."""
    print("reboot the ST so it runs the new cartridge code")
    boot = LogCursor()
    write_sentinel(sent, CMD_RESET)
    time.sleep(0.6)
    write_sentinel(sent, CMD_NOP)
    if not wait_st_menu(elf, 60):
        sys.exit("the ST did not come back to the setup menu")
    boot_trace = boot.new()
    print("  ST back in the setup menu")
    rc, out = run(sys.executable, os.path.join(DEV, "swd.py"), "shared", "--elf", elf, "--all")
    svars = {int(m.group(1)): int(m.group(2), 16)
             for m in re.finditer(r"\[\s*(\d+)\][^\n]*?0x([0-9a-f]{8})", out)}
    set_lines = re.findall(r"Setting shared variable (\d+) to (\w+)", boot_trace)
    print(f"  after boot: shared variable 0 = 0x{svars.get(0, 0):08x}, 1 = 0x{svars.get(1, 0):08x}; "
          f"set by the ST during boot: {set_lines or 'nothing'}")
    hello = "The ST has booted" in boot_trace
    print(f"  hello from the ST during boot: {'yes' if hello else 'NO'}")
    if not hello:
        sys.exit("the RP never saw the ST's hello, so [F]irmware is refused")
    return svars, set_lines, boot_trace


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--oversize", action="store_true")
    ap.add_argument("--long", action="store_true", help="also run T6, a 3000 x 1 KB burst")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--no-flash", action="store_true", help="use what the RP runs now")
    ap.add_argument("--no-reboot", action="store_true",
                    help="the ST already sits in the setup menu with this cartridge code "
                         "(after a run): start the tests at once")
    ap.add_argument("--mste", choices=sorted(MSTE_SETTINGS),
                    help="on a Mega STE, the speed and cache for the run (with the build)")
    ap.add_argument("--label", default="run")
    ap.add_argument("--connect-race", action="store_true",
                    help="reboot the ST and the RP together, so the ST's boot "
                         "commands land during the RP's Wi-Fi connect")
    args = ap.parse_args()

    if not args.no_build:
        build(args.oversize, args.long, MSTE_SETTINGS.get(args.mste, -1))
    elf = elf_path()

    if not args.no_flash:
        print("flash")
        cur = LogCursor()
        rc, out = run(os.path.join(DEV, "flash.sh"), "debug", "--src",
                      os.path.join(TREE, "rp", "src"))
        if rc != 0:
            sys.exit("flash failed:\n" + out[-2000:])
        if not cur.wait(r"Start the app loop", 200):
            sys.exit("the RP did not reach its main loop")
    sent = sentinel_addr(elf)
    svars, set_lines, boot_trace = {}, [], ""
    if args.no_reboot:
        print("the ST is in the setup menu with this cartridge code already: no reboot")
    else:
        svars, set_lines, boot_trace = reboot_st(elf, sent)

    if args.connect_race:
        return connect_race(elf, sent, args.label)


    print("run the test ([F]irmware)")
    cur = LogCursor()
    t0 = time.time()
    run(sys.executable, os.path.join(DEV, "swd.py"), "key", "f")
    run(sys.executable, os.path.join(DEV, "swd.py"), "key", "\n")
    time.sleep(1.0)
    write_sentinel(sent, CMD_NOP)      # so the ST's closing reboot lands in the menu
    done = cur.wait(r"Command ID: 32639\.", 240)
    elapsed = time.time() - t0
    text = cur.new()
    rep = parse_reports(text)
    back = wait_st_menu(elf, 60) if done else False
    faults = re.findall(r"PANIC|HardFault|\*\*\* PANIC", text)
    rc, running = run(sys.executable, os.path.join(DEV, "swd.py"), "running", elf,
                      "--timeout", "5")

    def val(cid, i, default=None):
        v = rep.get(cid, [])
        return v[i] if len(v) > i else default

    result = {"label": args.label, "done": done, "svars_after_boot": {str(k): v for k, v in svars.items() if k < 3},
              "set_during_boot": set_lines, "seconds": round(elapsed, 1),
              "st_back_in_menu": back, "rp_running": rc == 0, "faults": faults,
              "boot_commands": sorted(set(int(x) for x in
                                          re.findall(r"Command ID: (\d+)\.", boot_trace))),
              "reports": {hex(k): v for k, v in sorted(rep.items()) if k >= 0x7F00}}
    checks = []

    def check(name, ok, detail):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})
        print(f"  {'PASS' if ok else 'FAIL'}  {name}: {detail}")

    print(f"\nresults ({elapsed:.1f} s)")
    ret, sp = val(0x7F10, 0), val(0x7F10, 1, 0)
    in_rom = ret is not None and (0xE00000 <= ret < 0xF00000 or 0xFC0000 <= ret < 0xFF0000)
    check("T0 handover returns into TOS", in_rom,
          f"(sp) = 0x{ret:08x}, sp = 0x{sp:08x}" if ret is not None else "no report")
    if 0x7F00 in rep:
        mch, tos, lf = (val(0x7F00, i, 0) for i in range(3))
        print(f"        T0 machine: _MCH 0x{mch:08x}, TOS {tos >> 8:x}.{tos & 0xff:02x}, _longframe {lf}")
        if mch == 0x00010010:  # a Mega STE: its speed and cache register
            print(f"        T0 Mega STE at the handover: {mste_setting(val(0x7F10, 2, 0))}")
            run_reg = val(0x7F7F, 1)
            if run_reg is not None:
                print(f"        Mega STE for the run: {mste_setting(run_reg)}")
            if args.mste:
                want = MSTE_SETTINGS[args.mste]
                check(f"the run's Mega STE setting is --mste {args.mste}",
                      run_reg is not None and run_reg & 3 == want,
                      mste_setting(run_reg) if run_reg is not None else "no report")
    for cid, name in ((0x7F02, "send_sync"), (0x7F04, "send_write_sync")):
        d0, sr = val(cid, 0), val(cid, 1, 0)
        z = sr >> 2 & 1
        check(f"T1 {name} returns d0 = 0 with Z set", d0 is not None and (d0 & 0xFFFF) == 0 and z,
              f"d0={d0 & 0xFFFF:#06x} Z={z}" if d0 is not None else "no report")
    # What each send keeps (CLAUDE.md): send_sync d1-d6, send_write_sync d1-d5 and a4.
    for i, (name, kept) in enumerate((("send_sync", 0x03F), ("send_write_sync", 0x81F))):
        mask = val(0x7F07, i)
        check(f"T2 {name} keeps {regs(kept)}", mask is not None and not mask & kept,
              f"changes {regs(mask)}" if mask is not None else "no report")
    tests = [(3, 0x7F09, "small (4 B)", 100), (4, 0x7F0B, "big (1 KB)", 10)]
    if args.long:
        tests.append((6, 0x7F1F, "long burst (1 KB)", 3000))
    for t, cid, name, want in tests:
        ticks, fails, n = (val(cid, i) for i in range(3))
        check(f"T{t} {want} {name} commands, none failed",
              n == want and fails == 0,
              f"{n} sent, {fails} failed, {ticks * 5} ms, {ticks * 5 / max(n, 1):.1f} ms each"
              if n is not None else "no report")
    if args.oversize:
        v = val(0x7F0E, 0)
        check("T5 a command after an oversize frame is answered", v == 0,
              "answered" if v == 0 else "NOT answered" if v is not None else "no report")
    check("the tests finished and the ST is back in the setup menu", done and back,
          f"finished {done}, back {back}")
    check("the RP kept running, with no fault", rc == 0 and not faults,
          f"running {rc == 0}, faults {faults or 'none'}")
    failed = [c for c in checks if not c["ok"]]
    print(f"\n{len(checks) - len(failed)}/{len(checks)} checks passed")
    result["checks"] = checks
    report = os.path.join(DEV, "logs", f"st-harness-{args.label}-{time.strftime('%Y%m%d-%H%M%S')}.json")
    with open(report, "w") as f:
        json.dump(result, f, indent=2)
    print(f"report: {report}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
