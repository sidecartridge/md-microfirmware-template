# SidecarTridge Multi-device Microfirmware App template

This repository is the starting point for building SidecarTridge Multi-device
"microfirmware" apps (self-contained UF2 payloads the Multidevice can load on
boot).

> ⚠️ **The authoritative, always-up-to-date guide now lives in the official
> [SidecarTridge Multi-device documentation](https://docs.sidecartridge.com/sidecartridge-multidevice/programming/).**
> This repo only contains the build tooling and a minimalist reference app.

## Quick start

```bash
# Clone your fork and fetch the SDK submodules
./build.sh                   # shows usage / available defaults
./build.sh pico_w release <your-app-uuid>
```

Arguments:
- `board_type` – defaults to `pico_w`. Supported values: `pico`, `pico_w`.
- `build_type` – defaults to `release`. Supported values: `release`, `debug`.
- `app_uuid_key` – required. Must match the UUID in `desc/app.json`.

The script copies `version.txt` into each component, builds the Atari ST payload
+ RP firmware, produces the UF2, computes its MD5 hash, and writes the matching
`<UUID>.json` descriptor in `dist/`.

## Headless builds

`stcmd` (the Sidecar command-line helper) now supports the environment variable
`STCMD_NO_TTY=1` to disable the `-it` Docker flags. The template uses this flag
internally so you can trigger builds from automations or remote agents without a
TTY. Make sure your local `stcmd` installation is up to date (see
[SidecarTridge Atari ST toolkit](https://github.com/sidecartridge/atarist-toolkit-docker)).

## License

The source code of the project is licensed under the GNU General Public License
v3.0. The full license is accessible in the [LICENSE](LICENSE) file.
