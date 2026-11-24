# WFTEST: zápis do VRAM v režimu MZ-700 přes registr WF

`wftest.mzf` (zdroj `wftest.asm`, pasmo 0.5.3: `pasmo --bin wftest.asm wftest.bin`, hlavička MZF: OBJ, načtení
a start 2000h) ověří na MZ-800, jestli zápis procesoru do VRAM v režimu MZ-700 (znaky a atributy na D000h) jde
přes registr formátu zápisu WF (port CCh) jako zápis do roviny I. Spustí se z čehokoli: z Unicard Manageru,
z monitoru, v emulátoru (Soubor, načíst MZF, nebo `--run-mzf`). Konec: RESET.

Program přepne do režimu MZ-700 a pro každý režim WF (bity 7-5: 000 SINGLE, 001 XOR, 010 OR, 011 RESET,
10x REPLACE, 11x PSET) s vybranou rovinou I (WF xx1h) i bez ní (jen rovina II, xx2h) jednu buňku obrazovky:

1. přes WF 01h do ní zapíše PR;
2. přes zkoušený WF zapíše NW;
3. přes WF 01h a RF 01h ji přečte (RD).

Na řádku testu je WF, režim, rovina, PR, NW, RD, pak WF = hodnota, když zápis jde přes WF jako do roviny I,
DI = hodnota přímého zápisu, výsledek (OK = jako WF, DIR = jako přímý zápis, ?? = jinak) a dvě buňky: T je
zkoušená buňka sama (zapsaná přes WF), E očekávaná hodnota zapsaná přes WF 01h. T a E musejí na obrazovce
vypadat stejně: tím se kontroluje i obraz (videovýstup MZ-800, VGA karty, která obraz kreslí sama).

## Naměřeno 2026-11-24

| Kde | Výsledek |
|---|---|
| skutečné MZ-800 (spuštěno z Unicard mk3b) | `RESULT: ALL 12 AS WF`, ve všech 12 RD = WF |
| mz800emu 2.0.7 | `RESULT: DIRECT WRITE (OLD EMULATOR)`, 10 rozlišujících případů DIR |
| mz800emu s opravou | `RESULT: ALL 12 AS WF` |

Bez vybrané roviny I tedy SINGLE, XOR, OR a RESET nezapíšou nic, REPLACE zapíše 0 a PSET smaže bity NW, stejně
jako nevybrané roviny v režimu MZ-800. Stejné případy kontroluje automaticky `tests/mcp/test_mz700_vram_wf.py`.

Copyright (C) 2026 Martin Lukášek, GPL-3.0-or-later.
