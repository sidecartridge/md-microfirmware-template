# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See also: `programming.md` (full shared-region table and budget rules), `README.md` (high-level region/userfw overview), `AGENTS.md` (host setup, copy-pasteable commands, and a symptom → fix table).

## What this repo is

Template for a **Sidecartridge Multi-device microfirmware app** targeting Atari ST / STE / MegaST(E). Each "app" is a UF2 image that runs on a Raspberry Pi Pico (RP2040) plugged into the Multi-device cartridge slot, emulating a ROM cartridge for the Atari while also handling networking, SD card I/O, and config. Public build/usage docs are at <https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

## Build

Top-level build is driven by `build.sh` in the repo root:

```bash
# <board_type> = pico | pico_w | sidecartos_16mb
# <build_type> = debug | release   (note: always compiled as MinSizeRel — see below)
# <app_uuid_key> = UUID4 identifying this app, must match desc/app.json
./build.sh pico_w release 123e4567-e89b-12d3-a456-426614174000
```

Required host environment:
- ARM GNU Toolchain 14.2 — export `PICO_TOOLCHAIN_PATH` to its `arm-none-eabi/bin` dir.
- `atarist-toolkit-docker` (`stcmd`) — needed for the m68k target. It runs `docker run -it`, so it needs a TTY unless `STCMD_NO_TTY=1` is exported (the build scripts set it; see the gotcha below).
- SDK paths (auto-set from the repo if unset): `PICO_SDK_PATH`, `PICO_EXTRAS_PATH`, `FATFS_SDK_PATH`.

Build flow (orchestrated by `build.sh`):
1. Copies `version.txt` into `rp/` and `target/atarist/`.
2. Builds the Atari ST target (`target/atarist/build.sh`) via `stcmd make`. Enforces an **8 KB hard limit** on `BOOT.BIN` (the cartridge code budget — `CHANDLER_CARTRIDGE_CODE_SIZE` in `rp/src/include/chandler.h`, mirrored as `CARTRIDGE_CODE_SIZE` in `target/atarist/src/main.s`); a build that exceeds it aborts with `ERROR: cartridge code is N bytes; limit is 8192`. A separate copy (`FIRMWARE.IMG`) is then padded to 64 KB to fill the entire shared region, and `firmware.py` converts it into `rp/src/include/target_firmware.h` (a C byte array embedded in the RP firmware).
3. Builds the RP firmware (`rp/build.sh`): pins submodule versions (pico-sdk 2.2.0, pico-extras sdk-2.2.0, fatfs-sdk at a specific commit), runs CMake, produces `rp/dist/rp-<board>.uf2`. The FatFs configuration lives at `rp/src/ff/ffconf.h` and shadows the submodule's default via `target_include_directories(... BEFORE PRIVATE)` in `rp/src/CMakeLists.txt`, so the `fatfs-sdk` submodule stays pristine.
4. Computes MD5, renames to `dist/<APP_UUID>-<VERSION>.uf2`, and substitutes UUID/MD5/version into `dist/<APP_UUID>.json` from the `desc/app.json` template.

### Build gotchas
- **CMake always builds with `-DCMAKE_BUILD_TYPE=MinSizeRel`** regardless of the `<build_type>` argument. A full `Release` previously caused breakage (memory/over-optimization). The legacy line is left commented in `rp/build.sh`. `<build_type>` only controls the `DEBUG_MODE` macro and the dist filename.
- `CHARACTER_GAP_MS` must remain defined (700) in `rp/src/include/blink.h` — removing it breaks the RP build.
- `rp/build.sh` runs `git submodule update --init --recursive` and hard-`checkout`s the pinned revisions on **every** build, and `build.sh` deletes and recreates `dist/` first — any local edit inside a submodule, or anything left in `dist/`, is gone on the next build.
- Harmless VASM warnings during the m68k build (`target data type overflow`, `trailing garbage after option -D`) can be ignored.
- VASM/`stcmd` errors like `the input device is not a TTY` mean `stcmd` was invoked without a PTY. `target/atarist/build.sh` already exports `STCMD_NO_TTY=1` for every `stcmd` call it makes; you only need to export it yourself if invoking `stcmd` directly from a non-TTY context (CI, sub-shells, build wrappers). Without it the m68k build can fail silently and the previous `BOOT.BIN` survives — leading to a working RP firmware that displays garbage on the ST because `target_firmware.h` is stale.

### CI / release
- `.github/workflows/build.yml` builds `pico_w` Release on PR.
- `.github/workflows/release.yml` triggers on `v*` tags: builds, attaches UF2 + JSON to the GitHub Release, uploads to `s3://atarist.sidecartridge.com/`.
- `make tag` tags HEAD with the contents of `version.txt` and pushes the tag (which triggers release). **In this template repo, don't tag** — a change lands as a `version.txt` bump plus a `CHANGELOG.md` entry; tagging/releasing is the repo owner's call. Apps generated from the template do tag.
- `upload_s3.sh <file>` is a manual one-off uploader; needs `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY`.

### Tests / verification
There is no test suite, and no way to run this off-hardware. "Verification" is: the build succeeds (including the 8 KB cartridge assertion), the UF2 boots on the device, and you interact with it manually over the serial debug console and the ST terminal menu.

- `DPRINTF` / `DPRINTFRAW` (`rp/src/include/debug.h`) compile to nothing unless `_DEBUG != 0`, and `pico_enable_stdio_uart` is only turned on for debug builds — so a `release` UF2 prints nothing at all. Build with `./build.sh <board> debug <uuid>` to get a serial console. (Up to v1.2.1 `rp/src/CMakeLists.txt` turned `DEBUG_MODE=0` into `_DEBUG=1`, so every "release" build was a debug build; if a release build ever prints, check `-D_DEBUG=` in its `flags.make`.)
- On-chip debugging is via a Picoprobe/Debug Probe and the `cortex-debug` VS Code launch config in `.vscode/launch.json`; it needs `ARM_GDB_PATH` and `PICO_OPENOCD_PATH` exported. CMake source dir is `rp/src`, build dir is `rp/build`.
- The m68k side has no debugger: its only observable output is what it prints on the ST screen.

With a Raspberry Pi Debug Probe attached, `tools/dev/` turns that manual loop into something
repeatable (its `README.md` has the full command list):

- `console.py watch` captures the debug UART (921,600 baud) to `tools/dev/logs/console.log` with
  timestamps, and `since-boot` / `grep` / `wait` query the log while it runs. Only one program can
  hold the port, so use this instead of a serial terminal; other tools read the log, not the port.
  `wait` only matches lines that arrive after it starts.
- `flash.sh <debug|release>` builds out of tree in `tools/dev/builds/` incrementally (seconds, not
  a full rebuild), flashes over USB or the probe, then verifies over SWD that the RP booted the
  ELF, that its flash matches byte for byte, and that it carries the expected build ID.
- `swd.py` reads a *running* RP over SWD: `screen` renders the framebuffer as the ST sees it
  (a PNG), `text` prints the terminal buffer, `shared` dumps the sentinel/token/shared variables,
  `heap` reads newlib's allocator, `crash` explains the last reboot and `postmortem` halts for
  backtraces. On debug builds `key`, `inject` and `app` drive the firmware through the devhooks
  mailbox (`rp/src/include/devhooks.h`): `swd.py key h` then `swd.py key $'\n'` runs the `h`
  menu command, `swd.py app heap_hold 16` holds 16 KB of heap. They rely on the ELF keeping its
  symbols and on the build ID in flash; both are part of every build.
- Neither this firmware nor the siblings expose picotool's USB reset interface, so `flash.sh`
  flashes through the probe (about 25 s with verification).
- Flash through the probe only with `swd.py program` (or `flash.sh --probe`, which uses it), never
  with OpenOCD's own `program`: halting the cores does not stop the RP2040's DMA, and while the ST
  touches the cartridge the ROM3 capture ring writes bus samples into the RAM the flash write is
  staged in. `swd.py program` stops every PIO state machine and DMA channel first.

### Planning backlog (`docs/`)
`docs/epics/` holds the local planning notes (`cockpit.sh` regenerates `STATUS.md`; `ITERATIONS.md`
carries the narrative; `DECISIONS.md` the decisions and standing constraints). **The whole `docs/`
tree is gitignored** and lives only on the developer's machine.

- **Never name an epic, story, iteration or task in anything that is committed** — code comments,
  documentation, `CHANGELOG.md`, commit messages, PR descriptions. An identifier like
  `EPIC-03 STORY-01` tells a reader of this repository nothing and cannot be looked up, and a
  commit message cannot be scrubbed afterwards. Write what the code does and why instead: *"the
  heap limit now stops at the start of the cartridge mirror"*, not a pointer to a planning note.
- **Never reference a path under `docs/`** from anything that ships, for the same reason: someone
  who clones the repo has no `docs/`. Inline the information itself.
- Traceability runs the other way: once work lands, record the commit hash in the planning note.
- Before tagging a release, check:
  `git grep -IiE "EPIC-|STORY-|\bepics?\b|\bstor(y|ies)\b|docs/" -- . ':!CLAUDE.md' ':!.gitignore'`
  must come back empty (this section and the `.gitignore` entry are the only places the backlog
  is mentioned).

## Architecture

The firmware is a **two-target build**: m68k assembly that runs on the Atari ST is compiled into a ROM image, embedded as a C array inside the RP2040 firmware, and served back to the Atari over the cartridge bus that the RP2040 emulates via PIO + DMA.

### Atari ST side (`target/atarist/`)
- `src/main.s` — m68k cartridge boot + dispatch + terminal. Lives at `$FA0000` in the ST address space (ROM4 cartridge region). Defines the cartridge header (`CA_MAGIC`, `CA_INIT`, …), command magic numbers, and the shared-variable layout used to talk to the RP2040.
- `src/userfw.s` — **the primary extension point for app-specific m68k code.** `src/userfw.ld` places `main.s` at offset `0x0000` (2 KB budget) and `userfw.s` at offset `0x0800` (6 KB budget); `main.s` exposes the latter as `USERFW equ (ROM4_ADDR + $800)`. When the RP-side terminal command `f` ([F]irmware) is selected, the RP writes `CMD_START = 4` to the cartridge sentinel; the m68k's vsync-polled `check_commands` dispatches to `rom_function`, which `jmp`s to `USERFW`. The default `userfw.s` is a Cconws demo that returns to TOS — replace its body with your own logic. It includes `inc/sidecart_layout.s` (the window and the command channel, shared with `main.s`), the macros and, at its end, the senders followed by the NOP tail. Its code must be PC-relative: it is linked at offset `$0800`, not at `$FA0800`.
- Adding more m68k modules: add a new `.text_<name>` section in `userfw.ld`, mirror the offset with an `equ (ROM4_ADDR + $????)` in `inc/sidecart_layout.s`, and add the `.o` target to `target/atarist/Makefile` (same pattern as `gemdrive.ld` in `md-drives-emulator`).
- Built via `stcmd make release` (m68k assembler in Docker); the cartridge image (header + all `.text_*` sections) must fit in 8 KB. A 64 KB padded copy is then converted to `target_firmware.h` for inclusion in the RP build.

### Shared 64 KB cartridge region
The Atari ST sees a 64 KB window at `$FA0000`–`$FAFFFF` (mirrored RP-side at `0x20030000`). This is the **single source of truth** for any cross-target data layout — both sides derive every offset symbolically from constants in `rp/src/include/chandler.h` (RP-side) and `target/atarist/src/main.s` (m68k side). **Apps must never hard-code an address inside this region** — always reference the named offset/symbol.

| Offset | Symbol | Size | Purpose |
| --- | --- | --- | --- |
| `$FA0000` | cartridge image | 8 KB | m68k header + all `.text_*` sections (hard limit) |
| `$FA2000` | `CMD_MAGIC_SENTINEL` | 4 B | m68k polls here for NOP/RESET/command words |
| `$FA2004` | `RANDOM_TOKEN`, `RANDOM_TOKEN_SEED`, reserved, then 60 × 4 B indexed shared variables | 256 B | fixed-offset metadata block, ends at `$FA2100` |
| `$FA2100` | high-res translation table | 512 B | start of `APP_BUFFERS` |
| `$FA2300` | `APP_FREE` | ~48 KB | contiguous arena for app buffers |
| `$FAE0C0` | `FRAMEBUFFER` | 8000 B | 320×200 monochrome framebuffer; sits at the top of the region so an overrun walks off the end of the 64 KB window instead of corrupting the metadata block |

See `programming.md` for the full table and budget rules.

### RP2040 side (`rp/src/`)
- `main.c` — only sets clock/voltage, calls `gconfig_init` (global config) then `aconfig_init` (per-app config), and hands off to `emul_start()`. If config init fails it jumps to the **Booster** app via `reset_jump_to_booster()` to bootstrap. **Don't add features to `main.c`** — put them in `emul.c` or a new module.
- `emul.c` / `emul.h` — the application's main loop and entry point. This is where to add new features. `emul_start()` runs the fixed bring-up order: copy `target_firmware` into `ROM_IN_RAM` → `init_romemul(false)` → `commemul_init()` → `chandler_init()` + `chandler_addCB(...)` → display → SD → network → `init()` → main loop. The RP-side terminal UI is a `commands[]` table (`{"f", cmdFirmware}`, …) handed to `term_setCommands()`; add a menu command by adding a row plus its handler.
- `romemul.c` / `romemul.pio` — ROM4 **read** engine: a PIO SM latches the 16-bit address, and a chained DMA pair reads that offset out of `ROM_IN_RAM` and pushes it to the PIO TX FIFO. Fully DMA-driven, no IRQ, no CPU. Driven by the `READ_*` / `WRITE_*` GPIOs in `include/constants.h`. Changing these files produces very strange bugs.
- `commemul.c` / `commemul.pio` — ROM3 **command** capture: a PIO SM on `ROM3_GPIO` plus one ring-mode DMA channel continuously records every ROM3 access into a 16 384-word ring. Drained by polling (`commemul_poll`), never by IRQ.
- `chandler.c` / `include/chandler.h` — the polled command dispatcher on top of `commemul` + `tprotocol`, and the RP-side source of truth for the shared-region offsets (`CHANDLER_*`).
- `gconfig.c` / `aconfig.c` — global vs per-app configuration stored in dedicated flash sectors, on top of `settings/` (a key-value store).
- `network.c`, `httpc/`, `download.c` — Wi-Fi (CYW43, lwIP poll mode), HTTPS-capable HTTP client, firmware download support.
- `sdcard.c`, `hw_config.c` — FatFs over SPI/SDIO via the bundled `fatfs-sdk`.
- `display.c`, `display_term.c`, `term.c`, `u8g2/` — terminal-style display rendered into the Atari framebuffer at `$FAE0C0` and/or a local OLED.
- `blink.c`, `select.c`, `reset.c`, `tprotocol.c` — LED Morse status, SELECT-button helpers (debounce, short/long press callbacks — **not wired up**: `emul.c` only calls `select_configure()` and prints the pin state, so pressing SELECT does nothing in this template; never use `select_coreWaitPush()`, which launches core 1 and can fault both cores during a flash erase), soft reset/jump-to-booster, command-protocol parser and `TPROTO_*` payload accessors.

### Command path (Atari ST → RP2040)
The cartridge port is read-only, so the m68k sends commands by *reading* from addresses inside ROM3 (`$FB0000`+); the low 16 bits of each address are the data. `tprotocol.c` reassembles that address stream into framed commands (`0xABCD` header, command id, payload size, payload, checksum).

The whole path is **polled, not interrupt-driven** (since template v1.1.0):

```
m68k reads $FB….  →  commemul PIO+DMA ring  →  chandler_loop()  →  tprotocol_parse
                                                      ↓ (complete + checksummed)
                                        registered callbacks, in registration order
                                                      ↓
                        chandler writes the random-token reply into shared memory
```

- Register handlers with `chandler_addCB(cb)` after `chandler_init()`; signature is `void cb(TransmissionProtocol *p, uint16_t *payloadPtr)`, with `payloadPtr` already advanced past the 32-bit random token. Read parameters via the `TPROTO_GET_*` macros and write results back with `memfunc.h` helpers — never with raw pointer arithmetic, because of the endianness swap.
- **`chandler_loop()` must be called from every loop that can block.** The main loop calls it, and so must any long-running wait: `emul.c` installs `emul_pollTick` (`chandler_loop(); term_loop();`) via `network_setPollingCallback()` for the multi-second Wi-Fi connect. Passing `term_loop` alone drops commands.
- The m68k waiter requires the RP to advance `RANDOM_TOKEN_SEED` (a strictly-incrementing counter), not just echo `RANDOM_TOKEN` — echoing alone is indistinguishable from open-bus reads when no cartridge is present. Preserve that invariant in `chandler.c`.

#### What each side may assume about the other
The ST side is `target/atarist/src/inc/sidecart_functions.s` (`send_sync_command_to_sidecart` and its write variant, wrapped by the `send_sync` / `send_write_sync` macros); the RP side is `chandler.c` + `tprotocol.h`. Rules that hold across both, each learned on hardware:

- **The ST's timeout is a spin count, not time, and it is per file.** `COMMAND_TIMEOUT` (`$FFFF`) counts iterations of the token-compare loop, so it shrinks as the CPU gets faster, and each module that includes `inc/sidecart_functions.s` has its own copy.
- **On a Mega STE the cache must be off while the ST talks to the cartridge; the speed does not matter.** Measured on a Mega STE (TOS 2.06) in md-drives-emulator: at 16 MHz without the cache commands work as at 8 MHz; with the cache on they never reach the RP. `main.s` clears bit 0 of `$FFFF8E21` from `pre_auto` (by the `_MCH` cookie) until it hands over, and puts the user's setting back in `boot_gem` and before `jmp USERFW`; user firmware wraps each send in `megaste_cache_off` / `megaste_cache_back`. Never step the machine down to 8 MHz.
- **The ST says hello, then publishes the machine and TOS, at every boot.** `main.s` first sends `CMD_ST_HELLO` (`$FF01`, `CHANDLER_ST_HELLO`, no payload) until it is answered, then calls `detect_hw` and `get_tos_version`, which send `CMD_SET_SHARED_VAR` (`$FF00`, `CHANDLER_SET_SHARED_VAR`). chandler answers both itself, before any callback. The hello makes `chandler_stPresent()` true and `chandler_consumeStBoot()` true once, on which `emul.c` drops what was typed before the reset and redraws the menu; the random token and seed carry on across ST boots on purpose. `$FF00` makes `chandler_consumeSharedVarSet()` true once (the menu redraws its Atari line on it, rather than reading the variables on a timer) and sets shared variable 0 (the `_MCH` cookie, 0 for an ST) and 1 (ROM TOS version << 16 | GEMDOS `Sversion`). The senders' Mega STE and 68030 checks read variable 0.
- **What a send keeps**, measured on an ST. `send_sync` keeps d1-d6 and destroys d0, d7, a0-a1; `send_write_sync` keeps d1-d5 and a4 and destroys d0, d6 (its retry count), d7, a0-a1 (both also a2-a3 when `COMMAND_SYNC_USE_DSKBUF` is not 0). Both return with `d0 = 0` and Z set on success, so callers may branch on the flags. The senders leave a0-a1 pointing into the ROM3 command window: a payload read through them is itself sampled by the RP and fails the checksum on every retry. Take what you need into a kept register before the send.
- **A 68030 (TT, Falcon) runs code copied into RAM from a stale instruction cache** unless it is cleared after the copy (CACR's CI bit). This applies to the senders' wait loop when `COMMAND_SYNC_USE_DSKBUF` is not 0, and to anything else copied into RAM and run.
- **End every m68k module with a NOP tail** after `include "inc/sidecart_functions.s"` (`even`, eight `nop`s, a `<module>_end:` label). `firmware.py` strips trailing zeros, the RP copies only `target_firmware_length` words, and the write sender and the 68000's prefetch read past the end of the loop: without the tail those bytes are uninitialised RP RAM.
- **A retry is a new command.** The macros resend up to `CMD_RETRIES_COUNT` times, each with a *fresh* token (the token is the seed read just before sending). The RP may already have executed the first attempt, so a command must be idempotent or carry its own sequence number.
- **Token + seed is a two-phase commit.** The RP stores the token and then the seed; the ST accepts an answer only when the token matches *and* the seed has moved. That also covers absent hardware and a freshly zeroed window.
- **The sentinel is a level, not a queue.** The ST samples `CMD_MAGIC_SENTINEL` once per vsync, only while it is in its poll loop — not inside `send_sync`, not after `jmp USERFW`. A value written once and then replaced can be missed.
- **The two sides reboot independently.** The RP rewrites the 64 KB window when it boots, so anything the ST published at its own cold boot is gone after an RP-only reboot. Until the ST's next hello the setup menu says so and `[F]irmware` is refused, since user firmware relies on what the ST publishes at boot. The ST's print loop survives that (it runs from RAM and is stateless); `rom_function` does `jmp USERFW` into cartridge ROM, so user firmware that must outlive an RP reboot or a reflash has to relocate itself to RAM first.
- **Answer first.** In `chandler_loop()` the token write is what releases the ST; an LED pulse (on a Pico W that is a CYW43 bus transaction), a blocking `DPRINTF`, or any other slow work goes after it.

The RP drains the ring on every pass of its main loop and never waits: measured on an ST, about 2 ms per command with a 4-byte payload and 3 ms with 1 KB. The parser drops a frame whose payload size is past its buffer, and its 50 ms silence window restarts on every sample. Bringing Wi-Fi up blocks and drains nothing: `network_wifiInit()` took about 0.9 s on a Pico W, and at power-on the ST's boot hello waited that out, which it can, since `main.s` resends it until it is answered. Wi-Fi is polled every 10 ms from the same loop, not on every pass: polled flat out, the RP hard-faulted inside `cyw43_arch_poll()` for a reason not yet understood.

Known defects in this path as the code stands — check before building on it:

- A debug build prints its banner, the flash layout and the settings dumps *before* `emul_start()` makes the cartridge live, so a power-cycled ST can boot into GEM on a debug build and into the menu on a release build. At 921,600 baud the cartridge is live 43-69 ms after the banner, and three power cycles of an ST (TOS 1.04) all reached the menu, with the ST's hello 1.1 s after the banner; a machine that probes the cartridge sooner than that is not measured.
- The settings dump and `network.c`'s connect traces print the Wi-Fi password in clear on debug builds.

### Memory layout (`rp/src/memmap_rp.ld`)
The RP2040's 2 MB flash is sliced into named regions, and code is responsible for not stomping on them:

| Region | Origin | Length | Purpose |
| --- | --- | --- | --- |
| `FLASH` | `0x10000000` | 1024 K | App code |
| `ROM_TEMP` | `0x10100000` | 128 K | Scratch area for loaded ROMs |
| `BOOSTER_APP_FLASH` | `0x10120000` | 768 K | Reserved for the Booster app (do not write from this app) |
| `CONFIG_FLASH` | `0x101E0000` | 120 K | 30 sectors of per-app config |
| `GLOBAL_LOOKUP_FLASH` | `0x101FE000` | 4 K | UUID → config-sector lookup |
| `GLOBAL_CONFIG_FLASH` | `0x101FF000` | 4 K | Global config |
| `RAM` | `0x20000000` | 192 K | Normal RAM (data, BSS, heap) |
| `ROM_IN_RAM` | `0x20030000` | 64 K | The cartridge window the ST reads at `$FA0000` |
| `SCRATCH_X` / `SCRATCH_Y` | `0x20040000` / `0x20041000` | 4 K each | Core 1 / core 0 stacks (SDK default: 2 K reserved for core 0) |

The heap's limit, `__StackLimit`, is `ORIGIN(RAM) + LENGTH(RAM) + LENGTH(ROM_IN_RAM)`, so as the
script stands the heap can grow into the cartridge window and overwrite the image the ST is
reading. `tools/dev/swd.py heap` reports a heap size that includes the window for that reason.

The build assumes Core 0 owns flash writes (`PICO_FLASH_ASSUME_CORE0_SAFE=1`). The PIO bus emulation runs hot — Core 0 also overclocks to 225 MHz at `VREG_VOLTAGE_1_10`.

### App identity
`CURRENT_APP_UUID_KEY` (set from the `APP_UUID_KEY` env var at CMake time, with a placeholder default) is the app's UUID4. It must match the `uuid` field in `desc/app.json` and is used as the key into `GLOBAL_LOOKUP_FLASH` to find this app's config sector. Mismatch → app jumps to Booster.

## Publishing

A successful build leaves three files in `dist/`: `<APP_UUID>-<VERSION>.uf2` (the firmware users install), `<APP_UUID>.json` (the app descriptor) and `rp.uf2.md5sum` (a byproduct; its hash is already inside the descriptor).

The descriptor is generated, not hand-written. Edit `desc/app.json`: fill in `name`, `description`, `image`, `tags`, `devices` and `binary`, and **leave `<APP_UUID>`, `<APP_VERSION>` and `<BINARY_MD5_HASH>` as placeholders** because `build.sh` substitutes them on every run. Write the `binary` URL with those placeholders too, so it tracks the version across releases. `build.sh` aborts if `desc/app.json` is missing.

Getting an app into the public catalogue that Booster downloads is **one pull request** against [`sidecartridge-microfirmwares-store`](https://github.com/sidecartridge/sidecartridge-microfirmwares-store), adding an origin that points at an `apps.json` you host yourself. The store keeps a pointer, not a copy, so later releases need no further pull request. Walkthrough: <https://md-store.sidecartridge.com/build/atari-st/11-get-listed.html>

For an assistant that does not read this file automatically, the same guide publishes a portable copy of the architecture and the hard constraints at <https://md-store.sidecartridge.com/build/atari-st/context.md>. It supplements this file; where the two disagree, this file wins.

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, or `fatfs-sdk/` — they are git submodules pinned to specific upstream revisions, and the build re-pins them on every run. To change FatFs configuration, edit `rp/src/ff/ffconf.h` (project-owned override); the include path is set up so this file wins over the submodule's default.
- Don't touch `main.c` for feature work — start in `emul.c`.
- Match the existing C style (clang-format config in `.clang-format`, clang-tidy in `.clang-tidy`). VS Code runs both. The clang-tidy block in `rp/src/CMakeLists.txt` sets `CMAKE_C_CLANG_TIDY` after the target already exists, so it has never run from a command-line build — a clean build is not a clang-tidy pass.

---

## Working style

These behavioral guidelines bias toward caution over speed. For trivial tasks, use judgment.

### 1. Think before coding

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity first

Minimum code that solves the problem. Nothing speculative.
- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical changes

Touch only what you must. Clean up only your own mess.
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.
- When your changes orphan an import/variable/function, remove it. Don't remove pre-existing dead code unless asked.

The test: every changed line should trace directly to the user's request.

### 4. Goal-driven execution

Define success criteria. Loop until verified.
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan with a verification check per step.

### 5. No AI attribution

Never add AI-tool attribution to commits, PR descriptions, code comments,
docs, or any other artifact. This means **no**:
- "Generated with Claude Code", "Co-authored by Claude", "Made with ChatGPT",
  or any similar phrasing.
- `Co-Authored-By: Claude …`, `Co-Authored-By: ChatGPT …`, or any other
  AI co-author trailer.
- "AI-assisted", "written with the help of an LLM", etc., as comments or
  changelog entries.

Write the message as the human author. Do not mention AI tools used to
produce the work.
