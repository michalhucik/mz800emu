# MZ-800 Emulator

Projekt byl přesunut ze SourceForge na GitHub. Nejnovější zdrojový kód
a release verze jsou nyní dostupné na <https://github.com/michalhucik/mz800emu>.

Starší verze 1.0.x zůstávají archivované na původní SourceForge
stránce: <https://sourceforge.net/projects/mz800emu/>


## Windows

Pokud chcete na platformě Windows mít vždy zobrazenou konzoli, máte tři možnosti:

- Spustit program z MSYS2 konzole
- Vytvořit zástupce pro `mz800emu.exe` a přidat parametr `--console`
- Zkompilovat program s parametrem `FORCE_CONSOLE=1`

Volby `--help` a `--version` vypíšou text i do konzole, ze které byl program
spuštěn (cmd.exe, PowerShell) - bez `--console`. cmd.exe na program s oknem
nečeká, takže výpis se může objevit až za promptem; čistší je přesměrování do
souboru, např. `mz800emu --version > version.txt`.


## Mapování klávesnice

Většina kláves je na stejných pozicích jako na MZ-800 - podobně jako v emulátoru od Zdeňka Adlera.

### Zvláštnosti a rozdíly

| Klávesa Sharp | Klávesa PC              |
|---------------|-------------------------|
| `GRAPH`       | CapsLock                |
| `ALPHA`       | `\` (obě klávesy, viz níže) |
| `BLANK_KEY`   | `~`                     |
| `ESC`         | Esc, nebo End           |
| `INST`        | Insert                  |
| `DEL`         | Backspace, nebo Delete  |
| `@`           | F6                      |
| `\`           | F7                      |
| `?`           | F8                      |
| `LIBRA`       | F9                      |

**Pozn. k `ALPHA`:** Na klávesnicích s českým (a obecně ISO) rozložením
existují dvě fyzické klávesy, které generují `\`: standardní Backslash a ISO
klávesa u levého Shiftu (PC ji hlásí jako "Oem102" / Non-US Backslash). Jako
`ALPHA` fungují obě - není potřeba řešit, kterou z nich zrovna máte.


### Rozdíly mezi modely (MZ-800 vs MZ-700 / MZ-1500)

Klávesnice MZ-700 a MZ-1500 se od MZ-800 liší: nemají klávesu TAB. Na jejím
místě je delší klávesa ALPHA (stejné šířky jako TAB na MZ-800), díky čemuž
má řada se SHIFTy o jednu klávesu méně a levý SHIFT je stejně velký jako
pravý. Hostitelská klávesa Tab se proto na MZ-700 a MZ-1500 chová stejně
jako ALPHA.

| Klávesa Sharp | MZ-800        | MZ-700 / MZ-1500          |
|---------------|---------------|---------------------------|
| `ALPHA`       | PC `\`        | PC `\` nebo PC Tab        |


## Volby z příkazové řádky

Emulátor podporuje malou sadu long options. Volby, které nejsou uvedené níže,
jsou odmítnuty s chybou - pro vygenerovaný výpis použijte `--help`.

| Volba | Argument | Popis |
|-------|----------|-------|
| `--help` | - | Vypsat seznam options a skončit. |
| `--version` | - | Vypsat verzi, revizi, datum buildu, původ zdrojů (větev, commit, neuložené změny, pokud jsou k dispozici), překladač, zakompilované části, verze SDL a GLib a hostitelský OS, pak skončit. Funguje ve všech binárkách (`mz800emu`, `mz700emu-pal`, `mz700emu-ntsc`, `mz1500emu`) i s `--mcp-pipe`; emulátor ani okno se nespouští. |
| `--console` | - | Pouze Windows: alokovat okno konzole pro stdout/stderr. |
| `--run-mzf` | `<filepath>` | Po startu emulátoru automaticky načíst a spustit zadaný MZF soubor. |
| `--cdl-mode` | `<off\|window\|always>` | Nastavit režim CDL (Memory Heatmap) recordingu. |
| `--cdl-dir` | `<dirpath>` | Nastavit cílový adresář pro CDL export. Adresář se při exportu vytvoří, pokud neexistuje. |
| `--cdl-name` | `<basename>` | Nastavit basename pro CDL export (bez `.json`). Plná cesta meta souboru je `<dir>/<name>.json`; per-region binární soubory se ukládají jako `<dir>/<name>_<region>.cdl`. |
| `--cdl-save-on-exit` | `<on\|off>` | Zapnout/vypnout automatický export CDL při ukončení emulátoru. |
| `--cputrack-mode` | `<off\|window\|always>` | Nastavit režim CPU Tracking Log recordingu (jeden ze čtyř trace-suite subsystémů). |
| `--cputrack-dir` | `<dirpath>` | Cílový adresář pro cputrack export. |
| `--cputrack-name` | `<basename>` | Basename pro cputrack export soubory. |
| `--cputrack-chunk-mb` | `<N>` | Velikost RAM chunku před disk swap (default 64). |
| `--cputrack-max-total-mb` | `<N>` | Max celková velikost recordingu v MB, 0 = unlimited (default 2048 = 2 GB). |
| `--cputrack-save-on-exit` | `<on\|off>` | Auto-finalize cputrack exportu při ukončení emulátoru. |
| `--iorqlog-mode` | `<off\|window\|always>` | Nastavit režim IORQ Log recordingu. |
| `--iorqlog-dir` | `<dirpath>` | Cílový adresář pro iorqlog export. |
| `--iorqlog-name` | `<basename>` | Basename pro iorqlog export soubory. |
| `--iorqlog-chunk-mb` | `<N>` | Velikost RAM chunku před disk swap (default 64). |
| `--iorqlog-max-total-mb` | `<N>` | Max celková velikost recordingu v MB, 0 = unlimited (default 2048 = 2 GB). |
| `--iorqlog-save-on-exit` | `<on\|off>` | Auto-finalize iorqlog exportu při ukončení emulátoru. |
| `--intlog-mode` | `<off\|window\|always>` | Nastavit režim Interrupt Log recordingu. |
| `--intlog-dir` | `<dirpath>` | Cílový adresář pro intlog export. |
| `--intlog-name` | `<basename>` | Basename pro intlog export soubory. |
| `--intlog-chunk-mb` | `<N>` | Velikost RAM chunku před disk swap (default 64). |
| `--intlog-max-total-mb` | `<N>` | Max celková velikost recordingu v MB, 0 = unlimited (default 2048 = 2 GB). |
| `--intlog-save-on-exit` | `<on\|off>` | Auto-finalize intlog exportu při ukončení emulátoru. |
| `--hwlog-mode` | `<off\|window\|always>` | Nastavit režim HW Log recordingu. |
| `--hwlog-dir` | `<dirpath>` | Cílový adresář pro hwlog export. |
| `--hwlog-name` | `<basename>` | Basename pro hwlog export soubory. |
| `--hwlog-chunk-mb` | `<N>` | Velikost RAM chunku před disk swap (default 64). |
| `--hwlog-max-total-mb` | `<N>` | Max celková velikost recordingu v MB, 0 = unlimited (default 2048 = 2 GB). |
| `--hwlog-save-on-exit` | `<on\|off>` | Auto-finalize hwlog exportu při ukončení emulátoru. |
| `--hwlog-hs-decimation` | `<N>` | Decimace hwlog `GDG_VIDEO` HS/HBLN edges: emit každý N-tý edge. `0` = OFF (default; full rate je ~31000 events/sec). |
| `--all-traces-mode` | `<off\|window\|always>` | Shorthand: nastavit režim recordingu pro VŠECHNY čtyři trace-suite subsystémy (cputrack, iorqlog, intlog, hwlog) najednou. Per-subsystém `--<sys>-mode` má precedenci. |
| `--all-traces-dir` | `<dirpath>` | Shorthand: nastavit cílový adresář pro VŠECHNY čtyři trace-suite subsystémy. Per-subsystém `--<sys>-dir` má precedenci. |
| `--no-save-ini` | - | Nezapisovat `.ini` soubor při ukončení. CLI override pak platí jen pro aktuální session. |
| `--no-first-run-windows` | - | Potlačit automatické otevření oken About + Version Check Setup při prvním spuštění (kdy neexistuje `.ini` soubor). Užitečné pro headless / scriptované spouštění. |
| `--headless` | - | Spustit emulátor bez GUI okna a bez audio výstupu. SDL3 video a audio subsystémy běží v no-op módu (žádné SDL okno, žádný audio device se neotevírá). Framebuffer se stále renderuje do paměti (= připraveno pro pozdější MCP frame Resources). Určeno pro CI / batch / subprocess scénáře bez displeje nebo audio zařízení. Proces běží do SIGINT (Ctrl+C) nebo SDL quit eventu. Doporučeno kombinovat s `--no-first-run-windows`. |
| `--maxspeed-bench` | - | Spustit rovnou v MAX SPEED a periodicky (každých 5 s) vypisovat report MAX SPEED benchmarku na konzoli (efektivita %, throughput, FB-FPS, distribuce). Určeno pro headless měření efektivity emulace. Kombinujte s `--headless` a `--run-mzf`. Viz [`maxspeed-benchmark.md`](maxspeed-benchmark.md). |
| `--speed` | `<procenta\|max>` | Nastavit rychlost emulace při startu: `max` = MAX SPEED (bez synchronizace na reálný čas), celé číslo 1-4000 = vlastní rychlost v procentech (100 = normální). Funguje v okně i v `--headless` (kde jinak emulace běží v reálném čase). Neplatná hodnota = chyba a konec. Při souběhu s `--maxspeed-bench` vyhrává MAX SPEED. |
| `--mode-switch` | `<700\|800>` | Jen MZ-800: poloha zadního přepínače režimu (SW1) - `700` = MZ-700 mód, `800` = MZ-800 mód. Přepínač čte ROM při spouštění programu. Přepíše hodnotu `[MZ800] mode_switch` z `.ini` (stejně jako menu Rear DIP Switch). Neplatná hodnota = chyba a konec. |
| `--record` | `<soubor.avi>` | Zahájit nahrávání videa hned po startu. Viz [`video-recording.md`](video-recording.md). |
| `--record-frames` | `<počet>` | Ukončit nahrávání videa po zadaném počtu snímků; v režimu `--headless` pak emulátor skončí. |

### Layout CDL exportu

CDL export tvoří jeden `meta.json` soubor + jeden binární soubor per
paměťový region. S `--cdl-dir ./my-runs/` a `--cdl-name session1`
vznikne tento layout:

```
./my-runs/
    session1.json                # meta + tabulka regionů (text)
    session1_bus.cdl             # 64K cell × 12 bajtů (R, W, X uint32 LE)
    session1_ram.cdl
    session1_rom-lower.cdl
    ...
    session1_iorq-8bit.cdl
```

`meta.json` obsahuje konkrétní názvy souborů jednotlivých regionů, takže
GUI Import potřebuje jen meta - sám si z tabulky regionů přečte cesty
a z téhož adresáře otevře referencované soubory.

### Persistence

CLI volby pro CDL (`--cdl-mode`, `--cdl-dir`, `--cdl-name`,
`--cdl-save-on-exit`) přepíší hodnoty načtené z `.ini` souboru.
Při ukončení se `.ini` přepíše aktuálními (přepsanými) hodnotami, takže
přepsání se stane novým defaultem pro příští spuštění.

Chcete-li otestovat nastavení bez jeho perzistence, kombinujte přepsání
s `--no-save-ini`:

```
mz800emu --cdl-mode always --no-save-ini
```

### Příklady

```
mz800emu --help

# Spustit s konkrétním MZF, zaznamenávat CDL při otevřeném debug okně,
# exportovat CDL při ukončení (nastavení persistovaná do .ini):
mz800emu --run-mzf program.mzf --cdl-mode window --cdl-save-on-exit on

# Jednorázová session: zaznamenávat CDL trvale, exportovat při ukončení,
# bez změny .ini:
mz800emu --cdl-mode always --cdl-save-on-exit on --no-save-ini

# Vlastní exportní adresář + basename:
mz800emu --cdl-mode always \
         --cdl-dir ./my-cdl-runs/session-1/ \
         --cdl-name run-2026-05-01 \
         --cdl-save-on-exit on
```

Detaily o CDL formátu a sémantice jednotlivých souborů viz
[`debugger/formats/cdl_format.md`](debugger/formats/cdl_format.md).


## trace-suite (CPU Tracking, IORQ, Interrupt, HW logy)

Sada čtyř nezávislých sekvenčních logovacích subsystémů pro hloubkovou
analýzu vykonaného kódu, I/O traffic, interrupts a HW state changes.
Každý subsystém lze aktivovat nezávisle přes `--<sys>-mode=<off|window|always>`,
kde `<sys>` = `cputrack` / `iorqlog` / `intlog` / `hwlog`. Per-subsystém
options sledují stejný vzor jako CDL (`-dir`, `-name`, `-save-on-exit`)
plus chunk/total limity pro streamovaný recording.

Výstupní struktura (per-recording adresář):

```
./trace-suite/
    cputrack.json                    # meta + chunks anchor + initial regs/RAM dump
    cputrack_initial_ram.bin         # 64 KB CPU RAM snapshot
    cputrack_initial_vram.bin        # 32 KB VRAM (MZ-800)
    cputrack_initial_memext.bin      # 512 KB Memext (pokud připojeno)
    cputrack.000.bin                 # chunk 0 - 12 B per CPU instrukce
    cputrack.001.bin                 # chunk 1
    ...
    iorqlog.json                     # meta + chunks anchor
    iorqlog.000.bin                  # 24 B per I/O událost
    intlog.json
    intlog.000.bin                   # 24 B per interrupt událost
    hwlog.json
    hwlog.000.bin                    # 24 B per HW state change událost
```

Per-subsystém formáty:

- [`debugger/formats/CPU-track_format.md`](debugger/formats/CPU-track_format.md) - CPU Tracking Log
- [`debugger/formats/IORQ-log_format.md`](debugger/formats/IORQ-log_format.md) - IORQ Log
- [`debugger/formats/INT-log_format.md`](debugger/formats/INT-log_format.md) - Interrupt Log
- [`debugger/formats/HW-log_format.md`](debugger/formats/HW-log_format.md) - HW Log
- [`debugger/Trace_Suite.md`](debugger/Trace_Suite.md) - uživatelský přehled, GUI menu, INI persistence

Další debugger dokumenty:

- [`debugger/breakpoints/`](debugger/breakpoints/) - smart breakpoints
  V1.5 (typy, match modes, expression syntax, action DSL, $vars, ...)
- [`debugger/symbols.md`](debugger/symbols.md) - symbol systém (sym_db,
  formáty .noi/.map/.sym/.lbl, UI panel Symbols)
- [`debugger/io-ports.md`](debugger/io-ports.md) - I/O Ports panel V1.5
  (Overview tab, History tab, activity tracking, naming konvence)
- [`debugger/disassembler-window.md`](debugger/disassembler-window.md) -
  samostatné Disassembler okno V1 (range disasm, auto-labely S/L/D/W,
  sym_db/CDL gating, export .asm/.s pro pasmo/sjasmplus/sdcc-asz80)

### Příklady

```
# Záznam CPU instrukcí během session, do vlastního adresáře:
mz800emu --run-mzf program.mzf --cputrack-mode always \
         --cputrack-dir ./traces/session/ --cputrack-name run-001 \
         --cputrack-save-on-exit on

# Všechny čtyři subsystémy najednou:
mz800emu --cputrack-mode always --iorqlog-mode always \
         --intlog-mode always --hwlog-mode always

# Stejný efekt přes shorthand (vše aktivní + sdílený adresář):
mz800emu --all-traces-mode always --all-traces-dir ./logs/run-1/

# Kombinace shorthand + per-subsystém override (vše always
# kromě hwlog, který zůstane vypnutý):
mz800emu --all-traces-mode always --hwlog-mode off

# Omezení diskové spotřeby přes chunk size + total cap:
mz800emu --cputrack-mode always \
         --cputrack-chunk-mb 32 --cputrack-max-total-mb 512
```

Shorthand options `--all-traces-mode` a `--all-traces-dir` aplikují
hodnotu na všechny 4 subsystémy najednou. Per-subsystém option
(`--cputrack-mode`, `--iorqlog-dir`, ...) má vždy precedenci - shorthand
vyplní pouze ty subsystémy, kde uživatel per-subsys option nepředal.


## MAX SPEED benchmark

Měření efektivity emulace v režimu MAX SPEED - kolik "času skutečného MZ-800"
se stihne vykonat za jednu reálnou sekundu (100 % = rychlost reálného hardwaru).
Okno otevřete přes menu **Emulator -> MAX SPEED Benchmark...**, klávesa `Alt + T`
vypíše report na konzoli a `Alt + Shift + T` měření resetuje. Pro headless měření
slouží volba `--maxspeed-bench`.

Podrobnosti viz [`maxspeed-benchmark.md`](maxspeed-benchmark.md).


## Známé problémy

### ImGui

Pokud narazíte na problém, kde si aplikace pamatuje špatnou velikost nebo pozici oken,
můžete editovat nebo smazat soubor `mz800emu-imgui.ini`.
