; SidecarTridge Multi-device: the cartridge window and the command channel.
;
; Included by main.s and by every module that talks to the RP (userfw.s among
; them), before inc/sidecart_macros.s and inc/sidecart_functions.s. The layout
; must match rp/src/include/chandler.h: never hard-code an address in the
; window, use these names.
;
;   $FA0000  CARTRIDGE                  m68k header + code (max 8 KB)
;   $FA2000  CMD_MAGIC_SENTINEL_ADDR    4 B
;   $FA2004  RANDOM_TOKEN_ADDR          4 B
;   $FA2008  RANDOM_TOKEN_SEED_ADDR     4 B
;   $FA200C  reserved                   4 B
;   $FA2010  SHARED_VARIABLES           240 B (60 x 4-byte slots)
;   $FA2100  APP_BUFFERS_ADDR           TRANSTABLE (512 B), then
;   $FA2300  APP_FREE_ADDR              ~48 KB for the app's own buffers
;   $FAE0C0  FRAMEBUFFER_ADDR           8000 B (320x200 mono, at the top)
;   $FAFFFF  end of region

ROM4_ADDR               equ $FA0000
CARTRIDGE_CODE_SIZE     equ $2000       ; 8 KB max for cartridge header + code
SHARED_BLOCK_ADDR       equ (ROM4_ADDR + CARTRIDGE_CODE_SIZE)          ; $FA2000
CMD_MAGIC_SENTINEL_ADDR equ SHARED_BLOCK_ADDR                          ; $FA2000

FRAMEBUFFER_SIZE        equ 8000        ; 8000 bytes of a 320x200 monochrome screen
FRAMEBUFFER_ADDR        equ (ROM4_ADDR + $10000 - FRAMEBUFFER_SIZE)    ; $FAE0C0
APP_BUFFERS_ADDR        equ (SHARED_BLOCK_ADDR + $100)                 ; $FA2100
TRANSTABLE              equ APP_BUFFERS_ADDR                           ; high-res translation table (512 B)
APP_FREE_ADDR           equ (APP_BUFFERS_ADDR + $200)                  ; $FA2300: the app's own buffers,
                                                                       ; up to FRAMEBUFFER_ADDR

; User firmware entry point. The cartridge image places userfw.s at offset
; $0800 of BOOT.BIN (target/atarist/src/userfw.ld): main.s gets the first 2 KB
; ($0000..$07FF), userfw.s the next 6 KB ($0800..$1FFF). CARTRIDGE_CODE_SIZE
; covers both.
USERFW                  equ (ROM4_ADDR + $800)                         ; $FA0800

RANDOM_TOKEN_ADDR       equ (CMD_MAGIC_SENTINEL_ADDR + 4)              ; $FA2004
RANDOM_TOKEN_SEED_ADDR  equ (RANDOM_TOKEN_ADDR + 4)                    ; $FA2008
; $FA200C: 4-byte slot reserved for future framework use. chandler_init
; zeroes it at boot; apps must not write here.
RESERVED_SLOT_ADDR      equ (RANDOM_TOKEN_SEED_ADDR + 4)               ; $FA200C
SHARED_VARIABLES        equ (RESERVED_SLOT_ADDR + 4)                   ; $FA2010 (60 indexed 4-byte slots)
RANDOM_TOKEN_POST_WAIT  equ $1          ; Wait cycles after the RNG is ready

; The command channel: the ST sends by reading ROM3 addresses.
ROMCMD_START_ADDR       equ $FB0000
CMD_MAGIC_NUMBER        equ ($ABCD)     ; Magic number header to identify a command
CMD_SET_SHARED_VAR      equ $FF00       ; Set shared variable d3 to d4. Answered by the RP's
                                        ; chandler itself (CHANDLER_SET_SHARED_VAR)

; Per module: define these before including this file to give a module its
; own. The timeout is a spin count around the token check, so it is shorter
; in time on a faster CPU.
    ifnd COMMAND_TIMEOUT
COMMAND_TIMEOUT         equ $0000FFFF   ; Timeout for the command
    endif
    ifnd COMMAND_WRITE_TIMEOUT
COMMAND_WRITE_TIMEOUT   equ COMMAND_TIMEOUT ; Timeout for write commands
    endif
    ifnd CMD_RETRIES_COUNT
CMD_RETRIES_COUNT       equ 3           ; Retries after the first attempt
    endif

_dskbufp                equ $4c6        ; Address of the disk buffer pointer
