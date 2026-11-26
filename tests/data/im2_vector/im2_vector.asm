; im2_vector.asm - ověřovací program pro MZ-800: co CPU v IM 2 přečte jako vektor
;
; Otázky (báze mz800-knowledge, mz800emu/02-rozdily-proti-hw.md, I1 a I2):
;   I1  maskuje Sharp LH0080A bit 0 vektoru v IM 2? (Zilog: bit 0 = 0; Sean Young na Z80
;       naměřil, že bit 0 projde; mz800new do mutantu im2-vector maskoval)
;   I2  co CPU přečte při potvrzení přerušení z CTC2, když Z80 PIO vektor nedodá?
;       Sdělení Michala: duch sběrnice, poslední bajt. Emulátor bere poslední bajt přečtený
;       z paměti (regDBUS_latch); báze má [neověřeno] pro instrukce končící zápisem nebo IN.
;
; Překlad: python gen_tables.py im2_tables.inc
;          pasmo --bin im2_vector.asm im2_vector.bin
;          python make_mzf.py (emu-experiments gdg-meas) im2_vector.bin im2_vector.mzf "MZ800 IM2 VECTOR" 2000
;
; Princip rozpoznání vektoru: I = 50h, tabulka 5000h až 5100h (257 bajtů) je de Bruijnova
; posloupnost nad bajty 60h až 6Fh (gen_tables.py). Každý vektor v tak vede na jinou adresu
; T(v) = b[v] + 256 * b[v + 1] v oblasti 6060h až 6F6Fh. Na stránce 60h + h je od 6060h
; skluzavka 16 x INC C a na 70h PUSH AF, LD A,h, JP isrcom. Obsluha z C (vstup s C = 0)
; a h složí klíč a inverzní tabulkou vrátí v. Funguje pro všech 256 vektorů a nepotřebuje
; vědět, zda CPU maskuje bit 0: s maskou dojde vektor v AND 0FEh.
;
; Testy (klávesa 1 spustí všechny, každý sbírá vzorky do histogramu):
;   0  PIO VEC  kontrola metody: Z80 PIO, brána A, Mode 3, přerušení na PA5 (VBLN),
;               vektor 5Ah; CTC2 maskované (PC2 = 0), 64 vzorků
;   1  JP 4D33  smyčka JP 4D33h na 4D33h, poslední čtený bajt 4Dh (lichý)
;   2  LDA 9B   blok LD A,(3001h), na 3001h je 9Bh; poslední čtený bajt 9Bh (lichý, data)
;   3  HALT     HALT na 4E40h, za ním SCF (37h); emulátor během HALT čte 76h
;   4  LDHL 2C  blok LD (HL),A s A = 2Ch: poslední čtený bajt 77h, poslední zápis 2Ch
;   5  PUSH 4B  blok PUSH DE s D = 59h, E = 4Bh: poslední čtený bajt D5h, poslední zápis 4Bh
;   6  OUT A1   blok OUT (0A1h),A s A = 2Ch: poslední čtený bajt A1h, zápis I/O 2Ch
;   7  IN A0    blok IN A,(0A0h), volný port: poslední čtený bajt A0h, I/O čtení nebuzené
;   8  IN PIOB  blok IN A,(0FFh), Z80 PIO brána B v Mode 0 s výstupem A9h: poslední čtený
;               bajt FFh; zda PIO při čtení v Mode 0 budí sběrnici, je [neověřeno], ukáže A
; Testy 1 až 8: CTC1 Mode 2 dělič 2 z HSYNC, CTC2 Mode 0 N = 2, obsluha znovu nahraje CTC2;
; 1024 vzorků. Sloupec EM = co ukáže emulátor mz800new s mutantem im2-vector (bez masky).
; Řádky "A" u testů 7 a 8 = hodnota registru A v okamžiku přerušení (co přečetlo IN).
;
; Na obrazovce: číslo, název, EM, pak tři nejčastější vektory "VV CCCC" (CCCC = počet hex).
;
; Paměť (režim MZ-700 na MZ-800, OUT 0E4h: RAM 1000h až 0CFFFh):
;   2000h program, 3000h a 3001h data testů 2 a 4, 4800h až 4BFFh zásobník testu 5,
;   4D33h smyčka JP, 4E40h HALT, 4FF0h zásobník programu, 5000h tabulka IM 2, 5200h inverzní
;   tabulka, 5300h značky výpisu, 6060h až 6F75h skluzavky, 7000h až 7BFFh blok testu,
;   8000h + k * 400h výsledky testu k (vektory 256 slov, od +200h hodnoty A 256 slov).
; Porty 0A0h a 0A1h musí být volné (0A0h použil i mz800_romwait jako značku) [neověřeno
; s kartami v rozšiřujícím slotu].

        org 2000h

KEYPA   equ 0E000h              ; 8255 PA: výběr sloupce klávesnice
KEYPB   equ 0E001h              ; 8255 PB: čtení sloupce (stisk = 0)
PPICTL  equ 0E003h              ; 8255 řídicí registr (BSR: 05h = PC2 1, 04h = PC2 0)
CTC1D   equ 0E005h              ; 8253 čítač 1
CTC2D   equ 0E006h              ; 8253 čítač 2
CTCCTL  equ 0E007h              ; 8253 řídicí slovo
PIOCA   equ 0FCh                ; Z80 PIO řízení brány A
PIOCB   equ 0FDh                ; Z80 PIO řízení brány B
PIODB   equ 0FFh                ; Z80 PIO data brány B

IM2T    equ 5000h               ; tabulka IM 2 (I = 50h)
INVT    equ 5200h               ; inverzní tabulka (zarovnaná na stránku)
MARK    equ 5300h               ; značky vybraných vektorů při výpisu (256 bajtů)
SLEDS   equ 6060h               ; první skluzavka
BLK     equ 7000h               ; blok měřené instrukce
RESULT     equ 8000h               ; výsledky
STACK   equ 4FF0h
PUSHSP  equ 4C00h               ; vrchol zásobníku testu 5
JPLOOP  equ 4D33h
HALTAT  equ 4E40h
TDATA    equ 3000h

NTEST   equ 9
NSAMP   equ 1024                ; vzorků v testech CTC2
NPIO    equ 64                  ; vzorků v testu PIO
N1      equ 2                   ; dělič CTC1 (Mode 2)
N2      equ 2                   ; počet CTC2 (Mode 0)
PIOVEC  equ 5Ah                 ; vektor PIO v testu 0

STLINE  equ 0D000h + 16 * 40    ; stavový řádek

start:
        di
        ld      sp, STACK
        call    vinit
        call    setup
        call    showmenu
main:
        call    waitrel
        call    waitkey         ; A = 0 až 7 (klávesy 1 až 8)
        or      a
        jr      nz, main
        call    clrst
        call    runall
        ld      hl, STLINE
        ld      de, s_done
        call    puts
        jr      main

; ---------------------------------------------------------------------------
; setup: zákaz přerušení PIO, tabulky IM 2, skluzavky, I = 50h, IM 2.
;   Mění: AF, BC, DE, HL, I.
; ---------------------------------------------------------------------------
setup:
        ld      a, 03h          ; Interrupt Disable Word; dvakrát, kdyby automat PIO
        out     (PIOCA), a      ; čekal I/O select nebo masku
        out     (PIOCA), a
        out     (PIOCB), a
        out     (PIOCB), a
        ld      hl, im2tab
        ld      de, IM2T
        ld      bc, 257
        ldir
        ld      hl, invtab
        ld      de, INVT
        ld      bc, 256
        ldir
        ld      b, 0            ; h = index stránky
        ld      hl, SLEDS
sl1:    push    hl
        ld      c, 16
sl2:    ld      (hl), 0Ch       ; INC C
        inc     hl
        dec     c
        jr      nz, sl2
        ld      (hl), 0F5h      ; PUSH AF
        inc     hl
        ld      (hl), 3Eh       ; LD A,h
        inc     hl
        ld      (hl), b
        inc     hl
        ld      (hl), 0C3h      ; JP isrcom
        inc     hl
        ld      (hl), low isrcom
        inc     hl
        ld      (hl), high isrcom
        pop     hl
        inc     h
        inc     b
        ld      a, b
        cp      16
        jr      nz, sl1
        ld      a, high IM2T
        ld      i, a
        im      2
        ret

; ---------------------------------------------------------------------------
; isrcom: obsluha přerušení (ze skluzavky). Vstup: na zásobníku AF přerušeného kódu,
;   A = h (stránka - 60h), C = 16 - j (j = dolní bajt cíle - 60h; přerušený kód má C = 0).
;   Započte vektor do histogramu (curhist) a A přerušeného kódu do histogramu +200h.
;   Po target vzorcích zamaskuje CTC2 (PC2 = 0) a vrátí se RETI za CALL testu v runall
;   (zásobník drvsp). Jinak znovu nahraje CTC2 (reload <> 0) a vrátí se s C = 0.
;   Zachovává: AF, DE, HL přerušeného kódu, B, IX, IY. Končí RETI (kvůli PIO v testu 0).
; ---------------------------------------------------------------------------
isrcom:
        push    hl
        push    de
        rlca
        rlca
        rlca
        rlca                    ; A = h * 16
        ld      e, a
        ld      a, 16
        sub     c
        and     0Fh
        or      e               ; klíč
        ld      l, a
        ld      h, high INVT
        ld      a, (hl)         ; vektor
        ld      hl, (curhist)
        call    hinc
        ld      hl, 5
        add     hl, sp
        ld      a, (hl)         ; A přerušeného kódu (PUSH AF: A na sp + 5)
        ld      hl, (curhist)
        inc     h
        inc     h
        call    hinc
        ld      hl, (count)
        inc     hl
        ld      (count), hl
        ld      de, (target)
        or      a
        sbc     hl, de
        jr      z, hdone
        ld      a, (reload)
        or      a
        jr      z, h1
        ld      (CTC2D), a      ; CTC2 znovu: OUT2 = 0, další přerušení za N2 + 2 pulzů OUT1
h1:     ld      c, 0
        pop     de
        pop     hl
        pop     af
        ei
        reti
hdone:  ld      a, 04h
        ld      (PPICTL), a     ; PC2 = 0: CTC2 maskované
        ld      sp, (drvsp)
        reti                    ; IFF2 = 0, přerušení zůstávají zakázaná

; hinc: zvýší 16bitový čítač HL + 2 * A. Mění: AF, DE, HL.
hinc:   ld      e, a
        ld      d, 0
        ex      de, hl
        add     hl, hl
        add     hl, de
        inc     (hl)
        ret     nz
        inc     hl
        inc     (hl)
        ret

; ---------------------------------------------------------------------------
; runall: všechny testy po řadě, po každém řádek výsledku.
; ---------------------------------------------------------------------------
runall:
        ld      ix, tests
        xor     a
        ld      (tno), a
ra1:    ld      a, (tno)
        add     a, a
        add     a, a
        add     a, high RESULT
        ld      h, a
        ld      l, 0
        ld      (curhist), hl
        ld      d, h
        ld      e, 1
        ld      (hl), 0
        ld      bc, 400h - 1
        ldir
        ld      hl, 0
        ld      (count), hl
        ld      hl, STLINE
        ld      de, s_run
        call    puts
        ld      a, (tno)
        add     a, '0'
        call    putc
        ld      l, (ix + 0)
        ld      h, (ix + 1)
        ld      (racall + 1), hl
racall: call    0               ; test (adresa se přepisuje); návrat přes hdone
        ld      a, 03h
        out     (PIOCA), a      ; zákaz přerušení PIO (po testu 0)
        call    pline
        ld      de, TSTLEN
        add     ix, de
        ld      a, (tno)
        inc     a
        ld      (tno), a
        cp      NTEST
        jr      nz, ra1
        ld      a, 7
        call    pline_a
        ld      a, 8
        call    pline_a
        ld      hl, STLINE
        ld      de, s_blank
        jp      puts

; pline: řádek výsledku testu tno (řádek obrazovky 3 + tno), IX = položka tests.
pline:  ld      a, (tno)
        add     a, 3
        call    rowaddr
        ld      a, (tno)
        add     a, '0'
        call    putc
        ld      a, ' '
        call    putc
        push    ix
        pop     de
        inc     de
        inc     de
        inc     de
        ld      b, 7
pl1:    ld      a, (de)
        call    putc
        inc     de
        djnz    pl1
        ld      a, ' '
        call    putc
        ld      a, (ix + 2)
        call    hex2
        ld      a, ' '
        call    putc
        ld      de, (curhist)
        jp      top3

; pline_a: řádek hodnot A testu A (řádek obrazovky 13 + A - 7).
pline_a:
        push    af
        add     a, 13 - 7
        call    rowaddr
        pop     af
        push    af
        add     a, '0'
        call    putc
        ld      de, s_areg
        call    puts
        pop     af
        add     a, a
        add     a, a
        add     a, (high RESULT) + 2
        ld      d, a
        ld      e, 0
        jp      top3

; rowaddr: HL = VRAM + A * 40. Mění: AF, B, DE, HL.
rowaddr:
        ld      hl, VRAM
        ld      de, 40
        or      a
        ret     z
        ld      b, a
rw1:    add     hl, de
        djnz    rw1
        ret

; top3: vypíše tři nejčetnější položky histogramu DE (256 slov) jako "VV CCCC ".
;   HL = kurzor. Mění: AF, BC, DE.
top3:   ld      (tbase), de
        push    hl
        ld      hl, MARK
        ld      de, MARK + 1
        ld      bc, 255
        ld      (hl), 0
        ldir
        pop     hl
        ld      b, 3
t3a:    push    bc
        push    hl
        call    findmax         ; A = index, DE = počet
        pop     hl
        ld      c, a
        ld      a, d
        or      e
        jr      z, t3none
        ld      a, c
        call    hex2
        ld      a, d
        call    hex2n
        ld      a, e
        call    hex2
        push    hl
        ld      h, high MARK
        ld      l, c
        ld      (hl), 1
        pop     hl
        jr      t3n
t3none: ld      de, s_none
        call    puts
t3n:    pop     bc
        djnz    t3a
        ret

; findmax: nejvyšší neoznačený čítač histogramu tbase. Vrací A = index, DE = počet
;   (při shodě první index). Mění: AF, C, DE, HL.
findmax:
        ld      hl, (tbase)
        ld      de, 0
        xor     a
        ld      (bidx), a
        ld      c, 0
fm1:    push    de              ; nejlepší počet
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        inc     hl              ; DE = čítač c
        push    hl
        ld      h, high MARK
        ld      l, c
        ld      a, (hl)
        pop     hl
        or      a
        jr      nz, fmskip
        ex      (sp), hl        ; HL = nejlepší, na zásobníku ukazatel
        push    hl
        or      a
        sbc     hl, de
        pop     hl
        jr      nc, fmkeep      ; nejlepší >= čítač
        ld      h, d
        ld      l, e
        ld      a, c
        ld      (bidx), a
fmkeep: ex      (sp), hl
        pop     de
        jr      fmnext
fmskip: pop     de
fmnext: inc     c
        jr      nz, fm1
        ld      a, (bidx)
        ret

; hex2n: A jako dvě hex číslice bez mezery. Mění: AF.
hex2n:  push    af
        rrca
        rrca
        rrca
        rrca
        call    nib
        pop     af
        jp      nib

; ---------------------------------------------------------------------------
; Testy. Každý připraví kód a skočí na go_ctc nebo go_pio s HL = vstup měřeného kódu.
; ---------------------------------------------------------------------------

; go_ctc: přerušení z CTC2 (CTC1 Mode 2 / N1, CTC2 Mode 0 / N2), PC2 = 1.
go_ctc: ld      (gojp + 1), hl
        ld      hl, NSAMP
        ld      (target), hl
        ld      a, N2
        ld      (reload), a
        ld      a, 54h          ; čítač 1, jen LSB, Mode 2
        ld      (CTCCTL), a
        ld      a, N1
        ld      (CTC1D), a
        ld      a, 90h          ; čítač 2, jen LSB, Mode 0 (OUT2 = 0)
        ld      (CTCCTL), a
        ld      a, N2
        ld      (CTC2D), a
        ld      a, 05h
        ld      (PPICTL), a     ; PC2 = 1: přerušení z CTC2 povoleno
        jr      gorun

; go_pio: přerušení z Z80 PIO brány A (Mode 3, PA5 = VBLN aktivní 0, vektor PIOVEC), PC2 = 0.
go_pio: ld      (gojp + 1), hl
        ld      hl, NPIO
        ld      (target), hl
        xor     a
        ld      (reload), a
        ld      a, 04h
        ld      (PPICTL), a     ; PC2 = 0: CTC2 maskované
        ld      a, 0CFh         ; Mode 3
        out     (PIOCA), a
        ld      a, 3Fh          ; I/O select: PA0 až PA5 vstupy
        out     (PIOCA), a
        ld      a, PIOVEC       ; vektor
        out     (PIOCA), a
        ld      a, 97h          ; ICW: povoleno, OR, aktivní 0, maska následuje
        out     (PIOCA), a
        ld      a, 0DFh         ; maska: sleduje se jen PA5
        out     (PIOCA), a

; gorun: uloží SP (ukazuje na návrat do runall), nastaví registry testu a skočí na kód.
gorun:  ld      (drvsp), sp
        ld      a, (t_a)
        ld      hl, (t_hl)
        ld      de, (t_de)
        ld      c, 0
        ei
gojp:   jp      0               ; adresa se přepisuje

; jploop: na JPLOOP zapíše JP JPLOOP. Vrací HL = JPLOOP.
jploop: ld      hl, JPLOOP
        ld      (hl), 0C3h
        inc     hl
        ld      (hl), low JPLOOP
        inc     hl
        ld      (hl), high JPLOOP
        ld      hl, JPLOOP
        ret

test0:  call    jploop
        jp      go_pio

test1:  call    jploop
        jp      go_ctc

test2:  ld      a, 9Bh
        ld      (TDATA + 1), a
        ld      hl, p_lda
        ld      b, 3
        ld      de, 1024
        call    mkblk
        call    tailjp
        ld      hl, BLK
        jp      go_ctc

test3:  ld      hl, HALTAT
        ld      (hl), 76h       ; HALT
        inc     hl
        ld      (hl), 37h       ; SCF
        inc     hl
        ld      (hl), 18h       ; JR HALTAT
        inc     hl
        ld      (hl), 0FCh
        ld      hl, HALTAT
        jp      go_ctc

test4:  ld      a, 2Ch
        ld      (t_a), a
        ld      hl, TDATA
        ld      (t_hl), hl
        ld      hl, p_ldhl
        ld      b, 1
        ld      de, 2048
        call    mkblk
        call    tailjp
        ld      hl, BLK
        jp      go_ctc

test5:  ld      hl, 594Bh
        ld      (t_de), hl
        ld      hl, p_push
        ld      b, 1
        ld      de, 512
        call    mkblk
        push    hl              ; vstup = LD SP,PUSHSP za blokem
        ld      (hl), 31h
        inc     hl
        ld      (hl), low PUSHSP
        inc     hl
        ld      (hl), high PUSHSP
        inc     hl
        call    tailjp
        pop     hl
        jp      go_ctc

test6:  ld      a, 2Ch
        ld      (t_a), a
        ld      hl, p_out
        ld      b, 2
        ld      de, 1024
        call    mkblk
        call    tailjp
        ld      hl, BLK
        jp      go_ctc

test7:  ld      hl, p_in
        ld      b, 2
        ld      de, 1024
        call    mkblk
        call    tailjp
        ld      hl, BLK
        jp      go_ctc

test8:  ld      a, 0Fh          ; brána B: Mode 0 (výstup)
        out     (PIOCB), a
        ld      a, 0A9h
        out     (PIODB), a
        ld      a, 03h          ; zákaz přerušení brány B
        out     (PIOCB), a
        ld      hl, p_inpio
        ld      b, 2
        ld      de, 1024
        call    mkblk
        call    tailjp
        ld      hl, BLK
        jp      go_ctc

p_lda:  db      3Ah, low (TDATA + 1), high (TDATA + 1)    ; LD A,(3001h)
p_ldhl: db      77h                                     ; LD (HL),A
p_push: db      0D5h                                    ; PUSH DE
p_out:  db      0D3h, 0A1h                              ; OUT (0A1h),A
p_in:   db      0DBh, 0A0h                              ; IN A,(0A0h)
p_inpio: db     0DBh, PIODB                             ; IN A,(0FFh)

; mkblk: od BLK zapíše DE krát vzor (HL, B bajtů). Vrací HL = adresa za blokem.
;   Mění: AF, B, DE, HL.
mkblk:  ld      (mkpat), hl
        ld      a, b
        ld      (mklen), a
        ld      hl, BLK
mk1:    push    de
        ld      de, (mkpat)
        ld      a, (mklen)
        ld      b, a
mk2:    ld      a, (de)
        ld      (hl), a
        inc     hl
        inc     de
        djnz    mk2
        pop     de
        dec     de
        ld      a, d
        or      e
        jr      nz, mk1
        ret

; tailjp: na HL zapíše JP BLK. Mění: HL.
tailjp: ld      (hl), 0C3h
        inc     hl
        ld      (hl), low BLK
        inc     hl
        ld      (hl), high BLK
        ret

; tests: adresa testu, EM (vektor podle emulátoru s mutantem im2-vector), název 7 znaků
TSTLEN  equ 10
tests:  dw      test0
        db      PIOVEC, "PIO VEC"
        dw      test1
        db      4Dh, "JP 4D33"
        dw      test2
        db      9Bh, "LDA 9B "
        dw      test3
        db      76h, "HALT   "
        dw      test4
        db      77h, "LDHL 2C"
        dw      test5
        db      0D5h, "PUSH 4B"
        dw      test6
        db      0A1h, "OUT A1 "
        dw      test7
        db      0A0h, "IN A0  "
        dw      test8
        db      0FFh, "IN PIOB"

; ---------------------------------------------------------------------------
; Pomocné rutiny (jako mz800_romwait.asm)
; ---------------------------------------------------------------------------

; clrst: smaže stavovou oblast (od STLINE do konce obrazovky). Mění: AF, BC, DE, HL.
clrst:  ld      hl, STLINE
        ld      de, STLINE + 1
        ld      bc, VRAMEND - STLINE - 1
        ld      (hl), 00h
        ldir
        ret

; showmenu: vypíše nabídku od začátku obrazovky. Mění: AF, DE, HL.
showmenu:
        ld      hl, VRAM
        ld      de, menu
mn1:    ld      a, (de)
        cp      0Dh
        ret     z
        call    puts
        inc     de
        call    newline
        jr      mn1

; dly10ms: zpoždění asi 10 ms při 3,55 MHz. Mění: AF.
dly10ms:
        push    de
        ld      de, 1364
d10a:   dec     de
        ld      a, d
        or      e
        jr      nz, d10a
        pop     de
        ret

; waitkey: čeká na stisk klávesy 1 až 8, vrací A = 0 až 7. Mění: AF, BC.
waitkey:
        ld      a, 0F5h
        ld      (KEYPA), a
        ld      a, (KEYPB)
        cpl
        or      a
        jr      z, waitkey
        ld      b, 0
wk1:    rlca
        jr      c, wk2
        inc     b
        jr      wk1
wk2:    ld      a, b
        ret

; waitrel: čeká na uvolnění kláves 1 až 8 a pak 20 ms. Mění: AF.
waitrel:
        ld      a, 0F5h
        ld      (KEYPA), a
        ld      a, (KEYPB)
        cpl
        or      a
        jr      nz, waitrel
        call    dly10ms
        jp      dly10ms

; Proměnné
tno:    db      0               ; číslo běžícího testu
curhist: dw     0               ; histogram vektorů běžícího testu (+200h hodnoty A)
count:  dw      0               ; vzorků v běžícím testu
target: dw      0               ; požadovaný počet vzorků
reload: db      0               ; hodnota pro CTC2 v obsluze (0 = nenahrávat, test PIO)
drvsp:  dw      0               ; SP pro návrat z hdone do runall
t_a:    db      0               ; A, HL, DE na vstupu měřeného kódu
t_hl:   dw      0
t_de:   dw      0
tbase:  dw      0               ; histogram pro findmax
bidx:   db      0
mkpat:  dw      0
mklen:  db      0

menu:   db      "MZ-800 IM 2 VECTOR TEST", 0Dh
        db      "1 RUN ALL - EM IS EMULATOR VECTOR", 0Dh
        db      "N TEST    EM  VV CCCC VV CCCC VV CCCC", 0Dh
        db      0Dh
s_run:  db      "RUNNING TEST ", 0Dh
s_done: db      "DONE - PRESS 1", 0Dh
s_blank: db     "              ", 0Dh
s_none: db      "-- ---- ", 0Dh
s_areg: db      " A REG       ", 0Dh

        include "im2_tables.inc"
        include "vram_common.inc"

progend:
