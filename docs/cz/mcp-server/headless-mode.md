# Headless režim

Emulátor lze spustit v **bezhlavém režimu** přes CLI přepínač
`--headless`. V tomto režimu se nevytvoří grafické okno ani audio výstup,
ale emulační smyčka běží normálně.

## Použití

```
mz800emu --headless
mz800emu --headless --run-mzf <cesta-k-programu.mzf>
mz800emu --headless --no-first-run-windows
```

Proces běží do externího ukončení (`Ctrl+C` na Linuxu, `taskkill /F /IM
mz800emu.exe` na Windows, nebo SDL quit event), nebo do splnění podmínky
konce automatického běhu (viz [Automatické běhy](#automatické-běhy)).

## Kdy ho použít

- **CI / batch test runner** - automatizované testy bez nutnosti
  display / X serveru / audio zařízení
- **MCP subprocess** - MCP klient (Claude Code, Claude Desktop)
  spustí emulátor jako child proces přes named pipe komunikaci
- **Headless benchmark** - měření výkonu bez overhead GUI renderingu

## Co je v bezhlavém režimu k dispozici

| Komponenta | Stav |
|------------|------|
| Z80 CPU emulace | běží normálně |
| Periferie (GDG, CTC, PPI, PSG, FDC, ...) | běží normálně |
| Framebuffer (v paměti, BGRA 928×288) | renderuje se |
| Snapshot save/load | funguje |
| CMT hack (`--run-mzf`) | funguje |
| Načítání přes CMT hack spuštěné z ROM (boot menu C, `LOAD`) | **bez** dialogu pro výběr souboru: načtení se hned zruší (ROM ohlásí Break) a na stderr se vypíše varování. Program načtěte přes `media_load_mzf` nebo `--run-mzf`, nebo CMT hack vypněte (`cmt_hack_set`) a použijte virtuální pásku |
| Konzole stdout/stderr | jen v `FORCE_CONSOLE=1` buildu (viz níže) |
| GUI okno | **ne** (přeskočeno) |
| Audio výstup | **ne** (no-op SDL audio device) |
| Tempo emulace | podle systémových hodin: 100 % = reálný čas (MZ-800: 50 snímků/s), vlastní rychlost i MAX SPEED jako v okně |
| ImGui debugger | **ne** (kontext se nevytvoří) |

Bezhlavá emulace tedy běží v reálném čase. Pro rychlejší běh (testy, dávky) zvolte rychlost při startu volbou `--speed` (`--speed max` = MAX SPEED, `--speed 400` = čtyřnásobek reálné rychlosti; platné jsou 1-4000 %), nebo ji změňte za běhu MCP nástrojem `set_speed`. Volba `--speed` platí pro `mz800emu --headless`; s `--mcp-pipe` se `--speed` odmítne (`Unknown option: --speed`, start skončí chybou); v pipe režimu rychlost nastavte nástrojem `set_speed`.

## Automatické běhy

Pro CI a skripty bez MCP klienta emulátor v `--headless` skončí sám, když
nastane podmínka, zapíše výsledek do souboru JSON a vrátí návratový kód.

```
mz800emu --headless --speed max --run-mzf test.mzf \
         --exit-on-pc 2000h --fail-on-pc 2100h --timeout-seconds 30 \
         --automation-dir out
```

### Podmínky konce

| Volba | Ukončí běh | Návratový kód |
|-------|------------|---------------|
| `--exit-after-frames N` | po N emulovaných snímcích | 0 |
| `--timeout-frames N` | po N emulovaných snímcích, pokud dřív nenastala jiná podmínka | 2 |
| `--timeout-seconds S` | totéž v emulovaných sekundách (zaokrouhleno nahoru na celé snímky) | 2 |
| `--exit-on-pc ADDR[,skip=N][,code=C]` | když CPU vykoná ADDR (po přeskočení prvních N průchodů) | C, výchozí 0 |
| `--fail-on-pc ADDR[,skip=N]` | když CPU vykoná ADDR | 1 |
| `--exit-on-write ADDR[=VAL][,skip=N][,code=C]` | když program zapíše na adresu paměti ADDR (jen hodnotu VAL, je-li zadaná) | zapsaná hodnota, nebo C |
| `--exit-on-out PORT[,skip=N][,code=C]` | když program zapíše na port PORT (0-255) | zapsaná hodnota, nebo C |
| `--bpt FILE` | načte sadu breakpointů; breakpoint s akcí `exit` běh ukončí | podle akce |

- Vyhrává první splněná podmínka. Když `--exit-after-frames` a timeout
  padnou na stejný snímek, vyhrává `--exit-after-frames`.
- `--exit-on-*` a `--fail-on-pc` jde zadat víckrát. Založí obyčejné
  breakpointy s akcí `exit`; stejně funguje každý breakpoint s touto
  akcí (viz [Action DSL](../debugger/breakpoints/action-dsl.md)). Tyto
  volby a `--bpt` potřebují build s debuggerem.
- Čísla: desítkově, `0x2000` nebo `2000h`.
- Snímky se počítají od začátku běhu, po `--run-mzf`. Časové podmínky se
  kontrolují jednou za snímek a fungují i v buildu bez debuggeru.
- `--timeout-*` použijte jako pojistku, aby zaseknutý program nezablokoval
  úlohu CI.
- Port pro `--exit-on-out` volíte vy. Zvolte takový, který program ani
  vaše sestava hardwaru nepoužívá; zápis na něj se do emulovaného hardwaru
  dostane i tak.
- V automatickém běhu (s kteroukoliv volbou z tohoto oddílu) se
  breakpointy a proměnné `$` uložené z dřívějších relací nenačtou ani
  neuloží, takže zbytky z GUI výsledek neovlivní. Breakpointy jsou jen
  z `--bpt` a z voleb `--exit-on-*` / `--fail-on-pc`.

### Návratové kódy

| Kód | Význam |
|-----|--------|
| 0 | úspěch: doběhl `--exit-after-frames`, `--exit-on-pc`, `exit 0` |
| 1 | test selhal: `--fail-on-pc`, `exit 1` |
| 2 | timeout |
| 3 | chyba emulátoru nebo voleb (neplatná volba, chybějící soubor) |
| 4 | záznam přehrávaný s `--exit-at-replay-end` se liší od nahraného (`replay_desync`, viz [../replay.md](../replay.md)) |
| 64-255 | volné pro kódy zapsané programem (`--exit-on-out`, `exit Value`) |

Kód 3 za neplatnou volbu se použije jen tehdy, když je zadaná některá
volba automatického běhu; jinak neplatná volba vrací 1 jako dřív.

### Soubor s výsledkem a artefakty

| Volba | Zapíše |
|-------|--------|
| `--result-json FILE` | výsledek jako JSON |
| `--automation-dir DIR` | `DIR/result.json` a `DIR/screen.png` (konečná obrazovka); složka se založí |
| `--exit-screenshot FILE` | PNG konečné obrazovky |
| `--exit-snapshot FILE` | snapshot (`.mzs`) konečného stavu |

Výsledek se zapíše na konci i při timeoutu nebo chybě, takže ho skript
může přečíst vždy. Nahrávání videa (`--record`) a zachycení zvuku
(`--audio-capture`) se před koncem procesu dokončí při jakémkoliv
návratovém kódu.

Příklad (`--exit-after-frames 100 --automation-dir out`, MZ-800; hashe
a cesty zkrácené):

```json
{
  "schema" : "mz800emu-result/1",
  "emulator" : { "version" : "2.0.6 devel", "build_revision" : -1, "commit" : "5ce317db",
                 "platform" : "MZ-800", "screens_per_second" : 50, "debugger" : true },
  "reason" : "frames",
  "exit_code" : 0,
  "message" : "100 frames done",
  "trigger" : { "kind" : "frames", "option" : "--exit-after-frames" },
  "time" : { "frames" : 100, "frames_total" : 100, "gdg_ticks" : 35442605,
             "gdg_ticks_total" : 35443245, "emulated_seconds" : 1.999966425153485,
             "wall_seconds" : 0.081809 },
  "cpu" : { "pc" : 33667, "sp" : 4334, "af" : 65356, "bc" : 1372, "de" : 842, "hl" : 842,
            "ix" : 16612, "iy" : 0, "af2" : 0, "bc2" : 0, "de2" : 0, "hl2" : 0,
            "i" : 0, "r" : 25, "im" : 1, "iff1" : 0, "iff2" : 0, "halted" : false },
  "raster" : { "beam_row" : 0, "frame_ticks" : 45 },
  "hashes" : { "algorithm" : "sha256", "ram" : "64090b03...", "memext" : "2fb5b470...",
               "vram" : "edf097e8...", "screen" : "f58b3934...", "cpu" : "9c1e07d2..." },
  "artifacts" : { "screenshot" : ".../out/screen.png", "snapshot" : null,
                  "audio_capture" : null, "video" : null },
  "inputs" : { "run_mzf" : ".../Gardner.mzf", "bpt" : null },
  "warnings" : [ ]
}
```

Běh ukončený breakpointem má například `"reason" : "exit_action"` a
`"trigger" : { "kind" : "breakpoint", "option" : "--exit-on-pc", "bp_id" : 1, "bp_name" : "cli-exit-on-pc-1", "bp_type" : "PC_EXEC", "hits" : 1 }`
(`option` je null u breakpointu, který nevznikl z příkazové řádky).

- `emulator.build_revision`: číslo revize oficiálního buildu, -1 u
  ostatních buildů.
- `reason`: `frames`, `timeout`, `exit_action` (akce breakpointu `exit`),
  `error`, `quit` (proces ukončený zvenku dřív, než nastala podmínka,
  třeba `Ctrl+C`, nebo po `--record-frames`), nebo `replay_end` /
  `replay_desync` (konec záznamu přehrávaného s `--exit-at-replay-end`
  nebo `--replay-verify`).
- `replay`: `null`, nebo výsledek záznamu použitého v běhu (`path`,
  `outcome`: `ended`, `desync`, `recorded`, ..., `outcome_frame`,
  `frames`, `deviated`, `desync_parts`, `message`).
- `coverage`: `null` bez dat pokrytí kódu, jinak stav pokrytí stejný jako
  z `emu_coverage_status` (`level`, `complete`, `units`, `instructions`,
  `branches`, ...). Pokrytí od startu sbírá `--coverage branch` nebo
  `--coverage detail` (nebo `[COVERAGE] mode` v INI), všechny řádky na
  konci zapíše `--coverage-save-on-exit SOUBOR` (`.csv` = CSV, jinak
  JSON, viz [Přehled MCP tools](tools-overview.md); relativní cesta je
  vůči aktuálnímu adresáři jako u `--result-json`). `--coverage-key`
  a `--coverage-capacity` nastavují úroveň `detail`.
- `trigger.hits`: zásahy breakpointu počítané až po přeskočených.
- `time.frames` a `time.gdg_ticks` se počítají od začátku běhu,
  `frames_total` a `gdg_ticks_total` od začátku emulace. `emulated_seconds`
  je `gdg_ticks` v sekundách emulovaných hodin.
- `cpu`: registry jako čísla (`af2` ... `hl2` jsou stínové registry),
  `halted` true nebo false.
- `hashes`: SHA-256 vnitřní RAM (64 KB), RAM MemExt (jen připojený),
  VRAM (MZ-800: všechny čtyři plány), PCG (MZ-1500), obrazovky
  (framebuffer jako indexy palety) a `cpu` (registry Z80 včetně skrytého
  stavu: WZ, Q, odložení po EI, čekající INT a NMI). Dva běhy se stejnými
  volbami dají stejné hashe, takže je skript může porovnat s referencí.
- Po chybě před startem emulace (neplatná volba) má soubor jen `schema`,
  `emulator`, `reason`, `exit_code`, `message`, `trigger`, `replay`,
  `artifacts`, `inputs` a `warnings`.

Standardní build na Windows nemá výstup do konzole, proto je soubor
s výsledkem pro úlohu CI spolehlivý kanál.

### Konec v GUI a s MCP

Proces skončí jen v `--headless`, a s MCP TCP serverem jen tehdy, když je
zadaná volba automatického běhu. V GUI, s `--mcp-pipe` a v `--headless`
s MCP TCP serverem, ale bez takové volby, podmínka konce emulaci jen
pozastaví: na stderr je kód a událost MCP `paused` má `reason: "exit"`
s `exit_code`, `exit_reason` a `message`. `emu_run` pak vrátí
`stopped_by: "exit"`. Soubor s výsledkem se zapíše, je-li požadovaný.
O dalším postupu rozhodne klient.

## Build s konzolovým výstupem (Windows)

Standardní `mz800emu.exe` na Windows je linkován jako GUI subsystém =
stdout / stderr nejsou viditelné v pipe. Pro headless workflow kde
potřebujete vidět logy:

```
mingw32-make mz800emu FORCE_CONSOLE=1
```

(Ekvivalent `cmake -DMZ_FORCE_CONSOLE=ON`.) Build s `FORCE_CONSOLE=1`
alokuje konzolu při startu a stdout / stderr jsou viditelné v
terminálu nebo přesměrovatelné do souboru přes shell pipe.

Default zůstává GUI subsystém (pro koncové uživatele s ikonou v
průzkumníku Windows).

## Známá omezení

- **Window error log při startu** - 2-3 řádky `getSDL_Window_by_name():
  Failed to get window: main_window` - **benigní**, volající mají guard
  na `NULL` window a pokračují bez chyby. Hlášení lze v logu ignorovat.

## Screenshot v headless mode

Resource `emulator://frame/screenshot.raw` funguje **i v headless
mode** (= `--mcp-pipe`, `--headless`). Handler primárně
čte SDL display-ready snapshot (`g_iface_video->fbsnapshot_pixels`);
pokud je NULL, fallbackuje na GDG live buffer (`g_framebuffer.pixels`,
staticky alokovaný BSS).

Response obsahuje pole `fallback_source` se string hodnotou:

| `fallback_source` | Význam |
|-------------------|--------|
| `"sdl_snapshot"` | Primární SDL display path (= obvyklé v GUI módu) |
| `"gdg_live"` | Fallback na GDG live buffer (= obvyklé v headless / race condition) |

### Příklad Python klient (raw RGBA → PIL Image)

```python
import base64
from PIL import Image

resp = emu.call('get_frame_screenshot_raw')
data = resp['data']
if not data.get('available'):
    raise RuntimeError(f"Screenshot unavailable: {data.get('reason')}")
print(f"Source: {data['fallback_source']}, "
      f"frame {data['source_screen_id']}")

raw = base64.b64decode(data['data_b64'])
img = Image.frombytes('RGBA', (data['width'], data['height']), raw)
img.save('mz_screen.png')
```

Pro INDEX8 zpracování (= bez auto expand do RGBA) je k dispozici
Resource `emulator://frame/framebuffer/info` s 16-entry paletou; raw
pixels by ovšem bylo nutné získat jiným Tool (= ne přes screenshot.raw,
který RGBA expand provádí backend).

### PNG do souboru s oblastí, měřítkem a ořezem

`emu_screenshot_save_to_file` (příkaz `screenshot_save_to_file`) zapíše
PNG přímo do souboru na počítači se serverem. Volitelné parametry určují,
co se uloží:

| Parametr | Hodnoty | Význam |
|----------|---------|--------|
| `area` | `full` (výchozí), `canvas` | celý obraz s borderem, nebo jen obraz bez borderu |
| `scale` | `framebuffer` (výchozí), `native`, `1` až `4` | viz níže |
| `crop` | `{"x", "y", "width", "height"}` | výřez ve výstupních pixelech, jen s `framebuffer` nebo `native` |

- `framebuffer` - pixely framebufferu 1:1, jeden pixel na takt GDG
  vodorovně a na obrazový řádek svisle (MZ-800 celý obraz 928x288,
  obraz bez borderu 640x200). Tak se příkaz chová bez parametrů.
- `native` - jeden pixel obrázku na bod počítače. Když jsou v oblasti
  všechny dvojice sousedních pixelů framebufferu stejné (režimy 320
  bodů), šířka se zpoloviční (MZ-800 obraz 320x200), jinak zůstane
  (640x200); řádky 1:1. Odpověď obsahuje `hires`.
- `1` až `4` - poměr stran jako v okně emulátoru; `2` odpovídá oknu na
  100 % (MZ-800 celý obraz 928x576). Měřítko `1` v režimech 640 bodů
  vynechá každý druhý sloupec.

Příklad - prostředních 256x192 bodů obrazu 320x200:

```python
emu_screenshot_save_to_file(path="C:/tmp/viewport.png", area="canvas",
                            scale="native",
                            crop={"x": 32, "y": 4, "width": 256, "height": 192})
```

Neznámá hodnota nebo ořez mimo obraz je chyba, soubor se nezapíše.

### Série snímků - každý změněný snímek jako PNG

Pro hledání chyb v animacích uloží `emu_screenshot_series_start` PNG
každého emulovaného snímku, který se liší od posledního uloženého
(příkazy `screenshot_series_start`, `screenshot_series_stop`,
`screenshot_series_status`).

- Snímky se berou v emulovaném čase na konci každého snímku, takže se
  žádný nevynechá ani při MAX SPEED. Když zápis nestíhá, emulace počká.
  Série snímá jen za běhu emulace.
- `area`, `scale` a `crop` fungují jako u jednoho screenshotu (výchozí
  měřítko `native`). Počítají se jen změny uvnitř nich - s ořezem jde
  sledovat jeden sprite a zbytek obrazovky ignorovat.
- Soubory: `frame_NNNNNNN.png`, kde NNNNNNN je emulovaný snímek od startu
  série (mezery = nezměněné snímky), a `index.csv` se sloupci
  `frame,file,gdg_screen`.
- `only_changed` (výchozí true), `frames` (stop po N emulovaných snímcích,
  0 = do stop), `max_files` (pojistka, výchozí 10000), `dir` (výchozí:
  nová složka ve výstupní složce screenshotů).

```python
emu_screenshot_series_start(area="canvas", frames=250)   # 5 s snímků PAL
emu_run(frames=250)
emu_screenshot_series_status()   # files, unchanged, dir, stop_reason
```

`emu_screenshot_series_stop(wait=True)` ukončí sérii dřív a počká, až se
zapíšou všechny snímky.

### Známá omezení

- **Race condition v GUI mode** - pokud klient požaduje screenshot
  velmi často (= s frekvencí blízkou 50 Hz frame rate), může handler
  v GUI mode občas vrátit `fallback_source: "gdg_live"` namísto
  `"sdl_snapshot"` (= SDL render thread konzumoval mezi publish
  okny). Obsah je byte-identický, klient typicky nemusí řešit.
- **Pre-emu screenshot** - před prvním `frame_done` (= těsně po reset
  / power-on) může screenshot vrátit zeroed buffer s
  `source_screen_id: 0`. Klient se dozví že emu ještě nestihl
  vykreslit obrazovku z `source_screen_id == 0` test.

## Související

- [MCP server overview](README.md)
