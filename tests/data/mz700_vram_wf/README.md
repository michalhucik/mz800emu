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
