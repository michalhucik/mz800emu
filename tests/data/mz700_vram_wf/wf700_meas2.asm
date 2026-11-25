; wf700_meas2.asm - měřicí program pro MZ-800: co čte procesor v režimu MZ-700 s RF, který
; vybírá rovinu II nebo IV, a při operaci SEARCH
;
; Navazuje na wf700_meas1 (archive/wf700/), bod A15 báze (mz800emu/02-rozdily-proti-hw.md).
; Ověřuje mechanismus z C modelu netlistu (gdg-mz800, zpráva 2026-11-25, jen model):
;   - čtení CPU v režimu MZ-700 nahraje jen latche rovin I (VA) a III (VC) z buňky adresy;
;   - latche rovin II a IV plní video fetch: naposledy 4. čtení buňky 39 posledního
;     proběhlého řádku (VA do II, tatáž buňka VC do IV), i ve svislém zatemnění;
;   - READ: AND vybraných z VA[c], X2, VC[c], X4 (bity 0 až 3), nic vybráno = 0FFh;
;   - SEARCH, B/A = 0: (bit 0 ? VA[c] : ~VA[c]) AND (bit 1 ? X2 : ~X2);
;     B/A = 1: totéž s VC[c] a X4 (bity 2 a 3).
;   X2, X4 = bajt VA, VC na adrese 4. čtení: F = sada * 800h + (P >> 5) * 100h + ((8 * C + r)
;   AND 0FFh), C kód znaku buňky 39, P bajt vzoru, r řádek znaku, sada = bit 7 atributu.
;
; Překlad: pasmo --bin wf700_meas2.asm wf700_meas2.bin
; MZF:     hlavička 128 B, typ 01h (OBJ), zaváděcí i startovací adresa 2000h
;
; Obsah VRAM (vše v režimu MZ-700, DMD 08h, zápis přes WF podle wf700_meas1):
;   VC všude (WF 04h): bajt = dolní bajt adresy CPU XOR 80h (0C000h až 0DFFFh);
;   VA CG-RAM 0C000h až 0CFFFh (WF 01h): bajt = dolní bajt adresy;
;   VA atributy 0D800h až 0DFFFh: 00h (sada 0);
;   VA text 0D000h až 0D7FFh, offset o: varianta A = o mod 40 (kód = sloupec),
;                                         varianta B = o div 40 (kód = textový řádek).
; S CG-RAM "bajt = dolní bajt adresy" je bajt na adrese F vždy (8 * C + r) AND 0FFh, takže
; podle modelu: X2 = (8 * C + r) AND 0FFh, X4 = X2 XOR 80h.
;   Varianta A: C = 39 (buňka 39), X2 = 38h + r (38h až 3Fh), X4 = 0B8h až 0BFh.
;   Varianta B: C = textový řádek posledního řádku obrazu, X2 = (8 * řádek + r) AND 0FFh,
;   tedy číslo řádku rastru v rámci textu (mimo canvas podle toho, jak běží čítač řádku).
;
; Čte se z adres 0C0FFh (VA = 0FFh, VC = 7Fh) a 0C0AAh (VA = 0AAh, VC = 2Ah) s RF 01h, 04h,
; 02h, 08h, 0Ah, 03h, 0Fh, 80h, 81h, 82h, 84h, 88h (mezi čteními RF zpět 01h). Sada čtení
; se dělá čtyřikrát bez synchronizace s obrazem a dvakrát hned po začátku svislého zatemnění
; (bit 6 stavu 0CEh z 1 na 0), pro každou variantu.
;
; Obrazovka: 4 stránky (varianta A a B, každá bez synchronizace a v zatemnění), přepíná
; libovolná klávesa. Řádek pro každou hodnotu RF, pro každou adresu přečtené bajty všech sad
; stránky (4 nebo 2) šestnáctkově. Stačí fotka všech čtyř stránek.
;
; Kontrola: RF 01h musí dát FF a AA, RF 04h 7F a 2A (VA a VC buňky adresy). Bez rozšířené
; VRAM neplatí VC ani X4. Program obsah VRAM přepíše, CG-RAM pak obnoví z CG-ROM. Přerušení
; zakázaná, zásobník 7F00h, zaveden na 2000h. Výsledky zůstanou v paměti od 6000h (popis
; u RESB). Konec: RESET.

P_WF    equ     0CCh            ; GDG: formát zápisu
P_RF    equ     0CDh            ; GDG: formát čtení
P_DMD   equ     0CEh            ; GDG: OUT režim zobrazení, IN stavový registr
KEYPA   equ     0E000h          ; 8255 port A (výběr řádku klávesnice) v režimu MZ-700
KEYPB   equ     0E001h          ; 8255 port B (sloupce klávesnice, 0 = stisk)
NADDR   equ     2               ; počet adres
NRF     equ     12              ; počet hodnot RF
NSET    equ     6               ; sady čtení v jedné variantě (0 až 3 bez synchronizace, 4 a 5 ve VBLN)
; Výsledky: blok 32 bajtů pro variantu v (0 = A, 1 = B) a sadu s na 6000h + (v * 8 + s) * 32,
; v bloku bajt adresa * NRF + index RF.
RESB    equ     6000h

        org     2000h

start:  di
        ld      sp, 7F00h
        ld      a, 08h          ; režim MZ-700
        out     (P_DMD), a
        out     (0E4h), a       ; ROM, VRAM na 0D000h, porty 0E000h
        in      a, (0E0h)       ; CG-ROM na 1000h, CG-RAM na 0C000h
        xor     a
        ld      (varno), a
var_lp:
; ---- naplnit VRAM ----
        ld      a, 04h          ; VC: dolní bajt adresy XOR 80h
        out     (P_WF), a
        ld      hl, 0C000h
fv1:    ld      a, l
        xor     80h
        ld      (hl), a
        inc     hl
        ld      a, h
        cp      0E0h
        jr      c, fv1
        ld      a, 01h          ; VA, WF 01h
        out     (P_WF), a
        out     (P_RF), a
        ld      hl, 0C000h      ; CG-RAM: dolní bajt adresy
fv2:    ld      (hl), l
        inc     hl
        ld      a, h
        cp      0D0h
        jr      c, fv2
        ld      hl, 0D800h      ; atributy 00h
        ld      de, 0D801h
        ld      bc, 07FFh
        ld      (hl), 0
        ldir
        ld      hl, 0D000h      ; text: sloupec (A) nebo řádek (B)
        ld      bc, 0           ; B = textový řádek, C = sloupec
fv3:    ld      a, (varno)
        or      a
        ld      a, c
        jr      z, fv4
        ld      a, b
fv4:    ld      (hl), a
        inc     hl
        inc     c
        ld      a, c
        cp      40
        jr      c, fv5
        ld      c, 0
        inc     b
fv5:    ld      a, h
        cp      0D8h
        jr      c, fv3
; ---- čtení ----
        xor     a
        ld      (setno), a
set_lp: ld      a, (setno)
        cp      4
        jr      nc, set_vb
        ld      b, a            ; bez synchronizace: prodleva podle čísla sady
        inc     b
set_d:  call    dly1ms
        call    dly1ms
        call    dly1ms
        djnz    set_d
        jr      set_go
set_vb: call    waitvb
set_go: call    measure
        ld      a, (setno)
        inc     a
        ld      (setno), a
        cp      NSET
        jr      c, set_lp
        ld      a, (varno)
        inc     a
        ld      (varno), a
        cp      2
        jp      c, var_lp

; ---- obnovit obrazovku: CG-RAM z CG-ROM (CG-ROM je na 1000h po IN 0E0h) ----
        ld      a, 01h
        out     (P_WF), a
        out     (P_RF), a
        ld      hl, 1000h
        ld      de, 0C000h
        ld      bc, 1000h
        ldir
        in      a, (0E1h)

        xor     a
        ld      (page), a
show:   call    clrscr
        call    prpage
        call    waitrel
        call    waitkey
        ld      a, (page)
        inc     a
        and     3
        ld      (page), a
        jr      show

; ---- measure: jedna sada čtení do bloku (varno, setno). Ničí vše kromě SP. ----
measure:
        ld      a, (varno)      ; DE = 6000h + (varno * 8 + setno) * 32
        add     a, a
        add     a, a
        add     a, a
        ld      hl, setno
        add     a, (hl)
        call    blkaddr
        push    ix
        pop     de
        ld      ix, addrtab
        ld      b, NADDR
ms_a:   ld      l, (ix + 0)
        ld      h, (ix + 1)
        push    bc
        ld      iy, rftab
        ld      b, NRF
ms_r:   ld      a, (iy + 0)
        out     (P_RF), a
        ld      a, (hl)
        ld      (de), a
        ld      a, 01h
        out     (P_RF), a
        inc     de
        inc     iy
        djnz    ms_r
        pop     bc
        inc     ix
        inc     ix
        djnz    ms_a
        ret

; ---- blkaddr: IX = 6000h + A * 32. Ničí AF, DE. ----
blkaddr:
        ld      e, a
        ld      d, 0
        ld      ix, 0
        add     ix, de
        add     ix, ix
        add     ix, ix
        add     ix, ix
        add     ix, ix
        add     ix, ix
        ld      de, RESB
        add     ix, de
        ret

; ---- prpage: vypíše stránku (page): bit 1 = varianta, bit 0 = 0 sady 0 až 3, 1 sady 4 a 5 ----
prpage: xor     a
        call    setrow
        ld      hl, s_title
        call    puts
        ld      a, 1
        call    setrow
        ld      a, (page)
        add     a, a            ; 4 ukazatele po 2 bajtech
        ld      e, a
        ld      d, 0
        ld      hl, pgtab
        add     hl, de
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a
        call    puts
        ld      a, 2
        call    setrow
        ld      hl, s_head
        call    puts
        ld      c, 0            ; C = index RF
pp_r:   ld      a, c
        add     a, 3
        call    setrow
        ld      hl, rftab
        ld      b, 0
        add     hl, bc
        ld      a, (hl)
        call    prhex
        ld      a, ' '
        call    pchr
        ld      b, 0            ; B = index adresy
pp_a:   push    bc
        ld      a, ' '
        call    pchr
        ld      a, b            ; E = adresa * NRF + RF
        add     a, a
        add     a, b
        add     a, a
        add     a, a
        add     a, c
        ld      (offs), a
        ld      a, (page)       ; první sada a počet sad
        and     1
        ld      b, 4
        ld      c, 0
        jr      z, pp_s
        ld      b, 2
        ld      c, 4
pp_s:   ld      a, (page)       ; blok = varianta * 8 + sada
        and     2
        add     a, a
        add     a, a
        add     a, c
        push    bc
        call    blkaddr
        ld      a, (offs)
        ld      e, a
        ld      d, 0
        add     ix, de
        ld      a, (ix + 0)
        call    prhex
        ld      a, ' '
        call    pchr
        pop     bc
        inc     c
        djnz    pp_s
        pop     bc
        inc     b
        ld      a, b
        cp      NADDR
        jr      c, pp_a
        inc     c
        ld      a, c
        cp      NRF
        jr      c, pp_r
        ld      a, 16
        call    setrow
        ld      hl, s_l1
        call    puts
        ld      a, 17
        call    setrow
        ld      hl, s_l2
        call    puts
        ld      a, 18
        call    setrow
        ld      hl, s_l3
        call    puts
        ld      a, 24
        call    setrow
        ld      hl, s_key
        jp      puts

waitvb: in      a, (P_DMD)
        bit     6, a
        jr      z, waitvb
wvb1:   in      a, (P_DMD)
        bit     6, a
        jr      nz, wvb1
        ret

; ---- dly1ms: asi 1 ms (3546 T). Ničí AF. ----
dly1ms: push    bc
        ld      b, 0
d1:     djnz    d1              ; 256 x 13 T
        ld      b, 16
d2:     djnz    d2
        pop     bc
        ret

clrscr: ld      hl, 0D000h
        ld      de, 0D001h
        ld      bc, 07FFh
        ld      (hl), 0
        ldir
        ld      hl, 0D800h
        ld      de, 0D801h
        ld      bc, 07FFh
        ld      (hl), 71h
        ldir
        ret

; ---- waitkey: počká na stisk libovolné klávesy (řádky 0 až 9). Ničí AF, B. ----
waitkey:
        call    anykey
        jr      z, waitkey
        ret

; ---- waitrel: počká, až není stisknutá žádná klávesa, a pak asi 20 ms. Ničí AF, B. ----
waitrel:
        call    anykey
        jr      nz, waitrel
        ld      b, 20
wr1:    call    dly1ms
        djnz    wr1
        ret

; ---- anykey: NZ, když je stisknutá některá klávesa. Ničí AF, B. ----
anykey: ld      b, 0F0h
ak1:    ld      a, b
        ld      (KEYPA), a
        ld      a, (KEYPB)
        cp      0FFh
        ret     nz
        inc     b
        ld      a, b
        cp      0FAh
        jr      c, ak1
        xor     a               ; Z: nic stisknuto
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

; ---- puts: text ASCII od HL (končí 00h) na kurzor. Ničí AF, HL. ----
puts:   ld      a, (hl)
        or      a
        ret     z
        call    pchr
        inc     hl
        jr      puts

; ---- prhex: A jako dvě šestnáctkové číslice. Ničí AF. ----
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

; ---- pchr: znak ASCII 20h až 5Fh na kurzor (převod na display kód tabulkou). Ničí AF. ----
pchr:   push    hl
        sub     20h
        ld      hl, asc2dc
        add     a, l
        ld      l, a
        jr      nc, pc1
        inc     h
pc1:    ld      a, (hl)
        pop     hl

; ---- putc: display kód A na kurzor, kurzor o 1 dál. Zachovává vše kromě kurzoru. ----
putc:   push    hl
        ld      hl, (curs)
        ld      (hl), a
        inc     hl
        ld      (curs), hl
        pop     hl
        ret

; ASCII 20h až 5Fh -> display kód (báze, conventions/sharpmz-ascii.md, tabulka ROM ADCN)
asc2dc: db      00h, 61h, 62h, 63h, 64h, 65h, 66h, 67h, 68h, 69h, 6Bh, 6Ah, 2Fh, 2Ah, 2Eh, 2Dh
        db      20h, 21h, 22h, 23h, 24h, 25h, 26h, 27h, 28h, 29h, 4Fh, 2Ch, 51h, 2Bh, 57h, 49h
        db      55h, 01h, 02h, 03h, 04h, 05h, 06h, 07h, 08h, 09h, 0Ah, 0Bh, 0Ch, 0Dh, 0Eh, 0Fh
        db      10h, 11h, 12h, 13h, 14h, 15h, 16h, 17h, 18h, 19h, 1Ah, 52h, 59h, 54h, 50h, 45h


addrtab: dw     0C0FFh, 0C0AAh
rftab:  db      01h, 04h, 02h, 08h, 0Ah, 03h, 0Fh, 80h, 81h, 82h, 84h, 88h
pgtab:  dw      s_pg1, s_pg2, s_pg3, s_pg4

s_title: db     "WF700 MEAS2 - RF IN MZ-700 MODE", 0
s_pg1:  db      "1/4 TEXT = COLUMN, ANY BEAM POSITION", 0
s_pg2:  db      "2/4 TEXT = COLUMN, VERTICAL BLANKING", 0
s_pg3:  db      "3/4 TEXT = ROW, ANY BEAM POSITION", 0
s_pg4:  db      "4/4 TEXT = ROW, VERTICAL BLANKING", 0
s_head: db      "RF  C0FF READS    C0AA READS", 0
s_l1:   db      "RF 01 MUST GIVE FF AND AA", 0
s_l2:   db      "RF 04 MUST GIVE 7F AND 2A", 0
s_l3:   db      "ONE COLUMN PER REPEAT", 0
s_key:  db      "ANY KEY: NEXT PAGE     RESET: EXIT", 0

curs:   dw      0D000h          ; adresa kurzoru ve VRAM
varno:  db      0               ; varianta obsahu: 0 = A (sloupec), 1 = B (řádek)
setno:  db      0               ; sada čtení 0 až 5
page:   db      0               ; zobrazená stránka 0 až 3
offs:   db      0               ; offset čtení v bloku při výpisu
