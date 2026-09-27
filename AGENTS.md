# AGENTS.md — Microfirmware Template Playbook

**Read `CLAUDE.md` first.** It is the source of truth for what this repo is, the
build flow and its gotchas, the architecture (shared 64 KB region, command path,
flash layout), and the editing guardrails. This file does not repeat any of it.

What lives here instead: getting a host machine set up, the commands worth
copy-pasting, and a symptom → fix table for the failures this project actually
produces.

## 1. Host environment setup

- **ARM GNU Toolchain 14.2** — export `PICO_TOOLCHAIN_PATH` to its
  `arm-none-eabi/bin` directory. On this machine that is
  `/Applications/ArmGNUToolchain/14.2.rel1/arm-none-eabi/bin`.
- **`atarist-toolkit-docker`** (provides `stcmd`) — required for the m68k
  target. It shells out to `docker run -it`, so it needs a TTY unless
  `STCMD_NO_TTY=1` is set (the build scripts set it for you).
- **Raspberry Pi Debug Probe / Picoprobe** — wired to the Multi-device header.
  TX, RX **and both GND pins** must be connected.
- Git, GNU Make, and VS Code with the C/C++ Extension Pack, CMake Tools and
  Cortex-Debug.

SDK paths — the build scripts set these from the repo if unset, so you only need
them for editor/IntelliSense integration:

```bash
export PICO_SDK_PATH=$REPO_ROOT/pico-sdk
export PICO_EXTRAS_PATH=$REPO_ROOT/pico-extras
export FATFS_SDK_PATH=$REPO_ROOT/fatfs-sdk
```

Debugger-only variables, consumed by `.vscode/launch.json` and
`.vscode/settings.json`:

```bash
export ARM_GDB_PATH=/path/to/arm-none-eabi        # launch.json appends /bin/arm-none-eabi-gdb
export PICO_OPENOCD_PATH=/path/to/openocd/tcl
```

## 2. Common commands

```bash
# Full build: <board_type> <build_type> <app_uuid_key>
PICO_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/14.2.rel1/arm-none-eabi/bin \
  ./build.sh pico_w release 44444444-4444-4444-8444-444444444444

# Same, but with DPRINTF + UART serial console enabled (see CLAUDE.md > Tests)
PICO_TOOLCHAIN_PATH=... ./build.sh pico_w debug 44444444-4444-4444-8444-444444444444

# m68k target only — faster loop when you are just editing userfw.s / main.s
ST_WORKING_FOLDER=$(pwd)/target/atarist STCMD_NO_TTY=1 stcmd make release

# Inspect the built RP firmware
$PICO_TOOLCHAIN_PATH/arm-none-eabi-nm rp/build/rp.elf
$PICO_TOOLCHAIN_PATH/arm-none-eabi-objdump -h rp/build/rp.elf
```

A successful build leaves `dist/<UUID>-<version>.uf2` and
`dist/<UUID>.json` and prints the MD5 embedded in the manifest.

## 3. Troubleshooting

| Symptom | Fix |
| --- | --- |
| `the input device is not a TTY` from `stcmd` | Export `STCMD_NO_TTY=1`. `target/atarist/build.sh` already does this for every call it makes; you only need it when invoking `stcmd` yourself from a non-TTY context (CI, sub-shells, wrappers). |
| `arm-none-eabi-gcc not found` | `PICO_TOOLCHAIN_PATH` is unset or not pointing at the toolchain's `bin` dir. |
| Build stops on missing `CHARACTER_GAP_MS` | Re-add `#define CHARACTER_GAP_MS 700` to `rp/src/include/blink.h`. |
| `ERROR: cartridge code is N bytes; limit is 8192` | The m68k image outgrew its 8 KB budget. Trim `main.s`/`userfw.s` or move data out of the cartridge image into `APP_FREE` / the shared variables. |
| Final steps fail copying the UF2 | An upstream compile failed — scroll back to the first error, not the copy step. |
| The ST shows garbage but terminal commands still work | `target_firmware.h` is stale: `stcmd make` failed silently and the previous `BOOT.BIN` survived, so the m68k is running against the wrong shared-region addresses. Compare the timestamp of `target/atarist/dist/BOOT.BIN` against the rest of `dist/`. |
| Commands from the ST are dropped or arrive late | Some loop is blocking without draining the ROM3 ring. Every blocking wait needs `chandler_loop()` — see CLAUDE.md > Command path. |
| A `release` build prints nothing on serial | Expected: `DPRINTF` and UART stdio are compiled out unless the build type is `debug`. |
| Local changes inside `pico-sdk/`, `pico-extras/` or `fatfs-sdk/` vanished | `rp/build.sh` re-checks-out the pinned revisions on every build. Never edit the submodules; FatFs config belongs in `rp/src/ff/ffconf.h`. |

## 4. Hard rules

Full guardrails and working style are in `CLAUDE.md`. The two that are
non-negotiable and cheap to violate by accident:

- **Never modify `pico-sdk/`, `pico-extras/` or `fatfs-sdk/`.** They are pinned
  submodules and the build overwrites any local change.
- **Never add AI-tool attribution** to commits, PR descriptions, code comments,
  docs, or any other artifact. No `Co-Authored-By: Claude …`, no "Generated
  with Claude Code / ChatGPT", no "AI-assisted" notes. Write everything as the
  human author.

Keep this file updated as the process evolves so every agent starts with the
latest tribal knowledge.
