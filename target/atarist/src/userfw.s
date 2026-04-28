; User firmware module
; (C) 2026 by Diego Parrilla
; License: GPL v3
;
; This module is the m68k-side "user firmware" that main.s hands control
; to once the RP signals CMD_START via the cartridge sentinel. The
; cartridge image places this file at offset $0800 (USERFW = $FA0800);
; main.s reaches it through `rom_function: jmp USERFW`.
;
; Replace this body with whatever your app needs to run on the Atari ST
; side: read shared variables, send commands via send_sync, render
; directly to screen RAM, etc. The shared-region symbols defined in
; main.s (RANDOM_TOKEN_ADDR, SHARED_VARIABLES, APP_FREE_ADDR, ...) are
; available here too via the include.
;
; IMPORTANT: this stub ends with `rts`, which in the current main.s
; flow returns to nowhere well-defined (rom_function uses `jmp`, not
; `jsr`, so the stack does not contain a valid return). Apps that
; replace this body should either loop forever, hand off via `jmp` to
; another routine, or cooperate with main.s by adopting a `jsr` +
; explicit return pattern. The stub is laid out this way so that a
; cartridge image with the unmodified template still assembles and
; links cleanly.

	section text

userfw:
	; ── Replace from here ─────────────────────────────────────────────
	;   Example: bump shared variable index 0 by 1 to prove the module
	;   ran once.
	;
	;   move.l SHARED_VARIABLES, d0
	;   addq.l #1, d0
	;   move.l d0, SHARED_VARIABLES
	; ── ...to here ────────────────────────────────────────────────────

	rts
