; WFTEST: on the Sharp MZ-800 in the MZ-700 mode, do the CPU writes to the VRAM (D000h) go through the write
; format register WF (port CCh) of the GDG, like a write to plane I in the MZ-800 mode?
;
; SM800 asks for WF = 01h in the MZ-700 mode ("Writes WD into the DATA, ATB, and CG area"). A program that leaves
; WF in XOR (Unicard Manager 3.0 before 2026-11-12) gets garbage on a real MZ-800: the new characters are XORed into
; the old picture. mz800emu wrote the byte directly (fixed in the pull request "MZ-700 mode: VRAM writes through
; WF").
;
; For each WF mode (bits 7-5: 000 SINGLE, 001 XOR, 010 OR, 011 RESET, 10x REPLACE, 11x PSET), with plane I selected
; (WF xx1h) and without it (plane II only, WF xx2h), one cell of the screen:
;   1. WF 01h: the cell gets the value PR;
;   2. WF under test: the value NW is written to the cell;
;   3. WF 01h, RF 01h: the cell is read (RD).
; On the screen a line for each test: WF, mode, plane, PR, NW, RD, then WF = the value if the write goes through WF
; as plane I (the model of the fix), DI = the value of a direct write (the emulator before the fix), the result
; (OK = as WF, DIR = as the direct write, ?? = other) and two cells: T is the test cell itself (written through WF),
; E the expected value written with WF 01h. T and E must look the same: that checks the picture too (the video
; output of the MZ-800, the VGA of a card that draws the screen itself).
;
; Runs from any loader (Unicard Manager, the monitor, an emulator): an MZF at 2000h. It ends in a loop: RESET.
;
; Measured 2026-11-24 on a real MZ-800 (run from a Unicard mk3b): all 12 as WF ("RESULT: ALL 12 AS WF"). The VGA
; output of the Unicard (fw 0.38b1) showed the direct write in the column T (its own copy of the VRAM ignores WF in
; the MZ-700 mode). mz800emu 2.0.7: "RESULT: DIRECT WRITE (OLD EMULATOR)"; with the fix all 12 as WF.
; Copyright (C) 2026 Martin Lukasek. GPL-3.0-or-later, see LICENSE.

P_WF        equ 0CCh            ; GDG write format: bits 7-5 mode, bit 4 frame B, bits 3-0 planes IV..I
P_RF        equ 0CDh            ; GDG read format
P_DMD       equ 0CEh            ; GDG display mode: 08h = MZ-700 mode
P_BANK1     equ 0E1h            ; in: CG-ROM and VRAM unmapped (as the Unicard Manager's loader, src/core/run.asm)
P_BANK4     equ 0E4h            ; out: the power-on map (ROM, VRAM at D000h)

VCHR        equ 0D000h          ; characters (display codes), 40 x 25
VATR        equ 0D800h          ; attributes
ATTR        equ 71h             ; white on blue
COLS        equ 40
ROW0        equ 3               ; the first test line
NTEST       equ 12
C_WF        equ 0               ; columns of a test line
C_MODE      equ 3
C_PL        equ 10
C_PR        equ 13
C_NW        equ 16
C_RD        equ 19
C_MW        equ 23
C_DI        equ 26
C_RES       equ 29
C_T         equ 34
C_E         equ 36

        org 2000h

start:
        di
        ld a,08h                ; MZ-700 mode
        out (P_DMD),a
        nop
        ld a,01h                ; WF, RF 01h before any write to the VRAM (SM800)
        out (P_WF),a
        out (P_RF),a
        out (P_BANK4),a         ; ROM, VRAM at D000h
        in a,(P_BANK1)
        ld sp,2000h
        ld hl,VCHR              ; characters 0, attributes white on blue
        ld de,VCHR+1
        ld bc,0800h
        ld (hl),0
        ldir
        ld bc,07FFh
        ld (hl),ATTR
        ldir

        ld hl,texts             ; the fixed texts: row, column, ASCII, 00h; row FFh ends
tx_next:
        ld a,(hl)
        inc hl
        cp 0FFh
        jr z,tx_done
        ld b,a
        ld c,(hl)
        inc hl
        call cell_addr
        call prstr
        inc hl                  ; past the 00h
        jr tx_next
tx_done:

; ---- the tests ----
        ld ix,tests
        ld b,0                  ; the test number (= its line - ROW0)
        ld c,0
        ld hl,0
        ld (n_ok),hl            ; n_ok and n_dir
t_loop:
        push bc
        ld a,b
        add a,ROW0
        ld b,a
        ld c,C_T
        call cell_addr          ; DE = the test cell
        ld a,01h                ; 1: the preset through WF 01h
        out (P_WF),a
        ld a,(ix+1)
        ld (de),a
        ld a,(ix+0)             ; 2: the write through the WF under test
        out (P_WF),a
        ld a,(ix+2)
        ld (de),a
        ld a,01h                ; 3: WF and RF 01h, read
        out (P_WF),a
        out (P_RF),a
        ld a,(de)
        ld (rd),a
        inc de                  ; E: the expected value, two cells to the right
        inc de
        ld a,(ix+3)
        ld (de),a
        pop bc
        push bc
        ld a,b
        add a,ROW0
        ld b,a
        ld c,C_WF               ; the line: WF
        call cell_addr
        ld a,(ix+0)
        call prhex
        ld c,C_MODE             ; the mode by WF bits 7-5
        call cell_addr
        ld a,(ix+0)
        rlca
        rlca
        rlca
        and 07h
        add a,a
        ld hl,modes
        add a,l
        ld l,a
        jr nc,t_m1
        inc h
t_m1:   ld a,(hl)
        inc hl
        ld h,(hl)
        ld l,a
        call prstr
        ld c,C_PL               ; plane I in WF, or not
        call cell_addr
        ld hl,s_pl1
        bit 0,(ix+0)
        jr nz,t_pl
        ld hl,s_pl2
t_pl:   call prstr
        ld c,C_PR
        call cell_addr
        ld a,(ix+1)
        call prhex
        ld c,C_NW
        call cell_addr
        ld a,(ix+2)
        call prhex
        ld c,C_RD
        call cell_addr
        ld a,(rd)
        call prhex
        ld c,C_MW
        call cell_addr
        ld a,(ix+3)
        call prhex
        ld c,C_DI
        call cell_addr
        ld a,(ix+2)
        call prhex
        ld c,C_RES              ; OK = as WF, DIR = as the direct write, ?? = other
        call cell_addr
        ld a,(rd)
        cp (ix+3)
        jr nz,t_notok
        ld hl,n_ok
        inc (hl)
        ld hl,s_ok
        jr t_res
t_notok:
        cp (ix+2)
        ld hl,s_other
        jr nz,t_res
        ld hl,n_dir
        inc (hl)
        ld hl,s_dir
t_res:  call prstr
        pop bc
        ld de,4
        add ix,de
        inc b
        ld a,b
        cp NTEST
        jp c,t_loop

; ---- the summary: all as WF, all distinguishing tests as the direct write, or the count ----
        ld b,ROW0+NTEST+1
        ld c,0
        call cell_addr
        ld a,(n_ok)
        cp NTEST
        ld hl,s_all_wf
        jr z,sm_print
        ld a,(n_dir)
        cp N_DIST
        ld hl,s_all_dir
        jr z,sm_print
        ld hl,s_mixed
        call prstr
        ld a,(n_ok)
        call prdec
        ld hl,s_mixed2
sm_print:
        call prstr
        ld a,01h
        out (P_WF),a
        out (P_RF),a
done:   jr done

; ---- cell_addr: DE = VCHR + B * 40 + C ----
cell_addr:
        push hl
        ld l,b
        ld h,0
        add hl,hl               ; x 8
        add hl,hl
        add hl,hl
        push hl
        add hl,hl               ; x 32
        add hl,hl
        pop de
        add hl,de               ; x 40
        ld e,c
        ld d,0
        add hl,de
        ld de,VCHR
        add hl,de
        ex de,hl
        pop hl
        ret

; ---- prstr: the ASCII text at HL (20h-5Fh, 00h ends) as display codes at DE ----
prstr:
        ld a,(hl)
        or a
        ret z
        push hl
        sub 20h
        ld hl,asc2dc
        add a,l
        ld l,a
        jr nc,ps_1
        inc h
ps_1:   ld a,(hl)
        pop hl
        ld (de),a
        inc hl
        inc de
        jr prstr

; ---- prhex: A as two hex digits at DE ----
prhex:
        push af
        rrca
        rrca
        rrca
        rrca
        call ph_digit
        pop af
ph_digit:
        and 0Fh
        cp 10
        jr c,ph_num
        sub 9                   ; A-F: display codes 01h-06h
        jr ph_put
ph_num: add a,20h               ; 0-9: display codes 20h-29h
ph_put: ld (de),a
        inc de
        ret

; ---- prdec: A (0-99) in decimal at DE ----
prdec:
        ld b,0
pd_10:  cp 10
        jr c,pd_1
        sub 10
        inc b
        jr pd_10
pd_1:   ld c,a
        ld a,b
        or a
        jr z,pd_u
        add a,20h
        ld (de),a
        inc de
pd_u:   ld a,c
        add a,20h
        ld (de),a
        inc de
        ret

; ---- the tests: WF, preset PR, written NW, expected if the write goes through WF as plane I ----
; SINGLE, XOR, OR, RESET write only with plane I selected; REPLACE writes 0 and PSET clears the bits of NW when
; plane I is not selected (as the planes not selected in the MZ-800 mode). The expected values show 'O' (0Fh),
; REPLACE without plane I a space.
tests:
        defb 01h, 04h, 0Fh, 0Fh         ; SINGLE  I   (as the direct write: a check)
        defb 21h, 0Bh, 04h, 0Fh         ; XOR     I   0Bh xor 04h
        defb 41h, 0Bh, 04h, 0Fh         ; OR      I   0Bh or 04h
        defb 61h, 1Fh, 10h, 0Fh         ; RESET   I   1Fh and not 10h
        defb 81h, 04h, 0Fh, 0Fh         ; REPLACE I   (as the direct write: a check)
        defb 0C1h, 0Bh, 04h, 0Fh        ; PSET    I   0Bh or 04h
        defb 02h, 0Fh, 04h, 0Fh         ; SINGLE  II  not written
        defb 22h, 0Fh, 04h, 0Fh         ; XOR     II  not written
        defb 42h, 0Fh, 04h, 0Fh         ; OR      II  not written
        defb 62h, 0Fh, 04h, 0Fh         ; RESET   II  not written
        defb 82h, 0Fh, 04h, 00h         ; REPLACE II  0
        defb 0C2h, 1Fh, 10h, 0Fh        ; PSET    II  1Fh and not 10h
N_DIST      equ 10                      ; the tests where WF and the direct write differ

modes:  defw s_m0, s_m1, s_m2, s_m3, s_m4, s_m4, s_m5, s_m5     ; WF bits 7-5: 10x REPLACE, 11x PSET
s_m0:   defb 'SINGLE', 0
s_m1:   defb 'XOR', 0
s_m2:   defb 'OR', 0
s_m3:   defb 'RESET', 0
s_m4:   defb 'REPLAC', 0
s_m5:   defb 'PSET', 0
s_pl1:  defb 'I', 0
s_pl2:  defb 'II', 0
s_ok:   defb 'OK', 0
s_dir:  defb 'DIR', 0
s_other: defb '??', 0
s_all_wf: defb 'RESULT: ALL 12 AS WF (A REAL MZ-800)', 0
s_all_dir: defb 'RESULT: DIRECT WRITE (OLD EMULATOR)', 0
s_mixed: defb 'RESULT: ', 0
s_mixed2: defb ' OF 12 AS WF, SEE THE TABLE', 0

texts:
        defb 0, 0, 'WFTEST: MZ-700 MODE VRAM WRITES AND WF', 0
        defb 2, 0, 'WF MODE   PL PR NW RD  WF DI RES  T E', 0
        defb 18, 0, 'PR PRESET, NW WRITTEN WITH WF, RD READ', 0
        defb 19, 0, 'WF IF WF APPLIES, DI IF WRITTEN DIRECTLY', 0
        defb 20, 0, 'RES OK = AS WF, DIR = AS DIRECT, ?? NONE', 0
        defb 21, 0, 'T WRITTEN WITH WF, E EXPECTED (WF 01H):', 0
        defb 22, 0, 'T AND E MUST LOOK THE SAME ON THE SCREEN', 0
        defb 24, 0, 'WFTEST 1.0 (UC-UNIMGR)      RESET: EXIT', 0
        defb 0FFh

; ASCII 20h-5Fh -> MZ display code (as src/core/run.asm)
asc2dc:
        defb 00h, 61h, 62h, 63h, 64h, 65h, 66h, 67h, 68h, 69h, 6Bh, 6Ah, 2Fh, 2Ah, 2Eh, 2Dh    ; 20h  !"#$%&'()*+,-./
        defb 20h, 21h, 22h, 23h, 24h, 25h, 26h, 27h, 28h, 29h, 4Fh, 2Ch, 51h, 2Bh, 57h, 49h    ; 30h 0-9 :;<=>?
        defb 55h, 01h, 02h, 03h, 04h, 05h, 06h, 07h, 08h, 09h, 0Ah, 0Bh, 0Ch, 0Dh, 0Eh, 0Fh    ; 40h @A-O
        defb 10h, 11h, 12h, 13h, 14h, 15h, 16h, 17h, 18h, 19h, 1Ah, 52h, 59h, 54h, 0BEh, 0BFh  ; 50h P-Z [\]^_

rd:     defb 0
n_ok:   defb 0
n_dir:  defb 0
