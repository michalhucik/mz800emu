# Memory Map - per-arch banking + MemExt vizualizace

Okno **Memory Map** debuggeru zobrazuje aktuální banking stav 64K Z80
adresního prostoru a paralelní MemExt mapu. Je per-platform (MZ-800,
MZ-700, MZ-1500). Levý a pravý klik nad sloupcem Banking přepíná stav
banking flagů, MZ-800 navíc dostává roletku pro přepínání DMD modu (MZ-700
emulační mode vs MZ-800 native + jeho 8 video sub-modů). Sloupec MExt je
nezávislá vizualizace MemExt mapy s tlačítkem na otevření "MemExt Map
Settings" konfiguračního okna.

## Účel

- Vizualizovat co je aktuálně namapované v Z80 adresním prostoru
  (ROM / RAM / CG-ROM / CG-RAM / VRAM / MMIO / Prohibited shadow), aniž
  by uživatel musel manuálně studovat I/O porty a interpretovat banking
  flagy.
- Detekovat banking změny "v živém stavu" - přepínání ROM ($0000 / $E000),
  CG-ROM/VRAM, MZ-1500 SPEC oblasti, vstup do Prohibited modu.
- Zobrazit MemExt mapu (Luftner / PEHU) paralelně k Sharp banking, aby
  bylo vidět "kdyby na téhle 4K stránce byla RAM, jaká bude memext bank".
- Umožnit ručně přepínat banking + DMD mode přes UI klik pro debugging
  scénář "co se stane, když...".

## Architektura zobrazení

Každý řádek tabulky = 4 KB stránka adresního prostoru ($0000..$F000),
celkem 16 řádků. Render čte aktuální stav banking flagů, DMD registru a
MemExt mapy a vykreslí 4 sloupce. Pro MZ-800 je nad tabulkou navíc
roletka DMD modu. Změny se aplikují v reakci na user input (klik), nikoli
periodicky.

## Layout okna

```
+------------------------------------+
| <Roletka DMD mode>  (jen MZ-800)   |
+------+---+---------+---------------+
| Addr |   | Banking |     MExt      |
+------+---+---------+---------------+
| $0000| O | ROM     |    [$00]      |
| $1000|   | CG-ROM  |    [$01]      |
| $2000|   | RAM     |    [$02]      |
| $3000|   | RAM     |    [$03]      |
| $4000|   | RAM     |    [$04]      |
| $5000|   | RAM     |    [$05]      |
| $6000|   | RAM     |    [$06]      |
| $7000|   | RAM     |    [$07]      |
| $8000| O | VRAM I  |    [$08]      |
| $9000| O | VRAM I  |    [$09]      |
| $A000|   | RAM     |    [$0A]      |
| $B000|   | RAM     |    [$0B]      |
| $C000|   | RAM     |    [$0C]      |
| $D000|   | RAM     |    [$0D]      |
| $E000| O | ROM     |    [$0E]      |
| $F000| O | ROM     |    [$0F]      |
+------+---+---------+---------------+
```

Popis 4 sloupců:

| # | Sloupec | Obsah |
|---|---------|-------|
| 1 | **Addr** | hex labely $0000..$F000 (16 řádků á 4 kB) |
| 2 | **Marker** | úzký prefix sub-sloupec; obsahuje bílé kolečko pokud je řádek klikatelný; jinak prázdný (sdílí pozadí s Banking) |
| 3 | **Banking** | text + barva regionu, pevná šířka pro stabilitu okna při toggle |
| 4 | **MExt** | tlačítko `$XX` (Luftner: 4 kB raw bank vč. bit 7 FLASH; PEHU: text na sudém řádku, lichý prázdný); odpojený MemExt = plain "--", inertní |

Marker a Banking sdílí jednu řádkovou buňku (dvojsloupec). Klik kdekoliv
v této buňce - tj. nad markerem nebo nad Banking textem - přepíná stav
té oblasti (rotace, viz [Levý klik](#levy-klik-rotace)). RMB klik nad
celým dvojsloupcem otevírá popup (i nad neklikatelnou buňkou, i nad
prázdným markerem).

## Roletka DMD modu (jen MZ-800)

Nad tabulkou je `Combo` widget s 9 položkami. Mapuje aktuální hodnotu DMD
registru na položku a klikem na položku zapisuje vybranou hodnotu zpět.

| # | Label | DMD value |
|---|-------|-----------|
| 1 | MZ-700 | 0x08 |
| 2 | MZ-800 320x200 @ 4 / A | 0x00 |
| 3 | MZ-800 320x200 @ 4 / B | 0x01 |
| 4 | MZ-800 320x200 @ 16 | 0x02 |
| 5 | MZ-800 320x200 @ 16 / U | 0x03 |
| 6 | MZ-800 640x200 @ 2 / A | 0x04 |
| 7 | MZ-800 640x200 @ 2 / B | 0x05 |
| 8 | MZ-800 640x200 @ 4 | 0x06 |
| 9 | MZ-800 640x200 @ 4 / U | 0x07 |

Sufix `/ U` = nedokumentovaná kombinace bitů 1-0 = 11 (per
`hw/09a-undoc-dmd-modes.md`).

Mapování DMD -> položka při čtení:

- bit 3 = 1 -> "MZ-700" (bity 0-2 ignorovány)
- bit 3 = 0 -> položka podle bitů 2-0

Klik na položku = zápis do DMD registru se stejným účinkem jako OUT (CEh)
programu (včetně vazby časovače CTC0 na režim 700/800), jen se nezapíše do
trace logu a nespustí breakpoint na změnu módu. "MZ-700" zapisuje 0x08
(bity 0-2 nezachovává).

## Per-platform region mapy

### MZ-800 native (DMD bit 3 = 0)

| Adresa | Region | Flag |
|--------|--------|------|
| $0000-$0FFF | ROM low / RAM | `ROM_0000` |
| $1000-$1FFF | CG-ROM / RAM | sdílí `CGRAM_VRAM` s $8000+ |
| $2000-$7FFF | vždy RAM | - |
| $8000-$9FFF | VRAM I / RAM | `CGRAM_VRAM` |
| $A000-$BFFF | jen v 640x200 (DMD bit 2=1) VRAM II / RAM, jinak vždy RAM | `CGRAM_VRAM` (gated DMD) |
| $C000-$DFFF | vždy RAM | - |
| $E000-$FFFF | ROM high / RAM / Prohibited | `ROM_E000` + `PROHIBITED` |

### MZ-800 emul. MZ-700 mode (DMD bit 3 = 1)

| Adresa | Region | Flag |
|--------|--------|------|
| $0000-$0FFF | ROM / RAM | `ROM_0000` |
| $1000-$1FFF | CG-ROM / RAM | sdílí flag s $C000 |
| $2000-$BFFF | vždy RAM | - |
| $C000-$CFFF | CG-RAM / RAM | sdílí flag s $1000 |
| $D000-$DFFF | VRAM (text+attr) / RAM | sdílí `ROM_E000` flag s $E000 |
| $E000-$FFFF | MMIO+ROM / RAM / Prohibited | `ROM_E000` + `PROHIBITED` |

### MZ-700 standalone

| Adresa | Region | Klikatelnost |
|--------|--------|--------------|
| $0000-$0FFF | ROM / RAM | klikatelné |
| $1000-$1FFF | CG-ROM | **read-only** (klik nepřepíná) |
| $2000-$BFFF | vždy RAM | - |
| $C000-$CFFF | CG-RAM | read-only |
| $D000-$DFFF | VRAM | read-only |
| $E000-$FFFF | ROM+MMIO / RAM / Prohibited | klikatelné |

### MZ-1500

| Adresa | Region | Flag |
|--------|--------|------|
| $0000-$0FFF | ROM / RAM | `ROM_0000` |
| $1000-$CFFF | vždy RAM | - |
| $D000-$EFFF | SPEC oblast - rotace mezi NONE / CGROM / PCG1 / PCG2 / PCG3 | SPEC maska v map |
| $F000-$FFFF | ROM_UPPER (= ROM E800) / RAM | `ROM_UPPER` |

### Prohibited mode (MZ-800 + MZ-700)

- Aktivuje se OUT 0E5h, čte se 0x1A shadow z $E009-$FFFF, $E000-$E008
  zůstávají MMIO funkční
- Persistuje přes E0/E1/E2/E3/E4 + DMD bit 3 switch
- Reset: OUT 0E6h, GDG reset
- UI značí červenou barvou

## Interakce

### Levý klik (rotace)

Klik (na marker nebo Banking content buňku) **klikatelného** řádku rotuje
stav té oblasti.

**Důležitý HW invariant (MZ-800):** CG-ROM ($1000) a VRAM ($8000+) jsou
v 800 modu **vždy synchronní** - nelze mít jen jedno. Implementace
toggluje oba bity `ROM_1000` + `CGRAM_VRAM` naráz (per `hw/03-banking.md`).

Klikatelnost je **DMD-aware** na MZ-800. Marker bílé kolečko se objeví /
zmizí dynamicky podle aktuálního DMD modu (např. řádek $A000 je klikatelný
jen v 640x200 modu).

### Pravý klik (popup menu)

Pravý klik kdekoliv ve sloupci Banking (i nad neklikatelnou buňkou, i nad
marker buňkou) otevře globální popup s legitimními možnostmi pro daný
režim:

**MZ-800:**

- ROM $0000 -> Mount / Umount
- CG-ROM $1000 -> Mount / Umount (= alias pro CG-RAM/VRAM)
- CG-RAM/VRAM -> Mount / Umount
- ROM $E000 -> Mount / Umount / Inhibit (= aktivovat Prohibited)
- ----
- Mount All / Umount All

**MZ-700:**

- ROM $0000 -> Mount / Umount
- ROM $E000 -> Mount / Umount / Inhibit
- ----
- Mount All / Umount All

**MZ-1500:**

- ROM $0000 -> Mount / Umount
- Upper area -> Mount / Umount
- D000 SPEC -> radio: NONE / CGROM / PCG1 / PCG2 / PCG3
- ----
- Mount All / Umount All

Sémantika **Mount All** = nastavit banking tak, aby všechny
ROM / CG-ROM / VRAM byly připojené. **Umount All** = vše odpojeno (jen RAM).

### Header tooltip

Hover nad headerem "Banking" zobrazí tooltip "Left: rotate, Right: menu".

## Sloupec MExt

3 stavy podle typu připojeného MemExt:

| Stav | Render |
|------|--------|
| Odpojený | plain `--`, inertní (žádný klik, žádný hover) |
| Luftner (4K) | 16 buněk, každá tlačítko `$XX` (raw bank včetně bit 7 = FLASH/SRAM rozlišení) |
| PEHU (8K) | 8 dvojbuněk, sudý řádek tlačítko `$XX`, lichý řádek prázdný |

Klik na tlačítko = otevřít "MemExt Map Settings" okno (identické s
odpovídající položkou v Devices menu).

Tooltip nad tlačítkem: "MemExt remap...".

**Důležité:** sloupec MExt je **nezávislý na sloupci Banking** - ukazuje
vždy aktuální MemExt mapu bez ohledu na to, zda Sharp banking pokrývá
danou pozici. Nemá "covered" stav, žádné šedé / proškrtnuté buňky, žádné
barvy. MemExt jen říká "kdyby tady byla RAM, byla by to tahle banka";
co tam reálně je z pohledu Z80, říká sloupec Banking.

## Cross-window navigation

Memory Map okno je přístupné z I/O Ports Overview panelu přes "Show in
Memory Map" položku v right-click context menu pro banking porty
0xE0-0xE6 a MemExt 0xE7. Okno se otevře pokud bylo zavřené a získá OS
focus.
