# Konfigurace MCP serveru

MCP server je konfigurovatelný ze tří zdrojů, v pořadí priority od
nejnižší po nejvyšší:

1. **INI soubor** `mz800emu.ini` sekce `[MCP]` (= persistentní)
2. **GUI Settings dialog** v hlavním okně (= persistentní, ukládá
   do INI)
3. **CLI overrides** při spuštění (= runtime, neukládá se)

## INI sekce `[MCP]`

INI soubor `mz800emu.ini` v user config adresáři obsahuje sekci
`[MCP]` s těmito klíči:

| Klíč | Typ | Default | Význam |
|------|-----|---------|--------|
| `tcp_port` | unsigned | `23800` | TCP port pro MCP server (1024-65535) |
| `bind_address` | keyword | `127.0.0.1` | `127.0.0.1` (localhost) nebo `0.0.0.0` (vše) |
| `profile` | keyword | `wild` | Bezpečnostní profil: `wild` / `confined` / `sandboxed` / `observer` |
| `auto_start_tcp` | bool | `false` | Spustit TCP server automaticky při startu emu |
| `action_log_path` | text | `` (prázdné) | Cesta k souboru pro disk persist MCP/USER akcí; prázdné = vypnuto |
| `wrapper_log_level` | keyword | `off` | Úroveň logu Python wrapperu: `off` / `error` / `warning` / `info` / `debug` |
| `wrapper_log_path` | text | `` (prázdné) | Cesta k log souboru wrapperu; prázdné = default `mcp-server/mcp_server.log` |
| `wrapper_log_rotate_kind` | keyword | `none` | Druh rotace: `none` / `size` / `time` |
| `wrapper_log_rotate_size_mb` | unsigned | `10` | Velikost v MB před rotací (jen pro `size`, 1-10240) |
| `wrapper_log_rotate_when` | text | `midnight` | Token pro `TimedRotatingFileHandler` (jen pro `time`) |
| `wrapper_log_rotate_keep` | unsigned | `5` | Počet rotovaných souborů (backupCount, 0-1000) |

Čísla (`unsigned`) jsou v INI šestnáctková jako všude v konfiguračním
souboru: emulátor je ukládá s prefixem `0x` a čte šestnáctkově i bez něj
(`wrapper_log_rotate_size_mb = 50` znamená 80 MB). Hodnoty `bool` jsou
`0` a `1`. Tabulka uvádí výchozí hodnoty desítkově.

Příklad INI:

```ini
[MCP]
tcp_port=0x5cf8
bind_address=127.0.0.1
profile=wild
auto_start_tcp=1
action_log_path=/home/user/mz800-activity.log
wrapper_log_level=off
wrapper_log_rotate_kind=none
```

### action_log_path - disk persist Activity log

Pokud uživatel nastaví non-empty cestu, každý zaznamenaný MCP / USER
příkaz se appenduje do textového souboru ve formátu:

```
<ISO 8601 UTC> [<origin>] <cmd>: <description>
```

Příklad:

```
2026-05-27T14:23:01Z [mcp] mem_read: addr=0x4000 len=16
2026-05-27T14:23:02Z [user] pause:
2026-05-27T14:23:05Z [mcp] bp_add: addr=0x5000 type=PC_EXEC
```

Tokeny `origin` jsou `user` / `mcp` / `test` / `internal` a jsou
stable - bezpečně grepuje shell skripty. Zápis je best-effort: pokud
soubor nelze otevřít (= disk full, perm issue), entry se jen
zapíše do RAM ring bufferu Activity okna a chyba zápisu se ignoruje.

Vypnutí: vynech klíč v INI nebo nastav prázdnou hodnotu.

### MCP Wrapper Log - logování Python wrapperu

Skupina klíčů `wrapper_log_*` řídí logování Python MCP wrapperu
(`mcp-server/mcp_server.py`). C strana emu hodnoty pouze ukládá; vlastní
interpretace probíhá v Pythonu při jeho startu (= mz800emu na ně nemá
runtime vliv). Klíče nahrazují předchozí hardcoded INFO log.

**Default je log vypnutý** (`wrapper_log_level = off`) - žádný soubor
se nevytvoří, root logger dostane `NullHandler`. Pro diagnostiku je
potřeba úroveň zapnout.

#### Druhy rotace

| `wrapper_log_rotate_kind` | Python handler | Aktivní klíče |
|---------------------------|----------------|---------------|
| `none` | `logging.FileHandler` | jen `wrapper_log_path` |
| `size` | `logging.handlers.RotatingFileHandler` | `wrapper_log_rotate_size_mb`, `wrapper_log_rotate_keep` |
| `time` | `logging.handlers.TimedRotatingFileHandler` | `wrapper_log_rotate_when`, `wrapper_log_rotate_keep` |

Hodnoty `wrapper_log_rotate_when` odpovídají Python dokumentaci:

- `S` - sekundy
- `M` - minuty
- `H` - hodiny
- `D` - dny
- `midnight` - rotace o půlnoci
- `W0` až `W6` - rotace v daný den v týdnu (W0 = pondělí)

#### Příklady

Kontinuální INFO log do default cesty:

```ini
[MCP]
wrapper_log_level = info
```

Velikostně rotovaný DEBUG log, max 50 MB, 10 historických:

```ini
[MCP]
wrapper_log_level = debug
wrapper_log_path = C:/mz800-logs/wrapper.log
wrapper_log_rotate_kind = size
wrapper_log_rotate_size_mb = 0x32
wrapper_log_rotate_keep = 0x0a
```

Denní rotace s historií 30 dní:

```ini
[MCP]
wrapper_log_level = info
wrapper_log_path = /var/log/mz800/wrapper.log
wrapper_log_rotate_kind = time
wrapper_log_rotate_when = midnight
wrapper_log_rotate_keep = 0x1e
```

#### Tolerance chyb

Python wrapper je vůči chybám tolerantní - safe fallback na **vypnutý
log** (NullHandler):

- INI soubor neexistuje
- INI nelze parsovat
- Sekce `[MCP]` chybí
- Neznámá hodnota `wrapper_log_level`
- Otevření souboru selže (neexistující adresář, permission denied)
- Neznámý token `wrapper_log_rotate_when` (pak se použije FileHandler bez rotace)

V těchto případech wrapper běží dál, jen bez logu.

## Uživatelská knowledge base (`MZ800EMU_USER_KB_DIR`)

Kromě vestavěných AI-reference dokumentů (`docs/agent/*.md`, dostupné
jako `emulator://docs/*`) může uživatel vystavit AI klientům vlastní
Markdown poznámky bez zásahu do repozitáře.

Tato volba je **wrapper-side konfigurace** - nepatří do `[MCP]` sekce
`mz800emu.ini` (tu vlastní C jádro a při uložení ji přepisuje, takže by
neznámý klíč zmizel). Místo toho se nastavuje přes **env proměnnou
`MZ800EMU_USER_KB_DIR`**, typicky v `env` bloku souboru `.mcp.json`,
kterým MCP klient (Claude Code apod.) spouští server:

```json
"env": {
  "MZ800EMU_EXE": "...",
  "MZ800EMU_TRANSPORT": "pipe",
  "MZ800EMU_USER_KB_DIR": "C:/Users/me/moje-mz-poznamky"
}
```

`mcpinit.sh` generuje `.mcp.json` s prázdným `MZ800EMU_USER_KB_DIR`
(= vypnuto); stačí doplnit cestu. Prázdné = bez `kb` resources.

Všechny `*.md` v té složce se prohledají **rekurzivně** a zaregistrují
jako `emulator://kb/<relativní cesta bez .md>` (oddělovače cesty se
mění na `/`). Příklad: `moje-mz-poznamky/hw/vram.md` ->
`emulator://kb/hw/vram`. Namespace `emulator://kb/*` je oddělený od
vestavěného `emulator://docs/*`, takže uživatelské poznámky nikdy
nekolidují s projektovou referencí.

Title a description (zobrazené v `resources/list`) se získají z dokumentu:
volitelný front matter blok (`title:` / `description:` mezi `---`) má
přednost, jinak se použije první `# H1` nadpis jako title a první
textový odstavec jako description.

Tolerance chyb: nenastavená env proměnná i neexistující adresář neshodí
server - jen se nezaregistrují žádné `kb` resources.

## GUI Settings dialog

V hlavním okně **Tools -> MCP TCP Server -> Settings...** otevře
dialog s editovatelnými poli pro všechny klíče. Změny se ukládají
zpět do INI při potvrzení dialogu **OK**.

Pole:

- **TCP Port** - číselný input, validace rozsahu 1024-65535
- **Bind Address** - dropdown (`127.0.0.1` / `0.0.0.0`)
- **Security Profile** - dropdown (`wild` / `confined` / `sandboxed`
  / `observer`)
- **Auto-start TCP server** - checkbox

Sub-sekce **MCP Wrapper Log**:

- **Log Level** - dropdown (`OFF` / `ERROR` / `WARNING` / `INFO` / `DEBUG`)
- **Log Path** - text input (prázdné = default `mcp_server.log` v adresáři wrapperu)
- **Rotation** - dropdown (`none` / `size` / `time`)
- **Max Size (MB)** - číselný input (aktivní jen pro `size`)
- **Time When** - text input (aktivní jen pro `time`)
- **Keep Files** - číselný input

Disabled stavy se řídí logem level a rotation kind (= vypnutý log
disabluje vše ostatní, `none` rotation disabluje rotation pole). Změny
hodnot se uloží do INI při **Save**, ale **aplikují se až při příštím
startu** `mcp_server.py` - C strana emu wrapper běhově neřídí.

Změna `tcp_port` nebo `bind_address` se aplikuje až při příštím
**Start** TCP serveru (= existující běžící server přebere staré
hodnoty). Pro okamžitou aplikaci: Stop -> Settings... -> Save -> Start.

## CLI overrides

Při spuštění emu lze přebít INI hodnoty CLI flagy:

| Flag | Přebijí INI klíč |
|------|-----------------|
| `--mcp-tcp-port=PORT` | `tcp_port` |
| `--mcp-bind=ADDR` | `bind_address` |
| `--mcp-profile=PROFILE` | `profile` |

Použití `--mcp-tcp-port` zároveň **implikuje `auto_start_tcp = 1`
jen pro tento run** (= server naskočí, i když INI má `auto_start_tcp =
0`). Persistentní stav `auto_start_tcp` se tím nezmění.

Příklad:

```bash
# Jednorázový run s portem 24000, bind na všechna rozhraní,
# profile sandboxed (= INI hodnoty zůstanou jak jsou):
mz800emu.exe --mcp-tcp-port=24000 --mcp-bind=0.0.0.0 --mcp-profile=sandboxed
```

## Persistence flow

```
+----------+   load    +--------------+  override  +-------+
| INI file | --------> | runtime conf | <--------- | CLI   |
+----------+           +--------------+            +-------+
                              |
                              | read
                              v
                  +-------------------------+
                  | TCP server, dispatcher  |
                  +-------------------------+
                              ^
                              | save on OK
                              |
                       +-------------+
                       | GUI dialog  |
                       +-------------+
```

1. Při startu emu se nejdřív načte INI -> runtime konfigurace má
   persistentní hodnoty.
2. Pokud uživatel zadal CLI flag, ten přebije příslušné pole.
3. TCP server a dispatch čtou aktuální hodnoty z runtime konfigurace.
4. Pokud uživatel za běhu změní hodnotu v GUI Settings dialog,
   zapíše se zpět do INI (= příští start emu už dostane novou
   hodnotu i bez CLI flagů).

## Bezpečnostní profil

Pole `profile` se aktuálně **pouze ukládá** do INI a propaguje do
dispatch vrstvy. Plné vynucení (whitelist souborů, blokace
destruktivních tools, audit log) zatím není v této verzi k dispozici.

Stav jednotlivých profilů:

| Profile | Stav |
|---------|---------|
| `wild` | aktivní default - žádná omezení nad rámec default region check v `mem_write` |
| `confined` | persist-only - reálně se nedělá nic odlišného od `wild` |
| `sandboxed` | persist-only - nedělá nic odlišného od `wild` |
| `observer` | persist-only - nedělá nic odlišného od `wild` |

Do doby plného vynucení je doporučeno zůstat na `wild`
v důvěryhodném prostředí.

## Související

- [TCP transport](transport-tcp.md) - detail TCP serveru, GUI Start/Stop
- [Python wrapper](python-wrapper.md) - výběr transportu na klient
  straně
- [Přehled tools](tools-overview.md) - které tools jsou sensitive
