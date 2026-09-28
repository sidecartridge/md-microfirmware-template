; ST-side tests of the ST to RP command path, run by tools/dev/st_harness.py.
;
; Assembled in place of userfw.s, in a temporary copy of the tree, and started
; from the setup menu with [F]irmware. Every result goes to the RP as a command
; in the $7Fxx range, which the setup terminal logs on the debug console
; ("Command ID: <decimal>" followed by "Payload D3/D4/D5"). Ends by rebooting
; the ST, which comes back to the setup menu once the sentinel is NOP again.
;
; Runs from cartridge ROM at $FA0800, so everything is PC-relative.
; Set by the harness, in front of this file: STTEST_OVERSIZE 1 also sends an
; oversize frame (T5), STTEST_LONG 1 also runs the long burst (T6).

    include inc/sidecart_layout.s
    include inc/sidecart_macros.s
    include inc/tos.s

_hz_200                 equ $4ba        ; 200 Hz system timer
_sysbase                equ $4f2        ; the OS header
_longframe              equ $59e        ; 0 on a 68000

SMALL_N                 equ 100         ; commands in the small-command burst
BIG_N                   equ 10          ; 1 KB commands in the big-command burst
LONG_N                  equ 3000        ; 1 KB commands in the long burst (T6)

; d3, d4, d5 -> the RP's console, as command \1
REPORT      macro
            send_sync \1, 12
            endm

    section text

sttest:
; T0: what the cartridge handed over: the top of the stack must be the return
; address into TOS (whatever main.s pushed has been taken off), and the machine
    move.l (sp), d3
    move.l sp, d4
    moveq #0, d5
    REPORT $7F10
    bsr find_mch                        ; d3 = _MCH cookie, 0 when there is none
    move.l _sysbase.w, a0
    moveq #0, d4
    move.w 2(a0), d4                    ; TOS version
    moveq #0, d5
    move.w _longframe.w, d5
    REPORT $7F00

; T1: the flags each sender returns with, straight after the call
    move.w #$7F01, d0
    moveq #0, d1
    bsr send_sync_command_to_sidecart
    move.w sr, d6                       ; nothing between the call and this
    move.l d0, d3
    moveq #0, d4
    move.w d6, d4
    moveq #0, d5
    REPORT $7F02

    move.w #$7F03, d0
    moveq #0, d3
    moveq #0, d4
    moveq #0, d5
    moveq #16, d6
    lea sttest(pc), a4
    bsr send_sync_write_command_to_sidecart
    move.w sr, d6
    move.l d0, d3
    moveq #0, d4
    move.w d6, d4
    moveq #0, d5
    REPORT $7F04

; T2: which of d1-d7/a0-a4 each send macro changes (bit 0 = d1 ... bit 6 = d7,
; bit 7 = a0 ... bit 11 = a4)
    movem.l fill_values(pc), d1-d7/a0-a4
    movem.l d1-d7/a0-a4, -(sp)          ; before
    send_sync $7F05, 0
    movem.l d1-d7/a0-a4, -(sp)          ; after
    bsr diff_mask
    lea 96(sp), sp
    move.l d3, -(sp)                    ; send_sync's mask

    movem.l fill_values(pc), d1-d7/a0-a4
    lea sttest(pc), a4                  ; the write sender reads through a4
    movem.l d1-d7/a0-a4, -(sp)
    send_write_sync $7F06, 16
    movem.l d1-d7/a0-a4, -(sp)
    bsr diff_mask
    lea 96(sp), sp
    move.l d3, d4                       ; send_write_sync's mask
    move.l (sp)+, d3
    moveq #0, d5
    REPORT $7F07

; T3: SMALL_N commands with a 4-byte payload, timed in 5 ms ticks
    move.l _hz_200.w, d2
    moveq #0, d4                        ; failures (all retries used up)
    move.w #SMALL_N-1, d5
.small:
    move.l d5, d3
    send_sync $7F08, 4
    tst.w d0
    beq.s .small_ok
    addq.l #1, d4
.small_ok:
    dbf d5, .small
    move.l _hz_200.w, d3
    sub.l d2, d3
    move.l #SMALL_N, d5
    REPORT $7F09

; T4: BIG_N commands with a 1 KB payload, timed in 5 ms ticks
    lea sttest(pc), a4
    move.l _hz_200.w, d2
    moveq #0, d4
    move.w #BIG_N-1, d5
.big:
    move.l d5, d3
    send_write_sync $7F0A, 1024
    tst.w d0
    beq.s .big_ok
    addq.l #1, d4
.big_ok:
    dbf d5, .big
    move.l _hz_200.w, d3
    sub.l d2, d3
    move.l #BIG_N, d5
    REPORT $7F0B

    ifne STTEST_LONG
; T6: a long burst of 1 KB commands (about 7 s), for a forced stall on the RP
; side while it runs; d3 = elapsed 5 ms ticks, d4 = failures, d5 = count
    lea sttest(pc), a4
    move.l _hz_200.w, d2
    moveq #0, d4
    move.w #LONG_N-1, d5
.long:
    move.l d5, d3
    send_write_sync $7F0F, 1024
    tst.w d0
    beq.s .long_ok
    addq.l #1, d4
.long_ok:
    dbf d5, .long
    move.l _hz_200.w, d3
    sub.l d2, d3
    move.l #LONG_N, d5
    REPORT $7F1F
    endif

    ifne STTEST_OVERSIZE
; T5: a frame whose payload size is far past the RP's buffer, then 6000 bytes
; of payload, then an ordinary command: is it still answered?
    move.l #ROMCMD_START_ADDR, a0
    add.l #$8000, a0
    move.w #CMD_MAGIC_NUMBER, d0
    tst.b (a0, d0.w)
    move.w #$7F0C, d0
    tst.b (a0, d0.w)
    move.w #$FFF0, d0
    tst.b (a0, d0.w)
    move.w #3000-1, d1
.flood:
    move.w d1, d0
    tst.b (a0, d0.w)
    dbf d1, .flood
    send_sync $7F0D, 0
    move.l d0, d3                       ; 0: answered
    moveq #0, d4
    moveq #0, d5
    REPORT $7F0E
    endif

; Done. Give the RP time to log, then reboot the ST as main.s does.
    move.l #$600D, d3
    moveq #0, d4
    moveq #0, d5
    REPORT $7F7F
    move.l _hz_200.w, d0
    add.l #200, d0                      ; one second
.linger:
    cmp.l _hz_200.w, d0
    bhi.s .linger
    clr.l $420.w
    clr.l $43A.w
    clr.l $51A.w
    move.l $4.w, a0
    jmp (a0)

; d3 = the _MCH cookie's value, or 0
find_mch:
    moveq #0, d3
    move.l _p_cookies.w, d0
    beq.s .find_mch_done
    move.l d0, a0
.find_mch_next:
    move.l (a0)+, d0
    beq.s .find_mch_done
    cmp.l #'_MCH', d0
    beq.s .find_mch_found
    addq.w #4, a0
    bra.s .find_mch_next
.find_mch_found:
    move.l (a0), d3
.find_mch_done:
    rts

; d3 = bit n set when register n differs between the snapshot at 4(sp)
; (after) and the one at 52(sp) (before). Order: d1-d7, a0-a4.
diff_mask:
    lea 4(sp), a0
    lea 52(sp), a1
    moveq #0, d3
    moveq #0, d1
    moveq #11, d0
.diff_next:
    cmpm.l (a0)+, (a1)+
    beq.s .diff_same
    bset d1, d3
.diff_same:
    addq.w #1, d1
    dbf d0, .diff_next
    rts

fill_values:
    dc.l $11111111, $22222222, $33333333, $44444444, $55555555, $66666666, $77777777
    dc.l $A0A0A0A0, $A1A1A1A1, $A2A2A2A2, $A3A3A3A3, $A4A4A4A4

    include "inc/sidecart_functions.s"

    even
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
sttest_end:
