# Screenshoty

PNG obrázky obrazovky emulátoru na všech platformách (MZ-800, MZ-700 PAL
a NTSC, MZ-1500). Menu **Tools -> Screenshot**:

| Položka | Klávesa | Co dělá |
|---------|---------|---------|
| Save Screenshot | `Alt + F` | Uloží PNG obrazu zobrazeného v okně do výstupní složky se jménem `<platforma>_RRRRMMDD_HHMMSS.png` (např. `mz800_20261013_183005.png`). Notifikace ukáže rozměr a cestu. |
| Save Screenshot As... | | Totéž, ale zeptá se na jméno souboru. |
| Record Changed Frames | `Alt + Shift + F` | Spustí / ukončí sérii snímků (viz níže). |
| Screenshot Control... | `Alt + G` | Zobrazí / skryje okno Screenshot Control (viz níže). |
| Settings... | | Oblast, měřítko a výstupní složka (viz níže). |
| Open Output Folder | | Otevře výstupní složku ve správci souborů. |

Screenshot je přesně ten obraz, který okno v tu chvíli zobrazuje, bez
scanlines CRT (menu **Display -> CRT Scanlines**), které jen ztmavují obraz
v okně.

**Screenshot Control** je plovoucí okno, které může zůstat otevřené i při
hraní: ukazuje oblast, měřítko, výstupní složku, poslední screenshot
(soubor a rozměr, nebo chybu) a sérii snímků (za běhu dosavadní soubory
a snímky, po konci výsledek a její složku). Jeho tlačítka uloží
screenshot, uloží ho pod jménem, spustí nebo ukončí sérii snímků a otevřou
výstupní složku a nastavení; klávesy jsou vypsané vedle tlačítek. Okno,
které bylo otevřené, se po novém spuštění emulátoru otevře znovu.

## Nastavení

Menu **Tools -> Screenshot -> Settings...**, ukládá se do konfigurace
(sekce `[SCREENSHOT]`):

| Nastavení | Hodnoty | Klíč v INI |
|-----------|---------|------------|
| Oblast | Celý obraz s borderem / Obraz bez borderu | `area` = `full` / `canvas` |
| Měřítko | Nativní / 1x / 2x / 3x / 4x | `scale` = `native` / `1` .. `4` (výchozí `2`) |
| Výstupní složka | prázdná = složka `screenshots` vedle emulátoru | `output_dir` |

- **Nativní** - jeden pixel obrázku na bod počítače: obraz MZ-800 má
  320x200 (v režimech 640 bodů 640x200), celý obraz s borderem 464x288
  (928x288). Hodí se pro porovnání s grafickými daty.
- **1x až 4x** - poměr stran jako v okně emulátoru; **2x** odpovídá oknu
  na 100 % (MZ-800 celý obraz 928x576, obraz bez borderu 640x400). 1x
  v režimech 640 bodů vynechá každý druhý sloupec.

Dialog ukazuje výsledný rozměr obrázku.

## Série snímků

Pro hledání chyb v animacích: **Record Changed Frames** (`Alt + Shift + F`)
ukládá PNG každého emulovaného snímku, který se liší od předchozího, dokud
ji znovu nestisknete. Oblast a měřítko jsou ty z nastavení.

- Snímky se berou v emulovaném čase, takže se ani při MAX SPEED žádný
  nevynechá; když disk nestíhá, emulace se zpomalí.
- Soubory jdou do nové složky `<platforma>_frames_RRRRMMDD_HHMMSS` ve
  výstupní složce: `frame_NNNNNNN.png`, kde NNNNNNN je emulovaný snímek od
  startu (mezery = snímky, které se nezměnily), a `index.csv`.
- Během série je v levém horním rohu obrazu počítadlo `FRAMES soubory /
  snímky`; po skončení notifikace ukáže složku.
- Settings... -> **Frame series: only changed frames** (klíč v INI
  `series_only_changed`, výchozí zapnuto). Nezaškrtnuto = každý snímek.

## MCP

AI klienti ukládají screenshoty nástrojem `emu_screenshot_save_to_file`
(stejná oblast a měřítko, navíc ořez) a série snímků nástroji
`emu_screenshot_series_start` / `_stop` / `_status`, viz
[mcp-server/headless-mode.md](mcp-server/headless-mode.md) a MCP dokument
`emulator://docs/screenshot_workflow`.
