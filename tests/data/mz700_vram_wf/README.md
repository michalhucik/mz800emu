# WF700 MEAS1: registry WF a RF v režimu MZ-700 na MZ-800

`wf700_meas1.mzf` (zdroj `wf700_meas1.asm`, pasmo; zavádí se a startuje na 2000h) ověří
na MZ-800, že zápis procesoru do VRAM v režimu MZ-700 (znaky, atributy, CG-RAM) jde přes
registr formátu zápisu WF (port 0xCC) a čtení přes registr RF (port 0xCD). Projde všech
256 hodnot WF a RF; popis zkoušek, tabulek výsledků a obrazovky je v hlavičce zdroje.

`wf700_meas1_tables.asm` jsou očekávané hodnoty: `EXPM` podle modelu netlistu GDG, `MASK`
(1 = hodnota se porovnává), `EXPP` a `EXPD` dvě jiné hypotézy (zápis přes WF jen jako
rovina I a přímé čtení; přímý zápis i čtení).

## Naměřeno 2026-11-25

| Kde | Výsledek |
|---|---|
| skutečné MZ-800 s rozšířenou VRAM | sloupec `M` = `CMP` ve všech 10 tabulkách, žádný rozdíl |
| mz800emu 2.0.7 | sloupec `D` = `CMP` ve všech tabulkách (přímý zápis a čtení) |
| mz800emu s opravou | jako skutečné MZ-800 |

Tytéž tabulky kontroluje automaticky `tests/mcp/test_mz700_vram_wf.py`.

# WF700 MEAS2: RF s rovinou II, IV a SEARCH v režimu MZ-700

`wf700_meas2.mzf` (zdroj `wf700_meas2.asm`; zavádí se a startuje na 2000h) ověří, co čte
procesor v režimu MZ-700 s RF, který vybírá rovinu II nebo IV, a při operaci SEARCH.
Roviny II a IV drží bajt čtvrtého video čtení buňky 39 posledního řádku (X2 ze základní,
X4 z rozšířené VRAM), i ve svislém zatemnění, kde čítač adresy řádku běží dál. Obsah VRAM
volí program tak, aby šel bajt předpovědět (popis v hlavičce zdroje). Čtyři stránky:
text = sloupec nebo řádek, čtení v libovolné poloze paprsku nebo na začátku svislého
zatemnění; přepíná libovolná klávesa.

## Naměřeno 2026-11-25 (MZ-800 s rozšířenou VRAM)

Svislé zatemnění, obě opakování shodná:

| RF | text = sloupec, C0FF | C0AA | text = řádek, C0FF | C0AA |
|---|---|---|---|---|
| 01 | FF | AA | FF | AA |
| 04 | 7F | 2A | 7F | 2A |
| 02 | 7A | 78 | 02 | 08 |
| 08 | FA | F8 | 82 | 89 |
| 0A | 7B | 79 | 03 | 09 |
| 03 | 7B | 2A | 03 | 0A |
| 0F | 7C | 2A | 04 | 0A |
| 80 | 00 | 04 | 00 | 54 |
| 81 | 82 | 80 | FA | A0 |
| 82 | 00 | 54 | 00 | 04 |
| 84 | 00 | 01 | 00 | 51 |
| 88 | 00 | 00 | 00 | 50 |

Čtení bez synchronizace se mění s řádkem. Totéž kontroluje `tests/mcp/test_mz700_rf_latch.py`:
hodnoty musí odpovídat pravidlu z Doxygenu `vramctrl_mz700_read_rf()` a ve svislém zatemnění
bajtu čtvrtého čtení naměřenému na stroji, s tolerancí jednoho řádku (program se
synchronizuje smyčkou, která čte stavový registr asi po 25 taktech).
