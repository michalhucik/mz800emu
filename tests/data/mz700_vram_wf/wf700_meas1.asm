; wf700_meas1.asm - měřicí program pro MZ-800: registry WF a RF v režimu MZ-700
;
; Otázka: jde na MZ-800 v režimu MZ-700 (DMD 08h) zápis procesoru do VRAM (znaky, atributy,
; CG-RAM) přes registr formátu zápisu WF (port 0CCh) a čtení přes registr RF (port 0CDh)?
; Podnět: pull request č. 12 (GitHub), který měřil 12 případů zápisu do znakové buňky.
; Program měří bez sond: procesor zapisuje a čte zpět, výsledek je na obrazovce.
;
; Překlad: pasmo --bin wf700_meas1.asm wf700_meas1.bin
; MZF:     hlavička 128 B, typ 01h (OBJ), zaváděcí i startovací adresa 2000h
; Tabulky očekávaných hodnot (wf700_meas1_tables.asm) jsou vygenerované z C modelu netlistu GDG.
;
; Zkoušené buňky: znak 0D3E7h a atribut 0DBE7h (pravý dolní roh obrazovky), CG-RAM 0C7F8h
; (znak FFh, řádek 0; po IN 0E0h). Původní obsah buňky základní VRAM se po zkoušce vrací.
;
; Zkouška zápisu (wsweep), pro WF = 00h až FFh:
;   WF 01h: zápis 33h;  WF 04h: zápis 33h;  WF = zkoušená hodnota: zápis 55h;
;   WF 01h, RF 01h: čtení (tabulka VA);  RF 04h: čtení (tabulka VC).
;   33h a 55h obsahují všechny čtyři kombinace bitů, výsledek tedy určuje logickou funkci
;   (55h data, 66h XOR, 77h OR, 22h mazání bitů, 00h nula, 33h beze změny).
; Zkouška čtení (rsweep), pro RF = 00h až FFh:
;   WF 01h: zápis V1;  WF 04h: zápis V3;  RF = zkoušená hodnota: čtení.
; Po každém zápisu program čeká asi 290 us (přes 2 řádky obrazu); model netlistu dává
; u buňky atributu při čtení hned po zápisu jiný výsledek. Tabulka 10 proto opakuje
; tabulku 2 bez čekání.
;
; Tabulky výsledků (každá 256 bajtů, od 6000h):
;    0  6000h  znak, zápis, čtení RF 01h          1  6100h  znak, zápis, čtení RF 04h
;    2  6200h  atribut, zápis, čtení RF 01h       3  6300h  atribut, zápis, čtení RF 04h
;    4  6400h  CG-RAM, zápis, čtení RF 01h        5  6500h  CG-RAM, zápis, čtení RF 04h
;    6  6600h  znak, čtení, V1 = 01h, V3 = 04h    7  6700h  znak, čtení, V1 = FEh, V3 = FBh
;    8  6800h  DMD 0Ch, adresa znaku, zápis, čtení RF 01h
;    9  6900h  totéž, čtení RF 04h
;   10  6A00h  jako tabulka 2, bez čekání po zápisu (6B00h: totéž s RF 04h, nevyhodnocuje se)
;
; Obrazovka: pro tabulky 0 až 9 řádek "Tn jméno CMP c M m P p D d":
;   c = počet porovnávaných hodnot (hodnoty, které podle modelu závisejí na obsahu jiných
;       buněk VRAM, se neporovnávají),
;   m = shod s modelem netlistu, p = shod s opravou z pull requestu, d = shod s přímým
;       zápisem a čtením (emulátor bez opravy).
; Dál počet shod tabulky 10 s tabulkou 2, kontrolní součet všech výsledků (16 bitů)
; a prvních 9 rozdílů proti modelu (tabulka, index = WF nebo RF, změřeno, model).
;
; Co se od stroje předpokládá: IN 0E0h připojí CG-ROM na 1000h a CG-RAM na 0C000h, IN 0E1h
; je odpojí (báze, hw/03-banking.md); program si CG-RAM z CG-ROM sám naplní, aby byl text
; čitelný i po programu, který kreslil v režimu MZ-800. Rozšířená VRAM se obsahem zkoušek
; přepíše (buňky téže adresy). Přerušení zakázaná, konec: RESET.
;
; Zaveden na 2000h, zásobník 7F00h.

P_WF    equ     0CCh            ; GDG: formát zápisu
P_RF    equ     0CDh            ; GDG: formát čtení
P_DMD   equ     0CEh            ; GDG: režim zobrazení (08h = MZ-700)
RESB     equ     6000h           ; tabulky výsledků
CHRC    equ     0D3E7h          ; zkoušená buňka znaku
ATRC    equ     0DBE7h          ; zkoušená buňka atributu
CGC     equ     0C7F8h          ; zkoušená buňka CG-RAM
NTAB    equ     10              ; počet vyhodnocovaných tabulek
MAXMIS  equ     9               ; počet vypisovaných rozdílů

        org     2000h

start:  di
        ld      sp, 7F00h
        ld      a, 08h          ; režim MZ-700
        out     (P_DMD), a
        ld      a, 01h          ; WF a RF podle servisního manuálu pro režim MZ-700
        out     (P_WF), a
        out     (P_RF), a
        out     (0E4h), a       ; ROM, VRAM na 0D000h
        in      a, (0E0h)       ; CG-ROM na 1000h, CG-RAM na 0C000h
        ld      hl, 1000h       ; CG-RAM naplnit z CG-ROM
        ld      de, 0C000h
        ld      bc, 1000h
        ldir
        in      a, (0E1h)
        ld      hl, 0D000h      ; znaky 0
        ld      de, 0D001h
        ld      bc, 07FFh
        ld      (hl), 0
        ldir
        ld      hl, 0D800h      ; atributy: bílá na modré
        ld      de, 0D801h
        ld      bc, 07FFh
        ld      (hl), 71h
        ldir
        xor     a
        call    setrow
        ld      hl, s_title
        call    puts
        ld      a, 24
        call    setrow
        ld      hl, s_run
        call    puts

; ---- měření ----
        ld      a, 1
        ld      (dly), a
        ld      hl, CHRC
        ld      de, RESB
        call    wsweep          ; tabulky 0 a 1
        ld      hl, ATRC
        ld      de, RESB + 0200h
        call    wsweep          ; tabulky 2 a 3
        in      a, (0E0h)
        ld      hl, CGC
        ld      de, RESB + 0400h
        call    wsweep          ; tabulky 4 a 5
        in      a, (0E1h)
        ld      hl, CHRC
        ld      de, RESB + 0600h
        ld      bc, 0104h
        call    rsweep          ; tabulka 6
        ld      hl, CHRC
        ld      de, RESB + 0700h
        ld      bc, 0FEFBh
        call    rsweep          ; tabulka 7
        ld      a, 0Ch          ; nedokumentovaný režim DMD 0Ch
        out     (P_DMD), a
        ld      hl, CHRC
        ld      de, RESB + 0800h
        call    wsweep          ; tabulky 8 a 9
        ld      a, 08h
        out     (P_DMD), a
        xor     a
        ld      (dly), a
        ld      hl, ATRC
        ld      de, RESB + 0A00h
        call    wsweep          ; tabulka 10 (a 11): atribut bez čekání po zápisu

; ---- vyhodnocení ----
        ld      a, 1
        call    setrow
        ld      hl, s_legend
        call    puts
        xor     a
        ld      (tno), a
        ld      (nmis), a
        ld      hl, RESB
        ld      de, EXPM
        exx
        ld      hl, MASK
        ld      de, EXPP
        ld      bc, EXPD
        exx
ev_t:   push    hl
        ld      hl, 0
        ld      (n_cmp), hl
        ld      (n_m), hl
        ld      (n_p), hl
        ld      (n_d), hl
        pop     hl
        ld      b, 0
ev1:    exx
        ld      a, (hl)         ; maska: 0 = neporovnávat
        exx
        or      a
        jr      z, ev_next
        push    hl
        ld      hl, (n_cmp)
        inc     hl
        ld      (n_cmp), hl
        pop     hl
        ld      a, (de)         ; model netlistu
        cp      (hl)
        jr      nz, ev_mm
        push    hl
        ld      hl, (n_m)
        inc     hl
        ld      (n_m), hl
        pop     hl
        jr      ev_p
ev_mm:  call    mismatch
ev_p:   exx
        ld      a, (de)         ; oprava z pull requestu
        exx
        cp      (hl)
        jr      nz, ev_d
        push    hl
        ld      hl, (n_p)
        inc     hl
        ld      (n_p), hl
        pop     hl
ev_d:   exx
        ld      a, (bc)         ; přímý zápis a čtení
        exx
        cp      (hl)
        jr      nz, ev_next
        push    hl
        ld      hl, (n_d)
        inc     hl
        ld      (n_d), hl
        pop     hl
ev_next:
        inc     hl
        inc     de
        exx
        inc     hl
        inc     de
        inc     bc
        exx
        dec     b
        jp      nz, ev1
        call    prline
        ld      a, (tno)
        inc     a
        ld      (tno), a
        cp      NTAB
        jp      c, ev_t

; ---- tabulka 10 proti tabulce 2 ----
        ld      hl, RESB + 0200h
        ld      de, RESB + 0A00h
        ld      bc, 0           ; BC = počet shod
        xor     a
        ld      (tno), a        ; počítadlo 256 průchodů
nd1:    ld      a, (de)
        cp      (hl)
        jr      nz, nd2
        inc     bc
nd2:    inc     hl
        inc     de
        ld      a, (tno)
        dec     a
        ld      (tno), a
        jr      nz, nd1
        push    bc
        ld      a, 12
        call    setrow
        ld      hl, s_nodly
        call    puts
        pop     hl
        call    prdec
        ld      hl, s_of256
        call    puts

; ---- kontrolní součet tabulek 0 až 10 ----
        ld      hl, RESB
        ld      de, 0           ; DE = součet
        ld      bc, 0B00h
cs1:    ld      a, (hl)
        add     a, e
        ld      e, a
        jr      nc, cs2
        inc     d
cs2:    inc     hl
        dec     bc
        ld      a, b
        or      c
        jr      nz, cs1
        push    de
        ld      a, 13
        call    setrow
        ld      hl, s_sum
        call    puts
        pop     de
        ld      a, d
        call    prhex
        ld      a, e
        call    prhex
        ld      a, 14
        call    setrow
        ld      hl, s_first
        call    puts
        ld      a, 24
        call    setrow
        ld      hl, s_done
        call    puts
        ld      a, 01h
        out     (P_WF), a
        out     (P_RF), a
done:   jr      done

; ---- wsweep: zkouška zápisu všemi hodnotami WF ----
; Vstup: HL = buňka, DE = tabulka pro čtení RF 01h (dolní bajt 00h), DE + 100h = tabulka RF 04h.
; Výstup: WF = RF = 01h, buňka základní VRAM má původní obsah. Ničí A, C, E.
wsweep: ld      a, 01h
        out     (P_WF), a
        out     (P_RF), a
        ld      a, (hl)
        ld      (savev), a
        ld      c, 0            ; C = zkoušená hodnota WF
ws1:    ld      a, 01h
        out     (P_WF), a
        ld      (hl), 33h       ; výchozí obsah základní VRAM
        call    wdelay
        ld      a, 04h
        out     (P_WF), a
        ld      (hl), 33h       ; výchozí obsah rozšířené VRAM
        call    wdelay
        ld      a, c
        out     (P_WF), a
        ld      (hl), 55h       ; zkoušený zápis
        call    wdelay
        ld      a, 01h
        out     (P_WF), a
        out     (P_RF), a
        ld      a, (hl)
        ld      (de), a
        ld      a, 04h
        out     (P_RF), a
        ld      a, (hl)
        inc     d
        ld      (de), a
        dec     d
        ld      a, 01h
        out     (P_RF), a
        inc     e
        inc     c
        jr      nz, ws1
        ld      a, (savev)
        ld      (hl), a
        call    wdelay
        ret

; ---- rsweep: zkouška čtení všemi hodnotami RF ----
; Vstup: HL = buňka, DE = tabulka (dolní bajt 00h), B = bajt zapsaný s WF 01h, C = s WF 04h.
; Výstup: WF = RF = 01h, buňka základní VRAM má původní obsah. Ničí A, C, E.
rsweep: ld      a, 01h
        out     (P_WF), a
        out     (P_RF), a
        ld      a, (hl)
        ld      (savev), a
        ld      (hl), b
        call    wdelay
        ld      a, 04h
        out     (P_WF), a
        ld      (hl), c
        call    wdelay
        ld      a, 01h
        out     (P_WF), a
        ld      c, 0            ; C = zkoušená hodnota RF
rs1:    ld      a, c
        out     (P_RF), a
        ld      a, (hl)
        ld      (de), a
        inc     e
        inc     c
        jr      nz, rs1
        ld      a, 01h
        out     (P_RF), a       ; bit 4 je společný pro RF a WF, proto WF až po RF
        out     (P_WF), a
        ld      a, (savev)
        ld      (hl), a
        call    wdelay
        ret

; ---- wdelay: čekání po zápisu do VRAM (asi 1040 T), jen když (dly) <> 0. Ničí A. ----
wdelay: ld      a, (dly)
        or      a
        ret     z
        push    bc
        ld      b, 80
wd1:    djnz    wd1
        pop     bc
        ret

; ---- mismatch: zapamatuje a vypíše rozdíl proti modelu (nejvýš MAXMIS) ----
; Vstup: HL = změřená hodnota, DE = hodnota modelu, B = 256 - index (0 = index 0).
; Zachovává HL, DE, BC.
mismatch:
        ld      a, (nmis)
        cp      MAXMIS
        ret     nc
        ld      a, (hl)
        ld      (m_got), a
        ld      a, (de)
        ld      (m_exp), a
        ld      a, b
        neg
        ld      (m_idx), a
        push    hl
        ld      a, (nmis)
        add     a, 15
        call    setrow
        ld      a, (nmis)
        inc     a
        ld      (nmis), a
        ld      a, 'T'
        call    pchr
        ld      a, (tno)
        add     a, '0'
        call    pchr
        ld      hl, s_idx
        call    puts
        ld      a, (m_idx)
        call    prhex
        ld      hl, s_got
        call    puts
        ld      a, (m_got)
        call    prhex
        ld      hl, s_m
        call    puts
        ld      a, (m_exp)
        call    prhex
        pop     hl
        ret

; ---- prline: řádek výsledku tabulky (tno) na řádek obrazovky 2 + tno ----
; Zachovává HL, DE; ničí A, BC.
prline: push    hl
        push    de
        ld      a, (tno)
        add     a, 2
        call    setrow
        ld      a, 'T'
        call    pchr
        ld      a, (tno)
        add     a, '0'
        call    pchr
        ld      a, ' '
        call    pchr
        ld      a, (tno)        ; jméno: 7 bajtů na tabulku
        ld      l, a
        add     a, a
        add     a, l
        add     a, a
        add     a, l
        ld      l, a
        ld      h, 0
        ld      de, names
        add     hl, de
        call    puts
        ld      hl, s_cmp
        call    puts
        ld      hl, (n_cmp)
        call    prdec
        ld      hl, s_m
        call    puts
        ld      hl, (n_m)
        call    prdec
        ld      hl, s_p
        call    puts
        ld      hl, (n_p)
        call    prdec
        ld      hl, s_d
        call    puts
        ld      hl, (n_d)
        call    prdec
        pop     de
        pop     hl
        ret

; ---- setrow: kurzor na začátek řádku A (0 až 24). Zachovává HL, DE, BC. ----
setrow: push    hl
        push    de
        ld      l, a
        ld      h, 0
        add     hl, hl
        add     hl, hl
        add     hl, hl          ; x 8
        ld      d, h
        ld      e, l
        add     hl, hl
        add     hl, hl          ; x 32
        add     hl, de          ; x 40
        ld      de, 0D000h
        add     hl, de
        ld      (curs), hl
        pop     de
        pop     hl
        ret

; ---- puts: text ASCII od HL (končí 00h) na kurzor. Ničí A, HL. ----
puts:   ld      a, (hl)
        or      a
        ret     z
        call    pchr
        inc     hl
        jr      puts

; ---- prdec: HL (0 až 999) jako tři desítkové číslice. Ničí A, HL. ----
prdec:  push    bc
        push    de
        ld      de, 100
        call    pd_dig
        ld      de, 10
        call    pd_dig
        ld      a, l
        add     a, 20h
        call    putc
        pop     de
        pop     bc
        ret
pd_dig: ld      b, 0
pd1:    or      a
        sbc     hl, de
        jr      c, pd2
        inc     b
        jr      pd1
pd2:    add     hl, de
        ld      a, b
        add     a, 20h
        jr      putc

; ---- prhex: A jako dvě šestnáctkové číslice. Ničí A. ----
prhex:  push    af
        rrca
        rrca
        rrca
        rrca
        call    ph_n
        pop     af
ph_n:   and     0Fh
        cp      10
        jr      c, ph_d
        sub     9               ; A až F: display kódy 01h až 06h
        jr      putc
ph_d:   add     a, 20h          ; 0 až 9: display kódy 20h až 29h
        jr      putc

; ---- pchr: znak ASCII (mezera, 0-9, A-Z, '-', '/') na kurzor. Ničí A. ----
pchr:   cp      ' '
        jr      nz, pc1
        xor     a
        jr      putc
pc1:    cp      '-'
        jr      nz, pc2
        ld      a, 2Ah
        jr      putc
pc2:    cp      '/'
        jr      nz, pc3
        ld      a, 2Fh
        jr      putc
pc3:    cp      'A'
        jr      nc, pc4
        sub     10h             ; číslice: 30h -> 20h
        jr      putc
pc4:    sub     40h             ; písmena: 41h -> 01h

; ---- putc: display kód A na kurzor, kurzor o 1 dál. Zachovává vše kromě kurzoru. ----
putc:   push    hl
        ld      hl, (curs)
        ld      (hl), a
        inc     hl
        ld      (curs), hl
        pop     hl
        ret

s_title:  db    "WF700 MEAS1 - WF AND RF IN MZ-700 MODE", 0
s_run:    db    "MEASURING", 0
s_legend: db    "M NETLIST MODEL  P PULL REQ  D DIRECT", 0
s_cmp:    db    " CMP ", 0
s_m:      db    " M ", 0
s_p:      db    " P ", 0
s_d:      db    " D ", 0
s_idx:    db    " IDX ", 0
s_got:    db    " GOT ", 0
s_nodly:  db    "T2 WITHOUT DELAY - SAME ", 0
s_of256:  db    " OF 256", 0
s_sum:    db    "SUM ", 0
s_first:  db    "FIRST DIFFERENCES FROM M", 0
s_done:   db    "RESULTS 6000-6AFF      RESET TO EXIT", 0
names:    db    "CHR VA", 0
          db    "CHR VC", 0
          db    "ATR VA", 0
          db    "ATR VC", 0
          db    "CGR VA", 0
          db    "CGR VC", 0
          db    "RD 01 ", 0
          db    "RD FE ", 0
          db    "0C VA ", 0
          db    "0C VC ", 0

curs:   dw      0D000h          ; adresa kurzoru ve VRAM
dly:    db      1               ; 1 = po zápisu do VRAM čekat
savev:  db      0               ; původní obsah zkoušené buňky
tno:    db      0               ; číslo vyhodnocované tabulky
nmis:   db      0               ; počet vypsaných rozdílů
m_got:  db      0               ; rozdíl: změřená hodnota
m_exp:  db      0               ; rozdíl: hodnota modelu
m_idx:  db      0               ; rozdíl: index v tabulce
n_cmp:  dw      0               ; počet porovnávaných hodnot tabulky
n_m:    dw      0               ; shody s modelem netlistu
n_p:    dw      0               ; shody s opravou z pull requestu
n_d:    dw      0               ; shody s přímým zápisem

; EXPM, MASK, EXPP, EXPD: po 2560 bajtech (10 tabulek)
        include "wf700_meas1_tables.asm"
