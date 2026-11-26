# IM2 VECTOR: co procesor MZ-800 přečte jako vektor v IM 2

`im2_vector.mzf` (zdroj `im2_vector.asm`, pasmo; zavádí se a startuje na 2000h) zjistí,
jaký vektor procesor přečte při přerušení v IM 2. Tabulka IM 2 (257 bajtů od 5000h) je
de Bruijnova posloupnost nad bajty 60h až 6Fh (`gen_tables.py` vyrobí `im2_tables.inc`),
takže každý z 256 vektorů vede na jinou adresu a obsluha pozná, který vektor přišel.
Program přeruší devět různých instrukcí (přerušení z CTC2, u testu 0 ze Z80 PIO) a pro
každou sbírá histogram vektorů a hodnot registru A. Popis testů, paměti a obrazovky je
v hlavičce zdroje. `vram_common.inc` jsou pomocné rutiny výpisu na obrazovku.

Překlad: `python gen_tables.py im2_tables.inc`, `pasmo --bin im2_vector.asm im2_vector.bin`,
MZF s hlavičkou "MZ800 IM2 VECTOR", zavedení i start 2000h.

## Naměřeno 2026-11-26

| Test | Přerušený kód | Skutečné MZ-800 | mz800new před opravou (`a4ea7c43`) |
|------|---------------|-----------------|----------------|
| 0 | vektor dodá Z80 PIO | 5Ah | 5Ah |
| 1 | `JP 4D33h` | 4Dh | 4Ch (bit 0 maskován) |
| 2 | `LD A,(3001h)`, data 9Bh | 9Bh | 9Ah |
| 3 | `HALT`, za ním `SCF` (37h) | 37h | 76h |
| 4 | `LD (HL),A`, A = 2Ch | 2Ch | 76h |
| 5 | `PUSH DE`, E = 4Bh | 4Bh | D4h |
| 6 | `OUT (0A1h),A`, A = 2Ch | 2Ch | A0h |
| 7 | `IN A,(0A0h)`, volný port | A0h, A = A0h | A0h, A = A0h |
| 8 | `IN A,(0FFh)`, PIO B Mode 0 s výstupem A9h | A9h, A = A9h | FEh, A = A9h |

Závěr: procesor bit 0 vektoru nemaskuje a vektor bez PIO je poslední bajt, který šel po
datové sběrnici, ať šlo o čtení nebo zápis paměti nebo I/O; během `HALT` je to bajt
za instrukcí `HALT`. Sloupec EM na obrazovce programu ukazuje předpověď emulátoru před
opravou (bez masky), ne tyto hodnoty.

Hodnoty skutečného MZ-800 kontroluje automaticky `tests/mcp/test_im2_vector.py`.
