# Developer tools

Host-side tools for working on this microfirmware with the hardware attached: a SidecarTridge
Multi-device on an Atari ST, with a Raspberry Pi Debug Probe wired to the RP2040's SWD pins and to
its debug UART (GPIO 0/1). Python tools use the standard library only.

## Firmware support these tools rely on

- Debug builds run the console at 921,600 baud (`PICO_DEFAULT_UART_BAUD_RATE` in
  `rp/src/CMakeLists.txt`); release builds have no console at all.
- `rp.elf` keeps its symbol table (the link does not strip it): most commands find their addresses
  by symbol. The symbols never reach the `.uf2`.
- Every build carries its build ID in flash as `release_build_id` (`rp/src/build_id.cmake`).
- Debug builds carry the devhooks mailbox (`rp/src/include/devhooks.h`, included once from
  `emul.c`, served by `devhooks_poll()` in the main loop), which `key`, `app` and `inject` write.

## Debug console: `console.py`

Captures the debug console of a `debug` build (921,600 baud) to `tools/dev/logs/console.log`, with
a timestamp on every line, and shows it in the terminal. Use it instead of a serial terminal such
as CoolTerm: only one program can open the port.

```bash
python3 tools/dev/console.py watch          # leave running in a terminal
```

`watch` finds the Debug Probe by its USB name (`--port` to choose another device), waits for it when
it is unplugged, and reopens it when it returns. While it runs, other commands read the log:

```bash
python3 tools/dev/console.py since-boot                     # everything since the last boot
python3 tools/dev/console.py since-boot --boot 2            # the boot before that
python3 tools/dev/console.py tail 100
python3 tools/dev/console.py grep 'Checksum error' --since-boot
python3 tools/dev/console.py wait 'Start the app loop' --timeout 30
```

`grep` and `wait` take Python regular expressions and exit with 3 when nothing matches. `wait` only
matches lines that arrive after it starts, so start it before the action that should print the
line. The log rotates to `console.log.1` at 32 MB.

The settings dump can print bytes that make macOS `grep` treat the log as binary and print
nothing; use `console.py grep` or `grep -a`.

## Build, flash and verify: `flash.sh`

```bash
tools/dev/flash.sh debug                  # build, flash with picotool, check over SWD
tools/dev/flash.sh release --probe        # flash through the Debug Probe instead
tools/dev/flash.sh debug --build-only     # build only
tools/dev/flash.sh debug --src /tmp/src   # build a copy of rp/src (for example a patched linker script)
```

Builds out of tree in `tools/dev/builds/<type>`, incrementally. It does not touch `rp/build` or
the submodules, and warns when a submodule is not at the version `rp/build.sh` pins. It builds
with the same CMake build type as `rp/build.sh` (MinSizeRel today; `RP_CMAKE_BUILD_TYPE`
overrides it). The m68k
image is not rebuilt: after changing `target/atarist`, run `target/atarist/build.sh` first (it
regenerates `rp/src/include/target_firmware.h`), then `flash.sh`.

Every build carries a build ID: the git commit, `<sha7>`, or `<sha7>-dirty.<diff7>` when the tree
has uncommitted changes, followed by `+debug` in a debug build, so a debug and a release build of one
tree never share an ID. The same tree always gives the same ID, and at the same checkout path a
byte-identical binary (release builds embed source paths, so another path gives other bytes). The
ID is stored in flash as the `release_build_id` string, and `rp.elf` is kept as
`tools/dev/builds/elf/<type>-<id>.elf` for resolving crash addresses later.

Flashing uses `picotool load -f -x`, which reboots the running firmware into BOOTSEL over USB; when
picotool cannot see the RP it falls back to the Debug Probe. Then `flash.sh` checks the result over
SWD with `swd.py`: the RP booted the ELF, its flash matches the ELF byte for byte, and it carries
the new build ID. On failure it exits with 1 and prints the console since the last boot.

## Debug probe: `swd.py`

The tools talk to the RP only through picotool, the Debug Probe and the console UART, never through
the firmware's own services, so they work with any microfirmware built from this template and with
a hung RP. Memory is read while the CPU keeps running.

```bash
python3 tools/dev/swd.py running tools/dev/builds/debug/rp.elf   # booted this firmware?
python3 tools/dev/swd.py verify tools/dev/builds/debug/rp.elf    # flash identical to the ELF?
python3 tools/dev/swd.py build-id                                # which build is on the RP?
python3 tools/dev/swd.py read 0x2003e0c0 8000 fb.bin             # dump memory
python3 tools/dev/swd.py program tools/dev/builds/debug/rp.elf   # flash through the probe
python3 tools/dev/swd.py screen menu.png                         # the setup menu as the ST shows it
python3 tools/dev/swd.py text                                    # the setup menu as text
python3 tools/dev/swd.py shared                                  # token + shared variables
python3 tools/dev/swd.py resume                                  # release cores a debugger left halted
python3 tools/dev/swd.py reset                                   # reset the whole chip, watchdog-style
python3 tools/dev/swd.py select short                            # press SELECT (short press)
python3 tools/dev/select_harness.py bounce                       # 15 ms press: ignored
python3 tools/dev/select_harness.py short                        # short press: the RP restarts
python3 tools/dev/select_harness.py backup settings.bin          # save the settings flash
python3 tools/dev/select_harness.py long --force                 # 10 s press: factory reset
python3 tools/dev/select_harness.py restore settings.bin         # put the settings back
python3 tools/dev/swd.py key g                                   # a keystroke, as if typed on the ST
python3 tools/dev/swd.py app heap_hold 16                        # hold 16 KB more heap (0 releases)
python3 tools/dev/swd.py inject 0x0001 0x0067 0                  # any protocol command
python3 tools/dev/swd.py crash                                   # why did it last reboot?
python3 tools/dev/swd.py postmortem                              # halt, backtraces, resume
python3 tools/dev/swd.py heap                                    # heap size, peak, free space
python3 tools/dev/swd.py counters                                # command channel counters, no halt
python3 tools/dev/swd.py ring                                    # the ST's commands, decoded, no halt
python3 tools/dev/swd.py ring --mark                             # ...then, after a test:
python3 tools/dev/swd.py ring --since-mark                       # only what the ST sent since the mark
python3 tools/dev/swd.py heap --watch 5 --csv tools/dev/logs/heap.csv   # sample during a test
```

`screen` renders the 320×200 framebuffer at `DISPLAY_BUFFER_OFFSET` of the cartridge window as a
PNG (scaled 2×, `--scale`). It shows what the RP draws for the ST: the setup menu, not GEM or a
running program. `text` prints the terminal's character buffer (the `screen` array of term.c); the
bottom status line is drawn straight to the framebuffer and only shows in `screen`. `shared`
prints the command sentinel, the random token, the token seed and the shared variables, named
after the `*_SVAR_*` / `*_SHARED_VARIABLE_*` indexes in `rp/src/include` (`chandler.h` names the
first three slots). They take the
window address from the ELF; without `--elf` they use the cached ELF whose build ID the RP
carries, so flash the build with `flash.sh` first.

`program` halts both cores and stops every PIO state machine and DMA channel before it writes:
halting the cores does not stop the RP2040's DMA, and while the ST touches the cartridge the ROM3
capture ring keeps writing bus samples into RAM, where the flash write stages its data (it has
programmed bus samples into an image). Flash through the probe with this tool, not with OpenOCD's
own `program`.

`program` and `reset` restart the chip through the watchdog (PSM `WDSEL` + `WATCHDOG_CTRL.TRIGGER`),
never with OpenOCD's `reset`. A firmware that launches core 1 early (as the SELECT watcher this
template used to ship, `select_coreWaitPush()`, would have) meets OpenOCD's multi-core reset
sequence touching core 1 again just after that: core 1 died in the middle of its first trace holding
the SDK's stdio mutex, and every piece of debug output then waited out the 1 s
`PICO_STDIO_DEADLOCK_TIMEOUT_MS` (a debug boot of 220 s instead of 0.7 s, and a dead SELECT button).
A watchdog-style reset is the one that leaves the chip exactly as a power-on reset does, with DMA
and PIO stopped. If an old build shows that symptom, run `swd.py reset` or power-cycle. The same
applies to a VS Code debug session's restart button.

OpenOCD loads small routines into a RAM work area (`verify_image`'s CRC, the flash-size probe of a GDB
connect). `rp2040.cfg` puts it at `0x20010000`, inside this firmware's RAM, where it once overwrote
the Wi-Fi driver's async context and the next `cyw43_arch_poll()` HardFaulted. `swd.py` gives every
run that does not write flash a 4 KB work area in `SCRATCH_X` instead, backed up and restored: that
is core 1's stack, and this firmware never starts core 1. A firmware that does must move it. Flash
writes keep the default, with the cores halted and a reset after. Do not attach GDB (`postmortem`)
while the RP is rebooting: the flash probe of the connect, landing while boot2 sets up XIP, left
flash unreadable and the firmware executing zeros until the next reset.

A halted RP can still be read. Halting core 1 also pauses the RP2040's timer, so after a debugger
halt run `resume`, which releases both cores; OpenOCD's own `resume` fails in a new OpenOCD run.

`select` needs no firmware code: it forces the SELECT pin's input high through the RP2040's GPIO
input override for 300 ms (`short`) or `SELECT_LONG_RESET` + 1 s (`long`). A long press needs
`--force`, because in a microfirmware that wires SELECT the usual way it is a factory reset: it
erases the global settings, and Booster then clears every app's settings. In this template a short
press restarts the RP. `select release` clears an
override left behind. The press is held inside one OpenOCD session, so nothing else can use the
probe until it ends: to look at the device during a press, put the reads in that same session
(`mww` the override, `sleep`, `mdw`/`mdb` what you want, `mww` it back).

`key`, `app` and `inject` need a `debug` build. They write a small mailbox in RAM
(`rp/src/include/devhooks.h`, found by its `devhooksMailbox` symbol) and wait for the main loop to
acknowledge it. `key` and `inject` queue a protocol command as if the ST had sent it, through
`chandler_injectProtocol()`, so the firmware handles it through its normal path: the same
callbacks, the same answer. The setup terminal is line-based, so a menu command is its key and
then Enter (`swd.py key h`, then `swd.py key $'\n'`). `app NAME` runs the app command defined as
`DEVHOOKS_APP_<NAME>` in `rp/src/include/emul.h`, handled by `emul_devhooksApp()`:

- `heap_hold KB`: hold KB more kilobytes of heap, on top of what is already held (`heap_hold 0`
  releases everything). Result 0 when the allocation is refused, so repeated calls walk the heap
  down to a known remainder. Watch it with `swd.py heap`.

Add an app's own commands the same way: a `DEVHOOKS_APP_<NAME>` define and a case in the handler.
Useful ones in other microfirmwares: stop a boot countdown; stall or fail the next answer on
purpose, to exercise the ST's retry path.

`ring` decodes what the ST sent from the ROM3 capture ring (`commemul.c`) without halting the RP,
on a release build as on a debug one. It holds the last 8,192 bus samples: about 800 small
commands (6 to 10 samples each), or 15 writes of 1 KB. Each frame shows its sample number since the RP booted, the command (named
from the `APP_<APP>` / `APP_<APP>_<COMMAND>` defines, as `term.h` has them, and chandler's
framework commands; others print as hex), its payload size, the random token, the first four 32-bit
parameters in the order `TPROTO_GET_PAYLOAD_PARAM32` reads them, and a mark on a bad checksum.
The oldest frame is often cut by the ring's start and shows as a bad checksum. A frame whose size
is past `MAX_PROTOCOL_PAYLOAD_SIZE` shows as dropped, as the RP drops it.
The DMA keeps writing while the ring is read, so `ring` reads its write position before and
after and drops the samples it may have overwritten meanwhile. During a sustained burst of 1 KB
writes the ST sends about 200,000 samples a second and refills the ring in about 40 ms, faster
than SWD copies it: `ring` then says so instead of printing half-overwritten frames, and reads
normally once the burst is over. `--mark` stores the position in
`tools/dev/logs/ring.mark` and `--since-mark` shows only what came after it, or says how much the
ring lost in between. Commands sent with `key` or `inject` go through the mailbox, not the bus, so
they never appear here. An ST reset through the sentinel shows up as `CHANDLER_ST_HELLO` and two
`CHANDLER_SET_SHARED_VAR` (the machine, then the TOS version).

`crash` prints the watchdog reason and scratch registers of the last reboot without stopping the
RP, with code addresses resolved to source lines by `addr2line`.

`postmortem` halts the RP and prints both cores' backtraces, the registers, the watchdog registers
and key variables through GDB (`$ARM_GDB_PATH/bin/arm-none-eabi-gdb`, as in `.vscode/launch.json`),
then resumes it; `--leave-halted` keeps it stopped for `swd.py resume`. Halting stops the
cartridge bus, so the ST sees a dead cartridge until the RP resumes.

`heap` reads newlib's own malloc state while the RP keeps running, so it needs no firmware code
and works on release builds. It prints the heap's size (from the end of `.bss` to
`__StackLimit`), the arena taken from it so far, the **peak** arena ever reached
(`__malloc_max_sbrked_mem`) with how close that came to the stack, and, by walking the heap's
chunks, the bytes in use, the free bytes inside the arena, how many free blocks they are in and the
largest one. The heap only grows (memory freed stays in the arena for reuse), so the peak also
catches short-lived allocations between samples. A shortage shows as a peak with little room left
before the stack, or as plenty of free bytes but a small largest block (fragmentation). `--watch
SECONDS` samples until Ctrl-C; `--csv FILE` appends every sample for later comparison. If the
heap changes while it is read, the chunk walk is retried once and otherwise reported as failed.
In this template the heap's limit, `__StackLimit`, lies past the end of the 64 KB cartridge window
(`rp/src/memmap_rp.ld`), so the size `heap` reports includes the window: an arena that grows into
it is overwriting the cartridge image the ST is reading.

OpenOCD is `$OPENOCD`, `openocd` on `PATH`, or `../pico/openocd/src/openocd`; its scripts come
from `$PICO_OPENOCD_PATH`, the variable `.vscode/launch.json` uses. A command that fails on a
momentary debug-port drop (common while the firmware changes its clock early in boot) is retried.
Close a VS Code debug session first: only one program can use the probe.

## Hardware harnesses

Scripts that run a set of checks on the device and print PASS or FAIL for each. All need the
Debug Probe and `console.py watch` running (they read its log, never the UART); each writes a JSON
report to `logs/` and exits 0 only when every check passes.

```bash
python3 tools/dev/tools_harness.py --build --flash --reset   # every tool above, against the device
python3 tools/dev/st_harness.py --oversize --long            # the command path, from the ST's side
python3 tools/dev/power_cycles.py                            # cold boots: menu or GEM? (3 by default)
python3 tools/dev/download_harness.py                        # real downloads, checked by MD5
```

- `tools_harness.py` checks the tools themselves on a debug build: the running firmware and its
  build ID, the console, `screen` / `text` / `shared`, the mailbox commands (`key`, `inject`,
  `app heap_hold`), `counters`, `ring`, a SELECT press (which restarts the RP), `crash` and
  `postmortem`. `--build` also builds both types and checks their flags and symbols, `--flash`
  flashes the debug build first, and `--reset` restarts the RP at the end.
- `st_harness.py` tests the command path from the ST's side. It copies the tree to
  `builds/sttree`, puts `sttest.s` in place of `userfw.s`, builds and flashes that debug firmware,
  resets the ST through the sentinel so it runs the new cartridge code, and starts the tests with
  `[F]irmware`. The ST reports each result as a `$7Fxx` command, which the setup terminal logs:
  - T0: the return address into TOS the cartridge hands over.
  - T1: `d0 = 0` with Z set after each sender.
  - T2: the registers each send keeps.
  - T3 and T4: 100 small commands and 10 of 1 KB, none failed.
  - T5 (`--oversize`): an oversize frame, then a command that must still be answered.
  - T6 (`--long`): a burst of 3,000 commands of 1 KB, about 7 s.

  `--connect-race` instead resets the ST and the RP together, and times the ST's boot commands
  against the RP's Wi-Fi connect. `--no-build --no-flash --no-reboot` runs the tests again at once
  on an ST still in the setup menu from a run: with `download_harness.py --url ... --no-wait` just
  before, the command path is tested while a download runs. The test firmware stays on the RP, so flash the real one
  afterwards. The working tree is never touched.
- `download_harness.py` downloads the files Booster downloads, on the device, and checks each by
  MD5. The expected hashes are read at run time: F1 is `upgrade.bin` against `upgrade.md5`, F2 a
  catalog microfirmware against the catalog's MD5, F3 the catalog itself, F4 a catalog entry behind
  a GitHub redirect chain, and F5 a missing file, which must fail. PORT writes the port out, and
  LONG is a URL past the build's limit, which must be refused. On an HTTPS build (`+https` in
  the build ID) every case runs over http:// and https://; on an HTTP build `https://` must fail
  as not built in. For a failure it checks the reason (`download_err_t`, read from `download.h`)
  and that no file was left. The download runs in the debug-only `rp/src/devdownload.c`, through
  `download.h` as an app would: the harness writes the URL into its RAM, starts it through the
  devhooks mailbox (`DEVHOOKS_APP_DOWNLOAD`) and reads its state over SWD. It needs the SD card and Wi-Fi. `--url URL` downloads one URL and prints the outcome;
  `--no-wait` starts it and frees the probe for another tool (to run `st_harness.py` during a
  download), and `--status` reads the outcome later.
- `power_cycles.py` watches the log while you power-cycle the ST. For each boot it prints when the
  cartridge went live and whether the ST said hello (setup menu) or not (GEM). Keep power cycles
  few: every other ST boot can come from a reset through the sentinel.

## Measuring builds

- `measure_builds.sh` builds each build type out of tree with `-fstack-usage` and
  `-fcallgraph-info=su`, and reports the flash, RAM and heap numbers.
- `stackdepth.py` computes worst-case stack depth from those builds (static call edges only, so
  treat its answer as a floor).

`logs/` and `builds/` are generated here and are gitignored.
