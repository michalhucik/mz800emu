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
mz800emu.exe` na Windows, nebo SDL quit event).

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
