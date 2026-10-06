# Python wrapper (mcp_server.py)

Python wrapper `mcp_server.py` je FastMCP klient, který přemosťuje
MCP klienty (Claude Code, Claude Desktop, ...) na MCP backend
zabudovaný v emulátoru. Wrapper komunikuje s emu přes JSONL transport
(pipe nebo TCP) a sám vystavuje standardní MCP JSON-RPC API přes stdio.

## Architektura

```
+----------------+        MCP JSON-RPC stdio        +-----------------+
|  Claude Code   |  <----------------------------> |  mcp_server.py  |
+----------------+                                 +--------+--------+
                                                            |
                                          JSONL přes        |
                                  pipe (subprocess) NEBO    |
                                  TCP socket (GUI emu)      |
                                                            v
                  +---------------- pipe transport ----------------+
                  |   subprocess: mz800emu.exe --mcp-pipe          |
                  |   --headless --no-first-run-windows            |
                  |   (per-session vlastní instance)               |
                  +------------------------------------------------+

                  +---------------- tcp transport -----------------+
                  |   běžící GUI mz800emu.exe                      |
                  |   s MCP TCP Server -> Start (port 23800)       |
                  |   (sdílená live session uživatel + AI)         |
                  +------------------------------------------------+
```

## Instalace

Předpoklady: MSYS2/MinGW64 nebo Linux, Python 3.10+, klon repozitáře.

### 1. Sestavení emulátoru

MCP backend (= JSONL transport + dispatch vrstva v emu) je
**default zapnutý** ve standardním buildu. Stačí:

```bash
# UCRT64 shell (= MSYSTEM=UCRT64)
mingw32-make mz800emu
# Po buildu musí v repo rootu existovat mz800emu.exe
ls mz800emu.exe
```

Ověř že binárka má MCP přepínače:

```bash
./mz800emu.exe --help | grep -i mcp
# Očekáváno:
#   --mcp-pipe                  spustit MCP server přes stdio pipe
#   --mcp-tcp-port=N            spustit MCP TCP listener na portu N
```

#### Compile-time přepínače

| Flag | Default | Význam |
|------|---------|--------|
| (žádný) | MCP zapnutý | Standardní build, MCP backend uvnitř binárky |
| `NO_MCP=1` | MCP vypnutý | `make NO_MCP=1 mz800emu` - binárka bez MCP, žádné `--mcp-*` přepínače |
| `NO_MCP_TCP=1` | TCP zapnutý | TCP listener vypnutý, pipe transport zůstává |
| `NO_DEBUGGER=1` | debugger zapnutý | Implikuje `NO_MCP=1` (MCP vyžaduje debugger subsystém) |

Kaskáda: `NO_DEBUGGER=1` ⇒ `NO_MCP=1` ⇒ `NO_MCP_TCP=1`. Pokud
nevíš proč ses ocitl v build bez MCP, je možné že jsi (nebo dist
maintainer) předal `NO_DEBUGGER` flag. Standardní `make mz800emu`
poskytne plnou MCP funkčnost.

### 2. Inicializace venv + generace `.mcp.json`

```bash
cd mcp-server/
./mcpinit.sh
```

Skript je idempotent a dělá vše potřebné:

- vytvoří `.venv` (skipne pokud existuje)
- nainstaluje `requirements.txt` přes venv Python
- sanity check `import mcp`
- vygeneruje `mcp-server/.mcp.json` s absolutními cestami odpovídajícími
  tomuto konkrétnímu installu (= venv Python, `mcp_server.py`, repo
  root, `mz800emu.exe`). Pokud `.mcp.json` už existoval a má jiný
  obsah, je zazálohován jako `.mcp.json.bak`.

Aktivace venv v tvém shellu **není** potřeba - Claude Code spustí
`mcp_server.py` jako subprocess s explicit cestou k venv Pythonu
z `.mcp.json`.

Pozn.: pod MSYS2 mingw64 Pythonem viz sekce "Pozor pod MSYS2" níže
(= specifický problém s `pydantic-core` při instalaci `mcp[cli]`).

#### Manuální postup bez skriptu

```bash
cd mcp-server/
python -m venv .venv
source .venv/bin/activate     # MSYS2: source .venv/Scripts/activate
pip install -r requirements.txt
# .mcp.json pak musíš upravit ručně - viz krok 3 níže pro vzor
# struktury, plus dosadit absolutní cesty (= venv Python, mcp_server.py,
# repo root, mz800emu.exe).
```

### 3. Připojení Claude Code

Vygenerovaný `mcp-server/.mcp.json` obsahuje dvě entries:

| Entry name | Transport | Použití |
|------------|-----------|---------|
| `mz800emu`     | pipe | CI, batch sessions, AI bez GUI emu |
| `mz800emu-tcp` | tcp  | "Human in the loop" se sdílenou GUI session |

Zkopíruj soubor na cílovou pozici:

- `~/.claude/.mcp.json` (= user-level), nebo
- `<project>/.claude/.mcp.json` (= project-level)

Restartuj Claude Code. Příkaz `/mcp` v chatu zobrazí oba servery a
`tools/list` vrátí dostupné `emu_*` tools.

#### Struktura .mcp.json

Pro představu, vygenerovaný soubor vypadá zhruba takto (cesty se
mění podle install lokace):

```json
{
  "mcpServers": {
    "mz800emu": {
      "command": "<absolutní cesta k venv Pythonu>",
      "args":    ["<absolutní cesta k mcp_server.py>"],
      "cwd":     "<absolutní cesta k repo rootu>",
      "env": {
        "MZ800EMU_EXE":       "<absolutní cesta k mz800emu.exe>",
        "MZ800EMU_TRANSPORT": "pipe"
      }
    },
    "mz800emu-tcp": {
      "command": "<absolutní cesta k venv Pythonu>",
      "args":    ["<absolutní cesta k mcp_server.py>"],
      "cwd":     "<absolutní cesta k repo rootu>",
      "env": {
        "MZ800EMU_TRANSPORT": "tcp",
        "MZ800EMU_TCP_HOST":  "127.0.0.1",
        "MZ800EMU_TCP_PORT":  "23800"
      }
    }
  }
}
```

## Pozor pod MSYS2 / mingw Python

MSYS2 mingw/ucrt Python (platform `mingw_x86_64_*`) **neumí pip-zbuildit**
Rust-native závislosti `mcp[cli]`. Není to jen `pydantic-core` -
`mcp[cli]` jich má víc (`pydantic-core`, `rpds-py` přes jsonschema, ...),
všechny padají na `Unsupported platform: mingw_*` (Rust toolchain je
nepodporuje). Instalovat je po jednom z pacman je whack-a-mole a některé
balíček nemají.

**Doporučené řešení: použij vanilla Windows Python z python.org** (má
prebuilt wheels pro všechny tyto deps, žádný Rust build).

`mcpinit.sh` to řeší **automaticky**: pokud je `python` na PATH mingw
build, skript sáhne po `py -3` launcheru (= python.org Python) sám.
Stačí mít python.org Python nainstalovaný. Lze ho i vynutit:

```bash
PYTHON='C:/cesta/k/python.exe' ./mcpinit.sh
```

Manuální postup (bez skriptu) s python.org Pythonem:

```bash
cd mcp-server/
py -3 -m venv .venv          # nebo plná cesta k python.org python.exe
.venv/Scripts/python -m pip install -r requirements.txt
```

Krajní možnost (krehká) - zůstat u mingw Pythonu a pacman-instalovat
**každou** Rust dep + venv s `--system-site-packages`:

```bash
pacman -S mingw-w64-ucrt-x86_64-python-pydantic \
          mingw-w64-ucrt-x86_64-python-rpds-py
python -m venv --system-site-packages .venv
.venv/bin/python -m pip install -r requirements.txt
```

(Prefix podle MSYSTEM: UCRT64 = `mingw-w64-ucrt-x86_64-`, MINGW64 =
`mingw-w64-x86_64-`. Pokud nějaká dep pacman balíček nemá, tudy to
nepůjde - proto je python.org jistější.)

## Výběr transportu

Wrapper čte transport z env proměnných:

| Env var | Default | Hodnoty / význam |
|---------|---------|------------------|
| `MZ800EMU_TRANSPORT` | `pipe` | `pipe`, `tcp` |
| `MZ800EMU_EXE` | `../mz800emu.exe` | cesta k binárce (pouze pipe) |
| `MZ800EMU_WORK_DIR` | (prázdné) | pracovní adresář spawnutého emulátoru, předá se jako `--work-dir` (pouze pipe); tam jdou relativní výstupy, např. CDL export při ukončení |
| `MZ800EMU_TCP_HOST` | `127.0.0.1` | hostname (pouze tcp) |
| `MZ800EMU_TCP_PORT` | `23800` | port number (pouze tcp) |

V `.mcp.json` se nastavuje přes `env`:

```json
{
  "mz800emu-tcp": {
    "command": ".../mcp-server/.venv/Scripts/python.exe",
    "args": [".../mcp-server/mcp_server.py"],
    "env": {
      "MZ800EMU_TRANSPORT": "tcp",
      "MZ800EMU_TCP_HOST": "127.0.0.1",
      "MZ800EMU_TCP_PORT": "23800"
    }
  }
}
```

Detail transportů: [Pipe](transport-pipe.md) | [TCP](transport-tcp.md).

## Lazy connect

Transport se nenavazuje při startupu MCP serveru - **až při prvním
tool callu**. Discovery (`tools/list`) z Claude klienta je proto
instantní. To znamená:

- Claude Code zobrazí seznam tools hned po `/mcp` query.
- První skutečný tool call (např. `emu_ping`) inicializuje transport
  (= pipe spawn nebo TCP connect).
- Pokud transport selže, chybu uvidíte až u prvního tool callu, ne
  při discovery.

## Konec emulátoru a automatický restart

Pokud proces emulátoru skončí ve chvíli, kdy tool call čeká na
odpověď (pád nebo jiné ukončení procesu), volání **hned** selže
chybou typu `emulator process exited (exit code 3221225477 =
0xC0000005) while waiting for the response (cmd=run); ...` - nečeká
se do 30s timeoutu požadavku. V pipe módu se stav emulátoru ztratil.
V TCP módu je text `emulator connection closed (...)`; ztratilo se
jen spojení - GUI emulátor může dál běžet se svým stavem.

Po takovém nečekaném konci:

- `emu_status` vrací `{"running": false, "connected": false,
  "last_exit": "<důvod>"}`.
- Další tool call, který emulátor potřebuje, spustí nový proces
  emulátoru (pipe; RAM, registry, breakpointy a vložená média jsou
  ve výchozím stavu) nebo se znovu připojí (TCP). Restart ohlásí právě
  ten tool call, který ho vyvolal: jeho výsledek nese
  `"restarted": true` a `"restart_reason"`; pokud toto volání selže,
  text chyby končí `[restarted: true; <důvod>]`. Ohlásí ho jen toto
  jedno volání, i když jich běží víc současně. Pokud restart vyvolalo
  čtení resource, pole nese výsledek nejbližšího toolu.
- `emu_stop` s následným voláním ani explicitní `emu_start` se jako
  restart nehlásí.

## Logging

Logování wrapperu je řízené INI sekcí `[MCP]` v `mz800emu.ini` přes
klíče `wrapper_log_*` (= viz [Konfigurace MCP serveru](configuration.md)
sub-sekce **MCP Wrapper Log**). **Default je log vypnutý** - žádný
soubor se nevytvoří, dokud uživatel explicit nezapne úroveň.

Důležitá vlastnost: log **nesmí jít na stdout** - stdout je vyhrazen
pro MCP wire protokol (JSON-RPC). Wrapper proto směřuje log výhradně
do souboru přes `FileHandler` / `RotatingFileHandler` /
`TimedRotatingFileHandler`.

Pokud INI klíč `wrapper_log_path` je prázdný, default je
`mcp-server/mcp_server.log` (vedle `mcp_server.py`):

```bash
# Aktivace minimálního logu (jednorázová editace INI):
# [MCP]
# wrapper_log_level = info

tail -f mcp-server/mcp_server.log
```

Wrapper čte INI při svém startu - změna v GUI Settings (Tools -> MCP
TCP Server -> Settings... -> MCP Wrapper Log) **se aplikuje až při
příštím spuštění** `mcp_server.py` (= Claude Code typicky restart MCP
hosta nebo přepnutí workspace).

Pro env-based test override INI cesty: `MZ800EMU_INI=/path/to/test.ini`.

### Tolerance chyb

Při selhání (chybějící INI, neplatné hodnoty, nemožnost otevřít
soubor) wrapper neselhává - jen instaluje `NullHandler` a běží dál
bez logu. Diagnóza pak vyžaduje opravu INI a restart wrapperu.

### DEBUG wire trace (TX/RX)

Na úrovni `wrapper_log_level = DEBUG` wrapper navíc loguje surové
JSON-RPC wire byty obousměrně:

- `TX: <json>` - odchozí JSONL request do emu (z `_send_request`)
- `RX: <json>` - příchozí JSONL řádek z emu (ze stdout reader tasku;
  zachytí i async broadcasty bez `req_id`, např. MCP_ACTION)

Wire trace je **jen na DEBUG** - úrovně INFO a nižší zůstávají čisté
(jen události connect / EOF / error), aby běžný provozní log nebobtnal
každou zprávou protokolu. Dlouhé payloady (base64 screenshot, mem dump)
se v logu zkrátí na prvních 500 znaků a doplní marker `...[N chars]`,
aby log nenarostl o megabajty.

## Spuštění manuálně

```bash
# Předpokládá nainstalované mcp[cli] (= venv aktivní):
python mcp_server.py
# Server poslouchá na stdio - bez MCP klienta zůstane viset.
# Ctrl+C ukončí.
```

Test FastMCP CLI:

```bash
mcp --help
# FastMCP poskytuje nástroje pro dev mode:
mcp dev mcp_server.py
```

## Troubleshooting

### "mz800emu binary not found at ..."

Spawn pipe transportu selhal - wrapper nenašel binárku. Diagnóza:

1. Ověř že existuje `mz800emu.exe` v repo rootu:
   ```bash
   ls mz800emu.exe
   ```
2. Pokud ne, sestav: `mingw32-make mz800emu`.
3. Pokud binárka leží jinde, nastav env: `MZ800EMU_EXE=/full/path/to/mz800emu.exe`.

### TCP transport: "Connection refused"

Wrapper v tcp módu nemůže navázat konexi. Diagnóza:

1. V GUI emu zkontroluj: Tools -> MCP TCP Server -> stav (Start).
2. Alternativně spusť GUI s argumentem: `mz800emu.exe --mcp-tcp-port=23800`.
3. Ověř že port 23800 není blokovaný firewall / není obsazený jiným
   procesem (`netstat -ano | grep 23800` na Win).

### Claude Code: tools nezobrazené v `/mcp`

1. Ověř že `.mcp.json` byl zkopírovaný na cílovou pozici
   (`~/.claude/.mcp.json` nebo `<project>/.claude/.mcp.json`) a obsahuje
   absolutní cesty. Pokud používáš `mcpinit.sh`, absolutní cesty jsou
   garantované; pokud jsi editoval ručně, znovuověř.
2. Zkontroluj logy Claude Code (Help -> Show logs).
3. Otestuj manuálně bridge: `python mcp_server.py < /dev/null` by mělo
   vypsat MCP `initialize` error (= bridge naskočil). Pokud `ImportError`
   nebo `ModuleNotFoundError`, chybí FastMCP v aktivním Pythonu (=
   spusť `./mcpinit.sh` nebo manuální `pip install -r requirements.txt`).
4. Zkontroluj `mcp_server.log` - pokud je prázdný, server vůbec
   nedoběhl do `main()` (= špatný interpret v `.mcp.json command`).

### Subprocess spawn issue (CreateProcess fail)

Windows specifické: pokud `.mcp.json` `command` ukazuje na shell
script nebo `python.exe` bez plné cesty, CreateProcess může selhat
s WinError 193 (`%1 is not a valid Win32 application`). Použij vždy
plnou cestu k `python.exe` (= `.../.venv/Scripts/python.exe`).

### `mem_write` vrátí "MEM_WRITE region check failed"

Cílová adresa padla do non-RAM regionu (ROM, CG-ROM, prohibited,
unmapped). To je očekávané - běžný `mem_write` zapisuje jen do RAM.
Pro nucený zápis i do ROM/CG-ROM použij `mem_write_force` (sensitive
tool). RAM region typicky pokrývá 0x1000..0x7FFF v MZ-800 banking,
ale závisí na aktuálním banking stavu.

## Související

- [Pipe transport](transport-pipe.md)
- [TCP transport](transport-tcp.md)
- [Konfigurace](configuration.md) - INI klíče sekce `[MCP]`
- [Přehled tools](tools-overview.md) - dostupné tools, sensitive flag
- [Přehled resources](resources-overview.md) - read-only resources
