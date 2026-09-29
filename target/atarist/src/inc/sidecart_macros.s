; SidecarTridge Multi-device macros for the shared SidecarTridge library

; Macros

; What a send keeps and what it destroys. Check it before holding anything
; across one of these: the senders leave a0-a1 (a0-a3 when they copy their wait
; loop, COMMAND_SYNC_USE_DSKBUF not 0) pointing into the ROM3 command window,
; and reading a payload from there makes the RP reject the command.
;
;   send_sync        keeps d1-d6          destroys d0, d7, a0-a3
;   send_write_sync  keeps d1-d5, a4      destroys d0, d6, d7, a0-a3
;
; Each keeps its retry count in a register it destroys: send_sync in d7,
; send_write_sync in d6 (the function it calls also uses d7).
;
; Take what you need into a kept register before the send, and reload address
; registers after it.

; A Mega STE's cache answers the cartridge's reads from copies it made before,
; and the command channel breaks with it on; the CPU speed does not matter (all
; measured at 16 MHz). So a driver turns the cache off, which empties it, once
; it knows a call is its own, and puts the user's setting back when it is done.
; What it reads to decide - its settings, the reentry lock - does not change
; while the cache could hold a copy. The machine is the one detect_hw wrote in
; the first shared variable; on every other machine the register is not there.
; \1: where the setting is kept, a byte
megaste_cache_off   macro
                    cmp.l #COOKIE_JAR_MEGASTE, HARDWARE_TYPE_ADDR
                    bne.s .\@megaste_cache_off_done
                    move.b MEGASTE_SPEED_CACHE_REG.w, \1
                    bclr #0, MEGASTE_SPEED_CACHE_REG.w
.\@megaste_cache_off_done:
                    endm

; \1: where megaste_cache_off kept the setting
megaste_cache_back  macro
                    cmp.l #COOKIE_JAR_MEGASTE, HARDWARE_TYPE_ADDR
                    bne.s .\@megaste_cache_back_done
                    move.b \1, MEGASTE_SPEED_CACHE_REG.w
.\@megaste_cache_back_done:
                    endm

; Send a synchronous command to the Multi-device passing arguments in the Dx registers
; /1 : The command code
; /2 : The payload size (even number always)
send_sync           macro
                    move.w #CMD_RETRIES_COUNT, d7        ; Set the number of retries
.\@send_sync_retry:
                    movem.l d1-d7, -(sp)                 ; Save the registers
                    moveq.l #\2, d1                      ; Set the payload size of the command
                    move.w #\1,d0                        ; Command code
                    bsr send_sync_command_to_sidecart    ; Send the command to the Multi-device
                    movem.l (sp)+, d1-d7                 ; Restore the registers
                    tst.w d0                             ; Check the result of the command
                    beq.s .\@send_sync_ok                  ; If the command was ok, exit
                    dbf d7, .\@send_sync_retry           ; If the command failed, retry
.\@send_sync_ok:
                    endm    

; Send a synchronous write command to the Multi-device passing arguments in the D3-D5 registers
; A4 address of the buffer to send
; /1 : The command code
; /2 : The buffer size to send in bytes (will be rounded to the next word)
send_write_sync     macro
                    move.w #CMD_RETRIES_COUNT, d6        ; Set the number of retries
.\@send_write_sync_retry:
                    movem.l d1-d6/a4, -(sp)                 ; Save the registers
                    move.w #\1,d0                           ; Command code
                    move.l #\2,d6                           ; Number of bytes to send
                    bsr send_sync_write_command_to_sidecart ; Send the command to the Multi-device
                    movem.l (sp)+, d1-d6/a4                 ; Restore the registers
                    tst.w d0                                ; Check the result of the command
                    beq.s .\@send_write_sync_ok             ; If the command was ok, exit
                    dbf d6, .\@send_write_sync_retry        ; If the command failed, retry
.\@send_write_sync_ok:
                    endm    

; Wait for second (aprox 50 VBlanks)
wait_sec                macro
                        move.l d7, -(sp)                    ; Save the number counter reg
                        move.w #50, d7                      ; Loop to wait a second (aprox 50 VBlanks)
.\@wait_sec_loop:
                        move.w 	#37,-(sp)                   ; Wait for the VBlank. Add a delay
                        trap 	#14
                        addq.l 	#2,sp
                        dbf d7, .\@wait_sec_loop
                        move.l (sp)+, d7                    ; Restore the number counter reg
                        endm

; Wait for second (aprox 50 VBlanks) and cancel if a key is pressed
wait_sec_or_key_press   macro
                        move.l d7, -(sp)                    ; Save the number counter reg
                        move.w #50, d7                      ; Loop to wait a second (aprox 50 VBlanks)
.\@wait_sec_key_loop:
                        move.w  #$ff,-(sp)
                        move.w  #Crawio,-(sp)               ; Check if a key is pressed
                        trap    #1
                        addq.l  #4,sp
                        tst.l   d0                          ; Returns 0 in the register if no key is pressed
                        bne.s   .\@wait_sec_key_loop_exit   ; If no key is pressed, continue waiting
                        move.w 	#37,-(sp)                   ; Wait for the VBlank. Add a delay
                        trap 	#14
                        addq.l 	#2,sp
                        dbf d7, .\@wait_sec_key_loop
                        clr.l   d0                          ; Clear the register because no key was pressed
.\@wait_sec_key_loop_exit:
                        move.l (sp)+, d7                    ; Restore the number counter reg
                        endm

