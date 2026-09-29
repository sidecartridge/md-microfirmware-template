#!/usr/bin/env python3
"""Check what the SELECT button does, on the running RP, without a finger.

Each case presses SELECT through swd.py's GPIO input override, then reads the
firmware's own state over SWD and says whether it matches the behaviour:

  bounce           15 ms press: ignored (the debounce is 30 ms)
  short            short press: the RP restarts into the app
  long             a press held past SELECT_LONG_RESET: factory reset (the
                   global settings are erased and the RP restarts into
                   Booster, which then clears every app's settings). Needs
                   --force and saves the settings flash first (put it back
                   with `restore`)

  status           print the state the cases read
  backup FILE      save the settings flash (app configs, lookup, global config)
  restore FILE     write a backup back and reset the RP

A short press restarts only the RP: the ST keeps running its menu loop from
RAM, and its menu then says the RP has not heard its hello yet.

Usage: python3 tools/dev/select_harness.py <case> [FILE] [--elf ELF] [--force]
"""

import argparse
import os
import sys
import time

import swd

TIMERAWL = 0x40054028
FLASH_END = 0x10200000  # 2 MB flash on the SidecarTridge Multi-device
BOUNCE_MS = 15

SYMBOLS = ("menuScreenActive", "keepActive", "_config_flash_start")


class Harness:
    def __init__(self, elf: str | None):
        self.elf = swd.matching_elf(elf)
        self.sym = swd.elf_symbols(self.elf, *SYMBOLS)
        missing = [s for s in SYMBOLS if s not in self.sym]
        if missing:
            raise swd.SwdError(f"{self.elf} lacks {', '.join(missing)}")
        self.long_ms = swd.include_defines()["SELECT_LONG_RESET"]

    def byte(self, name: str) -> int:
        return swd.read_memory(self.sym[name][0], 1)[0]

    def state(self) -> dict:
        return {
            "uptime_us": swd.read_word(TIMERAWL),
            "menu": self.byte("menuScreenActive"),
            "running": self.byte("keepActive"),
        }

    def press(self, hold_ms: int, force: bool = False) -> None:
        kind = "long" if hold_ms >= self.long_ms else "short"
        swd.cmd_select(argparse.Namespace(press=kind, hold_ms=hold_ms,
                                          force=force))

    def wait_reset(self, before_us: int, timeout: float = 15.0) -> bool:
        """True once the RP has restarted (its microsecond timer went back)."""
        end = time.time() + timeout
        while time.time() < end:
            time.sleep(1.0)
            try:
                if swd.read_word(TIMERAWL) < before_us:
                    return True
            except swd.SwdError:
                pass  # the debug port drops while the chip restarts
        return False

    def wait_menu(self, timeout: float = 30.0) -> bool:
        """True once the restarted app has drawn its setup menu."""
        end = time.time() + timeout
        while time.time() < end:
            try:
                if self.byte("menuScreenActive") and self.byte("keepActive"):
                    return True
            except swd.SwdError:
                pass
            time.sleep(0.5)
        return False


def show(label: str, st: dict) -> None:
    print(f"{label}: setup menu {'shown' if st['menu'] else 'not shown'}; "
          f"main loop {'running' if st['running'] else 'leaving'}; "
          f"uptime {st['uptime_us'] / 1e6:.1f} s")


def verdict(ok: bool, what: str) -> int:
    print(("PASS  " if ok else "FAIL  ") + what)
    return 0 if ok else 1


def case_bounce(h: Harness) -> int:
    before = h.state()
    show("before", before)
    h.press(BOUNCE_MS)
    time.sleep(1.5)
    after = h.state()
    show("after ", after)
    return verdict(after["uptime_us"] > before["uptime_us"]
                   and after["running"] == before["running"],
                   f"a {BOUNCE_MS} ms press is ignored")


def case_short(h: Harness) -> int:
    before = h.state()
    show("before", before)
    h.press(swd.SELECT_SHORT_MS)
    reset = h.wait_reset(before["uptime_us"])
    back = reset and h.wait_menu()
    if reset:
        show("after ", h.state())
    return verdict(reset and back, "a short press restarts the RP into the "
                   "app's setup menu (reset the ST for its hello)")


def settings_region(h: Harness) -> tuple[int, int]:
    start = h.sym["_config_flash_start"][0]
    return start, FLASH_END - start


def case_backup(h: Harness, path: str) -> int:
    start, length = settings_region(h)
    with open(path, "wb") as f:
        f.write(swd.read_memory(start, length))
    print(f"saved {length} bytes of settings flash from 0x{start:08x} to {path}")
    return 0


def case_restore(h: Harness, path: str) -> int:
    start, length = settings_region(h)
    if os.path.getsize(path) != length:
        raise swd.SwdError(f"{path} is not a {length}-byte settings backup")
    out = swd.openocd(*swd.quiesce_commands(),
                      f"flash write_image erase {path} 0x{start:08x} bin",
                      f"verify_image {path} 0x{start:08x} bin", check=False,
                      work_area=True)
    swd.chip_reset()
    ok = "verified" in out
    return verdict(ok, f"settings flash restored from {path} and RP reset")


def case_long(h: Harness, force: bool, backup: str) -> int:
    if not force:
        raise swd.SwdError("a long press is a factory reset: add --force "
                           "(the settings flash is backed up first)")
    case_backup(h, backup)
    before = h.state()
    show("before", before)
    h.press(h.long_ms + 1000, force=True)
    reset = h.wait_reset(before["uptime_us"], timeout=20.0)
    print(f"      restore with: python3 tools/dev/select_harness.py restore "
          f"{backup}")
    return verdict(reset, f"a press held {h.long_ms / 1000:.0f} s factory-resets "
                   "and reboots the RP")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("case", choices=["status", "bounce", "short", "long",
                                     "backup", "restore"])
    ap.add_argument("file", nargs="?", help="backup file (backup/restore/long)")
    ap.add_argument("--elf")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    try:
        h = Harness(args.elf)
        if args.case == "status":
            show("now", h.state())
            return 0
        if args.case in ("backup", "restore"):
            if not args.file:
                raise swd.SwdError(f"{args.case} needs a FILE")
            return (case_backup if args.case == "backup"
                    else case_restore)(h, args.file)
        if args.case == "bounce":
            return case_bounce(h)
        if args.case == "short":
            return case_short(h)
        return case_long(h, args.force, args.file or os.path.join(
            swd.HERE, "logs", time.strftime("settings-%Y%m%d-%H%M%S.bin")))
    except swd.SwdError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
