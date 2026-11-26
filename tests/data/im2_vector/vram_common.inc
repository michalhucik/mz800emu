; bus_common.inc - spolecne rutiny mericich programu M2 / M4 / M5
;
; Vystup jde primo do textove VRAM 0D000h (display kody), ne pres ROM:
; po zavedeni programu emulatorem (media_run_mzf) nemusi byt kurzor ROM
; inicializovany, na realnem stroji (prikaz L monitoru) je jedno.
; Program nejdriv nastavi rezim MZ-700 (OUT 0CEh = 08h; na MZ-700 zapis na
; volny port) a vychozi mapu pameti (OUT 0E4h: ROM 0000h, VRAM 0D000h,
; ROM 0E000h), smaze obrazovku a pise od prvniho radku.
;
; Pouziti: include na konci programu; na zacatku CALL vinit, pak
; CALL puts (DE -> retezec ukonceny 0Dh), CALL hex2 (A), CALL newline.
; Pracovni registry: HL = kurzor ve VRAM (vinit ho nastavi).

VRAM    equ 0D000h
VRAMEND equ 0D3E8h
VRATTR  equ 0D800h

; vinit: rezim MZ-700, mapa pameti, smazani obrazovky (znaky 00h, atributy 70h)
vinit:
        ld      a, 08h
        out     (0CEh), a
        out     (0E4h), a
        ld      hl, VRAM
        ld      de, VRAM + 1
        ld      bc, 1000 - 1
        ld      (hl), 00h
        ldir
        ld      hl, VRATTR
        ld      de, VRATTR + 1
        ld      bc, 1000 - 1
        ld      (hl), 70h
        ldir
        ld      hl, VRAM
        ret

; puts: retezec ASCII (DE) ukonceny 0Dh na kurzor HL
puts:
        ld      a, (de)
        cp      0Dh
        ret     z
        call    putc
        inc     de
        jr      puts

; putc: ASCII v A -> display kod -> (HL), HL++
;   '0'-'9' -> 20h-29h, 'A'-'Z' -> 01h-1Ah, ' ' -> 00h, '-' -> 2Ah, '/' -> 2Fh
putc:
        push    af
        cp      ' '
        jr      nz, pc1
        xor     a
        jr      pcw
pc1:    cp      '-'
        jr      nz, pc2
        ld      a, 2Ah
        jr      pcw
pc2:    cp      '/'
        jr      nz, pc3
        ld      a, 2Fh
        jr      pcw
pc3:    cp      '0'
        jr      c, pcq
        cp      '9' + 1
        jr      nc, pc4
        sub     '0' - 20h
        jr      pcw
pc4:    cp      'A'
        jr      c, pcq
        cp      'Z' + 1
        jr      nc, pcq
        sub     'A' - 01h
        jr      pcw
pcq:    ld      a, 49h          ; '?' v display kodu
pcw:    ld      (hl), a
        inc     hl
        pop     af
        ret

; newline: kurzor na zacatek dalsiho radku (40 znaku)
newline:
        push    de
        push    af
        ld      a, l
        sub     low VRAM
        ld      e, a
        ld      a, h
        sbc     a, high VRAM
        ld      d, a            ; DE = offset
nl1:    ld      a, e
        sub     40
        ld      e, a
        jr      nc, nl2
        dec     d
nl2:    bit     7, d
        jr      z, nl1          ; dokud offset >= 0, odecitej 40 -> zbytek radku (zaporny)
        ; DE = -(zbytek do konce radku); kurzor += -DE
        ld      a, l
        sub     e
        ld      l, a
        ld      a, h
        sbc     a, d
        ld      h, a
        pop     af
        pop     de
        ret

; hex2: vypise A jako dve hex cislice a mezeru
hex2:
        push    af
        rrca
        rrca
        rrca
        rrca
        call    nib
        pop     af
        call    nib
        ld      a, ' '
        call    putc
        ret
nib:
        and     0Fh
        add     a, '0'
        cp      '9' + 1
        jr      c, nib1
        add     a, 'A' - '9' - 1
nib1:   call    putc
        ret
