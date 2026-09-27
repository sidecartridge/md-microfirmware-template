; User firmware module
; (C) 2026 by Diego Parrilla
; License: GPL v3
;
; This module is the m68k-side "user firmware" that main.s hands control
; to once the RP signals CMD_START via the cartridge sentinel. The
; cartridge image places this file at offset $0800 (USERFW = $FA0800);
; main.s reaches it through `rom_function: jmp USERFW`. It runs from the
; cartridge, and is linked at offset $0800 rather than at $FA0800, so
; everything in it must be PC-relative (`lea label(pc), a0`).
;
; Replace this body with whatever your app needs to run on the Atari ST
; side. Everything needed to talk to the RP is included below:
;
;   inc/sidecart_layout.s     the cartridge window (RANDOM_TOKEN_ADDR,
;                             SHARED_VARIABLES, APP_BUFFERS_ADDR, ...) and
;                             the command channel
;   inc/sidecart_macros.s     send_sync / send_write_sync, and at its top
;                             what a send keeps and what it destroys
;   inc/sidecart_functions.s  the senders themselves (at the end of this
;                             file, followed by the NOP tail)
;
; A send is `send_sync <command>, <payload bytes>` with the payload in
; d3-d6; the RP answers with d0 = 0 and Z set. On a Mega STE, wrap each
; send in megaste_cache_off / megaste_cache_back.
;
; Demo: this stub prints "Example firmware load..." to the GEMDOS
; console (which lands on the Atari ST screen) and returns. The
; cartridge is reached after GEMDOS init (CA_INIT bit 27 set in
; main.s's header), so the Cconws trap is safe to use here.

	include inc/sidecart_layout.s
	include inc/sidecart_macros.s
	include inc/tos.s

	section text

; GEMDOS Cconws: print null-terminated string to console.
;   trap #1, function 9 (.w on stack), string ptr (.l on stack).
GEMDOS_Cconws		equ 9

userfw:
	lea	hello_msg(pc), a0	; address of message
	move.l	a0, -(sp)		; push string pointer
	move.w	#GEMDOS_Cconws, -(sp)	; push function code
	trap	#1			; call GEMDOS
	addq.l	#6, sp			; clean up arguments
	rts

hello_msg:
	; ESC E = VT52 clear screen + home cursor
	dc.b	27,"E"
	dc.b	"Example firmware load..."
	dc.b	0,255
	even

	include "inc/sidecart_functions.s"

; The NOP tail: this module is the last in the cartridge image, and the
; senders' wait loop must not be the image's last code (see main.s).
	even
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
userfw_end:
		