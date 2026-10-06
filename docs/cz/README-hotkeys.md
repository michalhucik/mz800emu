# Klávesové zkratky emulátoru MZ-800

Globální klávesové zkratky dostupné napříč celým oknem emulátoru.

## Reset a obecné

| Klávesa                    | Akce                                                                        |
|----------------------------|-----------------------------------------------------------------------------|
| `F12`                      | Reset emulovaného počítače                                                  |
| `F11`                      | Kalibrace joysticku (nebo Reset, pokud emulátor běží pod Windows Debugger)  |

## Okno a zobrazení

| Klávesa                    | Akce                                                                        |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + Ctrl + Enter`       | Přepnutí mezi fullscreen a okenním režimem                                  |
| `Alt + W`                  | Opravit poměr stran okna podle šířky                                        |
| `Alt + H`                  | Opravit poměr stran okna podle výšky                                        |

## Rychlost emulace

| Klávesa                    | Akce                                                                        |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + P`                  | Přepnutí Pauza / Spuštěno                                                   |
| `Alt + N`                  | Nastavit normální rychlost emulace (100 %)                                  |
| `Alt + M`                  | Přepínat mezi Max a (Normal nebo Custom) rychlostí emulace                  |
| `Alt + Shift + M`          | Přepínat mezi Custom a (Normal nebo Max) rychlostí emulace                  |
| `Alt + Up`                 | Zvýšit Custom rychlost (krok 1 % normální rychlosti)                        |
| `Alt + Shift + Up`         | Zvýšit Custom rychlost (krok 10 % normální rychlosti)                       |
| `Alt + Pg_Up`              | Zvýšit Custom rychlost (krok 100 % normální rychlosti)                      |
| `Alt + Down`               | Snížit Custom rychlost (krok 1 % normální rychlosti)                        |
| `Alt + Shift + Down`       | Snížit Custom rychlost (krok 10 % normální rychlosti)                       |
| `Alt + Pg_Down`            | Snížit Custom rychlost (krok 100 % normální rychlosti)                      |
| `Alt + T`                  | MAX SPEED benchmark: vypsat report efektivity emulace na konzoli            |
| `Alt + Shift + T`          | MAX SPEED benchmark: resetovat měření                                       |

## Disketové mechaniky (FDC)

| Klávesa                    | Akce                                                                        |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + [1..4]`             | Připojit FD obraz do mechaniky 1..4 řadiče FDC0 (a aktivovat WD279x)        |
| `Alt + Shift + [1..4]`     | Odpojit FD obraz z mechaniky 1..4 řadiče FDC0                               |

> Pozn.: tyto klávesové zkratky pracují vždy s primárním řadičem **FDC0 (standard)**, porty 0xD8 - 0xDF. Sekundární řadič **FDC1** (porty 0x58 - 0x5F) se obsluhuje přes menu HW & Devices -> FD Controller.

## Magnetofon (CMT) a virtuální klávesnice

| Klávesa                    | Akce                                                                        |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + C`                  | Zobrazit / Skrýt okno virtuálního magnetofonu                               |
| `Alt + K`                  | Zobrazit / Skrýt okno virtuální klávesnice                                  |

## Snapshoty

| Klávesa                    | Akce                                                                        |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + F6`                 | Uložit Snapshot (otevře dialog uložení)                                     |
| `Alt + F7`                 | Načíst Snapshot (otevře dialog načtení)                                     |
| `Alt + F8`                 | Rychlé uložení                                                              |
| `Alt + F9`                 | Rychlé načtení                                                              |

## Nahrávání videa

Všechny platformy. Podrobnosti viz [`video-recording.md`](video-recording.md).

| Klávesa                    | Akce                                                                        |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + O`                  | Zahájit / ukončit nahrávání videa                                           |
| `Alt + Shift + O`          | Pozastavit / obnovit nahrávání (emulace běží dál)                           |
| `Alt + L`                  | Vložit marker do nahrávky                                                   |
| `Alt + Shift + L`          | Zobrazit / skrýt okno dálkového ovládání nahrávání                          |
| `Alt + U`                  | Přepnout časovou základnu nahrávání: emulační čas / podle reality (i během nahrávání) |

## Debugger

Dostupné pouze v buildech s `MZ800EMU_CFG_DEBUGGER_ENABLED`.

Některé klávesy jsou dvouúčelové: bez `Shift` vykonají svou nedebuggerovou
akci (viz výše), se `Shift` přepnou okno debuggeru (např. `Alt + W` opraví
poměr stran, `Alt + Shift + W` přepne okno Watch).

| Klávesa                    | Akce                                                                        |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + D`                  | Zobrazit / Skrýt okno MZ-800 Debuggeru                                      |
| `Alt + Shift + D`          | Zobrazit / Skrýt okno Disassembler (range-based + export)                   |
| `Alt + B`                  | Zobrazit / Skrýt okno Breakpoints                                           |
| `Alt + Shift + B`          | Zobrazit / Skrýt okno Bookmarks (pojmenované adresové záložky)              |
| `Alt + V`                  | Zobrazit / Skrýt okno Variables                                             |
| `Alt + I`                  | Zobrazit / Skrýt okno I/O Ports                                             |
| `Alt + E`                  | Zobrazit / Skrýt okno Memory Browser                                        |
| `Alt + Y`                  | Zobrazit / Skrýt okno Symbols (NoICE / sdldz80 `.map` / sjasmplus `.sym`)   |
| `Alt + Shift + R`          | Zobrazit / Skrýt okno CPU Registers                                         |
| `Alt + S`                  | Zobrazit / Skrýt okno Stack Monitor                                         |
| `Alt + Shift + S`          | Zobrazit / Skrýt okno Stack Regions                                         |
| `Alt + Shift + H`          | Zobrazit / Skrýt okno Stack History                                         |
| `Alt + Shift + W`          | Zobrazit / Skrýt okno Watch (uživatelské paměťové hlídky)                   |
| `Alt + Shift + P`          | Zobrazit / Skrýt okno CPU Profiler                                          |

## Okna stavu čipů (hardware)

Okna stavu jednotlivých čipů ("F1" panely). Dostupné pouze v buildech
s `MZ800EMU_CFG_DEBUGGER_ENABLED`.

| Klávesa                    | Akce                                                                        |
|----------------------------|-----------------------------------------------------------------------------|
| `Alt + Shift + I`          | Zobrazit / Skrýt okno PPI 8255 State                                        |
| `Alt + Shift + C`          | Zobrazit / Skrýt okno CTC 8253 State                                        |
| `Alt + Shift + V`          | Zobrazit / Skrýt okno GDG State                                             |
| `Alt + Shift + Z`          | Zobrazit / Skrýt okno Z80 PIO State (jen MZ-800 / MZ-1500)                  |
| `Alt + Shift + G`          | Zobrazit / Skrýt okno PSG State (jen MZ-800 / MZ-1500)                      |
| `Alt + Shift + A`          | Zobrazit / Skrýt okno PSG Audio Scope (jen MZ-800 / MZ-1500)               |
