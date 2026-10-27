# TCP transport

TCP transport umožňuje **AI klientovi připojit se k běžícímu GUI
emulátoru** přes TCP socket. Na rozdíl od [Pipe transportu](transport-pipe.md)
zde nevzniká nová headless instance - AI sdílí stejnou živou session
jako uživatel sedící u GUI.

## Kdy ho použít

- **"Human in the loop" debug** - vy ladíte v GUI (krokujete,
  prohlížíte paměť, měníte breakpointy), AI paralelně analyzuje
  stav a navrhuje další kroky.
- **Demonstrace** - ukážete někomu, jak AI ovládá emulátor v reálném
  čase, on vidí v GUI co se děje.
- **Reverse engineering session** - AI prochází ROM rutiny, dělá
  anotace; vy ručně přepnete platformu, nahrajete jiný MZF, AI
  pokračuje na novém stavu.
- **Trvající session** - emu neběhá per-MCP-call, stav (snapshot,
  breakpointy, mounty) přežívá mezi voláními.

## Jak zapnout

### V GUI

1. Spusť `mz800emu.exe` normálně (= s GUI).
2. Otevři menu **Tools -> MCP TCP Server**.
3. Klikni **Start**.
4. V title baru hlavního okna se objeví indikátor:

   ```
   mz800emu [MCP:23800 0 clients]
   ```

5. Až se AI klient připojí, počet klientů se aktualizuje:

   ```
   mz800emu [MCP:23800 1 client]
   ```

### Z příkazové řádky (auto-start)

Pokud chcete, aby TCP server naskočil hned při startu emu:

```bash
mz800emu.exe --mcp-tcp-port=23800
```

Alternativně lze nastavit `auto_start_tcp = 1` v INI konfiguraci
(viz [Konfigurace](configuration.md)) - pak se TCP server zapne
automaticky při každém startu emu.

## Indikátor v title baru

Title bar hlavního okna ukazuje aktuální stav TCP serveru:

| Stav title baru | Význam |
|-----------------|--------|
| `mz800emu` | TCP server vypnutý |
| `mz800emu [MCP:23800 0 clients]` | server běží, čeká na klienta |
| `mz800emu [MCP:23800 1 client]` | jeden aktivní klient |
| `mz800emu [MCP:23800 N clients]` | N aktivních klientů (aktuálně max 1) |

## Připojení klienta

Python wrapper se připojí přes env proměnnou `MZ800EMU_TRANSPORT=tcp`:

```bash
export MZ800EMU_TRANSPORT=tcp
export MZ800EMU_TCP_HOST=127.0.0.1
export MZ800EMU_TCP_PORT=23800
python mcp_server.py
```

Tato konfigurace je v `.mcp.json` připravená pod entry `mz800emu-tcp`.
Detaily: [Python wrapper](python-wrapper.md).

## Eventy přes TCP

MCP eventy (`emu_event_subscribe` / `emu_event_poll`) fungují i přes
TCP transport, nejen přes pipe. AI klient připojený k živé GUI session
tak může odebírat topics (`breakpoint_hit`, `paused`, `step_done`,
`io_write`) a vyzvedávat pending eventy stejně jako v pipe režimu.

## Limitace

- **Max 1 klient** - server akceptuje konekci, ale druhý současný
  klient je odmítnut.
- **Pouze localhost** - výchozí `bind_address = 127.0.0.1`. Pro
  povolení remote připojení nastavte `bind_address = 0.0.0.0`, ale
  **bezpečnostní upozornění** níže.
- **Žádná autentizace** - kdokoliv kdo se dostane na bind adresu,
  může emu ovládat.
- **Žádné TLS** - protokol běží jako čistý JSONL (line-delimited
  JSON) bez šifrování.

## Bezpečnostní upozornění

Nastavení `bind_address = 0.0.0.0` zpřístupní MCP server **všem
síťovým rozhraním**. Kdokoliv ve stejné LAN (nebo přes firewall
forwarding i z internetu) může:

- Číst paměť emulátoru (= obsah programů, dat)
- **Přepisovat paměť** přes `mem_write` (= destruktivní operace)
- Pauzovat / restartovat emu
- Nastavovat breakpointy

Bez autentizace to znamená **plný přístup k emu pro kohokoliv ze
sítě**. Zůstaňte na `127.0.0.1` pokud nemáte konkrétní důvod
otevřít remote přístup, a v takovém případě omezte přístup firewallem.

## Související

- [Pipe transport](transport-pipe.md) - alternativní transport bez
  GUI emu
- [Python wrapper](python-wrapper.md) - připojení z Claude Code
- [Konfigurace](configuration.md) - persistentní nastavení TCP
  serveru v INI souboru
