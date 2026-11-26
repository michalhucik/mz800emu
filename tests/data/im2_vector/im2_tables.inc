; im2_tables.inc - vygenerováno gen_tables.py, needitovat
; im2tab: 257 bajtů tabulky IM 2 (de Bruijn B(16, 2) nad 60h až 6Fh)
; invtab: klíč h * 16 + j -> vektor (h = stránka - 60h, j = dolní bajt - 60h)
im2tab:
        db      060h, 060h, 061h, 060h, 062h, 060h, 063h, 060h, 064h, 060h, 065h, 060h, 066h, 060h, 067h, 060h
        db      068h, 060h, 069h, 060h, 06Ah, 060h, 06Bh, 060h, 06Ch, 060h, 06Dh, 060h, 06Eh, 060h, 06Fh, 061h
        db      061h, 062h, 061h, 063h, 061h, 064h, 061h, 065h, 061h, 066h, 061h, 067h, 061h, 068h, 061h, 069h
        db      061h, 06Ah, 061h, 06Bh, 061h, 06Ch, 061h, 06Dh, 061h, 06Eh, 061h, 06Fh, 062h, 062h, 063h, 062h
        db      064h, 062h, 065h, 062h, 066h, 062h, 067h, 062h, 068h, 062h, 069h, 062h, 06Ah, 062h, 06Bh, 062h
        db      06Ch, 062h, 06Dh, 062h, 06Eh, 062h, 06Fh, 063h, 063h, 064h, 063h, 065h, 063h, 066h, 063h, 067h
        db      063h, 068h, 063h, 069h, 063h, 06Ah, 063h, 06Bh, 063h, 06Ch, 063h, 06Dh, 063h, 06Eh, 063h, 06Fh
        db      064h, 064h, 065h, 064h, 066h, 064h, 067h, 064h, 068h, 064h, 069h, 064h, 06Ah, 064h, 06Bh, 064h
        db      06Ch, 064h, 06Dh, 064h, 06Eh, 064h, 06Fh, 065h, 065h, 066h, 065h, 067h, 065h, 068h, 065h, 069h
        db      065h, 06Ah, 065h, 06Bh, 065h, 06Ch, 065h, 06Dh, 065h, 06Eh, 065h, 06Fh, 066h, 066h, 067h, 066h
        db      068h, 066h, 069h, 066h, 06Ah, 066h, 06Bh, 066h, 06Ch, 066h, 06Dh, 066h, 06Eh, 066h, 06Fh, 067h
        db      067h, 068h, 067h, 069h, 067h, 06Ah, 067h, 06Bh, 067h, 06Ch, 067h, 06Dh, 067h, 06Eh, 067h, 06Fh
        db      068h, 068h, 069h, 068h, 06Ah, 068h, 06Bh, 068h, 06Ch, 068h, 06Dh, 068h, 06Eh, 068h, 06Fh, 069h
        db      069h, 06Ah, 069h, 06Bh, 069h, 06Ch, 069h, 06Dh, 069h, 06Eh, 069h, 06Fh, 06Ah, 06Ah, 06Bh, 06Ah
        db      06Ch, 06Ah, 06Dh, 06Ah, 06Eh, 06Ah, 06Fh, 06Bh, 06Bh, 06Ch, 06Bh, 06Dh, 06Bh, 06Eh, 06Bh, 06Fh
        db      06Ch, 06Ch, 06Dh, 06Ch, 06Eh, 06Ch, 06Fh, 06Dh, 06Dh, 06Eh, 06Dh, 06Fh, 06Eh, 06Eh, 06Fh, 06Fh
        db      060h
invtab:
        db      000h, 002h, 004h, 006h, 008h, 00Ah, 00Ch, 00Eh, 010h, 012h, 014h, 016h, 018h, 01Ah, 01Ch, 0FFh
        db      001h, 01Fh, 021h, 023h, 025h, 027h, 029h, 02Bh, 02Dh, 02Fh, 031h, 033h, 035h, 037h, 039h, 01Eh
        db      003h, 020h, 03Ch, 03Eh, 040h, 042h, 044h, 046h, 048h, 04Ah, 04Ch, 04Eh, 050h, 052h, 054h, 03Bh
        db      005h, 022h, 03Dh, 057h, 059h, 05Bh, 05Dh, 05Fh, 061h, 063h, 065h, 067h, 069h, 06Bh, 06Dh, 056h
        db      007h, 024h, 03Fh, 058h, 070h, 072h, 074h, 076h, 078h, 07Ah, 07Ch, 07Eh, 080h, 082h, 084h, 06Fh
        db      009h, 026h, 041h, 05Ah, 071h, 087h, 089h, 08Bh, 08Dh, 08Fh, 091h, 093h, 095h, 097h, 099h, 086h
        db      00Bh, 028h, 043h, 05Ch, 073h, 088h, 09Ch, 09Eh, 0A0h, 0A2h, 0A4h, 0A6h, 0A8h, 0AAh, 0ACh, 09Bh
        db      00Dh, 02Ah, 045h, 05Eh, 075h, 08Ah, 09Dh, 0AFh, 0B1h, 0B3h, 0B5h, 0B7h, 0B9h, 0BBh, 0BDh, 0AEh
        db      00Fh, 02Ch, 047h, 060h, 077h, 08Ch, 09Fh, 0B0h, 0C0h, 0C2h, 0C4h, 0C6h, 0C8h, 0CAh, 0CCh, 0BFh
        db      011h, 02Eh, 049h, 062h, 079h, 08Eh, 0A1h, 0B2h, 0C1h, 0CFh, 0D1h, 0D3h, 0D5h, 0D7h, 0D9h, 0CEh
        db      013h, 030h, 04Bh, 064h, 07Bh, 090h, 0A3h, 0B4h, 0C3h, 0D0h, 0DCh, 0DEh, 0E0h, 0E2h, 0E4h, 0DBh
        db      015h, 032h, 04Dh, 066h, 07Dh, 092h, 0A5h, 0B6h, 0C5h, 0D2h, 0DDh, 0E7h, 0E9h, 0EBh, 0EDh, 0E6h
        db      017h, 034h, 04Fh, 068h, 07Fh, 094h, 0A7h, 0B8h, 0C7h, 0D4h, 0DFh, 0E8h, 0F0h, 0F2h, 0F4h, 0EFh
        db      019h, 036h, 051h, 06Ah, 081h, 096h, 0A9h, 0BAh, 0C9h, 0D6h, 0E1h, 0EAh, 0F1h, 0F7h, 0F9h, 0F6h
        db      01Bh, 038h, 053h, 06Ch, 083h, 098h, 0ABh, 0BCh, 0CBh, 0D8h, 0E3h, 0ECh, 0F3h, 0F8h, 0FCh, 0FBh
        db      01Dh, 03Ah, 055h, 06Eh, 085h, 09Ah, 0ADh, 0BEh, 0CDh, 0DAh, 0E5h, 0EEh, 0F5h, 0FAh, 0FDh, 0FEh
