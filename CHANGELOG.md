# Changelog

## v1.1.0 (2026-04-27) - release

Architectural port of the framework improvements introduced in
md-drives-emulator. Apps derived from previous versions of this template
will need to migrate (see "Breaking changes" below).

### Memory layout
- RAM grown from 128 KB to 192 KB.
- ROM_IN_RAM reduced from 128 KB @ `0x20020000` to 64 KB @ `0x20030000`.
- Result: 64 KB of additional general-purpose RAM available to apps.

### ROM4 read engine (cartridge data path)
- Single-bank 64 KB ROM (ROM_BANKS 2 → 1). ROM3 is no longer a data
  bank; it is now used exclusively as the command channel.
- PIO program rewritten: waits directly on `ROM4_GPIO`, captures 16
  bits of address, two-channel chained DMA serves reads with no CPU
  or IRQ involvement.
- `ROMEMUL_BUS_BITS` 17 → 16; `FLASH_ROM3_LOAD_OFFSET` removed.

### ROM3 command channel — new (`commemul`)
- Dedicated PIO state machine on `ROM3_GPIO` captures every ROM3
  access into a 32 KB ring buffer via DMA in ring mode (no IRQ).
- `commemul_poll(callback)` drains the ring lock-free using
  `dma_hw->ch[ch].transfer_count` to derive the producer index.

### Command dispatcher — new (`chandler`)
- Polled command parser/dispatcher. `chandler_loop()` calls
  `commemul_poll`, parses via `tprotocol_parse`, and dispatches each
  command to a registered callback list.
- Apps register handlers with `chandler_addCB(callback)`.
- Replaces the previous `DMA_IRQ_1`-driven snoop in `term.c`.

### Terminal
- Removed `term_dma_irq_handler_lookup`. Terminal commands are now
  delivered through `term_command_cb`, which is registered with
  `chandler_addCB` during emulation startup.

### Orchestration (emul.c)
- Boot now calls
  `init_romemul(false); commemul_init(); chandler_init();
  chandler_addCB(term_command_cb);`.
- Main loop drains commands via `chandler_loop()` before `term_loop()`.
- WiFi polling callback drains both, so commands sent during the
  multi-second WiFi connect window are not dropped.

### m68k framework fixes
- `inc/tos.s`: fix `endmv` → `endm` typo in the `pchar2` macro.
- `inc/sidecart_functions.s`: rename transient `d0` → `d4` in the
  `_write_to_sidecart_*` loops. `d0` is the sync command reply
  register; reusing it mid-write clobbered the reply slot for any
  app that issued payload writes.
- New `COMMAND_WRITE_TIMEOUT` (defaults to `COMMAND_TIMEOUT`) used by
  `_start_sync_write_code_in_stack`, so apps can extend the write
  timeout for large payloads without affecting read-command timing.

### Breaking changes for downstream apps
- `init_romemul(IRQInterceptionCallback, IRQInterceptionCallback,
  bool)` → `init_romemul(bool)`.
- Removed: `dma_setResponseCB`, `romemul_getLookupDataRomDmaChannel`,
  `dma_irqHandlerLookup`, `dma_irqHandlerAddress`, the
  `IRQInterceptionCallback` typedef.
- `tprotocol`: `TransmissionProtocol.payload` is now `uint16_t[/2]`
  (was `unsigned char[]`). Static parser state is now extern; the
  template provides `tprotocol.c` defining the externs.
- `term.h`: `ADDRESS_HIGH_BIT` and the `ROM3_GPIO` define removed.

### Build
- Added `chandler.c`, `commemul.c` to `rp/src/CMakeLists.txt` sources.
- Added `pico_generate_pio_header` for `commemul.pio`.

---

## v0.0.3 (2025-07-01) - release
- First version

---
