#!/usr/bin/env python3
"""MCP server pro mz800emu - FastMCP wrapper s pipe nebo TCP transportem.

Tento modul implementuje Python MCP (Model Context Protocol) server, který
komunikuje s Claude Code (nebo jiným MCP klientem) přes stdio MCP transport
a dispatchuje příkazy na headless emulátor mz800emu. Komunikace s emu
backendem probíhá přes JSONL (line-delimited JSON) protokol a může běžet
přes dva alternativní transporty:

* ``pipe`` (default) - mcp_server.py spawne ``mz800emu.exe --mcp-pipe``
  jako child subprocess a komunikuje přes jeho stdin/stdout.
* ``tcp`` - mcp_server.py se připojí na běžící GUI emulátor, který má
  zapnutý MCP TCP server (Tools -> MCP TCP Server -> Start, nebo
  ``--mcp-tcp-port=23800`` na příkazové řádce). V tomto módu se
  nespawnuje žádný subprocess - sdílí se jedna live session mezi
  uživatelem (GUI) a AI klientem (MCP).

Výběr transportu je řízený env proměnnou ``MZ800EMU_TRANSPORT``:

    MZ800EMU_TRANSPORT=pipe (default) | tcp
    MZ800EMU_TCP_HOST=127.0.0.1 (pouze tcp)
    MZ800EMU_TCP_PORT=23800 (pouze tcp)
    MZ800EMU_EXE=./mz800emu.exe (pouze pipe)
    MZ800EMU_WORK_DIR=<adresář> (pouze pipe, předá se jako --work-dir)
    MZ800EMU_CFG_DIR=<adresář> (pouze pipe, předá se jako --cfg-dir)

Architektura:

    Claude Code  <-- MCP stdio JSON-RPC -->  mcp_server.py
                                                  |
                                                  | JSONL přes
                                                  | pipe NEBO TCP
                                                  v
                          mz800emu (subprocess --mcp-pipe NEBO běžící GUI)

Stav V1.A.1:

    - Dva transporty (pipe / tcp) přes polymorphic ``_Transport``
      abstrakci.
    - 22 toolů celkem:
        * V0.B.3 (10): emu_status, emu_ping, emu_pause,
          emu_get_registers, emu_mem_read, emu_mem_write (sensitive!),
          emu_run, emu_reset, emu_bp_add, emu_bp_list.
        * V0.B.6 (7): emu_bp_remove, emu_bp_clear, emu_bp_enable,
          emu_step_into, emu_step_over, emu_step_n, emu_run_until_addr.
        * V1.A.1 (5): emu_snapshot_save, emu_snapshot_save_buffer,
          emu_snapshot_load, emu_snapshot_load_buffer,
          emu_cooperation_hint_set.
    - 7 read-only Resources (V0.B.7):
        * emulator://state, emulator://cpu/registers,
          emulator://memory/{addr_hex}/{length} (= template URI),
          emulator://breakpoints, emulator://platform/info (stub),
          emulator://config/mcp, emulator://config/peripherals (stub).
    - Lazy connect - spawn pipe childa NEBO TCP konexe se provede až
      při prvním tool callu, takže discovery (``tools/list``,
      ``resources/list``) z Claude Code je instantní.
    - Žádné EVENT subscribe / TRAP forwarding (= V1.A+).

Logging:

    MCP wire protocol používá stdio (stdin/stdout) pro JSON-RPC zprávy.
    Cokoliv napsaného na stdout/stderr by mohlo MCP wire protokol rozbít
    (Claude Code by JSON-RPC frameování ztratil). Proto je veškerý
    logging směřován do souboru ``mcp_server.log`` v adresáři tohoto
    modulu. Diagnostiku tedy hledej tam, ne v terminálu.

Reference design:

    Inspirováno ``~/projects/ai2-mz800emu/mcp-server/mcp_server.py``
    (791 řádků, FastMCP). Tento V0.B.2 skeleton je výrazně menší
    a slouží jako základ pro V0.B.3+.
"""

import asyncio
import configparser
import contextvars
import functools
import json
import logging
import logging.handlers
import os
import subprocess
from pathlib import Path
from typing import Any, Optional

from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.resources import TextResource


# === Konfigurace ======================================================

# Cesta k mz800emu binárce. Env override umožňuje testovat alternativní
# build (např. release build z jiné worktree). Relevantní pouze pro
# pipe transport - v tcp módu se subprocess nespawnuje.
EMU_EXE: str = os.environ.get(
    "MZ800EMU_EXE",
    str(Path(__file__).resolve().parent.parent / "mz800emu.exe"),
)

# Pracovní adresář spawnutého emulátoru (--work-dir) - kam jdou relativní
# výstupy (CDL export při ukončení, trace, ...). Prázdné = výchozí
# work_dir emulátoru. Relevantní pouze pro pipe transport; používají ho
# testy, aby výstupy nevznikaly v kořeni repa.
EMU_WORK_DIR: str = os.environ.get("MZ800EMU_WORK_DIR", "").strip()

# Konfigurační adresář spawnutého emulátoru (--cfg-dir) - INI, breakpointy,
# symboly, watch, bookmarky. Prázdné = výchozí (adresář binárky, tedy
# konfigurace uživatele). Relevantní pouze pro pipe transport; používají ho
# testy, aby neměnily konfiguraci uživatele (breakpointy z testu by si jinak
# načetlo GUI spuštěné z kořene repa).
EMU_CFG_DIR: str = os.environ.get("MZ800EMU_CFG_DIR", "").strip()

# Výběr transportu: pipe (default) nebo tcp.
TRANSPORT_KIND: str = os.environ.get("MZ800EMU_TRANSPORT", "pipe").lower()

# Konfigurace TCP transportu (= pouze pokud TRANSPORT_KIND == "tcp").
# Default 127.0.0.1:23800 odpovídá V0.A.5 TCP serveru zapnutému přes
# ``--mcp-tcp-port=23800`` nebo Tools -> MCP TCP Server -> Start v GUI.
TCP_HOST: str = os.environ.get("MZ800EMU_TCP_HOST", "127.0.0.1")
TCP_PORT: int = int(os.environ.get("MZ800EMU_TCP_PORT", "23800"))

# Timeout per JSONL request - emu by měl odpovídat řádově do sekundy,
# 30 s je bezpečná horní mez pro startup hello + první příkaz.
SEND_TIMEOUT_S: float = 30.0

# HID nástroje s držením (send_key, send_keys, send_keys_with_delays,
# send_joystick) odpoví až po odehrání celé sekvence, která se měří ve
# snímcích EMULACE. Při normální rychlosti je snímek 20 ms (PAL), při
# zpomalené emulaci (např. zapnuté CDL) víc. Rezerva na snímek proto
# počítá s emulací až 3x pomalejší než reálný čas (60 ms/snímek).
HID_FRAME_TIMEOUT_S: float = 0.06


def _hid_timeout_s(total_frames: int) -> float:
    """Timeout požadavku HID nástroje pro sekvenci dlouhou ``total_frames``.

    Args:
        total_frames: součet snímků držení a mezer celé sekvence.

    Returns:
        ``SEND_TIMEOUT_S`` + ``total_frames`` x ``HID_FRAME_TIMEOUT_S``.
    """
    return SEND_TIMEOUT_S + max(0, int(total_frames)) * HID_FRAME_TIMEOUT_S

# Max délka jednoho JSONL řádku (TCP transport). asyncio.StreamReader má
# default limit 64 KiB; region_read smí číst až 65536 bajtů, base64 z toho
# je ~87.4 KB v jednom řádku -> přesáhne default limit a readline() vyhodí
# ValueError -> reader task umře -> bridge se zasekne (bug 0011). Zvyšujeme
# limit s rezervou pro budoucí bulk čtení. Pipe transport tímto netrpí
# (file.readline() bez limitu).
MAX_JSONL_LINE_BYTES: int = 16 * 1024 * 1024


# === Logging ==========================================================
# MCP server používá stdio = NESMÍ psát na stdout (= MCP wire protocol)
# ani by neměl psát na stderr (Claude Code stderr loguje jinam, ale ne
# vždy bezpečně). Logging proto směřujeme do souboru NEBO úplně tichý
# NullHandler pokud uživatel vypnul log v INI.
#
# Konfigurace se čte z `mz800emu.ini` sekce [MCP] (klíče s prefixem
# `wrapper_log_*`). Default je log vypnutý - žádný soubor se nevytváří.
# INI klíče jsou definované v `src/emulator/mcp/mcp_config.c`.
#
# POZOR: ``logging.basicConfig`` je no-op pokud root logger už má
# nainstalovaný handler. FastMCP / mcp library může během importu
# (řádek ``from mcp.server.fastmcp import FastMCP`` výše) nastavit
# root logger handler na stderr. ``force=True`` (Python 3.8+) explicit
# přebije existující handlery a garantuje, že naše konfigurace platí.

_WRAPPER_LOG_LEVELS = {
    "OFF":     None,          # NullHandler, žádný soubor
    "ERROR":   logging.ERROR,
    "WARNING": logging.WARNING,
    "INFO":    logging.INFO,
    "DEBUG":   logging.DEBUG,
}

# Default cesta k log souboru pokud INI klíč wrapper_log_path je prázdný.
_DEFAULT_LOG_FILE = Path(__file__).resolve().parent / "mcp_server.log"


def _find_ini_path() -> Optional[Path]:
    """Najde mz800emu.ini soubor.

    Hierarchie:
      1. MZ800EMU_INI env override (test scénáře).
      2. <repo_root>/mz800emu.ini, kde repo_root = parent adresáře
         tohoto modulu (mcp-server/.. = worktree root).

    Vrací Path objekt i pokud soubor neexistuje (caller si ověří).
    Vrací None jen při totální chybě (= nikdy v praxi).
    """
    env_ini = os.environ.get("MZ800EMU_INI")
    if env_ini:
        return Path(env_ini)
    return Path(__file__).resolve().parent.parent / "mz800emu.ini"


def _read_ini_mcp_section() -> dict:
    """Načte sekci [MCP] z INI souboru jako dict.

    Tolerantní vůči chybějícímu souboru, chybějící sekci i parsování
    chybám - v takovém případě vrací prázdný dict (= caller padne na
    defaults).
    """
    ini_path = _find_ini_path()
    if ini_path is None or not ini_path.exists():
        return {}
    parser = configparser.ConfigParser()
    try:
        # Neselhat na case sensitivity a duplicate keys.
        parser.optionxform = str  # zachovat case INI klíčů
        parser.read(str(ini_path), encoding="utf-8")
    except (configparser.Error, OSError, UnicodeDecodeError):
        return {}
    if not parser.has_section("MCP"):
        return {}
    return dict(parser.items("MCP"))


def _safe_int(value: Any, default: int) -> int:
    """Bezpečně převede hodnotu na int; vrátí default při selhání."""
    try:
        return int(str(value).strip())
    except (TypeError, ValueError):
        return default


def _build_log_handler(cfg: dict, path: str) -> logging.Handler:
    """Vyrobí logging handler podle rotate_kind. Vrátí FileHandler jako
    safe fallback při neznámé / chybné rotate konfiguraci.

    Pokud handler nelze vytvořit (např. neplatná cesta), zavolá fallback
    `NullHandler` v `_setup_logging`.
    """
    kind = cfg.get("wrapper_log_rotate_kind", "none").strip().lower()
    keep = _safe_int(cfg.get("wrapper_log_rotate_keep"), 5)
    if keep < 0:
        keep = 0
    if kind == "size":
        size_mb = _safe_int(cfg.get("wrapper_log_rotate_size_mb"), 10)
        if size_mb < 1:
            size_mb = 1
        return logging.handlers.RotatingFileHandler(
            path,
            maxBytes=size_mb * 1024 * 1024,
            backupCount=keep,
            encoding="utf-8",
        )
    if kind == "time":
        when = (cfg.get("wrapper_log_rotate_when") or "midnight").strip()
        try:
            return logging.handlers.TimedRotatingFileHandler(
                path,
                when=when,
                backupCount=keep,
                encoding="utf-8",
            )
        except (ValueError, TypeError):
            # Neznámý when token -> safe fallback bez rotace.
            return logging.FileHandler(path, encoding="utf-8")
    # kind == "none" nebo neznámé -> single file bez rotace.
    return logging.FileHandler(path, encoding="utf-8")


def _setup_logging() -> logging.Logger:
    """Nakonfiguruje root logger podle INI sekce [MCP].

    Při OFF nebo selhání nastaví NullHandler (žádný I/O).
    Při validní konfiguraci nainstaluje File / Rotating /
    TimedRotating handler. Idempotentní díky `force=True`.

    Vrací modul-level logger pro use v subprocess setup.
    """
    cfg = _read_ini_mcp_section()
    level_str = cfg.get("wrapper_log_level", "OFF").strip().upper()
    level = _WRAPPER_LOG_LEVELS.get(level_str)

    if level is None:
        # OFF nebo neznámá hodnota -> tichý NullHandler.
        logging.basicConfig(
            level=logging.CRITICAL + 1,
            handlers=[logging.NullHandler()],
            force=True,
        )
        return logging.getLogger("mcp_server")

    # Cesta k log souboru: INI nebo default.
    path = (cfg.get("wrapper_log_path") or "").strip()
    if not path:
        path = str(_DEFAULT_LOG_FILE)

    try:
        handler = _build_log_handler(cfg, path)
        handler.setFormatter(logging.Formatter(
            "%(asctime)s %(levelname)s %(name)s: %(message)s"))
        logging.basicConfig(
            level=level,
            handlers=[handler],
            force=True,
        )
    except (OSError, ValueError):
        # Selhání otevření souboru / handleru -> safe NullHandler.
        # Nelze cokoliv smysluplně zalogovat (=> tichý fail).
        logging.basicConfig(
            level=logging.CRITICAL + 1,
            handlers=[logging.NullHandler()],
            force=True,
        )

    return logging.getLogger("mcp_server")


log = _setup_logging()


# === Wire DEBUG trace ================================================
# Raw JSON-RPC wire trace se loguje JEN na DEBUG úrovni (= TX:/RX:
# řádky). INFO a nižší zůstávají čisté - jen události connect / EOF /
# error - aby se běžný provozní log nezahltil každou zprávou protokolu.
# DEBUG je opt-in přes wrapper_log_level=DEBUG v sekci [MCP] mz800emu.ini.
#
# Limit délky JSONL řádku: payloady jako base64 screenshot nebo mem dump
# mohou mít desítky kB až MB; logovat je celé by nafouklo log o
# megabajty bez užitku. Při překročení se řádek zkrátí a doplní marker
# s původní délkou.
_WIRE_LOG_MAX_CHARS = 500


def _wire_truncate(line: str) -> str:
    """Zkrátí dlouhý JSONL řádek pro DEBUG wire log.

    Args:
        line: surový JSONL řádek (TX nebo RX).

    Returns:
        Původní řádek, pokud je kratší nebo roven ``_WIRE_LOG_MAX_CHARS``;
        jinak prvních ``_WIRE_LOG_MAX_CHARS`` znaků + marker s celkovou
        délkou (= ``...[N chars]``).
    """
    if len(line) <= _WIRE_LOG_MAX_CHARS:
        return line
    return f"{line[:_WIRE_LOG_MAX_CHARS]}...[{len(line)} chars]"


# === FastMCP instance ================================================

mcp = FastMCP("mz800emu")


#: Úložiště oznámení o restartu pro právě běžící tool call. Obal toolu
#: (``_tool_with_restart_notice``) do něj na začátku volání vloží prázdný
#: dict; ``_send_request`` při automatickém restartu zapíše oznámení sem,
#: takže ho dostane právě to volání, které restart vyvolalo (i při souběžně
#: běžících voláních - každé běží ve vlastním asyncio kontextu). Mimo tool
#: call (čtení resource) je hodnota None a oznámení jde do globálního
#: ``_restart_notice`` pro nejbližší tool.
_restart_holder: contextvars.ContextVar[Optional[dict]] = \
    contextvars.ContextVar("mz800emu_restart_holder", default=None)


def _take_restart_notice(holder: Optional[dict]) -> Optional[dict]:
    """Vyzvedne oznámení o restartu pro dokončovaný tool call.

    Přednost má oznámení zapsané do ``holder`` (restart vyvolal tento
    call), jinak čekající globální ``_restart_notice`` (restart vyvolalo
    čtení resource). Vyzvednuté oznámení se smaže.

    Args:
        holder: dict tohoto volání (z ``_restart_holder``), nebo None.

    Returns:
        Oznámení ``{"restarted": True, "restart_reason": str}``, nebo None.
    """
    global _restart_notice
    if holder is not None and holder.get("notice") is not None:
        return holder.pop("notice")
    notice = _restart_notice
    _restart_notice = None
    return notice


def _attach_restart_notice(result: str, notice: Optional[dict]) -> str:
    """Přidá k výsledku toolu oznámení o automatickém restartu emulátoru.

    Tvar výsledku se mění jen při restartu: JSON objekt dostane pole
    ``restarted: true`` a ``restart_reason``; jiný výsledek se zabalí do
    ``{"result": <původní>, "restarted": true, "restart_reason": ...}``.

    Args:
        result: výsledek toolu (JSON text).
        notice: oznámení z ``_take_restart_notice``, nebo None.

    Returns:
        Původní výsledek, nebo výsledek doplněný o oznámení.
    """
    if notice is None:
        return result
    try:
        obj = json.loads(result)
    except (TypeError, ValueError):
        obj = None
    if isinstance(obj, dict):
        obj.update(notice)
        return json.dumps(obj)
    return json.dumps({"result": obj if obj is not None else result,
                       **notice})


_fastmcp_tool_decorator = mcp.tool


def _tool_with_restart_notice(*args: Any, **kwargs: Any):
    """Náhrada ``mcp.tool()``: registruje tool obalený oznámením o restartu.

    Signatura, jméno i docstring toolu zůstávají (``functools.wraps``,
    FastMCP čte signaturu přes ``__wrapped__``), mění se jen výsledek -
    viz ``_attach_restart_notice``. Všechny tooly jsou ``async`` a vracejí
    ``str``. Modulový symbol toolu je obalená funkce (testy ho volají
    přímo).

    Když tool skončí výjimkou (např. požadavek po automatickém restartu
    selže), oznámení se připojí k textu chyby
    (``... [restarted: true; <důvod>]``), aby se neztratilo ani
    nepřesunulo k jinému volání.
    """
    register = _fastmcp_tool_decorator(*args, **kwargs)

    def decorator(fn):
        @functools.wraps(fn)
        async def wrapper(*a: Any, **kw: Any) -> str:
            holder: dict = {}
            token = _restart_holder.set(holder)
            try:
                result = await fn(*a, **kw)
            except Exception as e:
                notice = _take_restart_notice(holder)
                if notice is None:
                    raise
                raise RuntimeError(
                    f"{e} [restarted: true; {notice['restart_reason']}]"
                ) from e
            finally:
                _restart_holder.reset(token)
            return _attach_restart_notice(result,
                                          _take_restart_notice(holder))
        register(wrapper)
        return wrapper

    return decorator


mcp.tool = _tool_with_restart_notice


# === Transport abstrakce =============================================
# Polymorphic API nad spawned subprocess pipe vs TCP socket konexí.
# Reader task volá ``read_line``, ``_send_request`` volá ``send_line``.
# Vlastní spawn / connect / disconnect logika je per-transport.


class _Transport:
    """Abstraktní base pro pipe / tcp transport.

    Definuje minimální API, které potřebuje reader task a
    ``_send_request``. Konkrétní implementace (``_PipeTransport``,
    ``_TcpTransport``) sdílejí společnou strukturu, ale liší se
    podkladovým I/O mechanismem.

    Invarianty:

    - Po ``connect()`` je transport plně funkční (= ``send_line``
      a ``read_line`` neselžou na "not connected").
    - ``disconnect()`` je idempotentní (= safe to call víckrát i bez
      předchozího ``connect``).
    - ``is_alive()`` vrací False pokud transport ztratil konexi (EOF,
      reset, subprocess exit).
    - ``expected_exit`` je True, jen když konec backendu vyvolal sám
      wrapper (``emu_stop``, ``_shutdown_emu``); jinak je konec nečekaný
      a další volání to klientovi oznámí (``restarted``).
    """

    #: True = konec backendu vyžádal wrapper (emu_stop / shutdown).
    expected_exit: bool = False

    async def connect(self) -> None:
        """Naváže konexi (spawn subprocess NEBO open socket)."""
        raise NotImplementedError

    async def disconnect(self) -> None:
        """Uzavře konexi (kill subprocess NEBO close socket)."""
        raise NotImplementedError

    async def send_line(self, line: str) -> None:
        """Pošle jeden JSONL řádek (bez trailing newline) emu backendu.

        Implementace přidá ``\\n`` na konec.
        """
        raise NotImplementedError

    async def read_line(self) -> Optional[str]:
        """Přečte jeden JSONL řádek (bez trailing newline) z emu backendu.

        Vrací ``None`` při EOF (= transport ztratil konexi).
        Pro legitimní prázdný řádek vrací ``""`` (= reader task ho
        skipuje stejně jako neJSONL řádek).
        """
        raise NotImplementedError

    def is_alive(self) -> bool:
        """Vrátí True dokud je konexe živá."""
        raise NotImplementedError

    async def exit_reason(self) -> str:
        """Popis, proč backend skončil (pro chybové hlášky, anglicky)."""
        return "connection closed"


class _PipeTransport(_Transport):
    """Transport spawnující ``mz800emu.exe --mcp-pipe`` jako subprocess.

    Komunikace přes stdin (REQUEST) a stdout (RESPONSE + EVENT) child
    procesu. Stderr je směrován do ``DEVNULL`` - emu může vypisovat
    volnoformátové banery, které by mátly JSONL parser, ale stderr
    nesleduje.

    Lifetime: ``connect()`` spawne proces, ``disconnect()`` ho
    terminuje (= ``shutdown`` JSONL příkaz NENÍ poslán z transportu;
    o to se stará ``_shutdown_emu``).
    """

    def __init__(self, exe_path: str) -> None:
        self.exe_path = exe_path
        self.process: Optional[subprocess.Popen] = None
        # Loop pro run_in_executor (= readline je blokující sync API,
        # musíme ho odlehčit do thread poolu).
        self._loop: Optional[asyncio.AbstractEventLoop] = None

    async def connect(self) -> None:
        exe = Path(self.exe_path)
        if not exe.is_file():
            raise RuntimeError(
                f"mz800emu binary not found at {self.exe_path} "
                f"(set MZ800EMU_EXE env var to override)"
            )
        args = [str(exe), "--mcp-pipe", "--headless", "--no-first-run-windows"]
        if EMU_WORK_DIR:
            args.append(f"--work-dir={EMU_WORK_DIR}")
        if EMU_CFG_DIR:
            args.append(f"--cfg-dir={EMU_CFG_DIR}")
        log.info("spawning emu: %s", " ".join(args))
        self.process = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
            cwd=str(exe.parent),
        )
        self._loop = asyncio.get_event_loop()

    async def disconnect(self) -> None:
        if self.process is None:
            return
        try:
            self.process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            log.warning("emu didn't exit in 5s, killing")
            self.process.kill()
            try:
                self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                pass
        self.process = None

    async def send_line(self, line: str) -> None:
        if self.process is None or self.process.stdin is None:
            raise RuntimeError("pipe transport not connected")
        try:
            self.process.stdin.write(line + "\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError) as e:
            raise RuntimeError(f"emu stdin write failed: {e}") from e

    async def read_line(self) -> Optional[str]:
        if self.process is None or self.process.stdout is None:
            return None
        loop = self._loop or asyncio.get_event_loop()
        # readline je blokující sync volání - musíme ho odlehčit do
        # thread poolu, aby neblokoval event loop.
        line = await loop.run_in_executor(
            None, self.process.stdout.readline)
        if not line:
            # Empty string z readline = EOF (file closed). Legitimní
            # prázdný řádek by byl alespoň "\n".
            return None
        return line.rstrip("\r\n")

    def is_alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    async def exit_reason(self) -> str:
        """Popis konce child procesu včetně exit kódu.

        Po EOF na stdout proces obvykle už skončil, ale exit kód nemusí
        být ještě k dispozici - čeká se na něj nejvýše 2 s (v thread
        poolu, neblokuje event loop). Exit kód nad 0x7FFFFFFF (Windows
        NTSTATUS, např. 0xC0000005 = access violation) nebo záporný (signál
        na POSIX) se vypíše i hexa.

        Returns:
            Např. ``"exit code 3221225477 = 0xC0000005"``,
            ``"exit code 0"`` nebo ``"process still running, stdout closed"``.
        """
        proc = self.process
        if proc is None:
            return "process not running"
        rc = proc.poll()
        if rc is None:
            loop = asyncio.get_event_loop()
            try:
                rc = await loop.run_in_executor(
                    None, functools.partial(proc.wait, timeout=2.0))
            except subprocess.TimeoutExpired:
                return "process still running, stdout closed"
        if rc > 0x7FFFFFFF or rc < 0:
            return f"exit code {rc} = 0x{rc & 0xFFFFFFFF:08X}"
        return f"exit code {rc}"


class _TcpTransport(_Transport):
    """Transport připojující se na běžící GUI emu přes TCP socket.

    GUI emu musí mít zapnutý MCP TCP server (Tools -> MCP TCP Server
    -> Start v GUI, NEBO ``--mcp-tcp-port=23800`` na příkazové řádce).
    V0.A.5 implementuje server, který akceptuje stejný JSONL wire
    formát jako pipe transport - protokolová parita je proto plná.

    Lifetime: ``connect()`` udělá ``asyncio.open_connection``,
    ``disconnect()`` uzavře writer a počká na flush.

    Pozn.: V tcp módu se nespawnuje subprocess - sdílí se jedna live
    session mezi uživatelem (GUI) a AI klientem (MCP). To je vhodné
    pro "human in the loop" debugging.
    """

    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port
        self.reader: Optional[asyncio.StreamReader] = None
        self.writer: Optional[asyncio.StreamWriter] = None
        self._alive: bool = False

    async def connect(self) -> None:
        log.info("connecting to emu TCP: %s:%d", self.host, self.port)
        try:
            # limit= zvyšuje StreamReader buffer z default 64 KiB na
            # MAX_JSONL_LINE_BYTES - velké region_read response (base64
            # 64 KB regionu ~87 KB) jinak přesáhnou limit a readline()
            # vyhodí ValueError (bug 0011).
            self.reader, self.writer = await asyncio.open_connection(
                self.host, self.port, limit=MAX_JSONL_LINE_BYTES)
        except (ConnectionRefusedError, OSError) as e:
            raise RuntimeError(
                f"TCP connect to {self.host}:{self.port} failed: {e} "
                f"(je v GUI emu zapnutý MCP TCP Server?)"
            ) from e
        self._alive = True

    async def disconnect(self) -> None:
        if self.writer is None:
            return
        try:
            self.writer.close()
            await self.writer.wait_closed()
        except Exception as e:
            log.warning("TCP writer close: %s", e)
        self.writer = None
        self.reader = None
        self._alive = False

    async def send_line(self, line: str) -> None:
        if self.writer is None:
            raise RuntimeError("tcp transport not connected")
        try:
            self.writer.write((line + "\n").encode("utf-8"))
            await self.writer.drain()
        except (ConnectionError, OSError) as e:
            self._alive = False
            raise RuntimeError(f"emu TCP write failed: {e}") from e

    async def read_line(self) -> Optional[str]:
        if self.reader is None:
            return None
        try:
            raw = await self.reader.readline()
        except (asyncio.IncompleteReadError, ConnectionError, OSError) as e:
            log.info("TCP read EOF/error: %s", e)
            self._alive = False
            return None
        except ValueError as e:
            # readline() re-raisuje LimitOverrunError jako ValueError, když
            # řádek přesáhne StreamReader limit. Buffer je tím zničený, takže
            # transport označíme za mrtvý -> reader task skončí čistě a
            # _send_request se při dalším volání reconnectne (místo aby
            # bridge tiše zůstal zaseknutý s is_alive()==True, bug 0011).
            log.error("TCP read line over limit (%d B): %s",
                      MAX_JSONL_LINE_BYTES, e)
            self._alive = False
            return None
        if not raw:
            # EOF na socketu.
            self._alive = False
            return None
        return raw.decode("utf-8", errors="replace").rstrip("\r\n")

    def is_alive(self) -> bool:
        return self._alive

    async def exit_reason(self) -> str:
        """Popis konce TCP spojení (exit kód GUI emulátoru není znám)."""
        return f"TCP connection to {self.host}:{self.port} closed"


def _create_transport() -> _Transport:
    """Vytvoří správnou transport instanci podle ``MZ800EMU_TRANSPORT``.

    Returns:
        Konkrétní instance ``_PipeTransport`` nebo ``_TcpTransport``.

    Raises:
        RuntimeError: pokud ``MZ800EMU_TRANSPORT`` má neznámou hodnotu.
    """
    if TRANSPORT_KIND == "tcp":
        return _TcpTransport(TCP_HOST, TCP_PORT)
    if TRANSPORT_KIND == "pipe":
        return _PipeTransport(EMU_EXE)
    raise RuntimeError(
        f"MZ800EMU_TRANSPORT={TRANSPORT_KIND!r} neznámý "
        f"(očekávané: pipe, tcp)"
    )


# === Globální stav bridge ============================================
# Stav transportu a JSONL kanálu. Všechny tyto proměnné jsou modifikované
# pouze z event loopu FastMCP (= jediný worker thread), takže nepotřebují
# explicitní lock, kromě _send_lock pro serializaci write na transport
# (= bezpečnost vůči souběžným tool callům).

_transport: Optional[_Transport] = None
_emu_hello: dict[str, Any] = {}
_reader_task: Optional[asyncio.Task] = None
_response_queue: Optional[asyncio.Queue] = None
_event_queue: Optional[asyncio.Queue] = None
_next_req_id: int = 1
_send_lock: Optional[asyncio.Lock] = None

#: Popis posledního NEČEKANÉHO konce backendu (pád procesu, zavřené TCP),
#: nebo None. Nastavuje reader task, maže ho automatický restart
#: v ``_send_request`` a ``emu_start``. ``emu_status`` ho hlásí jako
#: ``last_exit``.
_last_exit: Optional[str] = None

#: Oznámení, že ``_send_request`` po nečekaném konci backendu automaticky
#: spustil nový emulátor (pipe) nebo znovu navázal TCP spojení, pokud restart
#: vyvolalo volání mimo tool (čtení resource). Vyzvedne ho nejbližší
#: dokončený tool. Restart vyvolaný toolem jde přes ``_restart_holder``.
_restart_notice: Optional[dict[str, Any]] = None

#: Klíč sentinelu, který reader task při konci transportu vloží do front.
_TRANSPORT_CLOSED_KEY = "_transport_closed"


# === Bridge core =====================================================


async def _stdout_reader_task() -> None:
    """Asynchronní background task čtoucí JSONL řádky z transportu.

    Běží po celou dobu života transportu (= dokud transport hlásí
    ``is_alive``). Pro každý řádek:

    - Skipuje prázdné řádky a non-JSONL řádky (= emu během startupu
      vypisuje volnoformátové ``g_print``/``printf`` banery, viz
      lessons learned z V0.A.4 ``test_pipe.py``). V TCP módu by banery
      přicházet neměly (jen JSONL frames), ale pro robustnost filtr
      necháváme.
    - Parsuje JSON. Pokud zpráva obsahuje pole ``req_id``, jde
      o synchronní response na náš request a putneme ji do
      ``_response_queue``. Jinak jde o asynchronní event (TRAP,
      heartbeat, ...) a putneme ji do ``_event_queue`` (zatím
      nevyužito, čeká na V0.B.3+ EVENT subscribe).

    Side effects:

    - Modifikuje ``_response_queue`` / ``_event_queue``.
    - Při EOF (= emu skončil / TCP konexe shozená) task spontánně končí
      a do obou front vloží sentinel s chybou (``_signal_transport_closed``),
      takže čekající ``_send_request`` / čtení hello skončí hned chybou
      "emulator process exited ..." místo timeoutu. Nečekaný konec se
      zapíše do ``_last_exit``.

    Postconditions:

    - Po skončení už nikdo nebude readline-ovat z transportu.
    """
    global _transport
    log.info("stdout reader task started")
    # Fronty a transport tohoto spojení - _ensure_connected je při
    # reconnectu nahradí novými, sentinel ale patří do těchto.
    transport = _transport
    resp_queue = _response_queue
    event_queue = _event_queue
    try:
        # Čte se až do EOF (ne do is_alive()): proces může skončit dřív,
        # než reader vybere poslední odpovědi z bufferu stdout (např.
        # odpověď na shutdown / emu_stop). Čte se z transportu tohoto
        # spojení, ne z globálního _transport (ten mohl reconnect nahradit).
        while transport is not None:
            line = await transport.read_line()
            if line is None:
                # EOF na transportu - emu skončil nebo TCP konexe
                # shozená.
                log.info("transport EOF, reader task exiting")
                break
            if not line or not line.startswith("{"):
                # Prázdný řádek nebo volnoformátový banner z emu
                # startupu, viz test_pipe.py vzor. Mlčky ignorujeme.
                continue
            # RX wire trace - raw příchozí JSONL řádek, jen na DEBUG.
            # Logujeme tady v readeru (= jediný čtecí bod), takže
            # zachytíme i async broadcasty (MCP_ACTION) bez req_id,
            # nejen synchronní response v _send_request.
            log.debug("RX: %s", _wire_truncate(line))
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                log.warning("nelze parsovat JSONL: %r", line[:200])
                continue

            if "req_id" in msg:
                if _response_queue is not None:
                    await _response_queue.put(msg)
            else:
                if _event_queue is not None:
                    await _event_queue.put(msg)
    except asyncio.CancelledError:
        log.info("stdout reader task cancelled")
        raise
    except Exception as e:
        log.exception("stdout reader task selhal: %s", e)
    await _signal_transport_closed(transport, resp_queue, event_queue)


async def _signal_transport_closed(transport: Optional[_Transport],
                                   resp_queue: Optional[asyncio.Queue],
                                   event_queue: Optional[asyncio.Queue]) -> None:
    """Oznámí konec transportu čekajícím požadavkům (sentinel do front).

    Do response i event fronty spojení vloží zprávu
    ``{"_transport_closed": True, "error": "emulator process exited
    (...)"}``. ``_send_request`` i ``_read_hello_with_filter`` ji převedou
    na ``RuntimeError``. Pokud konec nevyžádal wrapper
    (``transport.expected_exit`` je False), zapíše popis do ``_last_exit``.

    Args:
        transport: transport, jehož reader právě skončil.
        resp_queue: response fronta tohoto spojení.
        event_queue: event fronta tohoto spojení.

    Side effects:

    - Plní fronty, může nastavit ``_last_exit``, loguje.
    """
    global _last_exit
    if transport is None:
        return
    reason = await transport.exit_reason()
    if isinstance(transport, _TcpTransport):
        error = f"emulator connection closed ({reason})"
    else:
        error = f"emulator process exited ({reason})"
    sentinel = {_TRANSPORT_CLOSED_KEY: True, "error": error}
    for q in (resp_queue, event_queue):
        if q is not None:
            q.put_nowait(dict(sentinel))
    if transport.expected_exit:
        log.info("backend ended as requested: %s", error)
    else:
        _last_exit = error
        log.warning("backend ended unexpectedly: %s", error)


async def _ensure_connected() -> None:
    """Lazy navázání transportu k emu backendu.

    Volá se z ``_send_request`` při prvním tool callu. Pokud transport
    už existuje a je živý, vrátí se okamžitě (= idempotentní).

    Postup:

    1. Inicializace front a locku.
    2. Vytvoření transport instance přes ``_create_transport()``.
    3. ``transport.connect()`` (spawn subprocess NEBO open TCP).
    4. Start ``_stdout_reader_task`` v event loopu.
    5. Čtení uvítací ``hello`` zprávy (= první JSONL řádek z emu).

    Preconditions:

    - Při pipe: ``EMU_EXE`` musí existovat a být spustitelný.
    - Při tcp: ``TCP_HOST:TCP_PORT`` musí akceptovat konexi (= GUI emu
      má zapnutý MCP TCP Server).

    Postconditions:

    - ``_transport`` != None, ``is_alive()`` True.
    - ``_emu_hello`` obsahuje hello payload z emu (``type=hello``,
      ``commands=[...]``).
    - ``_reader_task`` aktivně běží.

    Side effects:

    - Pipe: spawnuje OS proces.
    - TCP: otevírá síťovou konexi.
    - Loguje do ``mcp_server.log``.
    """
    global _transport, _emu_hello, _reader_task
    global _response_queue, _event_queue, _send_lock

    if _transport is not None and _transport.is_alive():
        return  # už připojeno, idempotentní

    _response_queue = asyncio.Queue()
    _event_queue = asyncio.Queue()
    _send_lock = asyncio.Lock()

    _transport = _create_transport()
    log.info("transport kind: %s", TRANSPORT_KIND)
    await _transport.connect()

    # Start background reader task v aktuálním event loopu.
    _reader_task = asyncio.create_task(_stdout_reader_task())

    # Čteme uvítací hello zprávu. Emu může před hello vypsat banery
    # (viz V0.A.4 test_pipe.py), proto čteme přes _read_hello_with_filter.
    _emu_hello = await _read_hello_with_filter(timeout_sec=15.0)
    log.info("emu hello: type=%s, commands=%d",
             _emu_hello.get("type"),
             len(_emu_hello.get("commands", [])))


async def _read_hello_with_filter(timeout_sec: float) -> dict[str, Any]:
    """Přečte první JSONL zprávu z transportu, přeskakuje banery.

    Speciální helper pro startup fázi - reader task musí být spuštěný,
    ale hello zpráva ještě nebyla doručena. Hello přichází mimo response
    queue (nemá ``req_id``), takže ji vyzvedáváme z event queue.

    Args:
        timeout_sec: max doba čekání na hello v sekundách.

    Returns:
        Dict s hello payload (``type``, ``commands``, ...).

    Raises:
        TimeoutError: pokud hello nedorazí do timeout.
        RuntimeError: pokud backend skončil dřív, než hello poslal.
    """
    if _event_queue is None:
        raise RuntimeError("_event_queue not initialized")
    try:
        msg = await asyncio.wait_for(_event_queue.get(), timeout=timeout_sec)
    except asyncio.TimeoutError:
        raise TimeoutError(
            f"emu hello timeout after {timeout_sec}s")
    if msg.get(_TRANSPORT_CLOSED_KEY):
        raise RuntimeError(f"{msg['error']} before hello")
    return msg


def _with_rev(data: dict, expected_state_rev: int | None) -> dict:
    """Doplní do parametrů podmínku revize stavu ``expected_state_rev``.

    Args:
        data: parametry požadavku (změní se a vrátí).
        expected_state_rev: revize stavu, ``None`` = bez podmínky.

    Returns:
        ``data`` s klíčem ``expected_state_rev``, je-li zadaný.
    """
    if expected_state_rev is not None:
        data["expected_state_rev"] = expected_state_rev
    return data


async def _send_request(cmd: str,
                        data: Optional[dict] = None,
                        timeout_s: Optional[float] = None) -> dict[str, Any]:
    """Pošle JSONL REQUEST emu backendu a čeká na synchronní response.

    Pokud transport ještě není navázaný, lazy ho připojí přes
    ``_ensure_connected``.

    Args:
        cmd: jméno příkazu (musí být v dispatch cmd_map emu - viz
            ``src/emulator/mcp/dispatch.c``).
        data: volitelný payload (např. ``{"addr": 0xE800, "len": 16}``
            pro ``mem_read``).
        timeout_s: limit čekání na response v sekundách; ``None`` =
            ``SEND_TIMEOUT_S``. Delší limit potřebují příkazy, které
            odpoví až po odehrání sekvence (HID, viz ``_hid_timeout_s``).

    Returns:
        Dict s wire formátem response: ``{"req_id": N, "success": bool,
        "data": {...}, "error": "..."?}``.

    Raises:
        RuntimeError: timeout, konec backendu během čekání (hned, ne až
            po timeoutu: "emulator process exited (exit code ...)"),
            out-of-order response, nebo connect failure.

    Side effects:

    - Inkrementuje ``_next_req_id``.
    - Zapisuje na transport pod ``_send_lock`` (serializace souběžných
      tool callů).
    - Pokud předchozí backend nečekaně skončil, spustí nový emulátor
      (pipe) / znovu se připojí (tcp) a připraví oznámení o restartu:
      do ``_restart_holder`` právě běžícího toolu (klient ho uvidí ve
      výsledku nebo chybě tohoto volání jako ``restarted: true``), mimo
      tool call do ``_restart_notice`` pro nejbližší tool.
    """
    global _next_req_id, _last_exit, _restart_notice

    if _transport is None or not _transport.is_alive():
        prev = _transport
        lost: Optional[str] = None
        if prev is not None and not prev.expected_exit:
            # Reader task nemusel EOF ještě zpracovat - důvod případně
            # zjistíme přímo z mrtvého transportu.
            lost = _last_exit or (await prev.exit_reason())
        await _ensure_connected()
        if lost is not None:
            _last_exit = None
            what = ("reconnected to the emulator (the GUI emulator keeps "
                    "its own state)"
                    if isinstance(_transport, _TcpTransport)
                    else "started a fresh emulator process "
                         "(RAM, registers, breakpoints and inserted media "
                         "were reset)")
            notice = {
                "restarted": True,
                "restart_reason": f"previous backend ended unexpectedly "
                                  f"({lost}); {what}",
            }
            holder = _restart_holder.get()
            if holder is not None:
                holder["notice"] = notice
            else:
                _restart_notice = notice
            log.warning("auto-restart after unexpected backend end: %s",
                        lost)

    assert _send_lock is not None
    assert _response_queue is not None
    assert _transport is not None

    async with _send_lock:
        req_id = _next_req_id
        _next_req_id += 1

        req: dict[str, Any] = {"req_id": req_id, "cmd": cmd}
        if data is not None:
            req["data"] = data

        req_line = json.dumps(req)
        # TX wire trace - raw odchozí JSONL request, jen na DEBUG úrovni.
        log.debug("TX: %s", _wire_truncate(req_line))
        await _transport.send_line(req_line)

        # Protokol je synchronní (jeden request naráz pod _send_lock),
        # takže response se správným req_id musí přijít jako další. Pokud
        # ale dřívější request vytimeoutoval a jeho pozdní response dorazila
        # až teď, zůstala ve frontě a způsobila by trvalý off-by-one desync
        # (bug 0011). Stale response (req_id != náš) proto zahazujeme a
        # čekáme dál až do společného deadline - self-heal desyncu.
        loop = asyncio.get_event_loop()
        limit_s = SEND_TIMEOUT_S if timeout_s is None else float(timeout_s)
        deadline = loop.time() + limit_s
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise RuntimeError(
                    f"emu request timeout after {limit_s:g}s (cmd={cmd})")
            try:
                resp = await asyncio.wait_for(
                    _response_queue.get(), timeout=remaining)
            except asyncio.TimeoutError as e:
                raise RuntimeError(
                    f"emu request timeout after {limit_s:g}s (cmd={cmd})"
                ) from e
            if resp.get(_TRANSPORT_CLOSED_KEY):
                # Backend skončil (pád procesu / zavřené TCP) - odpověď
                # nepřijde, nečekáme do timeoutu.
                if isinstance(_transport, _TcpTransport):
                    hint = ("the GUI emulator may still be running with its "
                            "state; the next call reconnects")
                else:
                    hint = ("emulator state is lost; the next call starts "
                            "a fresh emulator")
                raise RuntimeError(
                    f"{resp['error']} while waiting for the response "
                    f"(cmd={cmd}); {hint}")
            # Pozn.: RX wire trace se loguje v _stdout_reader_task (= jediný
            # čtecí bod), ne tady, aby se response neloggovala dvakrát a aby
            # se zachytily i async broadcasty bez req_id.
            if resp.get("req_id") == req_id:
                return resp
            log.warning("discarding stale response: expected %d, got %s",
                        req_id, resp.get("req_id"))


def _data_or_error(resp: dict[str, Any]) -> dict[str, Any]:
    """Vrátí ``resp["data"]``, nebo error dict když data chybí.

    Wire response na úspěch nese ``data``, na chybu jen ``error`` (bez
    ``data``). Tool wrappery dřív dělaly ``resp.get("data", {})``, což
    error tiše převedlo na holé ``{}`` - caller selhání nepoznal (bug
    0011). Tento helper error surfacuje.

    Args:
        resp: wire response z ``_send_request``.

    Returns:
        ``resp["data"]`` pokud existuje, jinak ``{"error": ...}`` se
        zprávou z ``resp["error"]`` (fallback generická hláška).
    """
    if "data" in resp:
        return resp["data"]
    return {"error": resp.get("error", "emu returned no data")}


def _write_cond_payload(payload: dict[str, Any],
                        expected_sha256: Optional[str],
                        want_hashes: bool) -> Optional[str]:
    """Doplní do požadavku zápisu podmínku ``expected_sha256`` a ``want_hashes``.

    Podmíněný zápis (observe-cas M10): emulátor zapíše jen tehdy, když
    SHA-256 přepisovaného rozsahu souhlasí s ``expected_sha256``; kontrola
    i zápis proběhnou v emu vlákně v jednom příkazu.

    Args:
        payload: data požadavku (doplní se na místě).
        expected_sha256: 64 hex znaků, nebo None (bez podmínky).
        want_hashes: True = vrátit ``before_sha256`` / ``after_sha256``.

    Returns:
        None = OK, jinak text chyby pro klienta (anglicky).
    """
    if expected_sha256 is not None:
        if (not isinstance(expected_sha256, str) or len(expected_sha256) != 64
                or any(c not in "0123456789abcdefABCDEF" for c in expected_sha256)):
            return "expected_sha256 must be 64 hex digits"
        payload["expected_sha256"] = expected_sha256
    if want_hashes:
        payload["want_hashes"] = True
    return None


async def _shutdown_emu() -> None:
    """Graceful shutdown emu backendu a uzavření transportu.

    V pipe módu pošle ``shutdown`` příkaz, počká max 5 s na exit
    child procesu, jinak kill.
    V TCP módu jen uzavře socket - GUI emu pokračuje (= nesahá se na
    "user's" instanci).

    Idempotentní (= safe to call víckrát).
    """
    global _transport, _reader_task

    if _transport is None:
        return

    # Konec backendu vyžádal wrapper - reader ho nesmí hlásit jako pád.
    _transport.expected_exit = True

    # V pipe módu posíláme shutdown (= killne child). V TCP módu NE -
    # nechceme killnout live GUI session uživatele.
    if isinstance(_transport, _PipeTransport):
        try:
            await asyncio.wait_for(_send_request("shutdown"), timeout=5.0)
        except Exception as e:
            log.warning("shutdown request failed: %s", e)

    try:
        await _transport.disconnect()
    except Exception as e:
        log.warning("transport disconnect failed: %s", e)
    _transport = None

    if _reader_task is not None:
        _reader_task.cancel()
        try:
            await _reader_task
        except (asyncio.CancelledError, Exception):
            pass
        _reader_task = None


# === MCP Tools (V0.B.6 - 17 handlerů celkem) ==========================
# Tool descriptions jsou anglicky - MCP wire protocol je locale-agnostic
# a Claude klient zobrazuje description uživateli (= anglicky per
# CLAUDE.md i18n pravidla).
#
# Skupiny:
#   V0.B.3 (10): emu_status, emu_ping, emu_pause, emu_get_registers,
#                emu_mem_read, emu_mem_write (sensitive!), emu_run,
#                emu_reset, emu_bp_add, emu_bp_list.
#   V0.B.6 (7):  emu_bp_remove, emu_bp_clear, emu_bp_enable,
#                emu_step_into, emu_step_over, emu_step_n,
#                emu_run_until_addr.


@mcp.tool()
async def emu_status() -> str:
    """Get emulator backend status.

    Returns JSON with fields ``running`` (bool), ``paused`` (bool),
    ``run_seq``, ``state_rev`` (revision of the machine state, from the
    same moment as ``running``; see ``emulator://docs/state_revision``)
    and optionally other state fields when emulator is alive. If the transport has not been connected yet or
    has been closed, returns ``{"running": false, "connected": false}``;
    when the backend ended unexpectedly (emulator process crashed or
    exited, TCP connection dropped), the field ``last_exit`` describes it
    (e.g. ``"emulator process exited (exit code 3221225477 =
    0xC0000005)"``). The next tool call that needs the emulator starts a
    fresh one and its result carries ``restarted: true``.
    """
    if _transport is None or not _transport.is_alive():
        status: dict[str, Any] = {"running": False, "connected": False}
        if _last_exit is not None:
            status["last_exit"] = _last_exit
        return json.dumps(status)

    resp = await _send_request("get_state")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_ping() -> str:
    """Ping the emulator backend (liveness check).

    Sends a ``ping`` command over the JSONL transport and returns the
    emulator response (typically ``{"pong": true}``). Triggers lazy
    transport connect (subprocess spawn in pipe mode, TCP open in tcp
    mode) on first call.
    """
    resp = await _send_request("ping")
    return json.dumps(_data_or_error(resp))


def _deadline_arg(deadline_frames: int | None, deadline_ms: int | None,
                  deadline_ticks: int | None) -> tuple[dict | None, str | None]:
    """Sestaví parametr ``deadline`` z nejvýš jedné z hodnot (frames / ms / ticks).

    Returns:
        (deadline dict nebo None, chybová hláška nebo None).
    """
    given = [(k, v) for k, v in (("frames", deadline_frames),
                                 ("emulated_ms", deadline_ms),
                                 ("ticks", deadline_ticks)) if v is not None]
    if not given:
        return None, None
    if len(given) > 1:
        return None, ("give at most one of deadline_frames, deadline_ms, "
                      "deadline_ticks")
    key, value = given[0]
    if not isinstance(value, int) or value < 1:
        return None, f"deadline {key} must be a positive integer"
    return {key: value}, None


@mcp.tool()
async def emu_pause(if_run_seq: int | None = None) -> str:
    """Pause emulation (stop the CPU instruction stream).

    Returns ``{"paused": true, "was_running": <bool>, "paused_now": <bool>,
    "run_seq": <int>}``. Idempotent (safe to call when emulator is already
    paused; a pause of a paused emulator is not a new stop and sends no
    ``paused`` event).

    With ``if_run_seq`` the call pauses only while run number
    ``if_run_seq`` is running (the number returned by ``emu_resume``,
    ``emu_run_until_addr``, ``emu_step_out`` or ``emu_get_state``). It
    never stops a run that someone else started meanwhile, e.g. the user
    in the GUI; then ``paused`` is false and ``skipped`` is
    ``"run_seq_mismatch"`` (or ``"not_running"``).

    Args:
        if_run_seq: pause only this run (optional).
    """
    args: dict[str, Any] = {}
    if if_run_seq is not None:
        if not isinstance(if_run_seq, int) or if_run_seq < 0:
            return json.dumps({"error": "if_run_seq must be >= 0"})
        args["if_run_seq"] = if_run_seq
    resp = await _send_request("pause", args or None)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_resume(deadline_frames: int | None = None,
                     deadline_ms: int | None = None,
                     deadline_ticks: int | None = None) -> str:
    """Resume emulation without blocking (free run), optionally with a deadline.

    Returns at once with ``{"running": true, "run_seq": <int>,
    "already_running": <bool>}``. The run continues until a breakpoint,
    a pause or the deadline stops it. Wait for the stop with
    ``emu_wait_stop(run_seq)``; the stop also sends the event ``paused``.

    A deadline limits the run in emulated time (not wall clock) and is
    deterministic: ``deadline_frames`` stops exactly at the N-th frame
    boundary, ``deadline_ms`` / ``deadline_ticks`` stop at the first
    emulation event point at or after the deadline (less than one frame
    late). The stop has the reason ``"deadline"``. A deadline holds until
    the next pause. Give at most one of the three.

    Args:
        deadline_frames: stop after N frame boundaries (1..1000000).
        deadline_ms: stop after N ms of emulated time (1..86400000).
        deadline_ticks: stop after N GDG clock ticks.

    Returns:
        JSON as above plus ``deadline_ticks`` (deadline length in GDG ticks)
        when a deadline was given.
    """
    deadline, err = _deadline_arg(deadline_frames, deadline_ms, deadline_ticks)
    if err:
        return json.dumps({"error": err})
    args: dict[str, Any] = {"frames": 0}
    if deadline is not None:
        args["deadline"] = deadline
    resp = await _send_request("run", args)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_wait_stop(run_seq: int | None = None,
                        timeout_ms: int = 5000) -> str:
    """Wait until a run of the emulator stops, and tell why and where.

    A run is the time between leaving a pause (resume, a step, a run to an
    address) and the next pause. Every run has a number ``run_seq``
    (returned by ``emu_resume``, ``emu_run_until_addr``, ``emu_step_out``,
    ``emu_step_into``, ``emu_step_over``, ``emu_pause``, ``emu_get_state``).
    Without ``run_seq`` the tool waits for the current run (or returns the
    last one when the emulator is paused).

    Returns ``{"done": true, "run_seq", "record": {...}}`` once the run
    stopped, or ``{"done": false, "running": true, ...}`` after
    ``timeout_ms``; then call the tool again. The record has ``reason``
    (``"run_to"``, ``"breakpoint"``, ``"deadline"``, ``"frames"``,
    ``"step"``, ``"manual"``, ``"exit"``, ``"bp_action_saturation"``,
    ``"event_trigger"``, ``"replay"``, ``"replay_end"``, ``"replay_desync"``,
    ``"replay_seek"``, ``"replay_goto"``, ``"run_until_ticks"``),
    ``pc``, ``bp_id`` (breakpoint), ``started_by`` /
    ``start_cmd`` (who started the run: ``"mcp"`` or ``"user"`` in the
    GUI), ``stopped_by`` (who paused it, for ``"manual"``),
    ``start_ticks`` / ``stop_ticks`` (GDG ticks since the emulator started -
    a machine reset does not restart them; same as ``Cycle`` in breakpoint
    expressions), ``elapsed_ticks`` and ``frame``.
    ``history_lost: true`` means the run stopped but its record is no longer
    in the history (32 newest).

    Args:
        run_seq: run number to wait for (optional).
        timeout_ms: longest wait in ms (0..25000, default 5000).
    """
    if not isinstance(timeout_ms, int) or timeout_ms < 0 or timeout_ms > 25000:
        return json.dumps({"error": "timeout_ms must be in range 0..25000"})
    args: dict[str, Any] = {"timeout_ms": timeout_ms}
    if run_seq is not None:
        if not isinstance(run_seq, int) or run_seq < 0:
            return json.dumps({"error": "run_seq must be >= 0"})
        args["run_seq"] = run_seq
    resp = await _send_request(
        "wait_stop", args, timeout_s=SEND_TIMEOUT_S + timeout_ms / 1000.0)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_get_stop_history(limit: int = 32) -> str:
    """Return the newest stop records of the emulator (oldest first).

    Each record describes one run: why and where it stopped, who started
    it (``started_by``: ``"mcp"`` for the agent, ``"user"`` for the GUI)
    and how long it ran, see ``emu_wait_stop``. Use it to notice that the
    user started or paused the emulator in the GUI. Also available as the
    resource ``emulator://run/history``.

    Args:
        limit: number of newest records (1..32, default 32).
    """
    if not isinstance(limit, int) or limit < 1 or limit > 32:
        return json.dumps({"error": "limit must be in range 1..32"})
    resp = await _send_request("get_stop_history", {"limit": limit})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_get_registers() -> str:
    """Read full Z80 register snapshot.

    Returns a JSON object with the following fields (16-bit unless
    noted): ``AF``, ``BC``, ``DE``, ``HL``, ``IX``, ``IY``, ``SP``,
    ``PC``, ``AF_``, ``BC_``, ``DE_``, ``HL_`` (alternate set), ``IR``
    (combined I+R, I in high byte). Reading registers does not pause
    the emulator; for a coherent snapshot, pause first with
    ``emu_pause``.
    """
    resp = await _send_request("get_registers")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_set_register(reg: str, value: int,
                           expected: Optional[int] = None,
                           expected_state_rev: int | None = None) -> str:
    """Write a 16-bit value to a Z80 register.

    Accepts the same case-insensitive register names returned by
    ``emu_get_registers``: ``AF``, ``BC``, ``DE``, ``HL``, ``AF_`` /
    ``AF2``, ``BC_`` / ``BC2``, ``DE_`` / ``DE2``, ``HL_`` / ``HL2``,
    ``IX``, ``IY``, ``SP``, ``PC``, ``WZ``, ``IR``. ``IR`` writes the
    complete I:R pair - I is the high byte, R the low byte including
    bit 7 (like LD I,A plus LD R,A) - so a saved CPU state can be
    restored exactly.

    WARNING: This is a destructive operation. Mutating ``PC`` mid-flight
    changes the execution path; mutating ``SP`` invalidates the current
    call stack. Use with care - prefer a snapshot before
    experimenting. Reading back via ``emu_get_registers`` after the
    write verifies the new value.

    Args:
        reg: register name (case-insensitive).
        value: 16-bit value 0..65535 (for ``IR``: I in the high
            byte, R in the low byte).
        expected: conditional write (compare-and-swap) - write only if
            the register still holds this value; compared and written in
            one step in the emulator thread. On a mismatch nothing is
            written and the error is ``Precondition failed: register <reg>
            changed (expected=0x...., current=0x....)``.
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        JSON ``{"reg", "value", "previous"}`` - ``previous`` is the value
        before the write.
    """
    if not (0 <= value <= 0xFFFF):
        return json.dumps({"error": "value must be in range 0..65535"})
    payload: dict[str, Any] = {"reg": reg, "value": value}
    if expected is not None:
        if not (0 <= expected <= 0xFFFF):
            return json.dumps({"error": "expected must be in range 0..65535"})
        payload["expected"] = expected
    resp = await _send_request("set_register", _with_rev(payload, expected_state_rev))
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_dasm(addr: int, count: int = 16) -> str:
    """Disassemble N Z80 instructions starting at a given address.

    Banking-aware (= reads bytes via the current Z80 memory map).
    Side-effect free.

    Returns a JSON object with ``addr``, ``count`` and ``lines``: an
    array of ``{addr, bytes_hex, num_bytes, mnemonic}`` entries. The
    mnemonic comes from the emulator's built-in Z80 disassembler.

    Args:
        addr: start address (0..65535).
        count: number of instructions to disassemble (1..256). Defaults
            to 16.
    """
    if not (0 <= addr <= 0xFFFF):
        return json.dumps({"error": "addr must be in range 0..65535"})
    if not (1 <= count <= 256):
        return json.dumps({"error": "count must be in range 1..256"})
    resp = await _send_request("dasm", {"addr": addr, "count": count})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_history_get() -> str:
    """Return the debugger history ring buffer (= 32 most recently
    executed instructions).

    No arguments. Returns ``{current_position, length, entries}`` where
    ``entries[i]`` is ``{addr, bytes_hex, regs}`` in raw ring order (not
    chronologically sorted). Use ``current_position`` to compute the
    chronological order: oldest entry is at ``(current_position + 1) %
    length``, newest at ``current_position``. ``regs`` is the CPU state
    just before that instruction (``af``, ``bc``, ``de``, ``hl``, ``ix``,
    ``iy``, ``sp``, ``wz`` hex16, ``i``, ``r`` hex8, ``iff1``, ``iff2``,
    ``im``); with a breakpoint hit this answers "what was HL before each
    of the last instructions" without a trace.

    Mnemonic is not included to keep the response small; pass a single
    ``addr`` to ``emu_dasm(addr, 1)`` to obtain it.
    """
    resp = await _send_request("history_get")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_mem_read(addr: int, length: int = 16, want_data: bool = True) -> str:
    """Read N bytes from Z80 memory at a given address.

    The address is interpreted via the current Z80 memory map (banking
    state applies). Returns a JSON object with fields ``addr`` (int),
    ``len`` (int), ``sha256`` (SHA-256 of the bytes, 64 lowercase hex
    digits) and ``data_b64`` (base64-encoded raw bytes). With
    ``want_data=False`` only the hash is returned - compare it with a
    previous read to see whether the memory changed without downloading
    it.

    See ``emulator://docs/memory_layout`` for per-platform memory maps,
    banking via IORQ ports 0xE0..0xE6 and key ROM entry points; live
    banking snapshot is in ``emulator://memory/map``.

    To read registers together with memory from the same moment (the
    emulator may run between two calls), use ``emu_state_observe``.

    Args:
        addr: Z80 memory address (0-65535).
        length: number of bytes to read (1-65536, ``addr + length`` at
            most 0x10000, so all 64 KiB in one call only from address
            0). Defaults to 16. Above a few KiB prefer
            ``want_data=False`` to keep the response small.
        want_data: False = only ``sha256``, no ``data_b64``.
    """
    resp = await _send_request("mem_read", {"addr": addr, "len": length,
                                            "want_data": bool(want_data)})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_mem_write(addr: int, data_hex: str,
                        expected_sha256: Optional[str] = None,
                        want_hashes: bool = False,
                        expected_state_rev: int | None = None) -> str:
    """Write bytes to Z80 memory at a given address.

    WARNING: This is a destructive operation. Writes to ROM, CG-ROM,
    prohibited or unmapped memory regions are rejected by the emulator
    (region check) - the call returns an error and NO bytes are written.
    A V0 build does not provide a force_rom override; bypassing the check
    is deferred to V1. Use with care - corrupting RAM mid-execution can
    crash the running program.

    MCP clients SHOULD prompt the user to confirm before invoking this
    tool (Claude Code asks for permission per tool category by default).

    See ``emulator://docs/memory_layout`` for per-platform memory maps
    and banking rules; live banking is in ``emulator://memory/map``.

    Args:
        addr: Z80 memory address (0-65535) where the write starts. The
            address is interpreted via the current Z80 memory map
            (banking state applies).
        data_hex: hex string of bytes to write. Both compact form
            ``"DEADBEEF"`` and space-separated form ``"DE AD BE EF"`` are
            accepted; both upper and lower case digits. The decoded byte
            count plus addr must not exceed 0x10000.

        expected_sha256: conditional write - write only if the SHA-256 of
            exactly the bytes being overwritten still equals this value
            (take it from ``emu_mem_read(addr, len, want_data=False)`` or
            a ``emu_state_observe`` window of the same range). The check
            and the write run together in the emulator thread. If the
            memory changed, nothing is written and the error starts with
            ``Precondition failed:``. The region check runs first.
            Note: the address goes through the current memory map; if the
            banking changed but the bytes happen to be equal, the write
            lands elsewhere - use ``emu_region_write`` for a physical cell.
        want_hashes: also return ``before_sha256`` and ``after_sha256``
            without a condition.
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        On success: JSON ``{"addr": <int>, "length": <int>}``, with a
        condition or ``want_hashes`` also ``before_sha256`` and
        ``after_sha256``.
        On failure: JSON ``{"error": "<text>"}`` with a region check
        diagnostic (e.g. ``"MEM_WRITE region check failed at 0x0000
        (kind=1)"``) or ``"Precondition failed: memory changed
        (expected_sha256=..., current_sha256=...)"``. See
        ``emulator://docs/conditional_writes``.
    """
    if not isinstance(data_hex, str) or not data_hex:
        return json.dumps({"error": "data_hex must be a non-empty string"})
    # Quick local validation - emulator side validates again, but failing
    # early avoids a transport roundtrip for obviously malformed input.
    stripped = "".join(c for c in data_hex if not c.isspace())
    if len(stripped) % 2 != 0:
        return json.dumps({"error": "data_hex must have even number of hex digits"})
    if any(c not in "0123456789abcdefABCDEF" for c in stripped):
        return json.dumps({"error": "data_hex contains non-hex characters"})
    payload: dict[str, Any] = {"addr": addr, "data_hex": data_hex}
    err = _write_cond_payload(payload, expected_sha256, want_hashes)
    if err:
        return json.dumps({"error": err})
    resp = await _send_request("mem_write", _with_rev(payload, expected_state_rev))
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "mem_write failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_run(frames: int) -> str:
    """Run N emulator frames and return current state.

    Resumes emulation if paused, **blocks** for the requested number
    of video frames (50 Hz on PAL = 20 ms per frame; 60 Hz on NTSC
    = ~16.7 ms per frame), then pauses the emulator and returns the
    post-run state snapshot. Use this for fine-grained stepping during
    debugging sessions.

    Blocking semantics: the emulator stops itself DETERMINISTICALLY
    exactly at the N-th frame boundary (emu-side frame-bounded stop),
    not via an asynchronous pause from the dispatch thread. Running the
    same number of frames from the same state is therefore bit-reproducible
    - important when tracing timing-sensitive programs.

    Response includes ``actual_frames`` (frames actually run), ``stopped_by``
    (why it stopped: ``"frames"`` = ran the full count, ``"breakpoint"`` = a
    breakpoint paused it first, ``"manual"`` = a manual/UI pause, ``"run_to"`` = a
    temporary breakpoint (run-to-cursor / step over), ``"exit"`` = an exit
    condition (breakpoint action ``exit``; the emulator only pauses, the event
    ``paused`` carries ``exit_code``), ``"timeout"``
    = safety timeout, ``"unknown"`` = other) and ``complete`` (= true only when
    the full requested count was reached, i.e. ``stopped_by == "frames"``).

    Args:
        frames: number of frames to run (1-1000).
    """
    if frames < 1 or frames > 1000:
        return json.dumps({"error": "frames must be in range 1..1000"})
    resp = await _send_request("run", {"frames": frames})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_reset() -> str:
    """Reset the emulator (full reset of CPU, peripherals and memory).

    Equivalent to a hardware power-on reset: CPU registers cleared,
    peripherals reinitialised, memory map restored to power-on defaults.
    Does not unload any inserted media (cartridges, floppies, tapes).
    """
    resp = await _send_request("reset")
    return json.dumps(_data_or_error(resp))


# Mapa short forem na číselný index en_BPT_TYPE (breakpoints.h:100-112).
# Drží C jako jedinou pravdu: short forma se zde převede na enum index,
# který _bp_fill_param_from_json čte přímo (dispatch.c:2451). Plné
# UPPER_SNAKE názvy (PC_EXEC/MEM_R/...) jsou kanonické na C straně.
_BP_ADD_TYPE_TO_ENUM: dict[str, int] = {
    "exec": 0,   # BPT_TYPE_PC_EXEC
    "memr": 1,   # BPT_TYPE_MEM_R
    "memw": 2,   # BPT_TYPE_MEM_W
    "ior": 3,    # BPT_TYPE_IORQ_R
    "iow": 4,    # BPT_TYPE_IORQ_W
}


@mcp.tool()
async def emu_bp_add(addr: int, type: str = "exec",
                     condition: str = "") -> str:
    """Add a breakpoint at the given Z80 address.

    The breakpoint is registered in the emulator's debugger subsystem
    and will trigger a TRAP event (forwarded as an MCP event in future
    phases). Currently the tool only registers the BP; event forwarding
    is planned for a later phase.

    A plain execution breakpoint (``type="exec"``, no condition) goes
    through the lightweight ``bp_add`` path. A typed breakpoint
    (``memr`` / ``memw`` / ``ior`` / ``iow``) or one with a ``condition``
    is transparently created via ``bp_create_with_init`` (= the single
    typed-breakpoint code path); ``emu_bp_create_with_init`` exposes the
    full field set directly. Verify the resulting type with
    ``emu_bp_list``.

    Args:
        addr: Z80 address (0-65535).
        type: breakpoint type, one of: ``exec`` (instruction fetch),
            ``memr`` (memory read), ``memw`` (memory write),
            ``ior`` (I/O read), ``iow`` (I/O write). Defaults to
            ``exec``. An unknown short form returns an error.
        condition: optional condition expression using the bp_expr DSL
            (e.g. ``A == 0x42``). Empty string means unconditional.
            See ``emulator://docs/bp_dsl`` for the full condition
            expression syntax (registers, memory deref, operators,
            built-in functions, ``$vars``). See
            ``emulator://docs/action_dsl`` for the action script
            syntax used by the ``action`` field on
            ``emu_bp_create_with_init`` / ``emu_bp_update``.

    Returns:
        JSON ``{"id": <int>, ...}`` on success, ``{"error": "..."}`` on
        an unknown ``type`` short form.
    """
    if type not in _BP_ADD_TYPE_TO_ENUM:
        return json.dumps({
            "error": f"unknown breakpoint type '{type}' "
                     f"(expected one of {sorted(_BP_ADD_TYPE_TO_ENUM)})"
        })
    # Exec BP bez condition = lehká bp_add cesta (addr-only).
    if type == "exec" and not condition:
        resp = await _send_request("bp_add", {"addr": addr})
        return json.dumps(_data_or_error(resp))
    # Typed nebo conditional BP = transparentní remap na
    # bp_create_with_init (jediná typed cesta, sdílí fields-mask).
    fields = ["type"]
    values: dict[str, Any] = {"type": _BP_ADD_TYPE_TO_ENUM[type]}
    if condition:
        fields.append("expr")
        values["expr"] = condition
    return await emu_bp_create_with_init(addr=addr, fields=fields,
                                         values=values)


@mcp.tool()
async def emu_bp_list() -> str:
    """List all active breakpoints.

    Returns a JSON object ``{"count": int, "breakpoints": [...]}`` where
    each breakpoint record carries the fields:
    ``id`` (int), ``addr`` (int),
    ``addr_end`` (int, inclusive upper bound, used only in ``RANGE`` mode),
    ``addr_match_mode`` (string: ``SINGLE`` = only ``addr``, ``RANGE`` =
    ``addr``..``addr_end``, ``MASK`` = ``(x & addr_mask) == (addr &
    addr_mask)``; applies to ``PC_EXEC`` / ``MEM_R`` / ``MEM_W``),
    ``addr_mask`` (int, used only in ``MASK`` mode),
    ``enabled`` (bool),
    ``type`` (string, canonical UPPER_SNAKE: ``PC_EXEC`` / ``MEM_R`` /
    ``MEM_W`` / ``IORQ_R`` / ``IORQ_W`` / ...),
    ``zone`` (string, memory zone: ``CPU_VIEW`` / ``RAM`` / ...),
    ``bank_id`` (int, bank index for ``MMEXT_BANK`` zone),
    ``bank_id_end`` / ``bank_match_mode`` / ``bank_id_mask`` (bank range,
    same meaning as the ``addr_*`` triple, applied to ``bank_id``),
    ``hits`` (int, trigger counter),
    ``condition`` (string or null if unconditional).
    """
    resp = await _send_request("bp_list")
    return json.dumps(_data_or_error(resp))


# === V0.B.6 tools (7 missing V0 Tools) ================================
# Tool descriptions jsou anglicky - MCP wire protocol je locale-agnostic.


@mcp.tool()
async def emu_bp_remove(id: int) -> str:
    """Remove a breakpoint by its numeric ID.

    The ID is the integer handle returned by ``emu_bp_add`` (field
    ``id``) or visible in ``emu_bp_list`` output. Removing a breakpoint
    that does not exist returns an error.

    Args:
        id: breakpoint ID to remove.

    Returns:
        JSON ``{"id": <int>, "removed": true}`` on success,
        ``{"error": "..."}`` if the ID is unknown.
    """
    resp = await _send_request("bp_remove", {"id": id})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "bp_remove failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_bp_clear() -> str:
    """Remove all breakpoints in one operation.

    Iterates the current breakpoint list and removes each entry. Used
    to reset debugger state quickly between debug sessions.

    Returns:
        JSON ``{"count": <int>, "cleared": true}`` where ``count`` is
        the number of breakpoints that were removed.
    """
    resp = await _send_request("bp_clear")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_bp_enable(id: int, enabled: bool) -> str:
    """Enable or disable a breakpoint without removing it.

    A disabled breakpoint stays in the list (visible via
    ``emu_bp_list``) but does not trigger the debugger when hit. Useful
    for temporarily silencing a BP without losing its address.

    Args:
        id: breakpoint ID.
        enabled: true = active (will trigger), false = inactive
            (kept in list).

    Returns:
        JSON ``{"id": <int>, "enabled": <bool>}`` on success.
    """
    resp = await _send_request("bp_enable", {"id": id, "enabled": enabled})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "bp_enable failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_step_into() -> str:
    """Execute exactly one Z80 instruction (steps into CALL/RST).

    Advances the program counter by exactly one instruction. For
    CALL/RST/JP/JR the next executed instruction is at the destination
    address (= the tool steps INTO the subroutine, unlike
    ``emu_step_over``).

    The emulator must be paused before calling this tool. In a paused
    emulator the tool returns after the step executed (``completed:
    true``). On a running emulator nothing happens and the emulator keeps
    running (``completed: false``, ``running: true``); call ``emu_pause``
    first.

    Returns:
        JSON ``{"stepped": true, "completed": <bool>, "running": <bool>,
        "run_seq": <int>}``. ``stepped`` is always true (compatibility);
        ``completed`` tells whether the step really executed.
    """
    resp = await _send_request("step_into")
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "step_into failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_step_over() -> str:
    """Execute one instruction, treating CALL/RST as atomic.

    For ordinary instructions behaves like ``emu_step_into`` (returns
    after the step executed, ``completed: true``). For CALL/RST/DJNZ and
    block instructions, a temporary breakpoint is placed at the return
    address and the emulator runs until it hits that BP (= the subroutine
    is executed as one logical step): the tool returns at once with
    ``completed: false``, ``running: true`` and the stop emits the event
    ``paused`` with ``reason: "run_to"``. Wait for it with
    ``emu_wait_stop(run_seq)``.

    Requires paused state; on a running emulator the call only pauses it
    (``paused_only: true``, nothing stepped).

    Returns:
        JSON ``{"stepped": true, "completed": <bool>, "running": <bool>,
        "run_seq": <int>}`` (plus ``paused_only``). ``stepped`` is always
        true (compatibility).
    """
    resp = await _send_request("step_over")
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "step_over failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_step_n(count: int) -> str:
    """Execute N Z80 instructions as a sequence of step_into calls.

    The backend loops ``DBGAPI_CMD_STEP_INTO`` N times. If any step
    fails mid-loop (= a breakpoint was hit, emulator left pause for
    another reason), the tool returns the partial count and marks
    ``partial=true`` in the response.

    Args:
        count: number of instructions to advance (1-1000). Limits
            match ``emu_run(frames)`` to keep transport latency
            bounded.

    Returns:
        JSON ``{"count": <int>, "requested": <int>, "partial": <bool>}``
        where ``count`` is the actual number of executed steps. The tool
        returns after the last step executed.
    """
    if not isinstance(count, int):
        return json.dumps({"error": "count must be an integer"})
    if count < 1 or count > 1000:
        return json.dumps({"error": "count must be in range 1..1000"})
    resp = await _send_request("step_n", {"count": count})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "step_n failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_run_until_addr(addr: int, max_cycles: int = 10_000_000,
                             deadline_frames: int | None = None,
                             deadline_ms: int | None = None,
                             deadline_ticks: int | None = None) -> str:
    """Run emulation until PC reaches the given address.

    Places a temporary breakpoint at ``addr`` and resumes emulation.
    The backend pauses when PC equals ``addr`` (or when a permanent
    breakpoint hits first) and emits the event ``paused`` with
    ``{"reason": "run_to", "pc": addr}`` (a permanent breakpoint that
    stops instead gives ``reason: "breakpoint"``, never both). Wait for
    the stop with ``emu_wait_stop(run_seq)`` (or the ``paused`` event).

    Without a deadline the run has no limit: when ``addr`` is never
    reached, the emulator runs until paused. Give a deadline in emulated
    time (``deadline_frames``, ``deadline_ms`` or ``deadline_ticks``, at
    most one, see ``emu_resume``) to stop it deterministically with the
    reason ``"deadline"``. ``max_cycles`` is informative only (echoed,
    never enforced).

    The emulator must be paused before calling this tool. If it is
    running, the call returns an error ("Emulator is running ...") and
    the emulator keeps running unchanged; call ``emu_pause`` first.
    (A freshly started emulator runs.)

    Args:
        addr: target Z80 PC address (0-65535).
        max_cycles: informative value in Z80 T-states (echoed back).
        deadline_frames: stop after N frame boundaries.
        deadline_ms: stop after N ms of emulated time.
        deadline_ticks: stop after N GDG clock ticks.

    Returns:
        JSON ``{"addr": <int>, "max_cycles": <int>, "running": true,
        "run_seq": <int>}`` (plus ``deadline_ticks``) on success.
    """
    if not isinstance(addr, int) or addr < 0 or addr > 0xFFFF:
        return json.dumps({"error": "addr must be in range 0..65535"})
    if not isinstance(max_cycles, int) or max_cycles < 1:
        return json.dumps({"error": "max_cycles must be a positive integer"})
    deadline, err = _deadline_arg(deadline_frames, deadline_ms, deadline_ticks)
    if err:
        return json.dumps({"error": err})
    args: dict[str, Any] = {"addr": addr, "max_cycles": max_cycles}
    if deadline is not None:
        args["deadline"] = deadline
    resp = await _send_request("run_until_addr", args)
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "run_until_addr failed")})
    return json.dumps(_data_or_error(resp))


# === V1.A.1 - Snapshot Tools + cooperation hint ======================
# Tyto Tools rozšiřují V0 sadu o snapshot save/load operace přes dva
# kanály (souborový NEBO inline base64 buffer) a "cooperation hint"
# self-binding instrukci, kterou si AI klient dobrovolně uloží v server
# stavu. Detailní spec viz rozbor sekce 5.2 (snapshot) a sekce 3.3.2
# (cooperation hint).


@mcp.tool()
async def emu_snapshot_save(path: str, description: str = "") -> str:
    """Save emulator state to a .mzs snapshot file on disk.

    The emulator must be paused before calling this tool. The .mzs file
    is a ZIP archive holding an XML manifest, binary RAM/VRAM dumps and
    a screenshot. Description is embedded into the manifest metadata.

    Args:
        path: filesystem path for the .mzs file (absolute or relative
            to the emulator working directory).
        description: optional human-readable description.

    Returns:
        JSON ``{"path": <str>, "ok": true, "result_code": 0}`` on success
        or ``{"error": "..."}`` on failure.
    """
    # Komentář cesko: validace path je optimisticky lokálně - jen empty
    # check; reálnou file I/O kontrolu provede emu backend.
    if not isinstance(path, str) or path == "":
        return json.dumps({"error": "path must be a non-empty string"})
    resp = await _send_request(
        "snapshot_save", {"path": path, "description": description})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "snapshot_save failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_snapshot_save_buffer(description: str = "") -> str:
    """Save emulator state to an inline base64-encoded buffer.

    Useful when the AI client cannot or does not want to write to the
    emulator host filesystem. The .mzs ZIP content is returned inline as
    base64 and can later be passed back via emu_snapshot_load_buffer.

    The emulator must be paused before calling this tool.

    Args:
        description: optional human-readable description embedded in
            snapshot metadata.

    Returns:
        JSON ``{"bytes_b64": <str>, "size": <int>, "ok": true,
        "result_code": 0}`` on success.
    """
    resp = await _send_request(
        "snapshot_save_buffer", {"description": description})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "snapshot_save_buffer failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_snapshot_load(path: str,
                            expected_state_rev: int | None = None) -> str:
    """Restore emulator state from a .mzs snapshot file on disk.

    The emulator must be paused (call ``emu_pause`` first; a freshly
    started emulator runs). Otherwise the call returns the error
    "snapshot_load failed: Emulator is not paused ..." and nothing is
    loaded. Other failures name the reason too (e.g. I/O error,
    incompatible architecture).

    After successful load the emulator stays paused at the snapshot's
    captured PC. Call ``emu_get_registers`` or ``emu_status`` to verify
    the post-load state.

    Args:
        path: filesystem path to the .mzs file.
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        JSON ``{"path": <str>, "ok": true, "result_code": 0}`` on success.
    """
    if not isinstance(path, str) or path == "":
        return json.dumps({"error": "path must be a non-empty string"})
    resp = await _send_request("snapshot_load", _with_rev({"path": path}, expected_state_rev))
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "snapshot_load failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_snapshot_load_buffer(bytes_b64: str,
                                   expected_state_rev: int | None = None) -> str:
    """Restore emulator state from an inline base64-encoded buffer.

    The buffer is typically produced by ``emu_snapshot_save_buffer`` in
    an earlier session.

    The emulator must be paused (call ``emu_pause`` first), the same as
    for ``emu_snapshot_load``; otherwise the error says "Emulator is not
    paused".

    Args:
        bytes_b64: base64-encoded .mzs ZIP content.
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        JSON ``{"size": <int>, "ok": true, "result_code": 0}`` on success
        or ``{"error": "..."}`` naming the reason (buffer invalid,
        emulator not paused, ...).
    """
    if not isinstance(bytes_b64, str) or bytes_b64 == "":
        return json.dumps({"error": "bytes_b64 must be a non-empty string"})
    resp = await _send_request("snapshot_load_buffer", _with_rev({"bytes_b64": bytes_b64}, expected_state_rev))
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "snapshot_load_buffer failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cooperation_hint_set(mode: str, until: str = "") -> str:
    """Set a self-binding cooperation hint that the AI agrees to honor.

    The hint is a persistent server-side instruction the AI uses to
    constrain its own behaviour between requests (MCP is per-request
    stateless, so without the slot the AI would have to remember the
    constraint in conversation context, which the user cannot audit).

    Per the design document section 3.3.2, V1.A.1 only persists the hint
    and broadcasts a UI notification (= future Activity Log panel V1.C).
    No hard policy enforcement happens server-side - violating the hint
    only results in an audit entry. Hard enforcement is deferred to the
    security profile work (V1.A.5+).

    Args:
        mode: one of:

            * ``"free"`` - no constraint (clear any previous hint).
            * ``"read_only"`` - AI will only call read-only tools and
              resources.
            * ``"paused_only"`` - AI will only call tools while the
              emulator is paused.

        until: optional human-readable expiration hint, e.g. an ISO 8601
            timestamp or natural-language phrase like "next user
            message" or "next 30 min". Empty string means open-ended
            (the hint stays until explicitly cleared).

    Returns:
        JSON ``{"mode": <str>, "until": <str>, "ok": true}`` on success
        or ``{"error": "..."}`` if the mode is invalid.
    """
    valid = ("free", "read_only", "paused_only")
    if not isinstance(mode, str) or mode not in valid:
        return json.dumps(
            {"error": f"Invalid mode: {mode!r}; expected one of {valid}"})
    resp = await _send_request(
        "cooperation_hint_set", {"mode": mode, "until": until})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "cooperation_hint_set failed")})
    return json.dumps(_data_or_error(resp))


# === Symbol management Tools (V1.A.2) ================================
# Symbol DB (= named addresses) je vystavena přes 4 Tools (add / remove /
# lookup / list). Reálné storage je sym_db v emu (src/emulator/debugger/
# symbols/sym_db.{c,h}), které podporuje LBL (user-defined) + import
# z NoICE/MAP/SYM/.lbl. MCP úroveň záměrně exponuje CRUD nad LBL kindem
# (= user-driven anotace); ostatní zdroje jsou read-only přes lookup/list.
#
# Žádný `emulator://symbols` Resource zatím (= V1.D scope) - klient si
# výpis dělá přes emu_symbol_list. Žádný .lbl/.sym/.noice file load
# tool (= V1.B/V2 scope).


@mcp.tool()
async def emu_symbol_add(addr: int, name: str,
                          comment: str = "", kind: str = "LABEL") -> str:
    """Add a user-defined symbol (named address) to the symbol database.

    Symbols give human-readable names to memory locations and are picked
    up by the disassembler view, breakpoint expressions and any AI
    introspection tools that resolve names against addresses.

    The reverse-engineering workflow per the design rozbor section 2.3.2
    expects the AI to call this tool when it identifies a routine or
    data label (= ``"this CALL target prints A as char"``).

    The ``kind`` parameter is currently echo-only - the emulator's symbol
    storage always treats user-added symbols as the ``LBL`` (user
    write-back) source kind regardless of the kind value supplied here.
    The slot is reserved for future kind-aware features (V2+ scope).

    Args:
        addr: Z80 address (0..65535).
        name: identifier - non-empty, ASCII letters / digits / underscore
            / dot only (no whitespace).
        comment: optional human-readable comment (default "").
        kind: informational kind label, e.g. "LABEL", "DATA", "BOOKMARK"
            (default "LABEL"). Echo-only in V1.A.2.

    Returns:
        JSON ``{"added": true, "addr": <int>, "name": <str>,
        "kind": <str>}`` on success or ``{"error": "..."}`` on failure.
    """
    # Komentář cesko: lokální validace odmítne whitespace a empty před
    # JSONL round-tripem - rychlá zpětná vazba pro AI klienta.
    if not isinstance(name, str) or name == "" or any(
            c.isspace() for c in name):
        return json.dumps(
            {"error": "name must be non-empty and contain no whitespace"})
    if not isinstance(addr, int) or addr < 0 or addr > 65535:
        return json.dumps({"error": "addr must be 0..65535"})
    resp = await _send_request(
        "symbol_add",
        {"addr": addr, "name": name, "comment": comment, "kind": kind})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "symbol_add failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_symbol_remove(name: str = "", addr: int = -1) -> str:
    """Remove a symbol from the symbol database, by name or by address.

    Exactly one of ``name`` / ``addr`` must be supplied. Removing by
    address resolves the symbol via lookup_by_addr first, then removes
    by the resolved name (emu storage supports remove-by-name only).

    Args:
        name: identifier to remove (mutually exclusive with ``addr``).
        addr: address whose symbol should be removed (0..65535).
            ``-1`` means "not supplied".

    Returns:
        JSON ``{"removed": <bool>, "name": <str>}`` or
        ``{"removed": <bool>, "addr": <int>}`` on success;
        ``{"error": "..."}`` on invalid input.
    """
    has_name = isinstance(name, str) and name != ""
    has_addr = isinstance(addr, int) and 0 <= addr <= 65535
    if not has_name and not has_addr:
        return json.dumps(
            {"error": "either name or addr must be specified"})
    if has_name and has_addr:
        return json.dumps(
            {"error": "specify either name OR addr, not both"})
    args: dict[str, Any] = {"name": name} if has_name else {"addr": addr}
    resp = await _send_request("symbol_remove", args)
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "symbol_remove failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_bookmark_add(input: str, comment: str = "") -> str:
    """Add a named address bookmark to the debugger bookmark store.

    Bookmarks are (input, comment) pairs surfaced in the debugger UI and
    via the ``emulator://bookmarks`` resource. The ``input`` is resolved
    to a 16-bit address dynamically: it may be a hex literal ("$1234",
    "0x1234", "#1234", "1234h", "1234") or a symbol name from the symbol
    database. Using a symbol name lets the bookmark follow the symbol if
    its address later changes (map reload, new import).

    The bookmark is tagged with the MCP owner origin, so it appears with
    the "mcp" badge in the UI activity log and the resource ``owner``
    field.

    Args:
        input: hex literal or symbol name (non-empty).
        comment: optional human-readable note (default "").

    Returns:
        JSON ``{"id": <int>, "input": <str>, "comment": <str>,
        "addr": <int|null>, "addr_resolved": <bool>, "owner": "mcp"}``
        on success; ``{"error": "..."}`` on failure. ``addr`` is null
        when ``input`` cannot be resolved to an address (e.g. unknown
        symbol) - the bookmark is still stored and resolves lazily later.
    """
    # Komentář česky: lokální validace odmítne prázdný input před
    # JSONL round-tripem - rychlá zpětná vazba pro AI klienta.
    if not isinstance(input, str) or input == "":
        return json.dumps({"error": "input must be non-empty"})
    resp = await _send_request(
        "bookmark_add", {"input": input, "comment": comment})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "bookmark_add failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_bookmark_remove(id: int) -> str:
    """Remove a bookmark from the debugger bookmark store by its ID.

    Bookmark IDs are monotonic and never reused within a session; obtain
    them from the ``emulator://bookmarks`` resource. Removing a
    non-existent ID is not an error - it returns ``removed=false``.

    Args:
        id: bookmark ID (>= 1).

    Returns:
        JSON ``{"id": <int>, "removed": <bool>}`` on success;
        ``{"error": "..."}`` on invalid input.
    """
    if not isinstance(id, int) or id < 1:
        return json.dumps({"error": "id must be >= 1"})
    resp = await _send_request("bookmark_remove", {"id": id})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "bookmark_remove failed")})
    return json.dumps(_data_or_error(resp))


# === CMT-A - cassette tape transport + recording + cmthack toggle =====
#
# Two distinct concepts:
#  * emu_cmt_play / play_paused / stop / pause / eject / record drive the
#    REAL emulated cassette tape (a proper state machine: it reads/writes
#    the tape signal cycle-by-cycle, just like physical hardware). This is
#    the accurate path and works with any program that loads from tape.
#  * emu_cmt_hack_set toggles the cmthack ROM patch (instant load). The
#    hack short-circuits the Sharp ROM load routine and copies the file
#    straight into RAM - it only works for programs that go through the
#    patched ROM entry points and is much narrower than real tape.
#
# All of these change emulator state (sensitive) and appear in the
# Activity log as MCP actions. Inspect the current tape state via the
# ``emulator://periph/cmt`` resource (includes ``cmthack_enabled``).


@mcp.tool()
async def emu_cmt_play() -> str:
    """Start REAL cassette tape playback (transport = PLAY).

    Drives the accurate emulated tape: the program reads the tape signal
    through the normal Sharp ROM load routine. Requires a tape image to be
    inserted first (``emu_media_insert(slot="cmt", ...)``). No-op if no
    tape is loaded or playback cannot start in the current state.

    This is the real-tape path, distinct from the cmthack instant load
    (see ``emu_cmt_hack_set``). Sensitive: changes emulator state and is
    logged as an MCP action.

    Returns:
        JSON ``{"ok": true, "action": "play"}`` on success or
        ``{"error": "..."}`` on failure.
    """
    resp = await _send_request("cmt_transport", {"action": "play"})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "cmt_play failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cmt_play_paused() -> str:
    """Start REAL cassette tape playback but immediately paused.

    Same as ``emu_cmt_play`` except the tape begins in the paused state;
    call ``emu_cmt_pause(paused=False)`` to actually advance the tape.
    Useful to arm playback at a precise moment. Sensitive: changes
    emulator state (MCP action).

    Returns:
        JSON ``{"ok": true, "action": "play_paused"}`` on success or
        ``{"error": "..."}`` on failure.
    """
    resp = await _send_request("cmt_transport", {"action": "play_paused"})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "cmt_play_paused failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cmt_stop() -> str:
    """Stop the REAL cassette tape transport (PLAY or RECORD -> STOP).

    Stops playback or recording and rewinds the transport state machine.
    No-op if the tape is already stopped. Without a loaded tape it still
    puts the transport into STOP (e.g. a playing state restored from a
    snapshot). Sensitive: changes emulator state (MCP action).

    Returns:
        JSON ``{"ok": true, "action": "stop"}`` on success or
        ``{"error": "..."}`` on failure.
    """
    resp = await _send_request("cmt_transport", {"action": "stop"})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "cmt_stop failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cmt_pause(paused: bool = True) -> str:
    """Pause or resume the REAL cassette tape transport.

    Pauses (``paused=True``) or resumes (``paused=False``) the running
    tape without resetting its position. If the tape is stopped, pausing
    has no useful effect. Sensitive: changes emulator state (MCP action).

    Args:
        paused: True = pause the tape, False = resume.

    Returns:
        JSON ``{"ok": true, "action": "pause"}`` on success or
        ``{"error": "..."}`` on failure.
    """
    resp = await _send_request(
        "cmt_transport", {"action": "pause", "pause": bool(paused)})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "cmt_pause failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cmt_eject() -> str:
    """Eject the cassette tape image from the REAL tape drive.

    Stops the transport (if running) and removes the loaded tape image.
    No-op if nothing is loaded. Equivalent to ``emu_media_eject(slot=
    "cmt")`` but scoped to the CMT subsystem. Sensitive: changes emulator
    state (MCP action).

    Returns:
        JSON ``{"ok": true, "action": "eject"}`` on success or
        ``{"error": "..."}`` on failure.
    """
    resp = await _send_request("cmt_transport", {"action": "eject"})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "cmt_eject failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cmt_record(path: str) -> str:
    """Start recording the REAL cassette tape output to a WAV file.

    Opens ``path`` as a WAV file and arms the tape for recording (the
    captured signal is whatever the program writes to the cassette
    output). Recording starts paused; call
    ``emu_cmt_pause(paused=False)`` to begin capturing. Only WAV output
    is supported. The tape must be in the STOP state and the path must be
    writable, otherwise this fails.

    This records the real tape signal; it is unrelated to the cmthack
    instant load. Sensitive: changes emulator state (MCP action).

    Args:
        path: target WAV file path (must be writable).

    Returns:
        JSON ``{"ok": true, "path": str}`` on success or
        ``{"error": "..."}`` on failure (bad state or path not writable).
    """
    if not path:
        return json.dumps({"error": "Missing required field: path"})
    resp = await _send_request("cmt_record", {"path": path})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "cmt_record failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cmt_hack_set(enabled: bool) -> str:
    """Enable or disable the cmthack ROM patch (instant tape load).

    The cmthack is a ROM-patch shortcut, NOT the real tape: when enabled,
    the patched Sharp ROM load routine copies a tape file directly into
    RAM, skipping the cycle-accurate tape signal. It is faster but only
    works for programs that load through the patched ROM entry points.
    For anything else (custom loaders, copy-protection, recording) use the
    real tape transport (``emu_cmt_play`` / ``emu_cmt_record``).

    The current patch state is also readable as ``cmthack_enabled`` in the
    ``emulator://periph/cmt`` resource. Sensitive: changes emulator state
    (MCP action).

    Args:
        enabled: True = install the ROM patch, False = remove it.

    Returns:
        JSON ``{"ok": true, "installed": bool}`` echoing the patch state
        after the operation, or ``{"error": "..."}`` on failure.
    """
    resp = await _send_request("cmt_hack_set", {"enabled": bool(enabled)})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "cmt_hack_set failed")})
    return json.dumps(_data_or_error(resp))


# CMT-B - tape speed ratio keys -> en_CMTSPEED integer values.
# The emulator enum (cmtspeed.h) is: 1_1=1, 2_1=2, 2_1_cpm=3, 3_1=4,
# 3_2=5, 7_3=6, 8_3=7, 9_7=8, 25_14=9, custom=10 (custom pulse lengths,
# see pulses_us). We expose stable string ratio keys plus the raw int for
# clients that prefer numeric.
_CMT_SPEED_KEYS = {
    "1:1": 1,
    "2:1": 2,
    "2:1_cpm": 3,
    "3:1": 4,
    "3:2": 5,
    "7:3": 6,
    "8:3": 7,
    "9:7": 8,
    "25:14": 9,
    "custom": 10,
}

# en_CMTSPEED value of custom pulse lengths (CMTSPEED_CUSTOM).
_CMT_SPEED_CUSTOM = 10


def _cmt_resolve_speed(speed, allow_custom: bool = True) -> int:
    """Resolve a CMT speed argument to the en_CMTSPEED integer.

    Accepts either a ratio string key ("1:1", "2:1", "2:1_cpm", "3:1",
    "3:2", "7:3", "8:3", "9:7", "25:14", "custom") or an integer /
    numeric string in range 1..10 (10 = custom pulse lengths, only when
    ``allow_custom``). Returns 0 for anything unrecognized (the backend
    then rejects it).
    """
    if isinstance(speed, str):
        key = speed.strip().lower()
        if key in _CMT_SPEED_KEYS:
            v = _CMT_SPEED_KEYS[key]
            return v if (allow_custom or v != _CMT_SPEED_CUSTOM) else 0
        try:
            speed = int(key)
        except ValueError:
            return 0
    try:
        v = int(speed)
    except (TypeError, ValueError):
        return 0
    top = _CMT_SPEED_CUSTOM if allow_custom else 9
    return v if 1 <= v <= top else 0


def _cmt_resolve_pulses(pulses_us):
    """Validate custom tape pulse lengths.

    Returns a list of 4 floats (microseconds: long high, long low, short
    high, short low - the order of the UniCMT CMTSPEED header) or None
    when the argument is not a sequence of 4 numbers in (0, 65535].
    A JSON string with such a list is accepted too.
    """
    if isinstance(pulses_us, str):
        try:
            pulses_us = json.loads(pulses_us)
        except ValueError:
            return None
    if not isinstance(pulses_us, (list, tuple)) or len(pulses_us) != 4:
        return None
    out = []
    for v in pulses_us:
        if isinstance(v, bool):
            return None
        try:
            f = float(v)
        except (TypeError, ValueError):
            return None
        if not (0 < f <= 65535):
            return None
        out.append(f)
    return out


@mcp.tool()
async def emu_cmt_set_speed(speed=None, pulses_us=None) -> str:
    """Set the default playback speed of the virtual cassette (Virtual CMT).

    The Sharp MZ tape can run at several speed ratios relative to the
    1200 Bd baseline, or with custom pulse lengths. Pass either ``speed``
    (ratio string key or the raw en_CMTSPEED integer) or ``pulses_us``:

    * "1:1" (1) - standard 1200 Bd
    * "2:1" (2), "2:1_cpm" (3), "3:1" (4), "3:2" (5)
    * "7:3" (6, Intercopy 10.2), "8:3" (7, CP/M cmt.com)
    * "9:7" (8), "25:14" (9)
    * "custom" (10) - the custom pulse lengths stored earlier
    * ``pulses_us=[long_high, long_low, short_high, short_low]`` - custom
      pulse lengths in microseconds, the order of the UniCMT CMTSPEED
      header (UniCMT 3x = [156, 164, 80, 92]); each in (0, 65535]. The
      default speed switches to custom (10) and the lengths are stored.

    The change applies to blocks with the default speed; per-block
    overrides are set with ``emu_cmt_tape_set_block_speed`` (blocks after
    a UniCMT CMTSPEED header in an .mzt already have their own speed).
    Reflected as ``cmtspeed``, ``default_pulses_us`` and
    ``custom_pulses_us`` in ``emulator://periph/cmt``. Sensitive: changes
    emulator state.

    Args:
        speed: ratio string ("2:1") or int (2); omit when using pulses_us.
        pulses_us: list of 4 numbers (microseconds); omit when using speed.

    Returns:
        JSON ``{"ok": true, "property": "speed", "value": int}`` or, for
        ``pulses_us``, ``{"ok": true, "property": "custom_pulses",
        "pulses_us": [...]}``; ``{"error": "..."}`` on failure.
    """
    if pulses_us is not None:
        if speed not in (None, "", "custom", _CMT_SPEED_CUSTOM):
            return json.dumps(
                {"error": "Pass either speed or pulses_us, not both"})
        pulses = _cmt_resolve_pulses(pulses_us)
        if pulses is None:
            return json.dumps(
                {"error": f"Invalid pulses_us: {pulses_us!r} (use 4 "
                          f"numbers in microseconds, each in (0, 65535])"})
        resp = await _send_request(
            "cmt_set_property",
            {"property": "custom_pulses", "pulses_us": pulses})
        if not resp.get("success", False):
            return json.dumps(
                {"error": resp.get("error", "cmt_set_speed failed")})
        return json.dumps(_data_or_error(resp))
    value = _cmt_resolve_speed(speed)
    if value == 0:
        return json.dumps(
            {"error": f"Invalid CMT speed: {speed!r} "
                      f"(use one of {sorted(_CMT_SPEED_KEYS)}, 1..10 "
                      f"or pulses_us)"})
    resp = await _send_request(
        "cmt_set_property", {"property": "speed", "value": value})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "cmt_set_speed failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cmt_set_polarity(inverted: bool) -> str:
    """Set the CMT rear DIP switch tape signal polarity.

    Some real machines have inverted cassette wiring; this flips the bit
    polarity used when decoding/encoding the tape signal. Reflected as
    ``polarity_inverted`` in ``emulator://periph/cmt``. Sensitive:
    changes emulator state (MCP action).

    Args:
        inverted: True = inverted polarity, False = normal.

    Returns:
        JSON ``{"ok": true, "property": "polarity", "value": 0|1}`` or
        ``{"error": "..."}`` on failure.
    """
    resp = await _send_request(
        "cmt_set_property",
        {"property": "polarity", "value": 1 if inverted else 0})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "cmt_set_polarity failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cmt_set_cpu_boost(enabled: bool) -> str:
    """Enable or disable CPU boost during tape transport.

    When enabled, the emulator runs at maximum speed while the tape is
    playing/recording, so long loads finish quickly. When disabled, the
    tape runs at real time. Reflected as ``cpu_boost`` in
    ``emulator://periph/cmt``. Sensitive: changes emulator state
    (MCP action).

    Args:
        enabled: True = boost during transport, False = real time.

    Returns:
        JSON ``{"ok": true, "property": "cpu_boost", "value": 0|1}`` or
        ``{"error": "..."}`` on failure.
    """
    resp = await _send_request(
        "cmt_set_property",
        {"property": "cpu_boost", "value": 1 if enabled else 0})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "cmt_set_cpu_boost failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cmt_set_mzfsize_check(enabled: bool) -> str:
    """Enable or disable the MZF size consistency check on tape load.

    When enabled, the emulator validates that the MZF body size matches
    the header file size, rejecting malformed images. Disabling it allows
    loading non-conforming tapes. Reflected as ``mzfsize_check`` in
    ``emulator://periph/cmt``. Sensitive: changes emulator state
    (MCP action).

    Args:
        enabled: True = enforce the size check, False = skip it.

    Returns:
        JSON ``{"ok": true, "property": "mzfsize_check", "value": 0|1}``
        or ``{"error": "..."}`` on failure.
    """
    resp = await _send_request(
        "cmt_set_property",
        {"property": "mzfsize_check", "value": 1 if enabled else 0})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "cmt_set_mzfsize_check failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cmt_open(path: str, play_immediately: bool = False) -> str:
    """Open a CMT tape file into the REAL tape drive (by extension).

    CMT-specific open that picks the right tape backend from the file
    extension (.mzf / .mzt / .m12 / .wav / ...). Unlike the generic
    ``emu_media_insert(slot="cmt")``, this can immediately start playback
    via ``play_immediately`` (= one round trip instead of open + play).

    This loads the real tape image; for the cmthack instant load use
    ``emu_cmt_hack_set`` + ``emu_media_load_mzf`` instead. Sensitive:
    changes emulator state (MCP action).

    Args:
        path: tape file path. The extension selects the backend.
        play_immediately: if True, start playback right after opening.

    Returns:
        JSON ``{"ok": true, "path": str, "playing": bool, "state": str,
        "paused": bool}`` on success or ``{"error": "..."}`` on failure
        (unknown extension / open error). ``playing``, ``state``
        (``"stop"`` / ``"play"`` / ``"record"``) and ``paused`` report the
        actual transport state after the operation (same meaning as in
        ``emulator://periph/cmt``); ``playing`` is true only for PLAY
        without pause. When ``play_immediately`` was requested but the tape
        is not playing afterwards, the result also carries ``warning``.
    """
    if not path:
        return json.dumps({"error": "Missing required field: path"})
    resp = await _send_request(
        "cmt_open", {"path": path,
                     "play_immediately": bool(play_immediately)})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "cmt_open failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cmt_tape_seek(block_id: int) -> str:
    """Seek the REAL tape to a specific block (SIMPLE_TAPE containers).

    Multi-file tape containers (SIMPLE_TAPE: .mzt, .tap) expose
    individual blocks; this positions the tape at block ``block_id``
    (0-based). Single-file containers (SINGLE: a plain .mzf or .wav,
    ``container_type`` 0 in ``emulator://periph/cmt/tape``) do NOT support
    seeking - the call fails even for block 0. To start such a tape from
    the beginning again, use ``emu_cmt_stop`` + ``emu_cmt_play`` (play
    from STOP always starts at the beginning of the tape). Requires a
    loaded tape. The list of blocks is in ``emulator://periph/cmt/tape``.
    Sensitive: changes emulator state (MCP action).

    Args:
        block_id: 0-based block index to seek to.

    Returns:
        JSON ``{"ok": true, "block_id": int}`` on success or
        ``{"error": "..."}`` on failure (no tape, SINGLE container or
        block out of range).
    """
    resp = await _send_request("cmt_tape_seek", {"block_id": int(block_id)})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "cmt_tape_seek failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cmt_tape_set_block_speed(block_id: int, speed=None,
                                       pulses_us=None) -> str:
    """Set the playback speed of a single tape block (cmt speed only).

    Per-block speed override for SIMPLE_TAPE containers. Only the cmt
    speed is adjustable per block (no other per-block parameters): pass
    either ``speed`` (the same ratio string keys or int 1..9 as
    ``emu_cmt_set_speed``) or ``pulses_us`` (custom pulse lengths in
    microseconds, [long_high, long_low, short_high, short_low], the order
    of the UniCMT CMTSPEED header; MZ blocks only). Requires a loaded
    tape. The speed applies from the next opening of the block (advancing
    to it during playback, ``emu_cmt_tape_seek``); the currently open
    block is not changed. Sensitive: changes emulator state (MCP action).

    Args:
        block_id: 0-based block index.
        speed: ratio string ("2:1") or int (2); omit when using pulses_us.
        pulses_us: list of 4 numbers (microseconds); omit when using speed.

    Returns:
        JSON ``{"ok": true, "block_id": int, "speed": int}`` (with
        ``"speed": 10`` and ``"pulses_us"`` for custom pulses) on success
        or ``{"error": "..."}`` on failure (no tape / bad speed).
    """
    if pulses_us is not None:
        if speed not in (None, "", "custom", _CMT_SPEED_CUSTOM):
            return json.dumps(
                {"error": "Pass either speed or pulses_us, not both"})
        pulses = _cmt_resolve_pulses(pulses_us)
        if pulses is None:
            return json.dumps(
                {"error": f"Invalid pulses_us: {pulses_us!r} (use 4 "
                          f"numbers in microseconds, each in (0, 65535])"})
        resp = await _send_request(
            "cmt_tape_block_speed",
            {"block_id": int(block_id), "pulses_us": pulses})
        if not resp.get("success", False):
            return json.dumps(
                {"error": resp.get("error", "cmt_tape_set_block_speed failed")})
        return json.dumps(_data_or_error(resp))
    value = _cmt_resolve_speed(speed, allow_custom=False)
    if value == 0:
        return json.dumps(
            {"error": f"Invalid CMT speed: {speed!r} "
                      f"(use a ratio key, 1..9 or pulses_us)"})
    resp = await _send_request(
        "cmt_tape_block_speed",
        {"block_id": int(block_id), "speed": value})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "cmt_tape_set_block_speed failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_symbol_lookup(query: str) -> str:
    """Look up a single symbol by name or by hex address (read-only).

    The query is auto-detected:

    * hex string ("0x4242", "4242h", "$4242") -> address lookup.
    * any other non-empty string -> name lookup (case-sensitive).

    Returns the highest-priority symbol matching the criterion (LBL
    beats MAP beats NOI beats SYM in the symbol database).

    Args:
        query: symbol name or hex address.

    Returns:
        JSON ``{"found": true, "addr": <int>, "name": <str>,
        "comment": <str>, "source": <int>}`` if a match exists, or
        ``{"found": false}`` otherwise. ``source`` is the symbol kind
        enum: 0=SJASMPLUS, 1=NOI, 2=MAP, 3=LBL (= user-added).
    """
    if not isinstance(query, str) or query == "":
        return json.dumps({"error": "query must be a non-empty string"})
    resp = await _send_request("symbol_lookup", {"query": query})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "symbol_lookup failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_symbol_list(prefix: str = "", limit: int = 100) -> str:
    """List symbols matching an optional name prefix.

    Useful for AI introspection of the symbol database, e.g. to browse
    all "ROM_*" routines or all "DATA_*" labels.

    Args:
        prefix: optional name prefix filter (e.g. ``"ROM_"`` matches
            "ROM_CHR_OUT", "ROM_GET_KEY", ...). Empty string means
            "no filter" (= all symbols).
        limit: maximum number of entries returned (1..1000, default 100).

    Returns:
        JSON ``{"count": <int>, "items": [{"addr": <int>, "name": <str>,
        "comment": <str>, "source": <int>}, ...]}`` on success or
        ``{"error": "..."}`` on invalid ``limit``.
    """
    if not isinstance(limit, int) or limit < 1 or limit > 1000:
        return json.dumps({"error": "limit must be 1..1000"})
    resp = await _send_request(
        "symbol_list", {"prefix": prefix, "limit": limit})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "symbol_list failed")})
    return json.dumps(_data_or_error(resp))


# === V1.A.3 - step out + run_until_* Tools (4 nové) ==================
# Pokročilé krokování pro AI ladění - step_out přes callstack frame
# pop, run_until_raster pro mid-frame efekty, run_until_tstate pro
# přesné cycle targeting, run_until_event jako základ event-driven
# debug workflow. Polling implementace - V1.A.4 přinese event
# subscribe a notifications.


@mcp.tool()
async def emu_step_out(max_cycles: int = 10_000_000,
                       deadline_frames: int | None = None,
                       deadline_ms: int | None = None,
                       deadline_ticks: int | None = None) -> str:
    """Run until RET from current Z80 subroutine (callstack frame pop).

    Uses shadow callstack tracking to look up the return address of the
    top frame, then sets a temporary breakpoint and runs until PC reaches
    it (the stop emits the event ``paused`` with ``reason: "run_to"``).
    Useful for the "step out of this function" debug workflow.

    Requires callstack tracking to be active (toggle via the Callstack
    debugger window or the --callstack CLI flag). Requires emulator
    to be paused before invocation.

    Wait for the stop with ``emu_wait_stop(run_seq)``. When the function
    may never return, give a deadline in emulated time (at most one of
    ``deadline_frames``, ``deadline_ms``, ``deadline_ticks``, see
    ``emu_resume``); the run then stops with the reason ``"deadline"``.

    Args:
        max_cycles: informative value (default 10M), echoed back, never
            enforced.
        deadline_frames: stop after N frame boundaries.
        deadline_ms: stop after N ms of emulated time.
        deadline_ticks: stop after N GDG clock ticks.

    Returns:
        JSON ``{"return_addr": <int>, "max_cycles": <int>, "running":
        true, "run_seq": <int>}`` (plus ``deadline_ticks``) on success, or
        ``{"error": "..."}`` if callstack tracking is inactive, the stack
        is empty, or the emulator is already running.
    """
    if not isinstance(max_cycles, int) or max_cycles < 1:
        return json.dumps({"error": "max_cycles must be >= 1"})
    deadline, err = _deadline_arg(deadline_frames, deadline_ms, deadline_ticks)
    if err:
        return json.dumps({"error": err})
    args: dict[str, Any] = {"max_cycles": max_cycles}
    if deadline is not None:
        args["deadline"] = deadline
    resp = await _send_request("step_out", args)
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "step_out failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_run_until_raster(line: int, col: int = -1,
                                max_cycles: int = 10_000_000) -> str:
    """Run until GDG raster reaches the given scanline (and optional column).

    Useful for debugging mid-frame raster effects (palette swaps, scroll
    register writes, BCOL changes) timed against the video beam position.

    Implementation is a polling loop: the server alternates ``step_into``
    with a raster snapshot until the target line/column is reached or
    ``max_cycles`` worth of T-states have been consumed. Precision is
    bounded by Z80 instruction length (~4-23 T-states ~ ~10 GDG ticks).

    Args:
        line: target scanline (0..511; PAL frames have 312 lines, NTSC
            has 262 - values outside the active frame may never be
            reached).
        col: target raster column on the target line (-1 = any column).
        max_cycles: safety cap on T-states spent polling (default 10M).

    The loop runs in the emulator's request thread and cannot be
    cancelled; it ends after at most 20 s of wall-clock time
    (``time_limited: true``) so that the call answers before the 30 s
    transport limit. The emulator then stays paused where the loop got.

    Returns:
        JSON ``{"scanline": <int>, "column_pixel": <int>, "frame_number":
        <int>, "total_cycles": <int>, "delta_cycles": <int>, "reached":
        <bool>, "time_limited": <bool>}``. ``reached=false`` means the
        loop stopped at the safety cap or at the time limit.
    """
    if not isinstance(line, int) or line < 0 or line > 511:
        return json.dumps({"error": "line must be 0..511"})
    if not isinstance(col, int) or col < -1 or col > 2047:
        return json.dumps({"error": "col must be -1..2047"})
    if not isinstance(max_cycles, int) or max_cycles < 1:
        return json.dumps({"error": "max_cycles must be >= 1"})
    resp = await _send_request(
        "run_until_raster",
        {"line": line, "col": col, "max_cycles": max_cycles})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "run_until_raster failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_run_until_tstate(target_total_cycles: int | None = None,
                                max_cycles: int = 10_000_000,
                                target_ticks: int | None = None) -> str:
    """Run until the emulated time reaches a target.

    Two ways to give the target (exactly one of them):

    - ``target_ticks`` (recommended): absolute emulated time in GDG ticks
      (64-bit, the ``t`` column of ``emu_trace_read``, ``Cycle`` in
      breakpoint conditions, ``stop_ticks`` of stop records, ``time.ticks``
      from ``emu_state_observe``). It
      never wraps and a machine reset does not clear it. The emulator
      stops itself at the start of the first instruction whose time is
      >= the target (an interrupt acknowledge in between belongs to the
      instruction before, so the state is the one after acknowledging).
      The emulator must be paused. A user breakpoint can stop the run
      earlier (``stopped_by``). A target in the past or now returns at
      once with ``reached: true`` and the current time.
    - ``target_total_cycles``: absolute Z80 T-state counter
      (``total_cycles`` in the raster snapshot), uint32 counted since the
      CPU reset; it wraps every ~20 minutes at 3.5 MHz and is not the GDG
      time divided by the CPU divider. Must be greater than the current
      counter. The server steps instruction by instruction.

    Args:
        target_total_cycles: absolute Z80 T-state target (must be greater
            than the current ``total_cycles``).
        max_cycles: safety cap on delta T-states from the start of the
            polling loop (default 10M); only with ``target_total_cycles``.
        target_ticks: absolute GDG tick target (see above).

    Both ways end after at most 20 s of wall-clock time
    (``time_limited: true``; with ``target_ticks`` the emulator is paused
    then).

    Returns:
        With ``target_total_cycles``: JSON ``{"total_cycles": <int>,
        "target": <int>, "delta_cycles": <int>, "reached": <bool>,
        "time_limited": <bool>}``.
        With ``target_ticks``: JSON ``{"ticks": <int>, "target_ticks":
        <int>, "delta_ticks": <int>, "exact": <bool>, "reached": <bool>,
        "stopped_by": <str>, "time_limited": <bool>, "run_seq": <int>}``;
        ``exact`` = an instruction started exactly at the target.
    """
    if (target_total_cycles is None) == (target_ticks is None):
        return json.dumps(
            {"error": "give exactly one of target_total_cycles and target_ticks"})
    if target_ticks is not None:
        if not isinstance(target_ticks, int) or target_ticks < 0:
            return json.dumps({"error": "target_ticks must be a non-negative int"})
        resp = await _send_request("run_until_tstate", {"target_ticks": target_ticks})
        if not resp.get("success", False):
            return json.dumps(
                {"error": resp.get("error", "run_until_tstate failed")})
        return json.dumps(_data_or_error(resp))
    if not isinstance(target_total_cycles, int) or target_total_cycles < 0:
        return json.dumps(
            {"error": "target_total_cycles must be a non-negative int"})
    if not isinstance(max_cycles, int) or max_cycles < 1:
        return json.dumps({"error": "max_cycles must be >= 1"})
    resp = await _send_request(
        "run_until_tstate",
        {"target_total_cycles": target_total_cycles,
         "max_cycles": max_cycles})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "run_until_tstate failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_run_until_event(kind: str,
                               params: dict = None,
                               max_cycles: int = 10_000_000) -> str:
    """Run until a specific event occurs.

    Supported event kinds in V1.A.3:

    * ``"frame_done"``     - run N video frames forward. ``params`` may
      contain ``{"count": <int>}`` (default 1, range 1..10000).
    * ``"breakpoint_hit"`` - launch the emulator and wait until any
      breakpoint pauses it (or the safety cap fires). ``params`` may
      contain ``{"id": <int>}`` but in V1.A.3 the id is ignored - any
      pause satisfies the wait.
    * ``"io_write"``       - **not implemented in V1.A.3** (requires
      eventlog IO category hookup; planned for V1.A.4). Returns an
      error.

    Args:
        kind: event kind string (see above).
        params: kind-specific parameters (optional).
        max_cycles: safety cap on T-states (default 10M).

    The wait ends after at most 20 s of wall-clock time
    (``time_limited: true``; for ``breakpoint_hit`` the emulator is then
    paused). For long waits prefer ``emu_resume`` with a deadline and
    ``emu_wait_stop``.

    Returns:
        JSON ``{"kind": <str>, "reached": <bool>, "delta_cycles": <int>,
        "time_limited": <bool>, ...}`` plus kind-specific fields
        (``frames_done`` for ``frame_done``, ``paused`` for
        ``breakpoint_hit``).
    """
    if kind not in ("frame_done", "breakpoint_hit", "io_write"):
        return json.dumps(
            {"error": f"Unsupported event kind: {kind}"})
    if not isinstance(max_cycles, int) or max_cycles < 1:
        return json.dumps({"error": "max_cycles must be >= 1"})
    args = {"kind": kind,
            "params": params if params is not None else {},
            "max_cycles": max_cycles}
    resp = await _send_request("run_until_event", args)
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "run_until_event failed")})
    return json.dumps(_data_or_error(resp))


# === V1.A.4 - EVENT subscribe + TRAP forwarding Tools ================
# Asynchronni interakce mezi AI klientem a emulatorem: misto pollovani
# get_state v cyklu (= drahe + pomale) klient subscribe na topics a
# pravidelne pollne event_poll. BP hit pripadne posle TRAP s trap_id;
# klient na nej odpovida emu_trap_respond (continue/step/abort).
#
# Topic catalog V1.A.4:
#   * breakpoint_hit   IMPLEMENTOVANO (BP hook v breakpoints.c)
#   * paused           IMPLEMENTOVANO (BP hook + CMD_PAUSE handler)
#   * step_done        deklarovan, emit body odlozen do V1.A.5+
#   * io_write         deklarovan, emit body odlozen do V1.A.5+


@mcp.tool()
async def emu_event_subscribe(topics: list[str]) -> str:
    """Subscribe to one or more emulator event topics.

    Supported topics in V1.A.4:

    * ``breakpoint_hit`` - any BP triggered with empty action (i.e. the
      BP would otherwise stop the emulator). Payload:
      ``{id, addr, type, hits, trap_id}``. The ``trap_id`` is used in
      ``emu_trap_respond``.
    * ``paused`` - exactly one event for every stop of the emulator
      (breakpoint, temporary breakpoint, step, ``emu_run`` frames,
      deadline, manual pause, exit). Payload: ``{reason, pc, run_seq,
      state_rev, step, start_ticks, stop_ticks, elapsed_ticks, frame,
      started_by, start_cmd, stopped_by}`` plus ``bp_id`` (breakpoint), ``exit_code`` /
      ``exit_reason`` / ``message`` (exit), ``bytes`` / ``detail``
      (``bp_action_saturation``). Same fields as the record of
      ``emu_wait_stop``.
    * ``resumed`` - the emulator left a pause into a run (not for a single
      step), also when the user resumed it in the GUI. Payload:
      ``{run_seq, state_rev, pc, started_by, start_cmd, start_ticks,
      deadline}``.
    * ``step_done`` - a single step finished. Payload: ``{pc, cycles,
      ticks, run_seq, state_rev}`` (``cycles`` is a 32-bit T-state counter
      that wraps after about 20 minutes of emulation, ``ticks`` are 64-bit
      GDG ticks since the emulator started).
    * ``state_changed`` - the machine state changed in pause by something
      other than an MCP client (GUI debugger windows, menus, device
      windows, replay requests), merged per pass of the pause loop.
      Payload: ``{state_rev, sources}`` (``debugger``, ``reset``,
      ``snapshot``, ``device``, ``replay``, ``input``, ``tape``). See
      ``emulator://docs/state_revision``.
    * ``io_write`` - declared topic, emit hook scheduled for V1.A.5+
      (requires Z80 OUT hot-path hook).

    Events are queued per-connection (pipe = single connection in V1.A.4)
    and retrieved via ``emu_event_poll``. Queue limit is 100 events;
    overflow drops the oldest event and increments a dropped counter.

    Args:
        topics: list of topic names to subscribe to.

    Returns:
        JSON with ``subscribed`` (echo of topic list), ``topics_count``,
        and ``ok`` boolean.
    """
    if not isinstance(topics, list) or not topics:
        return json.dumps({"error": "topics must be a non-empty list"})
    for t in topics:
        if not isinstance(t, str) or not t:
            return json.dumps({"error": "each topic must be a non-empty string"})
    resp = await _send_request("event_subscribe", {"topics": topics})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "event_subscribe failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_event_unsubscribe(topics: list[str] = None) -> str:
    """Unsubscribe from event topics. Empty list or None = unsubscribe all.

    Pending events already in the queue are NOT discarded - they can
    still be retrieved via ``emu_event_poll`` until they expire from
    the queue.

    Args:
        topics: list of topic names to remove, or empty/None for all.

    Returns:
        JSON with ``unsubscribed_all`` flag, ``topics_count``, and
        ``ok`` boolean.
    """
    args = {"topics": topics if topics is not None else []}
    resp = await _send_request("event_unsubscribe", args)
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "event_unsubscribe failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_event_poll(timeout_ms: int = 0,
                          max_events: int = 10) -> str:
    """Retrieve queued events (non-blocking or with short wait).

    The MCP protocol does not natively support server-pushed
    notifications in V1.A.4, so the AI client polls. Use
    ``timeout_ms > 0`` to wait up to N ms if no events are available
    immediately (= more efficient than a tight poll loop).

    Args:
        timeout_ms: wait up to N ms if queue empty (0 = pure poll,
                    max 60000).
        max_events: maximum events to return per call (1..100).

    Returns:
        JSON with ``events`` (array of ``{topic, ts_us, data}``),
        ``count``, ``pending`` (remaining in queue), ``dropped``
        (accumulated backpressure drops).
    """
    if not isinstance(max_events, int) or max_events < 1 or max_events > 100:
        return json.dumps({"error": "max_events must be in 1..100"})
    if not isinstance(timeout_ms, int) or timeout_ms < 0 or timeout_ms > 60000:
        return json.dumps({"error": "timeout_ms must be in 0..60000"})
    # Backend smí čekat až timeout_ms: limit odpovědi = čekání + běžná
    # rezerva SEND_TIMEOUT_S, jinak by poll nad 30 s skončil chybou wrapperu.
    resp = await _send_request(
        "event_poll",
        {"timeout_ms": timeout_ms, "max_events": max_events},
        timeout_s=SEND_TIMEOUT_S + timeout_ms / 1000.0)
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "event_poll failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_trap_respond(trap_id: int, action: str) -> str:
    """Respond to an active TRAP (breakpoint blocking pause).

    When a BP with empty action triggers, the emulator pauses and the
    event bus emits a ``breakpoint_hit`` topic with a ``trap_id`` field.
    The AI client retrieves that event via ``emu_event_poll`` and uses
    this tool to instruct the emulator how to proceed.

    Supported actions:

    * ``continue``   - resume execution (emu unpaused, dbgapi RUN).
    * ``step_into``  - single instruction step then re-pause.
    * ``step_over``  - step over CALL/RST/DJNZ then re-pause.
    * ``abort``      - in V1.A.4 maps to continue (graceful abort is
      scheduled for V1.A.5+).

    Args:
        trap_id: ID from the ``breakpoint_hit`` event ``data.trap_id``.
        action: one of ``continue|step_into|step_over|abort``.

    Returns:
        JSON with ``trap_id``, ``action``, ``ok`` (trap was known
        and consumed), and ``emu_cmd_ok`` (dbgapi CMD submit
        succeeded).
    """
    if not isinstance(trap_id, int) or trap_id <= 0:
        return json.dumps({"error": "trap_id must be a positive integer"})
    if action not in ("continue", "step_into", "step_over", "abort"):
        return json.dumps(
            {"error": f"Invalid action: {action!r} "
                      "(continue|step_into|step_over|abort)"})
    resp = await _send_request(
        "trap_respond",
        {"trap_id": trap_id, "action": action})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "trap_respond failed")})
    return json.dumps(_data_or_error(resp))


# === V1.A.5 - Chip-level fault injection Tools (5 tools) ==============
# Tato sada Tools vystavuje low-level fault injection a state
# manipulation. Tooly s WARNING anotací v description jsou destruktivní
# a měly by být klientem (= AI agent) zavolány jen po explicit user
# confirmu (per Anthropic MCP guidance).


@mcp.tool()
async def emu_io_read(port: int,
                      expected_state_rev: int | None = None) -> str:
    """Read from a Z80 I/O port (full Z80 IN semantics including side effects).

    Some peripherals have read-side effects (e.g. PSG status flag reset,
    FDC IDX strobe, GDG DMD register latch). This tool does NOT use the
    side-effect-free probe variant - use the in-emulator UI tools when
    you need a pure peek.

    WARNING: Read may cause destructive side effects on chip state.

    Args:
        port: 16-bit I/O port (0-65535; the Z80 uses the low 8 bits for
              most chips, the high byte is on the bus).
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        JSON ``{"port": <int>, "value": <0..255>}`` on success or
        ``{"error": "..."}`` on failure.
    """
    if not isinstance(port, int) or port < 0 or port > 65535:
        return json.dumps({"error": "port must be 0..65535"})
    resp = await _send_request("io_read", _with_rev({"port": port}, expected_state_rev))
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "io_read failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_io_write(port: int, value: int,
                       expected_state_rev: int | None = None) -> str:
    """Write to a Z80 I/O port (full Z80 OUT semantics with side effects).

    The chip on the addressed port reacts exactly as if a Z80 OUT
    instruction had been executed (PSG latch, FDC command, GDG mode,
    PIO output bits, ...).

    WARNING: Destructive operation. May alter banking, video output,
    floppy state, sound. Use only with user consent.

    Args:
        port: 16-bit I/O port (0-65535).
        value: 8-bit value (0-255).
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        JSON ``{"port": <int>, "value": <int>}`` echo on success or
        ``{"error": "..."}`` on failure.
    """
    if not isinstance(port, int) or port < 0 or port > 65535:
        return json.dumps({"error": "port must be 0..65535"})
    if not isinstance(value, int) or value < 0 or value > 255:
        return json.dumps({"error": "value must be 0..255"})
    resp = await _send_request("io_write", _with_rev({"port": port, "value": value}, expected_state_rev))
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "io_write failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_irq_inject(source: str = "manual", vector: int = -1,
                         expected_state_rev: int | None = None) -> str:
    """Force a Z80 maskable IRQ from a synthetic source.

    The Z80 INT line is asserted as if a hardware device requested an
    interrupt. Whether the IRQ is accepted depends on IFF1 (EI/DI):
    if IFF1=0 the IRQ is latched into ``int_pending`` and accepted at
    the next EI.

    WARNING: Destructive. May alter PC, SP, IFF1. Pause the emulator
    before injecting if timing-sensitive code is running.

    Args:
        source: short label for audit logs (default ``"manual"``).
        vector: IM2 vector byte (0-255); ``-1`` = use default
                ``intread_cb`` (typical for IM1 / unconnected).
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        JSON ``{"injected": true, "source": "...", "vector_used": <int|null>}``
        on success or ``{"error": "..."}`` on failure.
    """
    if not isinstance(source, str) or len(source) > 64:
        return json.dumps({"error": "source must be a string <= 64 chars"})
    if not isinstance(vector, int) or vector < -1 or vector > 255:
        return json.dumps({"error": "vector must be -1 or 0..255"})
    resp = await _send_request(
        "irq_inject", _with_rev({"source": source, "vector": vector},
                                expected_state_rev))
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "irq_inject failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_nmi_inject(expected_state_rev: int | None = None) -> str:
    """Force a Z80 NMI (non-maskable interrupt).

    NMI is non-maskable: after the current instruction completes the
    CPU jumps to 0x0066, copies IFF1 to IFF2 and clears IFF1. A
    matching RETN restores IFF1.

    WARNING: Very destructive. NMI cannot be vetoed by software. Pause
    the emulator before injecting.

    Args:
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        JSON ``{"injected": true}`` on success or ``{"error": "..."}``.
    """
    resp = await _send_request("nmi_inject", _with_rev({}, expected_state_rev))
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "nmi_inject failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_mem_write_force(addr: int, data_hex: str,
                              expected_sha256: Optional[str] = None,
                              want_hashes: bool = False,
                              expected_state_rev: int | None = None) -> str:
    """Write bytes to Z80 memory without region-write checks.

    Unlike ``emu_mem_write`` (which refuses to write into ROM, CG-ROM,
    VRAM in MZ-800 native mode and other prohibited regions), this tool
    writes the bytes through unconditionally using the banking-aware
    write path. Useful for fault injection and quick patching of
    routines that map RAM under ROM.

    WARNING: VERY DESTRUCTIVE. Skips the safety check that prevents
    accidental corruption. Use only for testing / fault injection.

    Args:
        addr: Z80 address (0-65535).
        data_hex: hex string of bytes (even length, e.g. ``"3E01C9"``).
            The decoded byte count plus addr must not exceed 0x10000
            (all 64 KiB in one call only from address 0).
        expected_sha256: conditional write, same as ``emu_mem_write``.
        want_hashes: also return ``before_sha256`` and ``after_sha256``.
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        JSON ``{"addr": <int>, "length": <int>}`` (plus the hashes with a
        condition or ``want_hashes``) on success or ``{"error": "..."}``
        on failure; ``Precondition failed:`` = memory changed, nothing
        written.
    """
    if not isinstance(addr, int) or addr < 0 or addr > 0xFFFF:
        return json.dumps({"error": "addr must be 0..65535"})
    if not isinstance(data_hex, str) or not data_hex or len(data_hex) % 2 != 0:
        return json.dumps(
            {"error": "data_hex must be a non-empty even-length hex string"})
    payload: dict[str, Any] = {"addr": addr, "data_hex": data_hex}
    err = _write_cond_payload(payload, expected_sha256, want_hashes)
    if err:
        return json.dumps({"error": err})
    resp = await _send_request("mem_write_force", _with_rev(payload, expected_state_rev))
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "mem_write_force failed")})
    return json.dumps(_data_or_error(resp))


# === V1.A.6 - Watch + Callstack + CDL Tools (9 nových) ===============
#
# Watch Tools (4) - wrapper kolem watch.h storage + bp_expr evaluator.
# Callstack Tool (1) - shadow stack snapshot přes existující GET_CALLSTACK.
# CDL Tools (4) - wrapper kolem mhmap.h Memory Heatmap (FCEUX-style CDL).
#
# Tool descriptions jsou anglicky (MCP wire protocol je locale-agnostic
# = klient zobrazí description uživateli). Docstring se používá jako
# description ve FastMCP.


@mcp.tool()
async def emu_watch_add(name: str = "", addr: int = 0,
                          mode: str = "address",
                          expr: str = "",
                          type: str = "u8") -> str:
    """Add a watch (memory observation) to the debugger watch panel.

    Three watch modes are supported:
      * ``address``     - literal Z80 address + type (default mode)
      * ``expr_scalar`` - expression evaluated as int32, type controls
                          display format only
      * ``expr_deref``  - expression evaluated as uint16 address, then a
                          read of size given by ``type`` is performed

    Supported types: ``u8`` (default), ``i8``, ``u16le``, ``u16be``,
    ``i16le``, ``i16be``, ``u32le``, ``u32be``, ``i32le``, ``i32be``,
    ``bit``, ``ascii``, ``mzascii``, ``bytes``.

    See ``emulator://docs/watch_dsl`` for mode + type details and
    ``emulator://docs/bp_dsl`` for the expression syntax shared with
    breakpoint conditions.

    Args:
        name: Optional row name (empty string = anonymous row).
        addr: Z80 address (0..65535, only used for mode=address).
        mode: One of ``address`` / ``expr_scalar`` / ``expr_deref``.
        expr: Expression text (required for ``expr_*`` modes; uses the
            same syntax as breakpoint condition / action expressions -
            see ``bp_expr`` documentation).
        type: Value type (see list above).

    Returns:
        JSON ``{"index": <int>, "name": ..., "mode": ..., "type": ...,
        "addr": ...}`` on success or ``{"error": "..."}`` on failure.
    """
    if not isinstance(addr, int) or addr < 0 or addr > 0xFFFF:
        return json.dumps({"error": "addr must be 0..65535"})
    if mode not in ("address", "expr_scalar", "expr_deref"):
        return json.dumps(
            {"error": "mode must be address / expr_scalar / expr_deref"})
    if mode != "address" and not expr:
        return json.dumps({"error": "expr is required for expr_* modes"})
    data = {"mode": mode, "addr": addr, "type": type}
    if name:
        data["name"] = name
    if expr:
        data["expr"] = expr
    resp = await _send_request("watch_add", data)
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "watch_add failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_watch_remove(name: str = "", index: int = -1) -> str:
    """Remove a watch row from the debugger watch panel.

    Specify either ``name`` (first row with matching name) or ``index``
    (0-based row index). If both are provided, ``name`` takes precedence.

    Args:
        name: Watch row name (empty = use index).
        index: Watch row index (0..count-1; -1 = use name).

    Returns:
        JSON ``{"removed": <bool>, "index": <int>}``. ``removed=false``
        means the row was not found.
    """
    if not name and (not isinstance(index, int) or index < 0):
        return json.dumps(
            {"error": "specify either name or non-negative index"})
    data = {"index": index}
    if name:
        data["name"] = name
    resp = await _send_request("watch_remove", data)
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "watch_remove failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_watch_list() -> str:
    """List all currently registered watches.

    Returns:
        JSON ``{"count": <int>, "items": [{"index", "name", "mode",
        "type", "addr", "expr", "value"}, ...]}`` with the current value
        formatted according to the row's type + display format.
    """
    resp = await _send_request("watch_list")
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "watch_list failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_watch_eval(name: str = "", index: int = -1,
                           expr: str = "") -> str:
    """Evaluate a watch (current value) or an ad-hoc expression.

    Two evaluation modes:
      * Existing watch: provide ``name`` or ``index``. The watch's
        current value is read using its type + display format.
      * Ad-hoc expression: provide ``expr``. The expression is parsed
        and evaluated with the current CPU state. Useful for one-shot
        checks without polluting the watch list.

    If ``expr`` is provided, it takes precedence over ``name`` / ``index``.
    A watch never changes the machine state: ``[addr]!`` and ``port[N]!``
    are read like ``[addr]`` / ``port[N]`` (no MEM_R breakpoint, no real
    IN); use ``emu_io_read`` for a port read with its side effect.

    See ``emulator://docs/watch_dsl`` for watch modes and
    ``emulator://docs/bp_dsl`` for the full ``expr`` syntax.

    Args:
        name: Watch row name (optional).
        index: Watch row index (optional).
        expr: Ad-hoc expression text (optional, see ``bp_expr`` syntax).

    Returns:
        JSON ``{"value_str": "...", "value_int": <int>, "error": ...}``.
        ``value_str`` holds the formatted display value; ``value_int``
        is the raw int32 result (0 for string/bytes types). On parse
        error ``error`` contains a short technical message and
        ``value_str`` is empty.
    """
    if not expr and not name and (not isinstance(index, int) or index < 0):
        return json.dumps(
            {"error": "specify one of name, index, or expr"})
    data = {"index": index}
    if name:
        data["name"] = name
    if expr:
        data["expr"] = expr
    resp = await _send_request("watch_eval", data)
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "watch_eval failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_callstack_get(max_depth: int = 64) -> str:
    """Read the current Z80 callstack (shadow stack snapshot).

    The Callstack V1 subsystem keeps a shadow stack independent of the
    CPU stack: pushes on CALL / RST / IRQ accept / NMI, pops on RET /
    RETI / RETN. This tool returns a snapshot of the top frames at the
    moment of the request, ordered with ``depth=0`` for the topmost
    (current) frame.

    The subsystem must be active for a meaningful snapshot - when
    ``active=false``, ``count`` will be 0. Activation is controlled by
    the GUI (or ``--callstack`` CLI flag); CP/M-style multi-stack OS
    code can produce divergence events tracked in ``divergence_count``.

    Args:
        max_depth: Maximum number of frames to return (1..256, default 64).

    Returns:
        JSON ``{"active": bool, "count": int, "current_depth": int,
        "max_depth_reached": int, "divergence_count": int,
        "overflow_count": int, "cycles_now": int,
        "frames": [{"depth", "return_addr", "call_site_addr",
                    "target_addr", "sp_at_entry", "cycles_at_entry",
                    "kind"}, ...]}`` where ``kind`` is one of
        ``call`` / ``rst`` / ``irq_im0`` / ``irq_im1`` / ``irq_im2`` /
        ``nmi`` / ``synthetic``.
    """
    if not isinstance(max_depth, int) or max_depth < 1 or max_depth > 256:
        return json.dumps({"error": "max_depth must be in range 1..256"})
    resp = await _send_request("callstack_get", {"max_depth": max_depth})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "callstack_get failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cdl_start() -> str:
    """Start CDL (Code/Data Logger) recording.

    Activates the Memory Heatmap subsystem in ``ALWAYS`` mode, which
    triggers a CPU callback swap: every memory / I/O access is
    classified as Read / Write / eXecute / Stack-write and counted per
    cell. The FCEUX-style CDL bitmap is derived as ``counter > 0``.

    Recording starts from the current emulator state. Existing counters
    are NOT zeroed - call ``emu_cdl_reset`` if you need a clean
    baseline.

    Returns:
        JSON ``{"started": true, "mode": "always"}`` on success or
        ``{"error": "..."}`` on failure.
    """
    resp = await _send_request("cdl_start")
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "cdl_start failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cdl_stop() -> str:
    """Stop CDL recording (data preserved until reset/export).

    Switches the Memory Heatmap subsystem to ``OFF`` mode. Counters
    are kept intact - the client may inspect them via ``emu_cdl_export``
    after stopping. Restart recording with ``emu_cdl_start``.

    Returns:
        JSON ``{"stopped": true, "mode": "off"}`` on success or
        ``{"error": "..."}`` on failure.
    """
    resp = await _send_request("cdl_stop")
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "cdl_stop failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cdl_reset() -> str:
    """Clear all CDL counters (zero the heatmap).

    The recording mode is NOT changed - if recording was active before
    the reset, it continues from a clean baseline. Use to delimit a
    measurement window (e.g. reset, run for a few frames, then export).

    Returns:
        JSON ``{"reset": true}`` on success or ``{"error": "..."}``.
    """
    resp = await _send_request("cdl_reset")
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "cdl_reset failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_cdl_export(path: str) -> str:
    """Export CDL data to a set of binary + JSON files.

    The ``path`` argument must point to the desired meta JSON output
    file (e.g. ``/tmp/cdl-export/snap1.json``). The exporter creates the
    parent directory if missing (``g_mkdir_with_parents``), writes the
    meta JSON with region descriptors, and writes one binary CDL file
    per region alongside it (``snap1_bus.cdl``, ``snap1_ram.cdl``,
    ``snap1_rom_lower.cdl``, ...).

    Each binary file uses 16 B per cell laid out as ``r``, ``w``, ``x``,
    ``s`` uint32 LE counters (FCEUX-style flag = counter > 0).

    Args:
        path: Output path for the meta JSON file (must end with .json
            by convention; not enforced).

    Returns:
        JSON ``{"path": "...", "region_count": <int>}`` on success or
        ``{"error": "..."}`` on failure.
    """
    if not isinstance(path, str) or not path:
        return json.dumps({"error": "path must be a non-empty string"})
    resp = await _send_request("cdl_export", {"path": path})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "cdl_export failed")})
    return json.dumps(_data_or_error(resp))


# === 0017 FÁZE 1 - Tracking lifecycle Tools (4) ======================
#
# Mirror of the CDL tools (emu_cdl_*) for the trace-suite recording
# subsystems. Unlike CDL (a single Memory Heatmap), the trace suite has
# four independent channels, so every tool takes a ``channel`` argument.
#
#   channel | subsystem | what it records
#   --------|-----------|--------------------------------------------
#   cputrack| cputrack  | one event per completed Z80 instruction
#   iorqlog | iorqlog   | I/O port reads / writes (incl. ghost bus)
#   intlog  | intlog    | interrupt / PIO state transitions
#   hwlog   | hwlog     | HW signal edges (CTC, PSG, ...)
#
# The data streams continuously to chunk files under the channel's
# configured dir/name. ``emu_trace_save`` finalizes the current segment
# (flush + close) and may redirect the NEXT segment to a named path.
# ``emu_trace_status`` / ``emu_trace_read`` read the records page by page
# (cursor, filter, character budget), also while recording runs.

# Valid trace channel identifiers (kept in sync with the C dispatch
# layer in src/emulator/mcp/dispatch.c::_trace_parse_channel).
_TRACE_CHANNELS = ("cputrack", "iorqlog", "intlog", "hwlog", "marklog")

# Channels readable with emu_trace_status / emu_trace_read: the trace-suite
# channels plus the Event Viewer ring (``eventlog``, controlled by
# emu_eventlog_start/stop/clear, not by emu_trace_start), the audio capture
# memory, the RAM write log and the breakpoint action output (``bplog``,
# always on, emptied by emu_bplog_clear).
_TRACE_READ_CHANNELS = _TRACE_CHANNELS + ("eventlog", "audio_psg_writes",
                                          "audio_levels", "audio_frames",
                                          "ramlog", "bplog")


def _trace_read_validate_channel(channel: str) -> "str | None":
    """Return an error message if ``channel`` cannot be read with trace_read."""
    if not isinstance(channel, str) or channel not in _TRACE_READ_CHANNELS:
        return ("channel must be one of: "
                + ", ".join(_TRACE_READ_CHANNELS))
    return None


def _trace_validate_channel(channel: str) -> "str | None":
    """Return an error message if ``channel`` is not a valid trace channel.

    Args:
        channel: Channel identifier supplied by the caller.

    Returns:
        ``None`` when the channel is valid, otherwise a human-readable
        error string to be returned as ``{"error": ...}``.
    """
    if not isinstance(channel, str) or channel not in _TRACE_CHANNELS:
        return ("channel must be one of: "
                + ", ".join(_TRACE_CHANNELS))
    return None


@mcp.tool()
async def emu_trace_start(channel: str, regs: bool | None = None) -> str:
    """Start trace recording for a trace-suite channel.

    Switches the selected channel's mode to ``ALWAYS`` and triggers the
    same callback recompute used by the debugger, so recording begins
    from the current emulator state.

    Args:
        channel: One of ``cputrack``, ``iorqlog``, ``intlog``, ``hwlog``,
            ``marklog``.
        regs: cputrack only. ``True`` records the CPU registers after each
            instruction (format 3, 32 bytes per record instead of 12), so
            ``emu_trace_read`` can show them before and after every
            instruction and filter by them; ``False`` records format 2;
            omitted = the configured default (``[TRACE_CPUTRACK] regs``).
            Fails while cputrack is already recording in the other format
            (stop it first).

    Returns:
        JSON ``{"started": true}`` on success or ``{"error": "..."}``.
    """
    err = _trace_validate_channel(channel)
    if err is not None:
        return json.dumps({"error": err})
    data: dict[str, Any] = {"channel": channel}
    if regs is not None:
        data["regs"] = bool(regs)
    resp = await _send_request("trace_start", data)
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "trace_start failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_trace_stop(channel: str) -> str:
    """Stop trace recording for a trace-suite channel.

    Switches the selected channel's mode to ``OFF`` and flushes + closes
    the current segment. The recorded chunk files stay on disk.

    Args:
        channel: One of ``cputrack``, ``iorqlog``, ``intlog``, ``hwlog``,
            ``marklog``.

    Returns:
        JSON ``{"stopped": true}`` on success or ``{"error": "..."}``.
    """
    err = _trace_validate_channel(channel)
    if err is not None:
        return json.dumps({"error": err})
    resp = await _send_request("trace_stop", {"channel": channel})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "trace_stop failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_trace_reset(channel: str) -> str:
    """Flush the current trace segment of a channel to disk (nothing is discarded).

    The streamed trace logs have no counters like CDL. A reset writes the
    pending chunk and the manifest (``<name>.json``) of the current segment
    to disk and recording continues in the **same** segment; already
    recorded data stay. For ``cputrack`` the HALT/self-loop collapse state
    is reset as well: the T-states of a collapsed run that are not written
    yet are dropped and the next instruction starts a new record.
    To start a separate segment, use ``emu_trace_save`` with a new path,
    or ``emu_trace_stop`` and ``emu_trace_start``.

    Args:
        channel: One of ``cputrack``, ``iorqlog``, ``intlog``, ``hwlog``,
            ``marklog``.

    Returns:
        JSON ``{"reset": true}`` on success or ``{"error": "..."}``.
    """
    err = _trace_validate_channel(channel)
    if err is not None:
        return json.dumps({"error": err})
    resp = await _send_request("trace_reset", {"channel": channel})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "trace_reset failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_trace_save(channel: str, path: str = "") -> str:
    """Finalize the current trace segment, optionally redirect output.

    When ``path`` is given, the current segment is CLOSED (its manifest
    finalized) and the channel's output dir/name is derived from path so
    the NEXT segment streams to that location - already written chunk files
    are NOT renamed (the tlog writer binds names at segment open).
    With an empty ``path`` the current segment is flushed to disk IN PLACE
    (pending chunk + manifest) and recording CONTINUES into the SAME segment
    (force save now); the manifest stays valid. Finalize a recording with
    ``trace_stop`` - that closes the segment with the correct manifest.

    Args:
        channel: One of ``cputrack``, ``iorqlog``, ``intlog``, ``hwlog``,
            ``marklog``.
        path: Optional output path for the next segment (the basename
            becomes the chunk-file prefix; the directory is created if
            missing on the next start). Empty string = keep current
            dir/name.

    Returns:
        JSON ``{"saved": true, "path": <str|null>}`` on success or
        ``{"error": "..."}`` on failure.
    """
    err = _trace_validate_channel(channel)
    if err is not None:
        return json.dumps({"error": err})
    data = {"channel": channel}
    if isinstance(path, str) and path:
        data["path"] = path
    resp = await _send_request("trace_save", data)
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "trace_save failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_ramlog_start(ranges: list[dict[str, Any]] | None = None,
                           mode: str = "ring",
                           capacity: int = 1048576) -> str:
    """Start the RAM write log: record every write into the given memory ranges.

    Each record: time (GDG ticks), the address of the writing instruction
    (``pc``), the CPU address, the physical cell (``space`` + ``offset``:
    ``ram``, ``vram``, ``pcg``, ``memext``; ``rom`` = write dropped under
    ROM), old and new value and the source (``insn``, ``int_ack`` /
    ``nmi_ack`` = return address pushed when an interrupt is accepted,
    ``debugger``, ``poke``, ``freeze``, ``cmt_hack``, ``load`` = one summary
    record of a loaded block). A banked address is a different cell: the
    same CPU address 8000h can be RAM or VRAM. A write into the MZ-800
    graphics VRAM gives one record per written plane.

    Read the records with ``emu_trace_read(channel="ramlog")`` (writes that
    did not change the cell are hidden unless ``filter={"all": true}``),
    ask ``emu_ramlog_who_wrote`` (last writer from a per-cell map - works
    even after the ring wrapped) or ``emu_ramlog_writers`` (all writing
    instructions of a range). Starting again replaces the previous log;
    reset and snapshot load clear it.

    Args:
        ranges: ``[{"from": 0x4000, "to": 0x40FF}]`` (CPU addresses) or
            ``[{"space": "vram", "from": 0, "to": 0x1FFF}]`` (cells), at
            most 16; empty = all memory (slower: no RAM fast path for
            writes).
        mode: ``"ring"`` (keep the newest records) or ``"fill"`` (keep the
            first ``capacity`` records, count the rest as ``dropped``).
        capacity: Records in the ring (16-16777216, 32 B each).

    Returns:
        Status (see ``emu_ramlog_status``) or ``{"error": ...}``.
    """
    data: dict[str, Any] = {"mode": mode, "capacity": capacity}
    if ranges:
        data["ranges"] = [dict(r) for r in ranges]
    resp = await _send_request("ramlog_start", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_ramlog_stop() -> str:
    """Stop the RAM write log; the records stay readable until the next start.

    Returns:
        Status (see ``emu_ramlog_status``).
    """
    resp = await _send_request("ramlog_stop", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_ramlog_clear() -> str:
    """Empty the RAM write log (new segment); a running log continues.

    Returns:
        Status (see ``emu_ramlog_status``).
    """
    resp = await _send_request("ramlog_clear", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_bplog_clear() -> str:
    """Empty the breakpoint action output ring (channel ``bplog``).

    The ring collects the text of every breakpoint action ``log`` and
    ``stop "..."`` (read it with ``emu_trace_read(channel="bplog")``); it
    is always on. Clearing starts a new segment, so old cursors return
    ``segment_replaced``.

    Returns:
        JSON ``{segment, allocated, capacity, first_seq, next_seq}`` after
        clearing (``allocated: false`` = no action has written anything
        yet, nothing to clear), or ``{"error": ...}``.
    """
    resp = await _send_request("bplog_clear", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_ramlog_status() -> str:
    """State of the RAM write log.

    Returns:
        ``{"active", "allocated", "segment", "mode", "capacity",
        "first_seq", "next_seq", "total", "dropped", "overwritten",
        "memory_bytes", "ranges"}``. ``first_seq`` > 0 = the ring
        overwrote its oldest records; ``dropped`` > 0 = the ``fill`` ring
        is full.
    """
    resp = await _send_request("ramlog_status", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_ramlog_who_wrote(addr: int = -1, space: str = "", offset: int = -1,
                               limit: int = 20,
                               include_unchanged: bool = False) -> str:
    """Who wrote a memory cell: last writer and the write history.

    Give ``addr`` (CPU address in the current banking; the MZ-800 graphics
    VRAM gives its 4 planes) or ``space`` + ``offset`` (a physical cell, as
    in the ``ramlog`` rows).

    Args:
        addr: CPU address 0-FFFFh.
        space: ``ram``, ``vram``, ``pcg``, ``memext`` or ``rom``.
        offset: Offset in the space.
        limit: Max history records per cell (0-1000).
        include_unchanged: Include writes that did not change the cell.

    Returns:
        ``{"active", "segment", "mmio", "cells": [{"space", "offset",
        "plane"?, "last": {"seq", "pc", "src", "in_ring"} | null,
        "history": [{"seq", "t", "frame", "pc", "addr", "old", "new",
        "src", "tin"?, ...}]}]}`` (history newest first; ``t`` = start
        of the writing instruction, ``tin`` = T-state inside it). ``last`` comes from a
        per-cell map and is kept even when the ring overwrote the record
        (``in_ring: false``).
    """
    data: dict[str, Any] = {"limit": limit, "all": include_unchanged}
    if addr >= 0:
        data["addr"] = addr
    else:
        data["space"] = space
        data["offset"] = offset
    resp = await _send_request("ramlog_who_wrote", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_ramlog_writers(from_addr: int, to_addr: int = -1, space: str = "",
                             limit: int = 50,
                             include_unchanged: bool = False) -> str:
    """Which instructions wrote into a range (grouped by writing instruction).

    Without ``space`` the range is CPU addresses at the time of the write;
    with ``space`` it is cell offsets in that space.

    Args:
        from_addr: First address / offset.
        to_addr: Last address / offset (-1 = from_addr).
        space: Empty, or ``ram``, ``vram``, ``pcg``, ``memext``, ``rom``.
        limit: Max writers (1-5000), most writes first.
        include_unchanged: Include writes that did not change the cell.

    Returns:
        ``{"active", "segment", "total", "truncated", "writers": [{"pc",
        "src", "count", "first_seq", "last_seq"}]}``.
    """
    data: dict[str, Any] = {"from": from_addr,
                            "to": from_addr if to_addr < 0 else to_addr,
                            "limit": limit, "all": include_unchanged}
    if space:
        data["space"] = space
    resp = await _send_request("ramlog_writers", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_trace_status(channel: str = "") -> str:
    """State of trace-suite channels for paged reading (``emu_trace_read``).

    Args:
        channel: One of ``cputrack``, ``iorqlog``, ``intlog``, ``hwlog``,
            ``marklog``, ``eventlog`` (Event Viewer ring),
            ``audio_psg_writes``, ``audio_levels``, ``audio_frames``
            (audio capture memory), ``ramlog`` (RAM write log),
            ``bplog`` (breakpoint ``log`` / ``stop`` output); empty = all.

    Returns:
        JSON ``{"channels": [{channel, segment, live, truncated,
        first_seq, next_seq, lost, record_size, format_version, chunks,
        bytes_on_disk, dir, name, has_time}]}``. ``segment`` 0 means the
        channel has recorded nothing yet; after ``emu_trace_stop`` the
        last segment stays readable (``live: false``) until the next
        start of the channel. cputrack also has ``has_regs`` (segment
        recorded with registers, format 3) and ``initial_regs`` (CPU
        state at the start of the last segment, integer values, null
        before the first recording).
    """
    data: dict[str, Any] = {}
    if channel:
        err = _trace_read_validate_channel(channel)
        if err is not None:
            return json.dumps({"error": err})
        data["channel"] = channel
    resp = await _send_request("trace_status", data)
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "trace_status failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_coverage_start(level: str = "branch", key: str = "opcode",
                             capacity: int = 65536, reset: bool = False,
                             reset_on_discontinuity: bool = False) -> str:
    """Start code coverage: which instructions ran and which branches went which way.

    ``level="branch"`` keys by logical PC: executed addresses and, for every
    conditional instruction (JR/JP/CALL/RET cc, DJNZ, LDIR/CPIR/INIR/OTIR
    and their decrementing forms), whether the condition was true
    (``taken``: jump, call, return, next block iteration) and/or false.
    About +16-29 % emulation time (measured). The bytes are decoded from memory after
    the step.

    ``level="detail"`` adds one unit per (physical cell of the first byte,
    PC, code version) with counters ``exec``, ``taken``, ``not_taken``,
    the last operand and the last next PC. The same PC in RAM and in ROM,
    or in another MemExt bank, is a different unit. ``key="opcode"``: a
    new version only when the opcode bytes change (self-modified operands
    stay one unit flagged ``operand_varied``); ``key="full_bytes"``: the
    first 4 bytes of the step including operands. Bytes come from the
    instruction fetch (exactly what the CPU read), which forces the slow
    memory callbacks: about +35-95 % emulation time (measured).

    Reset, snapshot load and replay seek/rewind do not clear the data, they
    are counted in ``discontinuities`` (``reset_on_discontinuity=true``
    clears instead). Read with ``emu_coverage_read``; recipe in
    ``emulator://docs/coverage``.

    Args:
        level: ``"branch"`` or ``"detail"``.
        key: ``"opcode"`` or ``"full_bytes"`` (detail only).
        capacity: Detail units (1-1048576, 32 B each); a unit that does not
            fit is counted in ``dropped`` and ``complete`` turns false.
        reset: True = start with empty data; False = continue the existing
            data if level, key and capacity are the same.
        reset_on_discontinuity: Clear the data on reset / snapshot load.

    Returns:
        Status (see ``emu_coverage_status``) or ``{"error": ...}``.
    """
    resp = await _send_request("coverage_start", {
        "level": level, "key": key, "capacity": capacity, "reset": bool(reset),
        "reset_on_discontinuity": bool(reset_on_discontinuity)})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_coverage_stop() -> str:
    """Stop code coverage collection; the data stay readable.

    Returns:
        Status (see ``emu_coverage_status``).
    """
    resp = await _send_request("coverage_stop", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_coverage_reset() -> str:
    """Clear the coverage data (new ``generation``); a running collection continues.

    Returns:
        Status (see ``emu_coverage_status``).
    """
    resp = await _send_request("coverage_reset", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_coverage_status() -> str:
    """Code coverage status and completeness.

    Fields: ``active``, ``level``, ``key``, ``units`` (detail: units,
    branch: executed addresses), ``capacity``, ``complete`` (nothing
    dropped, all bytes from the fetch, no saturated counter), ``dropped``,
    ``fetch_fallback``, ``saturated``, ``discontinuities``,
    ``host_state_edits`` (PC or AF changed by the debugger),
    ``instructions`` (steps without idle HALT), ``halt_idle``, ``epoch``,
    ``generation`` and ``branches`` {``decisions``, ``both``,
    ``taken_only``, ``not_taken_only``}.

    Returns:
        Status JSON.
    """
    resp = await _send_request("coverage_status", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_coverage_mark() -> str:
    """Start a new coverage epoch ("from now on").

    Every address and unit remembers the epoch in which it first ran;
    ``emu_coverage_read(filter={"new_since_epoch": N})`` then lists only
    code that ran for the first time after the mark (e.g. what a key press
    or a game level added).

    Returns:
        Status with the new ``epoch``.
    """
    resp = await _send_request("coverage_mark", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_coverage_read(cursor: str = "", columns: list[str] | None = None,
                            filter: dict[str, Any] | None = None,
                            limit: int = 500, max_chars: int = 20000) -> str:
    """Read code coverage rows page by page, sorted by PC.

    branch rows (executed addresses): ``pc``, ``state``, ``int_entry``,
    ``first_epoch``. detail rows (units): ``pc``, ``space``, ``offset``,
    ``key`` (opcode bytes in hex), ``len``, ``exec``, ``taken``,
    ``not_taken``, ``state``, ``operand`` (last operand, null without one),
    ``next_pc``, ``first_epoch``, ``flags``. ``state`` is ``none``
    (unconditional), ``unobserved``, ``taken_only``, ``not_taken_only`` or
    ``both``. Rows are arrays in the order of ``columns``.

    Args:
        cursor: ``next_cursor`` of the previous page; empty = from the start.
            After ``emu_coverage_reset`` or a new start an old cursor fails
            with ``segment_replaced:``.
        columns: Subset of the columns of the level (default all).
        filter: ``pc_from``, ``pc_to``, ``space`` (detail: ram, vram, pcg,
            memext, rom, bus), ``state`` (one name or a list; also
            ``"partial"`` = one branch only, ``"decision"`` = any
            conditional), ``new_since_epoch``, ``flags`` (list, any of:
            decision, operand_varied, int_entry, cmt_trap, fetch_fallback,
            saturated, halt, cell_bus, prefix_split, key_truncated).
        limit: Rows per page (1-5000).
        max_chars: Page size limit in characters (1000-45000).

    Returns:
        Status fields plus ``columns``, ``rows``, ``returned``,
        ``page_full``, ``end_of_data`` and ``next_cursor``.
    """
    data: dict[str, Any] = {"limit": limit, "max_chars": max_chars}
    if cursor:
        data["cursor"] = cursor
    if columns:
        data["columns"] = list(columns)
    if filter:
        data["filter"] = dict(filter)
    resp = await _send_request("coverage_read", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_coverage_export(path: str, format: str = "json") -> str:
    """Write all coverage rows into a file (overwritten).

    ``format="json"``: ``{"format": "mz800emu-coverage/1", "status",
    "columns", "rows": [{...}]}``; ``format="csv"``: header and rows, flags
    separated by ``|``. Two JSON exports of two runs can be compared to
    find code that ran in only one of them.

    Args:
        path: Target file path (as seen by the emulator process).
        format: ``"json"`` or ``"csv"``.

    Returns:
        Status plus ``path``, ``format`` and ``rows``.
    """
    resp = await _send_request("coverage_export", {"path": path, "format": format})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_trace_read(channel: str, cursor: str = "", start: str = "oldest",
                         limit: int = 200, format: str = "rows",
                         columns: list[str] | None = None,
                         filter: dict[str, Any] | None = None,
                         max_chars: int = 20000,
                         max_scan: int = 2000000,
                         regs: str = "after") -> str:
    """Read one page of trace-suite, Event Viewer or audio capture records,
    also while running.

    Records are numbered by ``seq`` within a ``segment`` (one recording).
    Page through with ``next_cursor``; ``end_of_data: true`` means the page
    reached the newest record (run the emulator and read on with the same
    cursor). Data still in RAM is readable too - no need to stop or save
    the trace first. A cursor from an older segment (the channel was
    restarted) returns an error starting with ``segment_replaced:``;
    read again from the start.

    Columns (``rows``, values in this canonical order):
      cputrack: seq, pc, bytes, asm, t_std, wait, t, sym (default), kind,
                af, bc, de, hl, ix, iy, sp, wz, i, r, iff1, iff2, im,
                halted (registers only on request)
      iorqlog:  seq, t, frame, pos, type, dir, src, addr, value, pulse
      intlog:   seq, t, frame, pos, class, chip, pin, edge, state
      hwlog:    seq, t, frame, pos, chip, sub, payload
      marklog:  seq, t, frame, pos, id, name
      eventlog: seq, t, frame, pos, cat, sub, pc, text
      audio_psg_writes: seq, t, time_us, frame, psg, byte, ch, reg, value,
                hz, note
      audio_levels: seq, t, time_us, frame, channel, level
      audio_frames: seq, t, time_us, len_ticks, edges, on_pct, rms_level,
                max_level, ref_rms, ref_peak, sdl_rms, sdl_peak, sdl_blocks
      ramlog:   seq, t, frame, pc, sym, addr, space, offset, plane, old, new,
                src, flags, count, tin
      bplog:    seq, t, frame, insn_pc, sym, pc, bp, name, kind, text, flags
    ``t`` is in GDG ticks since emulation start (cputrack: start of the
    instruction, only with ``has_time``, i.e. no PC range record filter).
    cputrack ``asm`` is decoded from the recorded bytes (not current
    memory); an interrupt acknowledge record shows ``INT ACK``. ``kind``
    is ``insn``, ``loop`` (collapsed HALT or loop on itself, time in
    ``wait``), ``int`` (interrupt acknowledge) or ``sync``. Register columns
    need a segment recorded with ``emu_trace_start(regs=True)``
    (``format_version`` 3): ``regs="after"`` shows the state after the
    record, ``"before"`` the state before it (= the previous record; null
    where unknown). A ``sync`` record (``asm`` ``SYNC start``,
    ``SYNC pc_range``, ``SYNC write``) carries the CPU state at the start
    of the segment, when execution returns into the PC range record
    filter, and after a change outside an instruction (debugger, MCP,
    reset, snapshot load); it takes no time. The
    Event Viewer ring (``eventlog``) overwrites its oldest events: a cursor
    below ``first_seq`` is no error, the page starts at ``first_seq`` and
    ``gap`` names the lost range.

    The ``audio_*`` channels read the memory of the audio capture
    (``emu_audio_capture_start`` with ``memory=True``, the default): the
    same data as psg_writes.csv and events.csv plus one summary row per
    audio frame (``seq`` = frame). ``time_us`` counts from the capture
    start; ``hz``/``note`` are given for a new tone divider (``reg``
    ``tone``), computed with the emulator's PSG formula. ``audio_frames``
    lists per-channel arrays in the order of ``emu_audio_capture_status``
    ``channels``: ``edges`` (level changes), ``on_pct`` (time with level >
    0), ``rms_level`` (0-15), ``max_level``; ``ref_*`` / ``sdl_*`` are
    ``[L, R]`` int16 RMS / peak of the reference render and the output
    path (null when not captured). These rings drop their oldest records
    when full (``gap``); the next capture start replaces them
    (``segment_replaced``).

    ``bplog`` holds the text of breakpoint actions ``log "..."`` and
    ``stop "..."`` (the same text the emulator prints as ``[BP-LOG]`` /
    ``[BP-STOP]`` on stdout, which an MCP client does not see). It is
    always on - no start needed, a cursor taken while it is still empty
    stays valid - and costs nothing until an action runs. ``t``, ``frame``, ``insn_pc`` and ``pc`` are the values of
    ``Cycle``, ``Frame``, ``InsnPC`` and ``PC`` in the breakpoint
    expression at that moment (``insn_pc`` = the accessing instruction
    in MEM_R/MEM_W/IORQ breakpoints, ``sym`` is its symbol); ``bp`` is
    the breakpoint id, ``name`` its name, ``kind`` ``log`` or ``stop``.
    Text over 239 bytes is cut (``flags`` ``text_truncated``). The ring
    keeps the last 4096 records (``gap`` / ``lost`` after that); empty it
    with ``emu_bplog_clear``.

    Args:
        channel: ``cputrack``, ``iorqlog``, ``intlog``, ``hwlog``,
            ``marklog``, ``eventlog``, ``audio_psg_writes``,
            ``audio_levels``, ``audio_frames``, ``ramlog`` (RAM write
            log, see ``emu_ramlog_start``) or ``bplog`` (breakpoint
            action output).
        cursor: ``next_cursor`` of the previous page (takes precedence over
            ``start``).
        start: ``"oldest"``, ``"newest"`` (last ``limit`` records) or
            ``"seq:N"``.
        limit: Max rows per page (1-5000); with ``raw_b64`` the record
            count (0 = as many as fit in 1 MiB).
        format: ``"rows"`` (decoded) or ``"raw_b64"`` (raw records, no
            filter; ``record_size`` and ``format_version`` describe them).
        columns: Subset of the channel's columns (default all).
        filter: Optional keys ``pc_from``/``pc_to`` (cputrack), ``port``
            (<= 0xFF matches the low byte), ``dir`` (``in``/``out``),
            ``type`` (``iorq``, ``mreq_mapped``, ``iorq_unconnected``)
            (iorqlog), ``chip``/``sub`` (hwlog), ``marker`` (marklog id),
            ramlog: ``pc_from``/``pc_to`` (writing instruction),
            ``addr_from``/``addr_to`` (CPU address), ``space``, ``src``,
            ``all`` (also writes that did not change the cell or were
            dropped under ROM - hidden by default),
            bplog: ``pc_from``/``pc_to`` (``insn_pc``), ``bp`` (breakpoint
            id, or its exact name as a string), ``kind`` (``log`` or
            ``stop``),
            ``channels`` (list of channel names such as ``"ctc0"``,
            ``"psg0_1"``; audio_levels, audio_psg_writes), ``reg``
            (``tone_lo``, ``tone``, ``volume``, ``noise``; audio_psg_writes),
            ``t_from``/``t_to`` (channels with ``t``), cputrack format 3:
            register columns (``af``, ``bc``, ``de``, ``hl``, ``ix``,
            ``iy``, ``sp``, ``wz``, ``i``, ``r``, ``iff1``, ``iff2``,
            ``im``, ``halted``)
            with a value (number or ``"0x..."`` string) or ``[lo, hi]``,
            applied to the state chosen by ``regs`` - e.g.
            ``{"hl": "0xC000"}`` with ``regs="after"`` finds where HL
            became C000h.
        regs: cputrack: ``"after"`` (default) or ``"before"`` - which state
            the register columns and filters show.
        max_chars: Character budget of a ``rows`` page (1000-45000); the
            whole response stays within it.
        max_scan: Max records examined per call; when exhausted the page
            returns ``scan_exhausted: true`` and a cursor to continue.

    Returns:
        JSON with ``channel``, ``segment``, ``first_seq``, ``next_seq``,
        ``live``, ``truncated``, ``lost``, ``gap``, ``scanned``,
        ``matched``, ``scan_exhausted``, ``page_full``, ``end_of_data``,
        ``columns`` + ``rows`` (or ``record_size``, ``format_version``,
        ``from_seq``, ``count``, ``data_b64``) and ``next_cursor``; or
        ``{"error": "..."}``.
    """
    err = _trace_read_validate_channel(channel)
    if err is not None:
        return json.dumps({"error": err})
    data: dict[str, Any] = {"channel": channel, "limit": limit,
                            "format": format, "max_chars": max_chars,
                            "max_scan": max_scan}
    if cursor:
        data["cursor"] = cursor
    else:
        data["from"] = start
    if columns:
        data["columns"] = list(columns)
    if filter:
        data["filter"] = dict(filter)
    if regs and regs != "after":
        data["regs"] = regs
    resp = await _send_request("trace_read", data)
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "trace_read failed")})
    return json.dumps(_data_or_error(resp))


# === video-capture Task 15 - Video recording Tools (5 + 1) ===========
#
# Lossless gameplay video recording (all builds: MZ-800, MZ-700 PAL/NTSC,
# MZ-1500): ZMBV AVI + 48 kHz PCM
# plus a ``<name>.cuts.json`` sidecar (segments, markers, state events).
# Task 18 added ``emu_videorec_timebase`` (emulated / real-time timebase).
# All six tools map 1:1 to the C dispatch commands ``videorec_*`` (dispatch.c), which run
# on the emulator thread via ``DBGAPI_CMD_VIDEOREC``. Every success reply
# carries the full recording status (see ``emu_videorec_status``).
#
# The complete agent workflow (start, play via send_keys, markers,
# snapshot retake, stop, export to MP4) is documented in ONE place:
# ``emulator://docs/videorec_workflow`` (docs/agent/videorec_workflow.md).

#: Poll interval (seconds) of ``emu_videorec_stop(wait=True)``.
_VIDEOREC_POLL_S = 0.1


@mcp.tool()
async def emu_videorec_start(path: str = "", frames: int = 0) -> str:
    """Start lossless video recording of the emulated screen + sound (any build).

    The target file is created immediately (errors such as a missing
    directory are reported right away); the recording itself begins at the
    end of the next emulated frame - while the emulation is paused it stays
    ``start_pending`` until you run it (e.g. ``emu_run(frames=N)``). The
    frame on which the start is processed is not recorded. Recording counts
    EMULATED frames (50 per second; 60 on MZ-700 NTSC and MZ-1500 - see
    ``fps`` in the status), so the emulation speed (MAX SPEED,
    pauses between tool calls) does not affect the video.

    Args:
        path: Target ``.avi`` on the emulator host (absolute path
            recommended; an existing file is overwritten). Empty = generated
            name ``<platform>_YYYYMMDD_HHMMSS.avi`` (``mz800_...``,
            ``mz700_...``, ``mz1500_...``) in the configured output
            directory (never overwrites an existing recording).
        frames: Auto-stop after this many recorded frames (0 = no limit).
            The emulator keeps running after the auto-stop.

    Returns:
        JSON status (see ``emu_videorec_status``) plus ``start_requested``
        and ``stop_after_frames``, or ``{"error": "..."}`` (e.g. recording
        already running, cannot create the file).

    See ``emulator://docs/videorec_workflow`` for the full workflow.
    """
    # bool je v Pythonu podtřída int - True by se tiše stalo frames=1.
    if isinstance(frames, bool) or not isinstance(frames, int) or frames < 0:
        return json.dumps({"error": "frames must be an integer >= 0"})
    data: dict[str, Any] = {"frames": frames}
    if isinstance(path, str) and path:
        data["path"] = path
    resp = await _send_request("videorec_start", data)
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "videorec_start failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_videorec_stop(wait: bool = True, timeout_s: float = 30.0) -> str:
    """Stop video recording and (by default) wait until the files are complete.

    The stop is executed at the end of the next emulated frame, or at once
    while the emulation is paused. A pending (not yet started) recording is
    cancelled and its file deleted (``start_cancelled``). Finalizing the AVI
    index and writing the ``.cuts.json`` sidecar runs on a background writer
    thread; with ``wait=True`` this tool polls ``videorec_status`` until the
    ``saved`` (or ``failed``) event arrives.

    Args:
        wait: Wait for the ``saved`` / ``failed`` event (default True).
        timeout_s: Maximum wait in seconds (default 30).

    Returns:
        JSON status plus ``stop_requested``; with ``wait`` also ``saved``
        (bool) - ``true`` when the recording was written successfully, then
        ``path`` / ``sidecar`` point to the finished files and
        ``recorded_frames`` is the number of frames in the recording (the
        status itself is already ``idle`` with ``frames`` = 0). On timeout
        ``saved`` is ``false`` and ``warning`` explains it. ``{"error": ...}``
        when no recording is running.
    """
    resp = await _send_request("videorec_stop")
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "videorec_stop failed")})
    data = _data_or_error(resp)
    if not wait or "error" in data or data.get("start_cancelled"):
        return json.dumps(data)
    seq0 = (data.get("last_event") or {}).get("seq", 0)
    deadline = asyncio.get_running_loop().time() + max(0.0, float(timeout_s))
    while True:
        st_resp = await _send_request("videorec_status")
        st = _data_or_error(st_resp)
        if "error" in st:
            return json.dumps(st)
        ev = st.get("last_event") or {}
        if (st.get("state") == "idle" and ev.get("seq", 0) > seq0
                and ev.get("kind") in ("saved", "failed")):
            st["stop_requested"] = True
            st["saved"] = ev.get("kind") == "saved"
            # Po uložení je stav IDLE (frames = 0); počet snímků nese událost.
            st["recorded_frames"] = ev.get("frame", 0)
            return json.dumps(st)
        if asyncio.get_running_loop().time() >= deadline:
            st["stop_requested"] = True
            st["saved"] = False
            st["warning"] = ("timed out waiting for the recording to be "
                             "saved; poll emu_videorec_status")
            return json.dumps(st)
        await asyncio.sleep(_VIDEOREC_POLL_S)


@mcp.tool()
async def emu_videorec_pause(paused: Optional[bool] = None) -> str:
    """Record-pause: keep emulating but stop writing frames (or resume).

    Takes effect at the end of the next emulated frame. Pausing closes the
    current segment of the recording, resuming opens a new one (the export
    joins segments with the configured transition, e.g. a fade).

    Args:
        paused: ``true`` = pause recording, ``false`` = resume, omitted /
            ``null`` = toggle. Explicit values are idempotent.

    Returns:
        JSON status plus ``pause_requested`` and ``paused_target``, or
        ``{"error": ...}`` when no recording is running.
    """
    data: dict[str, Any] = {}
    if paused is not None:
        if not isinstance(paused, bool):
            return json.dumps({"error": "paused must be a boolean or null"})
        data["paused"] = paused
    resp = await _send_request("videorec_pause", data)
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "videorec_pause failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_videorec_marker(label: str = "") -> str:
    """Add a marker (chapter) at the current position of the recording.

    The marker gets the index of the next recorded frame and is stored in
    the ``.cuts.json`` sidecar; the export turns markers into YouTube
    chapters. A retake (loading a snapshot taken during this recording)
    discards markers at or after the snapshot point.

    Args:
        label: Marker text; empty = ``"Marker at frame N"``.

    Returns:
        JSON status plus ``marker_requested`` and ``label`` (the label
        actually used), or ``{"error": ...}`` when no recording is running
        (also right after ``emu_videorec_start`` before any frame ran).
    """
    data: dict[str, Any] = {}
    if isinstance(label, str) and label:
        data["label"] = label
    resp = await _send_request("videorec_marker", data)
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "videorec_marker failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_videorec_timebase(timebase: str) -> str:
    """Switch the recording timebase: emulated time or real time.

    ``emulated`` (default): every emulated frame is one video frame (turbo
    and pauses do not change the video). ``realtime``: 50 (60 on MZ-700
    NTSC and MZ-1500) frames per second of wall-clock time with whatever was on the screen and the sound that
    went to the speaker - turbo plays fast, pauses follow the INI option
    ``realtime_pause``. Applies to the running recording (switching to real
    time within one frame period, back to emulated time at the end of the next emulated
    frame; each switch starts a new segment) and to the next start.
    It changes the persistent user setting (saved to the INI on exit, the
    same as the GUI switch) - on a shared instance switch back to
    ``emulated`` when done.
    Without an audio device (``--headless``, ``--mcp-pipe``) the real-time
    sound is rendered by the same SDL path without playing it.

    Args:
        timebase: ``"emulated"`` or ``"realtime"``.

    Returns:
        JSON status (see ``emu_videorec_status``) plus
        ``timebase_requested``, or ``{"error": ...}`` for an invalid value.
    """
    if timebase not in ("emulated", "realtime"):
        return json.dumps({"error": 'timebase must be "emulated" or "realtime"'})
    resp = await _send_request("videorec_timebase", {"timebase": timebase})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "videorec_timebase failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_videorec_status() -> str:
    """Return the video recording status.

    Returns:
        JSON ``{"supported": bool, "state": "idle"|"recording"|"paused",
        "start_pending": bool, "frames": int, "fps": int,
        "duration_s": float, "segment": int, "segment_open": bool,
        "bytes": int, "parts": int,
        "retake_mode": "off"|"discard"|"seam", "path": str|null,
        "sidecar": str|null, "last_error": str|null,
        "timebase": "emulated"|"realtime",
        "timebase_effective": "emulated"|"realtime",
        "rt_activity": "off"|"live"|"frozen"|"skipping",
        "last_event": null | {"seq": int, "kind": "started"|"saved"|
        "failed"|"retake"|"seam", "frame": int, "path": str|null,
        "text": str}}``.

        ``frames`` counts recorded frames (after a retake it drops back to
        the snapshot point). ``path`` / ``bytes`` / ``parts`` describe the
        running or the last recording; ``path`` is the first AVI part
        (long recordings continue in ``<name>_002.avi``, ...).
        ``retake_mode`` is the mode of the running recording (without a
        recording: the setting the next start will use); ``discard``
        rewinds the recording when a snapshot taken during it is loaded.
        ``timebase`` is the requested timebase (see ``emu_videorec_timebase``),
        ``timebase_effective`` the one in use right now (``realtime`` with
        ``realtime_speed = emulated_when_fast`` falls back to ``emulated``
        while the speed is not 100 %); ``rt_activity`` tells whether real-time
        recording writes live frames, frozen frames (paused emulation) or
        skips.
    """
    resp = await _send_request("videorec_status")
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "videorec_status failed")})
    return json.dumps(_data_or_error(resp))


# === V1.A.7 - Profiler Tools (5) =====================================
#
# Profiler V1 (per-function CPU profiler) je v master mz800new mergnut
# spolu s Callstack V1 (= listener API). Tato fáze vystavuje 5 control +
# inspection Tools nad existujícím Profiler V1 backendem.
#
# Performance pozn.: aktivní profiler má measurable hot-path overhead
# (= callstack listener fan-out + hash mapa update + parallel stack push/
# pop per CALL/RST/IRQ/NMI). Po dokončení měření doporučeno volat
# emu_profiler_stop. Když nikdo neregistroval listener, fan-out je nulový
# (= žádný if-active check v Z80 step).
#
# Profiler Resource (= 'emulator://profiler/...') je ponechán pro V1.D.
# EVENT subscribe pro profiler aktualizace je V1.D / V2.


@mcp.tool()
async def emu_profiler_start() -> str:
    """Start CPU profiler recording (per-function call counts and
    exclusive/inclusive Z80 cycles). Built on top of Callstack V1
    listener API.

    Activating the profiler also activates the callstack subsystem
    if it was off (ownership tracked - profiler stop restores prior
    state).

    WARNING: Active profiler has measurable hot-path overhead
    (callstack listener fan-out + hash-map update on every CALL/
    RST/IRQ/NMI). Stop the profiler when not needed.

    Returns:
        JSON ``{"active": true}`` on success or ``{"error": "..."}``.
    """
    resp = await _send_request("profiler_start")
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "profiler_start failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_profiler_stop() -> str:
    """Stop CPU profiler recording.

    Aggregated data is preserved - the client may still call
    ``emu_profiler_get`` or ``emu_profiler_export`` afterwards. To
    discard data, call ``emu_profiler_reset``.

    Returns:
        JSON ``{"active": false}`` on success or ``{"error": "..."}``.
    """
    resp = await _send_request("profiler_stop")
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "profiler_stop failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_profiler_reset() -> str:
    """Clear all profiler aggregate data and reset the baseline cycle
    counter.

    The active flag is not changed - if profiler is recording, it
    keeps recording (new samples start from zero).

    Returns:
        JSON ``{"reset": true}`` on success or ``{"error": "..."}``.
    """
    resp = await _send_request("profiler_reset")
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "profiler_reset failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_profiler_export(path: str, format: str = "csv") -> str:
    """Export profiler aggregate data to a file.

    The export operation is read-only with respect to profiler state
    (snapshot + formatter, no reset, no recording change).

    Format details:
      * ``csv`` - UTF-8, LF line endings, locale-safe numeric format.
        Header row: ``addr,kind,calls,excl_cycles,incl_cycles,
        min_cycles,max_cycles,avg_cycles``.
      * ``json`` - ``{ "stats": {...}, "entries": [...] }`` object.

    Args:
        path: Output filesystem path. Parent directory must exist.
        format: ``"csv"`` (default) or ``"json"``.

    Returns:
        JSON ``{"path": "...", "format": "csv"|"json",
        "entry_count": <int>}`` on success or ``{"error": "..."}``.
    """
    if not isinstance(path, str) or not path:
        return json.dumps({"error": "path must be a non-empty string"})
    if format not in ("csv", "json"):
        return json.dumps({"error": "format must be 'csv' or 'json'"})
    resp = await _send_request("profiler_export",
                                 {"path": path, "format": format})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "profiler_export failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_profiler_get(limit: int = 50) -> str:
    """Get the current profiler aggregate inline (no file export).

    The returned ``entries`` array is taken from the hash-map in
    arbitrary order - the client is expected to sort it (typically by
    ``excl_cycles`` descending) according to its needs. ``limit`` only
    bounds the number of returned entries; total entry count is
    available in the response.

    Args:
        limit: Maximum number of entries to return (1..1000,
            default 50).

    Returns:
        JSON ``{"active": bool, "entry_count": int, "limit": int,
        "total_cycles_64": int, "total_calls": int,
        "irq_entries": int, "unmatched_returns": int,
        "max_depth_reached": int, "overflow_count": int,
        "entries": [{"addr": int, "kind": str, "calls": int,
                     "excl_cycles": int, "incl_cycles": int,
                     "min_cycles": int, "max_cycles": int,
                     "avg_cycles": int}, ...]}``
        on success or ``{"error": "..."}`` on failure.

        ``kind`` is one of ``call``, ``rst``, ``irq_im0``,
        ``irq_im1``, ``irq_im2``, ``nmi``, ``synthetic``.
    """
    if not isinstance(limit, int) or limit < 1 or limit > 1000:
        return json.dumps({"error": "limit must be in range 1..1000"})
    resp = await _send_request("profiler_get", {"limit": limit})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "profiler_get failed")})
    return json.dumps(_data_or_error(resp))


# === V1.B.1 - Media Tools (5) ========================================
# Sjednocený přístup k media operacím: CMT pásek, FDC, QD, IDE8 HDD a
# raw memory load. Slot konvence: "cmt" | "fdc0_fd0".."fdc0_fd3" |
# "fdc1_fd0".."fdc1_fd3" | "qd" | "ide8".
# Pro CMT (load_mzf) je dostupná i CMT-hack instant variantra (= obejde
# tape emulation, nahraje přímo do RAM).


@mcp.tool()
async def emu_media_load_mzf(path: str = "", bytes_b64: str = "") -> str:
    """Load an MZF program via the CMT hack (instant load into RAM,
    bypasses cassette emulation). Loads BOTH the 128-byte header and the
    full program body into RAM at the MZF load address (LOAD_ADDR).

    Provide exactly one of ``path`` or ``bytes_b64``. The MZF file is
    parsed by the cmthack subsystem: the header is read into the standard
    Sharp tape header buffer (0x10F0), the body is then placed at the
    load address from the MZF header. No tape playback is simulated.

    This is a load-only primitive: it does NOT set PC or SP and restores
    the CPU scratch registers (HL/BC/AF) it uses. Use ``emu_media_run_mzf``
    to load and start execution.

    SIDE EFFECT: For programs whose load address is below 0x1000 the lower
    ROM (0x0000-0x0FFF) is temporarily unmapped during the body write so
    the program lands in RAM (otherwise it would be written under ROM and
    lost). The original memory map is restored after the load completes,
    so there is no lasting banking side effect.

    Args:
        path: Filesystem path to a .mzf file.
        bytes_b64: Inline base64-encoded MZF content (server decodes to
            a temporary file before passing to the emulator).

    Returns:
        JSON ``{"ok": true, "load_addr": int, "exec_addr": int,
        "size": int, "result_code": int}`` on success (``load_addr`` =
        MZF fstrt, ``exec_addr`` = MZF fexec, ``size`` = body length).
        ``{"error": "..."}`` on failure (file cannot be opened / invalid
        header / body load failed) - no false success is returned.
    """
    has_path = bool(path)
    has_b64 = bool(bytes_b64)
    if has_path == has_b64:
        return json.dumps(
            {"error": "Provide exactly one of: path, bytes_b64"})
    args = {"path": path} if has_path else {"bytes_b64": bytes_b64}
    resp = await _send_request("media_load_mzf", args)
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "media_load_mzf failed")})
    return json.dumps(_data_or_error(resp))


def _parse_mzf_header(raw: bytes) -> dict:
    """Parsuje 128-bajtovou MZF hlavičku.

    Návratová struktura:
      - ``file_type`` (int) - typ programu (1 = machine code, 2 = BASIC, ...)
      - ``filename`` (str) - Sharp ASCII filename, trimmed
      - ``file_size`` (int) - délka body bloku v bajtech
      - ``load_addr`` (int) - kam se body uloží do RAM
      - ``exec_addr`` (int) - kam se po LOAD skočí (= STRT)
    """
    if len(raw) < 0x18:
        return {"error": "MZF too short (need at least 24 bytes header)"}
    file_type = raw[0]
    filename = raw[1:0x11].rstrip(b" \x00\r").decode("ascii", errors="replace")
    file_size = raw[0x12] | (raw[0x13] << 8)
    load_addr = raw[0x14] | (raw[0x15] << 8)
    exec_addr = raw[0x16] | (raw[0x17] << 8)
    return {
        "file_type": file_type,
        "filename": filename,
        "file_size": file_size,
        "load_addr": load_addr,
        "exec_addr": exec_addr,
    }


@mcp.tool()
async def emu_media_run_mzf(path: str = "", bytes_b64: str = "",
                            bootstrap: bool = True) -> str:
    """Load an MZF program AND start executing it from the MZF EXEC
    address.

    With ``bootstrap=True`` (default) the emulator is RESET and the
    program is started exactly like the CLI option ``--run-mzf``: the
    machine is put into the state the ROM leaves it in after loading a
    program from tape (CPU registers incl. IM 1, DI and SP=0x10F0, 8255,
    8253 CTC, Z80 PIO, PSG, GDG, VRAM, monitor work area in RAM, the
    entry registers the platform ROM passes to the program), then PC =
    exec address. The reset is the same as ``emu_reset``, so the result
    does not depend on what ran in the emulator before. The pause state
    is kept: if the emulation was paused, the program waits at its first
    instruction. The file is checked before the reset - a missing,
    unreadable or truncated file returns an error and does NOT reset the
    machine.

    With ``bootstrap=False`` this is the older composite without a reset:
    ``emu_media_load_mzf`` + ROM disconnect via ``emu_io_write`` on ports
    0xE0/0xE1 + ``emu_set_register`` PC=exec_addr. Nothing else is
    initialised: interrupt mode, IFF, Z80 PIO, CTC, palette and all other
    state stay whatever the previous program left there, so a program can
    behave differently than after a real tape load (e.g. no interrupts).

    WARNING: Both variants are destructive. Save a snapshot beforehand if
    you need to recover the previous state.

    Provide exactly one of ``path`` or ``bytes_b64``. The MZF header is
    parsed client-side (Python) to report the header fields.

    Args:
        path: Filesystem path to a .mzf file.
        bytes_b64: Inline base64-encoded MZF content.
        bootstrap: True = reset + ``--run-mzf`` bootstrap (defined state),
            False = load into the running machine and jump (no reset).

    Returns on success:
      ``{"loaded": true, "header": {file_type, filename, file_size,
         load_addr, exec_addr}, "bootstrap": bool, "reset": bool,
         "pc_set_to": int}``; with ``bootstrap=False`` also
         ``"rom_disconnected": true``.

    Returns on error:
      ``{"error": "..."}``. With ``bootstrap=False`` the partial
      operations performed before the error are NOT rolled back.
    """
    has_path = bool(path)
    has_b64 = bool(bytes_b64)
    if has_path == has_b64:
        return json.dumps(
            {"error": "Provide exactly one of: path, bytes_b64"})

    # Klient-side header parse (= rychlé, bez round-tripu)
    try:
        if has_path:
            with open(path, "rb") as f:
                raw = f.read(128)
        else:
            import base64
            raw = base64.b64decode(bytes_b64)[:128]
    except Exception as e:
        return json.dumps({"error": f"MZF read failed: {e}"})

    hdr = _parse_mzf_header(raw)
    if "error" in hdr:
        return json.dumps(hdr)
    exec_addr = hdr["exec_addr"]
    args = {"path": path} if has_path else {"bytes_b64": bytes_b64}

    if bootstrap:
        # Reset + bootstrap jako CLI --run-mzf, celé v emulátoru v jednom
        # příkazu (soubor ověří emulátor před resetem).
        resp = await _send_request("media_run_mzf", args)
        if not resp.get("success", False):
            return json.dumps(
                {"error": resp.get("error", "media_run_mzf failed"),
                 "header": hdr})
        return json.dumps({
            "loaded": True,
            "header": hdr,
            "bootstrap": True,
            "reset": True,
            "pc_set_to": exec_addr,
        })

    # Varianta bez resetu: 1. Load MZF body do RAM
    resp = await _send_request("media_load_mzf", args)
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "media_load_mzf failed"),
             "header": hdr})

    # 2. Odpoj dolní + horní ROM (= GDG 0xE0/0xE1 bits per knowledge base)
    for port in (0xE0, 0xE1):
        r = await _send_request("io_write", {"port": port, "value": 0})
        if not r.get("success", False):
            return json.dumps(
                {"error": f"io_write port={port:#x} failed",
                 "header": hdr,
                 "loaded": True})

    # 3. Skoč na MZF EXEC
    r = await _send_request("set_register",
                            {"reg": "PC", "value": exec_addr})
    if not r.get("success", False):
        return json.dumps(
            {"error": "set_register PC failed",
             "header": hdr,
             "loaded": True,
             "rom_disconnected": True})

    return json.dumps({
        "loaded": True,
        "header": hdr,
        "bootstrap": False,
        "reset": False,
        "rom_disconnected": True,
        "pc_set_to": exec_addr,
    })


@mcp.tool()
async def emu_media_load_binary(path: str, addr: int) -> str:
    """Load raw binary data from a file to Z80 memory at the given
    address. WARNING: destructive operation - bytes are written through
    the banking-aware path but no region checks apply, so ROM-shadow RAM,
    video memory or any other mapped region in the current bank may be
    overwritten.

    Unlike ``emu_media_load_mzf`` this does NOT parse the MZF header -
    it writes raw bytes directly. Useful for loading raw assembly output
    or fragments. The write stops at the 64 KB boundary if the file is
    larger than the remaining address space.

    Args:
        path: Filesystem path to a binary file.
        addr: Z80 memory address to load at (0..65535).

    Returns:
        JSON ``{"ok": true, "addr": int, "size": int,
        "result_code": int}`` on success or ``{"error": "..."}`` on
        failure.
    """
    if not path:
        return json.dumps({"error": "path is required"})
    if not isinstance(addr, int) or addr < 0 or addr > 65535:
        return json.dumps({"error": "addr must be in range 0..65535"})
    resp = await _send_request(
        "media_load_binary", {"path": path, "addr": addr})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "media_load_binary failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_media_insert(
    slot: str,
    path: str = "",
    bytes_b64: str = "",
    ro: bool = False,
    expected_state_rev: int | None = None,
) -> str:
    """Insert a media image into a peripheral slot. WARNING: if the slot
    already has a mounted image, it is automatically unmounted before
    the new image is attached (no prompt - silent replace).

    Slot values:
        cmt          - CMT cassette tape (.mzf, .mzt, ...)
        fdc0_fd0..3  - WD279x FDC0 (standard, ports 0xD8-0xDF) drive 0..3 (.dsk)
        fdc1_fd0..3  - WD279x FDC1 (secondary, ports 0x58-0x5F) drive 0..3 (.dsk)
        qd           - MZ-1F11 Quick Disk image (.mzq); the file must
                       exist. If the drive is not connected as MZ-1F11
                       (default: Unicard boot loader), it is switched like
                       the MZ-1F11 menu choice (Unicard boot loader is
                       disconnected from the QD) and the response has
                       ``"switched_to_mz1f11": true``.
        ide8         - IDE8 master HDD image (.img)

    Provide exactly one of ``path`` or ``bytes_b64``. The slot must be
    available in the current architecture build (e.g. FDC is only
    compiled on MZ-800).

    Args:
        slot: One of 'cmt' | 'fdc0_fd0'..'fdc0_fd3' |
            'fdc1_fd0'..'fdc1_fd3' | 'qd' | 'ide8'.
        path: Filesystem path.
        bytes_b64: Inline base64-encoded image (decoded to tmp file).
        ro: Read-only mount (informational, honored where supported;
            for 'qd' it sets write protection like the menu toggle,
            saved to the configuration).
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        JSON ``{"ok": true, "slot": str, "result_code": int}`` on
        success (slot 'qd' adds ``"switched_to_mz1f11": bool``) or
        ``{"error": "..."}`` on failure.
    """
    valid_slots = (
        "cmt",
        "fdc0_fd0", "fdc0_fd1", "fdc0_fd2", "fdc0_fd3",
        "fdc1_fd0", "fdc1_fd1", "fdc1_fd2", "fdc1_fd3",
        "qd", "ide8",
    )
    if slot not in valid_slots:
        return json.dumps(
            {"error": f"Invalid slot (allowed: {', '.join(valid_slots)})"})
    has_path = bool(path)
    has_b64 = bool(bytes_b64)
    if has_path == has_b64:
        return json.dumps(
            {"error": "Provide exactly one of: path, bytes_b64"})
    args = {"slot": slot, "ro": ro}
    if has_path:
        args["path"] = path
    else:
        args["bytes_b64"] = bytes_b64
    resp = await _send_request("media_insert", _with_rev(args, expected_state_rev))
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "media_insert failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_media_eject(slot: str,
                          expected_state_rev: int | None = None) -> str:
    """Eject a media image from the given slot. No-op if the slot is
    empty.

    Args:
        slot: One of 'cmt' | 'fdc0_fd0'..'fdc0_fd3' |
            'fdc1_fd0'..'fdc1_fd3' | 'qd' | 'ide8'.
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        JSON ``{"ok": true, "slot": str}`` on success or
        ``{"error": "..."}`` on failure.
    """
    valid_slots = (
        "cmt",
        "fdc0_fd0", "fdc0_fd1", "fdc0_fd2", "fdc0_fd3",
        "fdc1_fd0", "fdc1_fd1", "fdc1_fd2", "fdc1_fd3",
        "qd", "ide8",
    )
    if slot not in valid_slots:
        return json.dumps(
            {"error": f"Invalid slot (allowed: {', '.join(valid_slots)})"})
    resp = await _send_request("media_eject",
                               _with_rev({"slot": slot}, expected_state_rev))
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "media_eject failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_media_state() -> str:
    """Get the current state of all media slots (CMT, FDC0 drives
    fdc0_fd0..3, FDC1 drives fdc1_fd0..3, QD, IDE8). Slots that are not
    compiled into the current architecture build still appear in the
    response but with ``inserted=false`` and ``path=""``.

    Returns:
        JSON ``{"count": int, "slots": [{"slot": str, "inserted": bool,
        "path": str, "ro": bool}, ...]}`` on success or
        ``{"error": "..."}`` on failure.
    """
    resp = await _send_request("media_state")
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "media_state failed")})
    return json.dumps(_data_or_error(resp))


# === V1.B.2 - Platform + Config Tools (5 new tools) ==================
# Implementační poznámky:
#  * settings_set whitelistuje live-settable klíče v cfgmain INI.
#    Boot-time klíče (paths, snapshot defaults) vyžadují restart =
#    zde vráceno jako error.
#  * platform_set vrací error - mz700/mz800/mz1500 jsou separátní
#    binárky (= compile-time MZARCH). Klient se musí restartovat s
#    jinou exe.
#  * periph_attach / detach zapisují "active" BOOL do cfgmain a
#    vrací requires_restart=true (V1.B.2 = bez live re-init).


_VALID_SETTINGS_KEYS = (
    "AUDIO/volume_8253",
    "AUDIO/volume_psg0", "AUDIO/volume_psg1", "AUDIO/volume_psg2",
    "AUDIO/volume_psg3",
    "AUDIO/volume_psg1_0", "AUDIO/volume_psg1_1", "AUDIO/volume_psg1_2",
    "AUDIO/volume_psg1_3",
    "DISPLAY/forced_full_screen_redrawing",
    "DISPLAY/locked_window_aspect_ratio",
    "DISPLAY/custom_fps",
    "TRACE_CPUTRACK/pc_range_lo",
    "TRACE_CPUTRACK/pc_range_hi",
)

_VALID_PLATFORM_KINDS = ("mz700", "mz800", "mz1500")
_VALID_PLATFORM_MODES = ("native", "compat")
_VALID_PERIPH_KINDS = ("memext", "fdc", "qd", "ide8", "gal5")


@mcp.tool()
async def emu_settings_set(key: str, value: str) -> str:
    """Set a live-settable emulator configuration value. WARNING: this
    mutates global emulator state and changes audio/video behaviour
    immediately, like the same change in the menu; the value is saved
    to the INI file on exit. Boot-time keys (paths, toolchain) are
    rejected - those require restarting the emulator with a different
    .ini file.

    Live-settable keys (no restart required):

        AUDIO/volume_8253           - 8253 CTC tone volume (0..MAX)
        AUDIO/volume_psg0..psg3     - PSG SN76489 channel volumes
        AUDIO/volume_psg1_0..psg1_3 - MZ-1500 second PSG channels
        DISPLAY/forced_full_screen_redrawing - bool
        DISPLAY/locked_window_aspect_ratio   - bool
        DISPLAY/custom_fps                   - unsigned, 50..1000 (Custom
                                               frame rate mode)
        TRACE_CPUTRACK/pc_range_lo - cputrack PC-range filter low bound (0..65535)
        TRACE_CPUTRACK/pc_range_hi - cputrack PC-range filter high bound (0..65535)

    Value is type-coerced server-side (unsigned: integer literal;
    bool: 'true'/'false' or '1'/'0'; text: passed as-is; float:
    decimal literal).

    Args:
        key: INI key in 'MODULE/element' format (case sensitive).
        value: string-encoded value.

    Returns:
        JSON ``{"key": str, "previous_value": str, "new_value": str,
        "type": str}`` on success or ``{"error": "..."}`` on failure.
    """
    if not key:
        return json.dumps({"error": "Missing required field: key"})
    if value is None:
        return json.dumps({"error": "Missing required field: value"})
    if key not in _VALID_SETTINGS_KEYS:
        return json.dumps(
            {"error": "Key is not live-settable (allowed: "
                       + ", ".join(_VALID_SETTINGS_KEYS) + ")"})
    resp = await _send_request(
        "settings_set", {"key": key, "value": str(value)})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "settings_set failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_settings_get(key: str) -> str:
    """Get the value of a configuration key from cfgmain INI.

    Any key registered by the emulator is readable (no whitelist for
    read access). Returns the string representation plus the element
    type (unsigned / bool / text / keyword / float). Use this before
    ``emu_settings_set`` to discover current value and type.

    ``value`` is the configured value (loaded at startup or written with
    ``emu_settings_set``); changes from the menu or other tools reach it
    only when the INI file is saved. ``live`` is the value the emulator
    is working with right now, present only for keys bound directly to
    a numeric variable (device connections and types, display
    settings). Example: after switching the Quick Disk drive,
    'QDISK/mz1f11_type' keeps ``value`` 'UNICARD' while ``live`` is
    'STANDARD'.

    Args:
        key: INI key in 'MODULE/element' format (case sensitive).
              Example: 'AUDIO/volume_8253'.

    Returns:
        JSON ``{"key": str, "value": str, "type": str, "live": str}``
        (``live`` optional) on success or ``{"error": "..."}`` on
        failure.
    """
    if not key:
        return json.dumps({"error": "Missing required field: key"})
    resp = await _send_request("settings_get", {"key": key})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "settings_get failed")})
    return json.dumps(_data_or_error(resp))


# === BACKLOG D - emulation speed control ==============================
# Tool descriptions jsou anglicky - MCP wire protocol je locale-agnostic.


@mcp.tool()
async def emu_set_speed(
    mode: str, percent: int = 100, step: int = 0
) -> str:
    """Control the emulation speed (throttle / warp).

    Use case: let a long-running operation finish quickly without
    waiting in real time. Switch to ``max`` (warp / unthrottled) for a
    fast boot, tape/disk load or long computation, then switch back to
    ``normal`` (100%) once done. The current speed is also readable via
    the ``emulator://speed`` resource.

    This is a visible behaviour change and appears in the emulator's
    Activity log (MCP action).

    Args:
        mode: one of
            ``normal`` - run at 100% (real-time tempo, turns warp off),
            ``custom`` - run at an exact ``percent`` (turns warp off),
            ``max``    - warp / unthrottled run as fast as the host
                         allows (turns warp on),
            ``step``   - relative adjust of the custom percentage by
                         ``step`` (does not change the warp flag).
        percent: target percentage for ``mode=custom`` (1..4000,
            clamped by the core). Ignored for other modes.
        step: relative delta for ``mode=step`` (positive = faster,
            negative = slower, 0 = no-op). Ignored for other modes.

    Returns:
        JSON ``{"ok": bool, "mode": str, "current_percent": int,
        "max_speed": bool}`` echoing the resulting state.

    Note:
        ``emu_run(frames=N)`` waits for N frames; with ``max`` speed the
        frame counter advances faster, so a blocking ``emu_run`` finishes
        in less wall-clock time while remaining correct.
    """
    if not mode:
        return json.dumps({"error": "Missing required field: mode"})
    data: dict[str, Any] = {"mode": mode, "percent": percent, "step": step}
    resp = await _send_request("set_speed", data)
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "set_speed failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_speed_step(delta: int) -> str:
    """Adjust the custom emulation speed by a relative percentage.

    Convenience wrapper around ``emu_set_speed(mode="step", step=delta)``.
    Positive ``delta`` speeds up, negative slows down. Does not change the
    warp (max speed) flag. The result is visible in the Activity log.

    Args:
        delta: relative percentage delta (positive = faster, negative =
            slower).

    Returns:
        JSON ``{"ok": bool, "mode": str, "current_percent": int,
        "max_speed": bool}`` echoing the resulting state.
    """
    data: dict[str, Any] = {"mode": "step", "step": delta}
    resp = await _send_request("set_speed", data)
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "set_speed failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_platform_set(
    kind: str, mode: str = "", save_snapshot: str = ""
) -> str:
    """Switch to a different MZ platform. WARNING: runtime platform
    switch is NOT supported in V1.B.2 - mz700/mz800/mz1500 are
    separate binaries selected at compile time via MZARCH. This tool
    returns an error indicating which platform binary is currently
    active. To use a different platform, the user must restart the
    emulator with the corresponding executable.

    The ``mode`` and ``save_snapshot`` parameters are accepted for
    forward compatibility (V2 plan: persist snapshot before restart
    + spawn replacement binary), but they are currently ignored.

    Args:
        kind: Target platform - 'mz700' | 'mz800' | 'mz1500'.
        mode: Optional mode - 'native' | 'compat' (mz800 only).
              V1.B.2 ignores this.
        save_snapshot: Optional .mzs path. V1.B.2 ignores this.

    Returns:
        JSON ``{"ok": bool, "active_kind": str, "target_kind": str,
        "error": str}`` - ok=false when restart with different
        binary is required. ok=true (with ``no_op=true``) only when
        target equals the already active platform.
    """
    if kind not in _VALID_PLATFORM_KINDS:
        return json.dumps(
            {"error": "Invalid kind (allowed: "
                       + ", ".join(_VALID_PLATFORM_KINDS) + ")"})
    if mode and mode not in _VALID_PLATFORM_MODES:
        return json.dumps(
            {"error": "Invalid mode (allowed: "
                       + ", ".join(_VALID_PLATFORM_MODES) + ")"})
    args = {"kind": kind}
    if mode:
        args["mode"] = mode
    if save_snapshot:
        args["save_snapshot"] = save_snapshot
    resp = await _send_request("platform_set", args)
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "platform_set failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_periph_attach(
    kind: str, options: dict | None = None
) -> str:
    """Attach a peripheral to the running emulator. WARNING: this
    changes the emulated machine immediately, the same way as the
    Devices menu (the choice is saved to the INI file on exit). Refused
    while a replay is being recorded or played back.

    Supported kinds and ``options.type`` values (first = default):

        memext - Memory expansion: 'pehu' | 'luftner' (default: the
                 current type). Attaching resets the bank map.
        fdc    - WD279x floppy controller: 'fdc0' (ports 0D8h-0DFh) |
                 'fdc1' (secondary, ports 58h-5Fh).
        qd     - Quick Disk drive: 'mz1f11' (.MZQ image) | 'virtual' |
                 'unicard' (Unicard boot loader, needs Unicard).
        ide8   - 8-bit IDE disk: 'master' | 'slave'. Opens the configured
                 disk image (IDE8/hdd0_filepath, hdd1_filepath) and
                 creates it when missing.
        gal5   - Not emulated; always an error (kept for compatibility).

    A state that already holds is not applied again (``changed: false``).

    Args:
        kind: Peripheral kind.
        options: Optional ``{"type": str}`` with the variant or unit.

    Returns:
        JSON ``{"ok": true, "kind": str, "type": str, "changed": bool,
        "requires_restart": false, "result_code": 0}`` on success or
        ``{"error": "..."}`` on failure.
    """
    if kind not in _VALID_PERIPH_KINDS:
        return json.dumps(
            {"error": "Invalid kind (allowed: "
                       + ", ".join(_VALID_PERIPH_KINDS) + ")"})
    args: dict = {"kind": kind}
    if options:
        args["options"] = options
    resp = await _send_request("periph_attach", args)
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "periph_attach failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_periph_detach(
    kind: str, options: dict | None = None
) -> str:
    """Detach a peripheral from the running emulator. WARNING: same as
    ``emu_periph_attach`` - the change applies immediately like the
    Devices menu and is saved to the INI file on exit.

    Args:
        kind: Peripheral kind (same set as ``emu_periph_attach``).
        options: Optional ``{"type": str}`` selecting the unit for
                 ``fdc`` ('fdc0' | 'fdc1') and ``ide8`` ('master' |
                 'slave'); ignored for ``memext`` and ``qd``.

    Returns:
        JSON ``{"ok": true, "kind": str, "type": str, "changed": bool,
        "requires_restart": false, "result_code": 0}`` on success or
        ``{"error": "..."}`` on failure.
    """
    if kind not in _VALID_PERIPH_KINDS:
        return json.dumps(
            {"error": "Invalid kind (allowed: "
                       + ", ".join(_VALID_PERIPH_KINDS) + ")"})
    args: dict = {"kind": kind}
    if options:
        args["options"] = options
    resp = await _send_request("periph_detach", args)
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "periph_detach failed")})
    return json.dumps(_data_or_error(resp))


# === V1.C.1 - HID input Tools ========================================
# Simulace klávesnice + joysticku přes PIO8255 VKBD matrix (= virtual
# keyboard matrix paralelní s fyzickou SDL scan). Klíčové pro user
# simulation use case z rozboru sekce 2.3.
#
# WARNING token v každé description je nutný (= AI klient může spustit
# nezamýšlené BASIC příkazy nebo přepsat BASIC program přes RUN+RETURN).
#
# Frame timing: frames / frame_per_key / hold_frames / gap_frames jsou
# snímky EMULACE (50 Hz PAL), nezávisle na rychlosti emulace (normal, MAX
# SPEED, zpomalení s CDL). Stisky a uvolnění provádí emu vlákno na
# hranicích snímků (backend hid_script.c). Default frames=3 (60 ms emulace)
# odpovídá realistické délce stisku. Pro precision timing použij
# emu_input_send_keys_with_delays. Timeout requestu roste s délkou
# sekvence (_hid_timeout_s).


@mcp.tool()
async def emu_input_send_key(key: str, frames: int = 3) -> str:
    """Send a single keypress (press + hold N frames + release).
    WARNING: User input simulation can cause unexpected emulator
    behavior (= triggers BASIC commands, modifies running program
    state). Use with explicit user consent.

    Args:
        key: Key identifier. ONE name table is shared by
            emu_input_send_key, emu_input_send_keys (key_names),
            emu_input_press_key, emu_input_release_key and
            emu_input_send_keys_with_delays. Supported names
            (case-insensitive): RETURN/ENTER/CR, SPACE, BACKSPACE/DELETE/DEL,
            INSERT/INS/INST, ESC/ESCAPE/BREAK, TAB, SHIFT, CONTROL/CTRL,
            GRAPH, ALPHA, BLANK, LIBRA, F1..F9, cursor keys
            ARROW_UP/DOWN/LEFT/RIGHT = UP/DOWN/LEFT/RIGHT =
            CURSOR_UP/DOWN/LEFT/RIGHT, and the arrow-glyph character keys
            UP_ARROW / DOWN_ARROW (not cursor movement). See
            emulator://docs/mz800_keyboard.
            ASCII fallback: single-char string ("A", "1", "?") or
            explicit "ASCII:<char>" prefix. An unknown name returns
            "Unknown key '<name>'. Closest valid names: ...".
        frames: Number of EMULATED frames to hold the key (default 3 =
            60 ms of emulated time at 50 Hz PAL). Maximum 600 frames.

    Timing: ``frames`` counts emulated frames, independent of the
    emulation speed (normal, ``emu_set_speed(mode="max")``, or slower
    than real time e.g. with CDL). The emulator itself presses and
    releases the key on frame boundaries. If the emulator is paused, it
    runs exactly ``frames`` frames and pauses again; if it is running, it
    keeps running. The call returns after the key was released.

    Returns:
        JSON ``{"key": str, "col": int, "bit": int, "shift": bool,
        "frames": int, "sent": true, "landing_verified": bool,
        "emu_frames": int, "complete": bool, "interrupted": bool}`` or
        ``{"error": "..."}``. ``emu_frames`` = emulated frames that
        actually elapsed between press and release. ``interrupted:
        true`` = the hold was cut short (breakpoint hit, ``emu_pause``,
        GUI pause, reset or a stuck emulation); the key was released.

        NOTE: ``sent: true`` only means the host-side key injection was
        performed; it is NOT a guarantee the guest read the key.
        ``landing_verified`` is a real readback: while the key is held
        the emulator watches the PPI Port B reads, and the flag is
        ``true`` only if the guest scanned the key's column during the
        hold (i.e. it actually read the key), ``false`` otherwise. The
        guest may miss the key if it currently scans only a narrow
        subset of keyboard columns (e.g. a running program with the ROM
        unmapped) or if ``frames`` is 0. If a key does not land, hold it
        longer (``frames``), make sure the guest is at a prompt that
        scans the keyboard, or reset the emulator.
    """
    if not key:
        return json.dumps({"error": "key must be non-empty"})
    if frames < 0 or frames > 600:
        return json.dumps({"error": "frames must be 0..600"})
    resp = await _send_request(
        "input_send_key", {"key": key, "frames": frames},
        timeout_s=_hid_timeout_s(frames))
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "input_send_key failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_input_send_keys(
    text: str, encoding: str = "ascii", frame_per_key: int = 3
) -> str:
    """Send a sequence of keys (= simulated typing).
    WARNING: User input simulation can cause unexpected emulator
    behavior. For typing into BASIC interpreter use carefully.

    Args:
        text: Text to type. Format depends on encoding.
        encoding: ``"ascii"`` (default, plain text e.g. ``"RUN\\r"``)
            or ``"key_names"`` (JSON array string of key names
            e.g. ``'["SHIFT","CURSOR_RIGHT","RETURN"]'``; same names
            as emu_input_send_key / emu_input_press_key, an unknown name
            fails the whole call before any key is sent).
        frame_per_key: EMULATED frames to hold each key (default 3).
            Consecutive keys are separated by 1 frame with no key
            pressed, so that repeated keys (e.g. "LL") register as two
            presses.

    Timing: frames are emulated frames, independent of the emulation
    speed; the emulator presses and releases the keys on frame
    boundaries. A paused emulator runs exactly the length of the
    sequence and pauses again; a running one keeps running.

    Returns:
        JSON ``{"keys_sent": int, "keys_landed": int,
        "total_frames": int, "encoding": str,
        "landing_verified": bool, "emu_frames": int,
        "complete": bool, "interrupted": bool}`` or
        ``{"error": "..."}``. ``emu_frames`` = emulated frames that
        actually elapsed; ``interrupted: true`` = the sequence was cut
        short (breakpoint, pause, reset); ``keys_sent`` then counts only
        the keys that were fully pressed and released.

        NOTE: ``keys_sent`` counts host-side key injections into the
        virtual matrix, NOT keys the guest actually read.
        ``keys_landed`` is a real readback counting how many of them the
        guest actually read: while each key is held the emulator watches
        the PPI Port B reads, and a key lands only if the guest scanned
        its column during the hold. ``landing_verified`` is ``true``
        only when every sent key landed (``keys_landed == keys_sent``
        and ``keys_sent > 0``), ``false`` otherwise. The guest may miss
        keys if it currently scans only a narrow subset of keyboard
        columns (e.g. a running program with the ROM unmapped). If keys
        do not land, hold them longer (``frame_per_key``), make sure the
        guest is at a prompt that scans the keyboard, or reset the
        emulator.
    """
    if encoding not in ("ascii", "key_names"):
        return json.dumps({"error": f"Invalid encoding: {encoding}"})
    if frame_per_key < 0 or frame_per_key > 600:
        return json.dumps({"error": "frame_per_key must be 0..600"})
    # Horní odhad počtu kláves: backend bere nejvýš 256 kláves a žádná
    # klávesa nemá méně než jeden znak textu (platí i pro key_names).
    max_keys = min(len(text), 256)
    resp = await _send_request(
        "input_send_keys",
        {"text": text, "encoding": encoding,
         "frame_per_key": frame_per_key},
        timeout_s=_hid_timeout_s(max_keys * (frame_per_key + 1)))
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "input_send_keys failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_input_press_key(key: str) -> str:
    """Press and HOLD a key (no auto-release).
    WARNING: Key remains held until emu_input_release_key is called.
    Forgetting to release can permanently block keyboard input in
    the emulated program. Use with care.

    Useful for held modifiers (SHIFT, CONTROL) combined with other
    keypresses, or for sustained joystick-like input via cursor keys.

    Args:
        key: Key identifier (see emu_input_send_key for supported
            names and ASCII fallback).

    Returns:
        JSON ``{"key": str, "col": int, "bit": int, "shift": bool,
        "pressed": true}`` or ``{"error": "..."}``.
    """
    if not key:
        return json.dumps({"error": "key must be non-empty"})
    resp = await _send_request("input_press_key", {"key": key})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "input_press_key failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_input_release_key(key: str = "") -> str:
    """Release a held key, or release ALL keys if no name given.
    WARNING: User input simulation. If ``key`` is empty, releases
    EVERY pressed key in the virtual keyboard matrix.

    Args:
        key: Key identifier (empty string = release all keys).

    Returns:
        JSON ``{"key": str|null, "released_all": bool}`` or
        ``{"error": "..."}``.
    """
    resp = await _send_request("input_release_key", {"key": key})
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "input_release_key failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_input_send_joystick(
    port: int, state: int, frames: int = 3
) -> str:
    """Send a joystick state (8-bit bitmask) + hold + release.
    WARNING: User input simulation. Bit assignments are Sharp MZ
    standard (active-HIGH on this MCP interface, native hardware
    is active-LOW; the bridge converts internally):

    - Bit 0: UP    Bit 1: DOWN   Bit 2: LEFT   Bit 3: RIGHT
    - Bit 4: FIRE1 Bit 5: FIRE2  Bit 6,7: unused

    Args:
        port: Joystick port (0 or 1).
        state: 8-bit bitmask (0 = no input, 0x01 = UP only,
            0x11 = UP + FIRE1).
        frames: Hold duration in EMULATED frames (default 3, max 600),
            independent of the emulation speed (see emu_input_send_key).

    Returns:
        JSON ``{"port": int, "state": int, "frames": int,
        "sent": true, "emu_frames": int, "complete": bool,
        "interrupted": bool}`` or ``{"error": "..."}``. An error is
        also returned by a platform without joystick (MZ-700).
    """
    if port not in (0, 1):
        return json.dumps({"error": "port must be 0 or 1"})
    if state < 0 or state > 255:
        return json.dumps({"error": "state must be 0..255"})
    if frames < 0 or frames > 600:
        return json.dumps({"error": "frames must be 0..600"})
    resp = await _send_request(
        "input_send_joystick",
        {"port": port, "state": state, "frames": frames},
        timeout_s=_hid_timeout_s(frames))
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "input_send_joystick failed")})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_input_send_keys_with_delays(events: list) -> str:
    """Send a timing-controlled key sequence (precision replay).
    WARNING: User input simulation. Use for game speedrun replay,
    BASIC stress tests, or any scenario requiring exact frame timing
    between keypresses.

    Each event in the list is a dict:
    - ``key`` (str): key identifier (see emu_input_send_key)
    - ``hold_frames`` (int): how long to hold (default 3, max 600)
    - ``gap_frames`` (int): frames between this release and next
      press (default 0, max 600); the gap after the last event is
      waited too

    Timing: all frames are EMULATED frames, exact and independent of the
    emulation speed (normal, MAX SPEED, slowed down by CDL). The emulator
    itself presses and releases the keys on frame boundaries. A paused
    emulator runs exactly the length of the sequence and pauses again; a
    running one keeps running. The call returns when the sequence ends
    (its timeout grows with the sequence length).

    Args:
        events: List of event dicts (max 256 events).

    Returns:
        JSON ``{"events_processed": int, "total_frames": int,
        "emu_frames": int, "complete": bool, "interrupted": bool}``
        or ``{"error": "..."}``. ``total_frames`` = sum of hold+gap of
        the processed events, ``emu_frames`` = emulated frames that
        actually elapsed (equal when complete). ``interrupted: true`` =
        cut short by a breakpoint, pause or reset; the held key was
        released and ``events_processed`` counts finished events only.
    """
    if not isinstance(events, list) or not events:
        return json.dumps(
            {"error": "events must be a non-empty list"})
    if len(events) > 256:
        return json.dumps({"error": "max 256 events per call"})
    total = 0
    for ev in events:
        if isinstance(ev, dict):
            for name, default in (("hold_frames", 3), ("gap_frames", 0)):
                v = ev.get(name, default)
                if isinstance(v, int):
                    total += min(max(v, 0), 600)
    resp = await _send_request(
        "input_send_keys_with_delays", {"events": events},
        timeout_s=_hid_timeout_s(total))
    if not resp.get("success", False):
        return json.dumps(
            {"error": resp.get("error", "input_send_keys_with_delays failed")})
    return json.dumps(_data_or_error(resp))


# === V1.B.3 - hot-swap workflow Tools (pipe transport only) ==========
# AI dev cyklus stop -> rebuild mz800emu.exe -> start bez restartu
# Claude Code session. Nevolá emu_stop v TCP módu - vrátí error
# (= TCP attach k existující GUI session, kterou by hot-swap zabil).
#
# State preservation NENI automatický - AI klient musí explicit
# snapshot_save_buffer před emu_stop a snapshot_load_buffer po emu_start
# (= deferred do AI workflow, viz devdoc/mcp-server/hot-swap.md).


@mcp.tool()
async def emu_stop() -> str:
    """Gracefully terminate the emulator child process (pipe transport only).

    The mcp_server.py wrapper itself keeps running and remains attached
    to the Claude Code session. After stop the user can replace
    ``mz800emu.exe`` on disk (e.g. ``make mz800emu``), then invoke
    ``emu_start`` to spawn a fresh child without restarting Claude Code.

    WARNING: Emulator state (RAM, registers, breakpoints, watches) is
    discarded on stop. Call ``emu_snapshot_save_buffer`` first if you
    need to restore state via ``emu_snapshot_load_buffer`` after
    ``emu_start``. Hot-swap is rejected in TCP transport mode (the
    server would kill the user's live GUI session).

    Returns:
        JSON ``{"stopped": true, "transport": "pipe"}`` on success,
        or ``{"error": "..."}`` (TCP transport, transport not connected,
        ...).
    """
    if TRANSPORT_KIND != "pipe":
        return json.dumps(
            {"error": "hot-swap requires pipe transport "
                       "(current: " + TRANSPORT_KIND + ")"})

    global _transport, _reader_task

    if _transport is None or not _transport.is_alive():
        return json.dumps(
            {"error": "transport not connected (call any tool first "
                       "or invoke emu_start)"})

    # Konec backendu vyžádal wrapper - reader ho nesmí hlásit jako pád.
    _transport.expected_exit = True

    # Pošli emu_stop request. Backend response = success + trigger
    # shutdown callback. Po triggeru emu thread končí, child proces
    # se brzy ukončí.
    resp_data: dict[str, Any] = {}
    try:
        resp = await asyncio.wait_for(_send_request("emu_stop"),
                                       timeout=5.0)
        resp_data = resp.get("data", {}) if resp.get("success") else \
            {"error": resp.get("error", "emu_stop failed")}
    except asyncio.TimeoutError:
        # I tak posílíme s cleanup - child už pravděpodobně skončil.
        log.warning("emu_stop response timeout (child likely exited)")
        resp_data = {"stopped": True, "transport": "pipe",
                     "warning": "response timeout"}
    except Exception as e:
        log.warning("emu_stop request failed: %s", e)
        resp_data = {"error": f"emu_stop request failed: {e}"}

    # Lokální cleanup transportu - reader task se zastaví na EOF,
    # ale my proaktivně disconnect + reset state pro emu_start.
    try:
        await _transport.disconnect()
    except Exception as e:
        log.warning("transport disconnect after emu_stop failed: %s", e)
    _transport = None

    if _reader_task is not None:
        _reader_task.cancel()
        try:
            await _reader_task
        except (asyncio.CancelledError, Exception):
            pass
        _reader_task = None

    return json.dumps(resp_data)


@mcp.tool()
async def emu_start(binary_path: str = "") -> str:
    """Spawn a fresh emulator child process (pipe transport only).

    Counterpart to ``emu_stop`` for the hot-swap workflow. Re-spawns
    ``mz800emu.exe --mcp-pipe`` as a child subprocess, reads the hello
    message and the wrapper is ready to dispatch further tools.

    This is the only MCP Tool that lives purely in the Python wrapper
    (no C-side dispatch handler) - the emu binary cannot spawn itself,
    only the wrapper can manage subprocess lifecycle.

    Args:
        binary_path: Optional override path to mz800emu.exe. If empty
            (default), uses the path configured via the ``MZ800EMU_EXE``
            env var or the default location next to mcp_server.py.

    Returns:
        JSON ``{"started": true, "exe": "<path>", "commands": <int>}``
        on success, or ``{"error": "..."}`` (TCP transport, already
        running, binary not found).
    """
    if TRANSPORT_KIND != "pipe":
        return json.dumps(
            {"error": "hot-swap requires pipe transport "
                       "(current: " + TRANSPORT_KIND + ")"})

    global _transport, EMU_EXE, _last_exit, _restart_notice

    if _transport is not None and _transport.is_alive():
        return json.dumps(
            {"error": "emulator already running (call emu_stop first)"})

    # Explicitní start: případný předchozí pád se už klientovi neoznamuje
    # (nový proces si vyžádal sám).
    _last_exit = None
    _restart_notice = None

    # Override binární cesty (= explicit argument vždy vítězí nad env).
    if binary_path:
        EMU_EXE = binary_path
        log.info("emu_start: EMU_EXE override -> %s", EMU_EXE)

    if not Path(EMU_EXE).is_file():
        return json.dumps(
            {"error": f"mz800emu binary not found at {EMU_EXE} "
                       "(set MZ800EMU_EXE env or pass binary_path)"})

    # _ensure_connected provede spawn + reader task + hello payload.
    try:
        await _ensure_connected()
    except Exception as e:
        log.exception("emu_start: _ensure_connected failed")
        return json.dumps({"error": f"spawn failed: {e}"})

    return json.dumps({
        "started": True,
        "exe": EMU_EXE,
        "commands": len(_emu_hello.get("commands", [])),
    })


# === V1.E.2 - CPU control + details Tools ============================
# Vystavuje 9 backend dbgapi handlerů jako MCP Tools (5 jednoduchých +
# 2 CPU flags + 2 details). Wire descriptions anglicky, doplňují
# emu_get_registers / emu_set_register o per-register read, IM/IFF
# selektivní get/set, raster/cycle introspection a atomic batch snapshot
# pro UI/AI klienty potřebující synchronizovaný stav CPU panelu.


@mcp.tool()
async def emu_get_reg(reg: str) -> str:
    """Read the value of a single Z80 register.

    Cheaper than ``emu_get_registers`` when the caller only cares about
    one register (= 1 backend round-trip with a 16-bit result vs. all 14
    registers). Accepts the same case-insensitive register names returned
    by ``emu_get_registers``: ``AF``, ``BC``, ``DE``, ``HL``, ``AF_`` /
    ``AF2``, ``BC_`` / ``BC2``, ``DE_`` / ``DE2``, ``HL_`` / ``HL2``,
    ``IX``, ``IY``, ``SP``, ``PC``, ``WZ``, ``IR``. For the ``IR``
    register the backend returns the composite ``(I << 8) | R``.

    Args:
        reg: register name (case-insensitive).

    Returns:
        JSON ``{"reg": "PC", "value": <int>}`` on success, or
        ``{"error": "..."}`` for unknown names / dispatch failures.
    """
    resp = await _send_request("get_reg", {"reg": reg})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_force_pause() -> str:
    """Force-pause the emulator (bypass standard pause path).

    Same semantics as ``emu_pause`` in the current implementation - the
    distinction is reserved for future contexts that could block a
    regular pause (e.g. running BP handler). Use this when you want a
    stable snapshot from any thread without worrying about pause path
    interference.

    Returns:
        JSON ``{"paused": true}`` on success.
    """
    resp = await _send_request("force_pause")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_set_user_cycle_origin(value: int = -1) -> str:
    """Reset the User cycle counter origin to a specific T-state snapshot.

    The User cycle counter is displayed in the CPU panel as
    ``total_cycles - user_cycle_origin``. Calling this tool re-baselines
    that counter at the supplied absolute T-state value, or at the
    current ``total_cycles`` if ``value`` is negative / omitted (=
    "Reset to 0" behaviour).

    Args:
        value: Absolute T-state snapshot to install as the new origin
            (0..0xFFFFFFFF). Pass -1 (default) to snapshot the current
            ``cpu->total_cycles``.

    Returns:
        JSON ``{"reset": true, "origin": <int>}`` on success.
    """
    payload: dict[str, int] = {}
    if value >= 0:
        if value > 0xFFFFFFFF:
            return json.dumps(
                {"error": "value must be in range 0..0xFFFFFFFF"})
        payload["value"] = value
    resp = await _send_request("set_user_cycle_origin", payload or None)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_get_im2_vector() -> str:
    """Return the Z80 IM 2 ISR vector snapshot (PIO-Z80 platforms only).

    Tool variant alongside the read-only Resource ``cpu/im2_vector``.
    Useful in a workflow where the AI client wants to assert the current
    IM 2 dispatch target without bringing in a separate Resource fetch.

    Returns:
        JSON ``{"available": bool, "im": int, "i": int, "vec": int,
        "isr_addr": int, "isr_target": int, "pio_irq_pending": bool,
        "pio_source": int}``. ``available`` is ``false`` on MZ-700 (no
        PIO-Z80 silicon); other fields are then meaningless.
        ``pio_source`` is ``0`` for PIO-A, ``1`` for PIO-B.
    """
    resp = await _send_request("get_im2_vector")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_get_raster_pos() -> str:
    """Return the current GDG raster position and Z80 cycle counters.

    Cheap read-only sample of where the GDG beam is and how many
    T-states have elapsed. Used by the CPU panel's "Cycles & raster"
    section; exposed to MCP for AI agents that synchronise actions to
    the raster (e.g. mid-frame palette swaps, BCOL effects).

    Returns:
        JSON ``{"frame_number": int, "scanline": int,
        "column_pixel": int, "total_cycles": int,
        "frame_cycles": int}``. ``scanline`` is in ``0..VIDEO_SCREEN_HEIGHT-1``
        (= 312 for MZ-800 PAL).
    """
    resp = await _send_request("get_raster_pos")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_get_cpu_flags() -> str:
    """Return the full auxiliary CPU state snapshot (IFF/IM/HALT/...).

    Returns the contents of ``st_DBGAPI_CPU_FLAGS`` in one round-trip:
    master and shadow interrupt enable, current Interrupt Mode, HALT
    state, INT/NMI pending lines, EI delay flag, internal Q register,
    cumulative and per-frame T-state counters, instruction T-state
    offset and the I / R registers.

    Returns:
        JSON ``{"iff1": bool, "iff2": bool, "im": int, "halted": bool,
        "int_pending": bool, "nmi_pending": bool, "ei_delay": bool,
        "q": int, "total_cycles": int, "frame_cycles": int,
        "op_tstate": int, "i": int, "r": int}``.
    """
    resp = await _send_request("get_cpu_flags")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_set_cpu_flags(iff1: int = -1, iff2: int = -1,
                            im: int = -1, i: int = -1,
                            r: int = -1,
                            expected: Optional[dict[str, int]] = None,
                            expected_state_rev: int | None = None) -> str:
    """Selectively update Z80 interrupt-related state (IFF1/IFF2/IM/I/R).

    WARNING: This is a destructive operation. Each argument is optional
    (pass ``-1`` / default to skip); only the fields that are explicitly
    provided are written. The emulator builds the ``update_mask`` from
    the present fields and applies them in a safepoint between
    instructions.

    Other ``st_DBGAPI_CPU_FLAGS`` members (HALT state, INT/NMI pending,
    Q register, cycle counters) are read-only via ``emu_get_cpu_flags``
    and cannot be mutated here - they are emulator-internal state.

    Args:
        iff1: master interrupt enable (0 or 1).
        iff2: shadow interrupt enable (0 or 1).
        im: Interrupt Mode (0, 1 or 2). Other values are rejected.
        i: Interrupt Vector register (0..255).
        r: Memory Refresh register (0..255). All 8 bits are written,
            including bit 7, the same as the LD R,A instruction (the
            refresh counter itself only increments bits 0-6).
        expected: conditional write - expected current values of the
            fields being written, e.g. ``{"iff1": 1}`` together with
            ``iff1=0`` (only fields this call writes; ``r`` compares the
            whole byte). On a mismatch nothing is written and the error
            starts with ``Precondition failed: cpu flags changed``.
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        JSON ``{"updated": ["iff1", "im", ...], "values": {...},
        "previous": {...}}`` where ``updated`` lists the field names that
        were written, ``values`` echoes their final values and
        ``previous`` their values before the write. ``{"error": "..."}``
        is returned when no field is provided or the IM value is out
        of range.
    """
    payload: dict[str, Any] = {}
    if iff1 >= 0:
        if iff1 not in (0, 1):
            return json.dumps({"error": "iff1 must be 0 or 1"})
        payload["iff1"] = iff1
    if iff2 >= 0:
        if iff2 not in (0, 1):
            return json.dumps({"error": "iff2 must be 0 or 1"})
        payload["iff2"] = iff2
    if im >= 0:
        if im not in (0, 1, 2):
            return json.dumps({"error": "im must be 0, 1 or 2"})
        payload["im"] = im
    if i >= 0:
        if not (0 <= i <= 0xFF):
            return json.dumps({"error": "i must be in range 0..255"})
        payload["i"] = i
    if r >= 0:
        if not (0 <= r <= 0xFF):
            return json.dumps({"error": "r must be in range 0..255"})
        payload["r"] = r
    if not payload:
        return json.dumps(
            {"error": "no field specified - provide at least one of "
                       "iff1/iff2/im/i/r"})
    if expected:
        payload["expected"] = dict(expected)
    resp = await _send_request("set_cpu_flags", _with_rev(payload, expected_state_rev))
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_get_last_instr() -> str:
    """Return the most recently completed Z80 instruction from the
    debugger history ring.

    No arguments. Looks up the newest slot in ``g_debugger_history``
    and returns the instruction's address, raw bytes and decoded
    length. The mnemonic is not included to keep the payload small;
    pass the returned ``addr`` to ``emu_dasm(addr, 1)`` if a textual
    mnemonic is needed.

    The history ring is recorded only while the debugger is active
    (``TEST_DEBUGGER_CPUHIST_ACTIVE``). If the ring is empty or the
    debugger has not yet executed an instruction, ``valid`` is ``false``
    and the other fields are zero / empty.

    Returns:
        JSON ``{"valid": bool, "addr": int, "bytes_hex": string,
        "length": int}``. ``bytes_hex`` is a space-separated hex string
        of 1..4 bytes; ``length`` is the instruction length in bytes.
    """
    resp = await _send_request("get_last_instr")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_get_cpu_panel_batch(want_im2: bool = False,
                                  want_raster: bool = False,
                                  want_last_instr: bool = False) -> str:
    """Atomically read registers + flags + raster + IM2 + last instruction.

    A single round-trip that synchronises the entire CPU panel: instead
    of 3-5 separate calls (``emu_get_registers`` + ``emu_get_cpu_flags``
    + ``emu_get_raster_pos`` + ``emu_get_im2_vector`` +
    ``emu_get_last_instr``) the emulator returns all of them sampled at
    the same safepoint. Removes visible jitter when the panel is
    refreshed while the emulator is running, and reduces UI thread
    blocking time.

    Args:
        want_im2: include ``im2`` sub-object (Z80 IM 2 ISR vector
            snapshot, PIO-Z80 platforms only). Defaults to ``False``.
        want_raster: include ``raster`` sub-object (GDG raster
            position + cycle counters). Defaults to ``False``.
        want_last_instr: include ``last_instr`` sub-object (newest
            history ring entry). Defaults to ``False``.

    Returns:
        JSON object with ``regs`` (14 Z80 registers), ``flags`` (same
        schema as ``emu_get_cpu_flags``), ``frame_number``,
        ``user_cycle_origin``, ``has_pioz80`` plus ``veca/vecb/isra/
        isrb`` and ``pio_int_vec_a/b`` (always populated as a schema;
        zero on MZ-700). Optional sub-objects ``im2``, ``raster`` and
        ``last_instr`` are present only when the corresponding
        ``want_*`` flag was set.
    """
    payload: dict[str, bool] = {}
    if want_im2:
        payload["want_im2"] = True
    if want_raster:
        payload["want_raster"] = True
    if want_last_instr:
        payload["want_last_instr"] = True
    resp = await _send_request("get_cpu_panel_batch", payload or None)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_state_observe(windows: list[dict[str, Any]],
                            include: list[str] | None = None,
                            hashes: list[str] | None = None,
                            want_data: bool = True,
                            if_state_rev_differs: int | None = None) -> str:
    """Read registers, flags and several memory windows from ONE moment.

    Use this instead of ``emu_get_cpu_panel_batch`` + ``emu_mem_read``
    whenever registers and memory must belong together (a pointer and the
    data it points to, a counter and its register copy). Two separate
    calls always come from two different moments while the emulator runs;
    this tool reads everything in one pass of the emulator thread. While
    running the moment is the start of a video frame (the call waits up
    to one frame, about 20 ms); in pause it is the paused state.

    Each window is either a CPU address range read through the current
    memory map, ``{"addr": 0x3000, "len": 2}``, or a physical region from
    ``emu_regions_list`` addressed by kind and sub_id (not by region id,
    so it stays valid after a hardware change),
    ``{"region": {"kind": "vram_phys_plane", "sub_id": 0}, "offset": 0,
    "length": 256}``. Optional per window: ``name`` (echoed back, max 64
    characters) and ``want_data``. At most 16 windows and 65536 bytes in
    total. Reads have no side effects.

    Each returned window has ``sha256`` (same as ``emu_mem_read`` /
    ``emu_region_read`` of the same range - pass it as ``expected_sha256``
    to a conditional write) and ``data_b64`` unless ``want_data`` is
    false. A region that is not connected gives the window
    ``error: "region_unavailable"`` and ``complete: false``; the call
    still succeeds. Prefer ``want_data=False`` above a few KiB to keep the
    response small. See ``emulator://docs/state_observe``.

    Args:
        windows: list of 0..16 windows (see above).
        include: optional sections ``raster``, ``im2``, ``last_instr``
            (schema of ``emu_get_cpu_panel_batch``) and ``memory_map``
            (16 slots of 4 KiB with the page ``kind`` of the current
            banking).
        hashes: optional SHA-256 of whole state parts ``cpu``, ``ram``,
            ``vram``, ``memext``, ``pcg`` (same definition as replay and
            ``result.json``; a part that does not exist is ``null``).
        want_data: default for windows without their own ``want_data``.
        if_state_rev_differs: ``state_rev`` of your previous read; in
            pause, when the state revision still equals it, nothing is
            read and the reply is only ``{"not_modified": true,
            "state_rev", "run_seq", "running": false, "stable": true}``.
            While running the full answer always comes.

    Returns:
        JSON object with ``run_seq``, ``state_rev`` (revision of the
        machine state at the read; while paused equal revisions mean the
        state did not change, see ``emulator://docs/state_revision``),
        ``running``, ``stable`` (true only
        in pause), ``drain`` (``frame`` or ``paused``), ``time``
        (``frame``, ``frame_ticks``, ``ticks`` = absolute GDG time that
        never wraps, ``total_cycles``), ``regs``,
        ``flags``, ``windows``, ``complete`` and the requested optional
        sections.
    """
    payload: dict[str, Any] = {"windows": windows, "want_data": bool(want_data)}
    if include:
        payload["include"] = include
    if hashes:
        payload["hashes"] = hashes
    if if_state_rev_differs is not None:
        payload["if_state_rev_differs"] = if_state_rev_differs
    resp = await _send_request("state_observe", payload)
    return json.dumps(_data_or_error(resp))


# === V1.E.3 - Debugger control + PIO-Z80 IM2 vector ==================
# Programaticka kontrola debugger state (dosud jen z UI) a override IM2
# vektoru pro Z80 PIO porty A / B (= MZ-800 / MZ-1500; MZ-700 nema PIO-Z80
# a Tool vraci available=false).


@mcp.tool()
async def emu_debugger_activate() -> str:
    """Activate the internal debugger programmatically.

    Sets ``g_debugger.active = 1`` in the backend and recomputes the
    CPU callbacks, as opening the debug window does (the GUI window
    itself is not opened). CPU instruction history (``cpuhist``) and
    the memory heatmap (``mhmap``) then record if their mode is tied
    to the debugger window (WITH_WINDOW, or WITH_WINDOW_OR_BP for
    ``cpuhist``), so ``emu_get_last_instr`` and ``emu_history_get``
    start returning real data.

    Idempotent: calling on an already active debugger is a no-op
    beyond the response.

    Returns:
        JSON ``{"active": true}`` on success, ``{"error": "..."}``
        on backend failure.
    """
    resp = await _send_request("debugger_activate")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_debugger_deactivate() -> str:
    """Deactivate the internal debugger programmatically.

    Clears ``g_debugger.active`` in the backend and recomputes the
    CPU callbacks. CPU history and memory heatmap recording tied to
    the debugger window stop (in WITH_WINDOW_OR_BP ``cpuhist`` keeps
    recording while an enabled breakpoint exists). If the GUI debug
    window is open, it re-activates the debugger immediately.

    Idempotent. Use ``emu_is_debugger_active`` if the current state
    matters before deciding to call this.

    Returns:
        JSON ``{"active": false}`` on success, ``{"error": "..."}``
        on backend failure.
    """
    resp = await _send_request("debugger_deactivate")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_is_debugger_active() -> str:
    """Query whether the internal debugger is currently active.

    Cheap read-only probe. Returns the live value of
    ``TEST_DEBUGGER_ACTIVE``. Safe to call at any frequency; does
    not influence emulator state.

    Returns:
        JSON ``{"active": bool}``. ``true`` means that CPU history
        and heatmap recording tied to the debugger window
        (WITH_WINDOW, or the default WITH_WINDOW_OR_BP for CPU
        history) is running.
    """
    resp = await _send_request("is_debugger_active")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_set_pioz80_interrupt_vector(port: int, vector: int) -> str:
    """Override the Z80 PIO IM 2 interrupt vector for port A or B.

    WARNING: This is a destructive operation. Writes
    ``g_pioz80.port[port].interrupt_vector`` directly; bit 0 is masked
    out by the backend (Z80 PIO IVW spec - bit 0 is always 0 in a
    daisy chain). The write happens at an emulator safepoint between
    instructions; ``uint8_t`` atomicity is implicit, no extra lock
    is required.

    Platform: MZ-800 and MZ-1500 (= ``HAVE_PIOZ80 = 1``). On MZ-700
    the tool returns ``{"available": false, "reason": "platform has
    no Z80 PIO"}`` instead of an error, so a client can call it
    platform-agnostically.

    Args:
        port: 0 for PIOZ80 port A, 1 for port B. Other values are
            rejected with ``invalid params``.
        vector: 0..255. The byte that will be returned on the data
            bus during IM 2 acknowledge. The backend forces bit 0 to
            0 before writing.

    Returns:
        JSON ``{"available": bool, "port": int, "vector": int,
        "applied": bool}`` on success. ``vector`` echoes the final
        masked value. On MZ-700 the payload is ``{"available": false,
        "reason": "..."}``. ``{"error": "..."}`` for invalid params
        or backend failure.
    """
    if port not in (0, 1):
        return json.dumps({"error": "port must be 0 or 1"})
    if not (0 <= vector <= 0xFF):
        return json.dumps({"error": "vector must be in range 0..255"})
    payload = {"port": port, "vector": vector}
    resp = await _send_request("set_pioz80_interrupt_vector", payload)
    return json.dumps(_data_or_error(resp))


# === V1.E.4 - BP advanced + Stack analytics Tools ====================
# 6 BP advanced wrapperov nad existujucimi DBGAPI prikazmi (bp_update,
# bp_set_parent, bp_create_with_init, bpgrp_add/remove/update) + 6 stack
# analytics nad SP history a stack regions monitoring.


@mcp.tool()
async def emu_bp_create_with_init(addr: int,
                                  fields: list[str] | None = None,
                                  values: dict | None = None) -> str:
    """Atomically create a breakpoint and initialise its fields.

    WARNING: This is a destructive operation. Combines
    ``breakpoints_add_auto(addr, name, parent)`` with a selective
    ``BP_UPDATE`` pass over ``fields``. Both happen inside a single
    DBGAPI safepoint, so other threads never observe the partially
    constructed breakpoint.

    Args:
        addr: Initial Z80 address for the breakpoint. Implicitly
            covered by the ``UM_ADDR`` bit (added to ``fields`` if
            omitted).
        fields: Names of payload fields to apply. Valid names match
            the ``DBGAPI_BP_UM_*`` mask bits, e.g. ``"enabled"``,
            ``"name"``, ``"type"``, ``"expr"``, ``"action"``,
            ``"hit_count"``, ``"bp_addr_space"``,
            ``"im2_vector_filter"``, ``"fwd_min_interval_ms"``,
            ``"fwd_max_fires"``, ...
        values: Map of field name to value. Only fields listed in
            ``fields`` are read from this map; surplus keys are
            ignored. ``name`` / ``expr`` / ``action`` / ``event_name``
            accept ``None`` to clear the value. ``name`` without
            ``auto_name`` in ``fields`` turns the automatic name
            (``Addr: 0x....``, renamed on every address change) off;
            ``auto_name: true`` brings it back.
            ``bp_addr_space`` (``"cpu_view"`` default / ``"bank_offset"``)
            controls how ``addr`` is interpreted for a MEM_R / MEM_W
            breakpoint in the ``MMEXT_BANK`` zone (PEHU only):
            ``"bank_offset"`` matches an offset 0x0000-0x1FFF inside the
            bank regardless of the current CPU mapping.
            ``fwd_min_interval_ms`` (int ms; ``0`` = use the global /
            built-in default) and ``fwd_max_fires`` (int; ``0`` =
            unlimited) override the per-breakpoint rate limit for heavy
            forward actions (``snapshot`` / ``trace_save``); they guard
            against disk saturation on a hot breakpoint.

    See ``emulator://docs/bp_dsl`` for the ``expr`` condition syntax,
    ``emulator://docs/action_dsl`` for the action script syntax used
    by the ``action`` field, and ``emulator://docs/smart_vars`` for
    the ``$name`` variables written by the action.

    A range breakpoint needs both ``"addr_end"`` and
    ``"addr_match_mode"`` (value ``"RANGE"``) in ``fields``; a new
    breakpoint starts in ``SINGLE`` mode, where ``addr_end`` is ignored.

    Returns:
        JSON ``{"id": int, "created": bool}``. ``id`` is the newly
        allocated breakpoint ID or ``-1`` on failure. When ``addr_end``
        differs from ``addr`` but the match mode stays ``SINGLE``, the
        breakpoint is created anyway (it matches only ``addr``) and the
        result also carries ``warning``.
    """
    if not (0 <= addr <= 0xFFFF):
        return json.dumps({"error": "addr must be in range 0..65535"})
    fields = list(fields or [])
    if "addr" not in fields:
        fields.append("addr")
    payload: dict = {"fields": fields, "addr": addr}
    if values:
        for k, v in values.items():
            if k not in payload:
                payload[k] = v
    resp = await _send_request("bp_create_with_init", payload)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_bp_set_parent(id: int, parent_id: int) -> str:
    """Reparent a breakpoint into another group (drag-drop semantics).

    Args:
        id: ID of an existing breakpoint.
        parent_id: Target group ID, or ``-1`` to move to root.

    Returns:
        JSON ``{"updated": bool}``. ``false`` indicates the breakpoint
        with ``id`` does not exist.
    """
    payload = {"id": id, "parent_id": parent_id}
    resp = await _send_request("bp_set_parent", payload)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_bp_update(id: int,
                        fields: list[str],
                        values: dict | None = None) -> str:
    """Selectively update fields of an existing breakpoint.

    WARNING: This is a destructive operation. The backend iterates
    the ``update_mask`` bits assembled from ``fields`` and calls the
    matching ``breakpoints_set_*()`` setter for each. Fields not listed
    in ``fields`` are left untouched.

    Args:
        id: ID of an existing breakpoint.
        fields: Names of fields to apply (subset of
            ``DBGAPI_BP_UM_*``). An empty list is a successful no-op.
            Includes ``"fwd_min_interval_ms"`` / ``"fwd_max_fires"``
            for the per-breakpoint heavy-forward-action rate limit, and
            ``"bp_addr_space"`` (``"cpu_view"`` / ``"bank_offset"``) for
            a MEM_R / MEM_W breakpoint in the ``MMEXT_BANK`` zone
            (PEHU only).
        values: Map of field name to value. Same conventions as
            ``emu_bp_create_with_init``.

    See ``emulator://docs/bp_dsl`` for the ``expr`` condition syntax
    and ``emulator://docs/action_dsl`` for the action script syntax
    used by the ``action`` field.

    Returns:
        JSON ``{"updated": bool}``. ``false`` when the breakpoint
        ``id`` does not exist or an unknown enum value was supplied.
    """
    payload: dict = {"id": id, "fields": list(fields)}
    if values:
        for k, v in values.items():
            if k not in payload:
                payload[k] = v
    resp = await _send_request("bp_update", payload)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_bpgrp_add(name: str, parent: int = -1) -> str:
    """Create a new breakpoint group.

    Args:
        name: Group label. Stored verbatim, max 31 chars + ``'\\0'``.
        parent: Parent group ID, or ``-1`` for root.

    Returns:
        JSON ``{"id": int}``. ``id`` is ``-1`` on failure (cyclic
        parent, missing parent ID, ...).
    """
    if not name:
        return json.dumps({"error": "name must be non-empty"})
    payload = {"name": name, "parent": parent}
    resp = await _send_request("bpgrp_add", payload)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_bpgrp_remove(id: int) -> str:
    """Remove a breakpoint group by ID.

    WARNING: This is a destructive operation. Cascading delete /
    reparent of child breakpoints and sub-groups is handled by the
    backend (``breakpoints.c``).

    Args:
        id: ID of an existing group.

    Returns:
        JSON ``{"removed": bool}``.
    """
    payload = {"id": id}
    resp = await _send_request("bpgrp_remove", payload)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_bpgrp_update(id: int,
                           fields: list[str],
                           enabled: bool | None = None,
                           name: str | None = None,
                           bg_rgb: int | None = None,
                           fg_rgb: int | None = None,
                           parent: int | None = None) -> str:
    """Selectively update fields of a breakpoint group.

    WARNING: This is a destructive operation. Fields not listed in
    ``fields`` are left untouched.

    Args:
        id: ID of an existing group.
        fields: Field names to apply: ``"enabled"``, ``"name"``,
            ``"colors"`` (= both ``bg_rgb`` and ``fg_rgb``),
            ``"parent"``.
        enabled: New enable flag (only when ``"enabled"`` in fields).
        name: New label (only when ``"name"`` in fields). ``None``
            clears the label.
        bg_rgb: New background RGB (only when ``"colors"`` in fields).
        fg_rgb: New foreground RGB (only when ``"colors"`` in fields).
        parent: New parent group ID, ``-1`` for root.

    Returns:
        JSON ``{"updated": bool}``. ``false`` if the group does not
        exist.
    """
    payload: dict = {"id": id, "fields": list(fields)}
    if enabled is not None:
        payload["enabled"] = bool(enabled)
    if name is not None:
        payload["name"] = name
    if bg_rgb is not None:
        payload["bg_rgb"] = int(bg_rgb)
    if fg_rgb is not None:
        payload["fg_rgb"] = int(fg_rgb)
    if parent is not None:
        payload["parent"] = int(parent)
    resp = await _send_request("bpgrp_update", payload)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_stack_history_enable(enabled: bool) -> str:
    """Enable or disable SP history recording.

    WARNING: This is a destructive operation. Disabling additionally
    flushes the ring buffer so the next enable starts with a clean
    state. The active flag wires into a hot-path call site in
    ``mzarch.c`` (= zero overhead while the default OFF state is in
    effect).

    Args:
        enabled: ``True`` to start recording, ``False`` to stop and
            flush.

    Returns:
        JSON ``{"enabled": bool}`` echoing the final state.
    """
    payload = {"enabled": bool(enabled)}
    resp = await _send_request("stack_history_enable", payload)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_stack_history_reset() -> str:
    """Flush the SP history ring buffer.

    WARNING: This is a destructive operation. Recording flag is
    preserved (= only the captured samples are dropped). Useful for
    the UI "Reset history" button before observing a specific code
    path.

    Returns:
        JSON ``{"reset": true}`` on success.
    """
    resp = await _send_request("stack_history_reset")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_stack_regions_add(name: str, base: int, limit: int) -> str:
    """Add a new monitored stack region.

    Args:
        name: Region label, max 31 chars + ``'\\0'``.
        base: Top of the region (highest address).
        limit: Bottom of the region (must satisfy ``base > limit``).

    Returns:
        JSON ``{"index": int, "added": bool}``. ``index`` is the new
        slot 0..MAX-1 on success or ``-1`` on overlap / duplicate /
        invalid range.
    """
    if not name:
        return json.dumps({"error": "name must be non-empty"})
    if not (0 <= base <= 0xFFFF) or not (0 <= limit <= 0xFFFF):
        return json.dumps({"error": "base and limit must be 0..65535"})
    payload = {"name": name, "base": base, "limit": limit}
    resp = await _send_request("stack_regions_add", payload)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_stack_regions_edit(index: int, name: str,
                                 base: int, limit: int) -> str:
    """Edit an existing monitored stack region.

    WARNING: This is a destructive operation. On success the backend
    also resets the region watermark + push/pop counters (= old stats
    do not match the new range).

    Args:
        index: Region slot 0..count-1 to edit.
        name: New label.
        base: New top of region.
        limit: New bottom (``base > limit``).

    Returns:
        JSON ``{"updated": bool}``. ``false`` on invalid args, name
        conflict, range overlap or index out of bounds.
    """
    if not name:
        return json.dumps({"error": "name must be non-empty"})
    if index < 0:
        return json.dumps({"error": "index must be >= 0"})
    if not (0 <= base <= 0xFFFF) or not (0 <= limit <= 0xFFFF):
        return json.dumps({"error": "base and limit must be 0..65535"})
    payload = {"index": index, "name": name,
               "base": base, "limit": limit}
    resp = await _send_request("stack_regions_edit", payload)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_stack_regions_remove(index: int) -> str:
    """Remove a monitored stack region.

    WARNING: This is a destructive operation.

    Args:
        index: Region slot to remove.

    Returns:
        JSON ``{"removed": bool}``. ``false`` if the index is out of
        range.
    """
    if index < 0:
        return json.dumps({"error": "index must be >= 0"})
    payload = {"index": index}
    resp = await _send_request("stack_regions_remove", payload)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_stack_regions_reset_watermark(index: int) -> str:
    """Reset watermark and push/pop counters of a single stack region.

    WARNING: This is a destructive operation. Region configuration
    (``name``, ``base``, ``limit``) is preserved; only the runtime
    statistics are zeroed.

    Args:
        index: Region slot whose stats should be reset.

    Returns:
        JSON ``{"reset": bool}``. ``false`` if the index is out of
        range.
    """
    if index < 0:
        return json.dumps({"error": "index must be >= 0"})
    payload = {"index": index}
    resp = await _send_request("stack_regions_reset_watermark", payload)
    return json.dumps(_data_or_error(resp))


# === V1.E.5 - Eventlog/TLOG Tools (6 Tools) =========================
# Wrappery nad Event Viewer ring bufferem. Backend je TLOG-style
# in-memory ring 24 B/event s 64-bit categories bitmask filter.
# Typický flow: set_mask -> start -> (emu run) -> stop -> iterate
# get_event(idx=0..count-1) -> clear.


@mcp.tool()
async def emu_regions_list() -> str:
    """List all physical memory regions of the current architecture.

    Returns a snapshot of regions (ROM lower/upper, CG-ROM, VRAM,
    CG-RAM/PCG, MemExt banks, Ramdisk banks, ...) with metadata. An
    ``id`` is an index into the list and stays valid until the hardware
    configuration changes (RAMdisk, MemExt, PEZIK, loading a snapshot
    with a different configuration). ``layout_gen`` identifies the
    layout of the list; ``emu_region_read`` / ``emu_region_write`` with
    an ``id`` from an older list fail with ``stale region id`` - call
    this tool again. To keep an address across such changes, store
    ``kind`` and ``sub_id``.

    Returns:
        JSON ``{"regions": [{id, kind, sub_id, name, logical_base,
        size, writable, connected, mapped_now}, ...], "count": int,
        "layout_gen": int}``.

        - ``kind`` is one of: ``logical, ram, rom_lower, rom_upper,
          cgrom, cgram_700, vram_700_char, vram_700_attr,
          vram_phys_plane, pcg_1500, memext_ram, memext_flash,
          ramdisk_std, ramdisk_pezik, prohibited_shadow``.
        - ``sub_id`` disambiguates regions of the same kind (plane
          index 0..3, bank index, PEZIK instance).
        - ``logical_base`` is the Z80 address where the region
          usually maps, or ``null`` if not in the Z80 view by default.
        - ``mapped_now`` indicates whether the region is currently
          visible in the Z80 address space (banking-aware).
    """
    resp = await _send_request("regions_list")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_region_read(region_id: int, offset: int = 0,
                          length: int = 256, want_data: bool = True,
                          layout_gen: int = 0) -> str:
    """Read raw bytes from a physical memory region (bypass Z80
    banking).

    No-side-effect read: no auto-inc latch, no GDG RF dispatch, no IRQ
    trigger. Reads directly from the physical RAM/ROM/VRAM/MemExt array
    of the emulator - exactly what the GUI Memory browser sees.

    Args:
        region_id: ID from a previous ``emu_regions_list`` call. Invalid
            after a hardware configuration change (error
            ``stale region id``).
        offset: Byte offset within the region (0..size-1).
        length: Number of bytes to read (1..65536). Clamps to region
            size if ``offset+length > size``.
        want_data: False = only ``sha256``, no ``data_b64``.
        layout_gen: ``layout_gen`` of the list ``region_id`` comes from;
            0 = the last ``emu_regions_list`` call of any client.

    Returns:
        JSON ``{"region_id": int, "offset": int, "length": int,
        "sha256": "...", "data_b64": "..."}``. ``length`` reflects the
        actual number of bytes read after clamping, ``sha256`` is the
        SHA-256 of those bytes. On error: ``{"error": "..."}``.
    """
    if region_id < 0:
        return json.dumps({"error": "region_id must be >= 0"})
    if offset < 0:
        return json.dumps({"error": "offset must be >= 0"})
    if length < 1 or length > 65536:
        return json.dumps({"error": "length must be in range 1..65536"})
    if layout_gen < 0:
        return json.dumps({"error": "layout_gen must be >= 0"})
    params = {"region_id": region_id, "offset": offset, "length": length,
              "want_data": bool(want_data)}
    if layout_gen:
        params["layout_gen"] = layout_gen
    resp = await _send_request("region_read", params)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_region_write(region_id: int, offset: int,
                           data_b64: str, layout_gen: int = 0,
                           expected_sha256: Optional[str] = None,
                           want_hashes: bool = False,
                           expected_state_rev: int | None = None) -> str:
    """Write raw bytes into a physical memory region (bypass Z80
    banking).

    WARNING: This is a destructive operation. The backend respects
    region writable flags - ``memext_flash`` and ``prohibited_shadow``
    are read-only and return an error. ROM regions ARE writable for
    debug fixtures (= patches do NOT persist across reset; the ROM
    file on disk is unchanged).

    For VRAM regions there is no automatic screen refresh - if the
    emulator runs while you write VRAM, the change becomes visible on
    the next frame; in pause mode the GUI needs an explicit refresh.

    Args:
        region_id: ID from a previous ``emu_regions_list`` call.
        offset: Byte offset within the region (0..size-1).
        data_b64: Base64-encoded bytes (max 65536 bytes per call).
        layout_gen: As for ``emu_region_read``; with an ``id`` from an
            older list nothing is written.
        expected_sha256: conditional write - write only if the SHA-256 of
            the bytes being overwritten (after clipping at the region
            end) still equals this value, e.g. the ``sha256`` of
            ``emu_region_read`` or of a ``emu_state_observe`` region window
            of the same range. Checked and written in one step in the
            emulator thread; on a mismatch nothing is written and the
            error starts with ``Precondition failed:``. A physical region
            is not affected by banking, so this is the safest conditional
            write. Errors of the id, ``layout_gen`` and read-only regions
            come first.
        want_hashes: also return ``before_sha256`` and ``after_sha256``.
        expected_state_rev: perform the command only if the state
            revision still equals this value (``state_rev`` from
            ``emu_get_state`` / ``emu_state_observe``, only while paused);
            otherwise nothing changes and the error starts with
            ``Precondition failed: state changed``. See
            ``emulator://docs/state_revision``.

    Returns:
        JSON ``{"region_id", "offset", "length", "written"}`` (plus
        ``before_sha256`` / ``after_sha256`` with a condition or
        ``want_hashes``). On error ``{"error": "..."}``.
    """
    if region_id < 0:
        return json.dumps({"error": "region_id must be >= 0"})
    if offset < 0:
        return json.dumps({"error": "offset must be >= 0"})
    if not data_b64:
        return json.dumps({"error": "data_b64 must be non-empty"})
    if layout_gen < 0:
        return json.dumps({"error": "layout_gen must be >= 0"})
    params: dict[str, Any] = {"region_id": region_id, "offset": offset,
                              "data_b64": data_b64}
    if layout_gen:
        params["layout_gen"] = layout_gen
    err = _write_cond_payload(params, expected_sha256, want_hashes)
    if err:
        return json.dumps({"error": err})
    resp = await _send_request("region_write", _with_rev(params, expected_state_rev))
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_file_load_to_region(path: str, region_id: int,
                                  dest_offset: int = 0,
                                  src_offset: int = 0,
                                  length: int = -1) -> str:
    """Composite: load file content into a memory region. WARNING:
    destructive.

    Python composite of file open + slice + ``emu_region_write``.
    Useful for prefill ramdisk, ROM patch, VRAM test fixture without
    base64-encoding on the client.

    Args:
        path: Source file path.
        region_id: Target region ID.
        dest_offset: Target region offset (default 0).
        src_offset: Skip N bytes at start of file (default 0).
        length: Bytes to copy (-1 = until EOF; hard cap 65536).
    """
    try:
        with open(path, "rb") as f:
            if src_offset:
                f.seek(src_offset)
            data = f.read() if length < 0 else f.read(length)
    except Exception as e:
        return json.dumps({"error": f"file read failed: {e}"})
    if not data:
        return json.dumps({"error": "no data to write"})
    if len(data) > 65536:
        return json.dumps({"error":
            f"length {len(data)} > 65536; split into multiple calls"})
    import base64
    b64 = base64.b64encode(data).decode("ascii")
    resp = await _send_request("region_write",
                               {"region_id": region_id,
                                "offset": dest_offset,
                                "data_b64": b64})
    # Augmented payload: na úspěch přidáme zdrojový soubor/offset.
    # Na chybu backendu MUSÍ projít error dict beze změny - jinak by
    # se src_file/src_offset vložily do error dictu a zamaskovaly by
    # selhání (bug 0011, augmented-{} varianta).
    out = _data_or_error(resp)
    if "error" in out:
        return json.dumps(out)
    out["src_file"] = path
    out["src_offset"] = src_offset
    return json.dumps(out)


@mcp.tool()
async def emu_file_save_from_region(path: str, region_id: int,
                                    src_offset: int = 0,
                                    length: int = 4096) -> str:
    """Composite: read a memory region and save to file.

    Useful for ROM dumps, ramdisk backups, raw VRAM captures.

    Args:
        path: Destination file (overwritten).
        region_id: Source region ID.
        src_offset: Region offset (default 0).
        length: Bytes to read (default 4096, max 65536).
    """
    if length < 1 or length > 65536:
        return json.dumps({"error": "length must be in range 1..65536"})
    resp = await _send_request("region_read",
                               {"region_id": region_id,
                                "offset": src_offset,
                                "length": length})
    data = resp.get("data", {})
    if "data_b64" not in data:
        return json.dumps(
            {"error": "region_read returned no data", "detail": data})
    import base64
    raw = base64.b64decode(data["data_b64"])
    try:
        with open(path, "wb") as f:
            f.write(raw)
    except Exception as e:
        return json.dumps({"error": f"file write failed: {e}"})
    return json.dumps({
        "region_id": region_id,
        "src_offset": src_offset,
        "length": len(raw),
        "dest_file": path,
    })


@mcp.tool()
async def emu_eventlog_start() -> str:
    """Start Event Viewer recording.

    WARNING: This is a destructive operation. Subsequent events that
    pass the active categories mask are written into the ring buffer.

    Returns:
        JSON ``{"started": bool}``. Errors if the underlying ring
        buffer cannot be allocated.
    """
    resp = await _send_request("eventlog_start")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_eventlog_stop() -> str:
    """Stop Event Viewer recording. Ring buffer contents are preserved.

    WARNING: This is a destructive operation (toggles the active
    recording flag).

    Returns:
        JSON ``{"stopped": bool}``. Idempotent (= calling on an
        already-stopped log is a no-op).
    """
    resp = await _send_request("eventlog_stop")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_eventlog_clear() -> str:
    """Flush the Event Viewer ring buffer.

    WARNING: This is a destructive operation. The recording flag is
    preserved; only the captured events are dropped.

    Returns:
        JSON ``{"cleared": bool}``.
    """
    resp = await _send_request("eventlog_clear")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_eventlog_set_capacity(capacity: int) -> str:
    """Resize the Event Viewer ring buffer.

    WARNING: This is a destructive operation. The existing ring
    contents are discarded (= the resize re-allocates and clears).

    Args:
        capacity: Requested ring size in events. The backend clamps
            the value to ``[EVENTLOG_MIN_CAPACITY..EVENTLOG_MAX_CAPACITY]``
            (= 10000..200000).

    Returns:
        JSON ``{"capacity_after": int}`` with the actual size after
        clamping.
    """
    if capacity < 0:
        return json.dumps({"error": "capacity must be >= 0"})
    payload = {"capacity": capacity}
    resp = await _send_request("eventlog_set_capacity", payload)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_eventlog_set_mask(mask) -> str:
    """Atomically replace the categories filter mask.

    WARNING: This is a destructive operation (mutates active filter
    used by subsequent emits).

    See ``emulator://docs/eventlog_mask`` for category bit assignments
    and ready-to-use mask recipes (memory only, IRQ only, etc.).

    Args:
        mask: 64-bit bitmask, bit ``i`` enables category ``i`` per
            ``en_EVENTLOG_CATEGORY`` (CPU_INT=0, CPU_PIN_EDGE=1,
            IRQ_ACK_IM2=2, IORQ_IN=3, IORQ_OUT=4, MMIO_R=5,
            MMIO_W=6, GDG_MODE=7, GDG_BANKING=8, GDG_HWSCROLL=9,
            GDG_COLORS=10, GDG_VIDEO=11, PIO8255=12, CTC8253=13,
            PIOZ80=14, PSG=15, FDC=16, MEMEXT=17, BP_FIRE=18,
            USER_MARK=19, CPU_CTRL=20, GDG_WFRF=21, QD=22, RD=23,
            SYS=24). Bits beyond the defined count are ignored.

            Accepts two forms:
            - **int** (0..2^63-1): standard JSON number, max 63 bits
              (bit 63 unreliable due to signed gint64 interpretation)
            - **hex string** (e.g. ``"0xFFFFFFFFFFFFFFFF"``, ``"FFFF"``,
              ``"0xff_ff"``): full 64-bit range including bit 63;
              underscores are stripped as visual separators

    Returns:
        JSON ``{"mask_hex": "0xN..."}`` - the applied value as a
        16-digit zero-padded hex string for unambiguous round-trip.
    """
    if isinstance(mask, int):
        if mask < 0:
            return json.dumps({"error": "mask must be >= 0"})
        payload = {"mask": mask}
    elif isinstance(mask, str):
        payload = {"mask": mask}
    else:
        return json.dumps({"error": "mask must be int or hex string"})
    resp = await _send_request("eventlog_set_mask", payload)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_eventlog_get_event(idx: int) -> str:
    """Read a single event from the Event Viewer ring buffer.

    Args:
        idx: Logical index (0 = oldest). Must be non-negative.

    Returns:
        JSON object. When the index is in range::

            {
              "available": true,
              "idx": int,
              "pxclk_total": int,
              "screens_total": int,
              "pxclk_in_screen": int,
              "category": int,
              "subtype": int,
              "pc": int,
              "payload": int
            }

        When ``idx >= count`` the response is
        ``{"available": false, "idx": int}``. Decoding of
        ``payload`` per ``category`` follows
        ``en_EVENTLOG_CATEGORY`` (see eventlog.h).
    """
    if idx < 0:
        return json.dumps({"error": "idx must be >= 0"})
    payload = {"idx": idx}
    resp = await _send_request("eventlog_get_event", payload)
    return json.dumps(_data_or_error(resp))


# === MCP Resources (V0.B.7 - 7 read-only Resources) ==================
# Resources mají sémanticky odlišnou roli než Tools (= viz rozbor sekce
# 4.5 "vše jako Tools" anti-pattern):
#
#   * Tool     = akce s vedlejšími efekty (= state-changing). AI klient
#                může před zavoláním ptát uživatele na potvrzení.
#   * Resource = read-only data identifikovaná URI. Levné na čtení,
#                idempotentní, AI klient si je smí cachovat.
#
# Resource descriptions jsou anglicky - MCP wire protocol je
# locale-agnostic (= klient zobrazí description uživateli).
#
# V0.B.7 vystavuje 7 URI; V1.D.1 přidalo 8 dalších = 15 celkem:
#   emulator://state                           - lightweight stav + last_user_action
#   emulator://cpu/registers                   - Z80 registry
#   emulator://memory/{addr_hex}/{length}      - template URI s params
#   emulator://breakpoints                     - aktivní BP
#   emulator://platform/info                   - stub V0 (V1.B dynamic)
#   emulator://config/mcp                      - INI hodnoty (V0.B.4)
#   emulator://config/peripherals              - stub V0 (V1.D per-chip)
#   --- V1.D.1 ---
#   emulator://config/settings                 - live cfgmain whitelist
#   emulator://media/state                     - CMT/FDC/QD/IDE8 mount info
#   emulator://cpu/im2_vector                  - IM2 vektor snapshot
#   emulator://cpu/interrupt_bus               - IRQ subsystem snapshot
#   emulator://cooperation/policy              - cooperation hint
#   emulator://security/profile                - MCP security profile + capabilities
#   emulator://memory/map                      - per-platform banking
#   emulator://memext/info                     - Luftner / PEHU info
#   --- V1.D.2.A (= 5 easy reuse Resources nad V1.A wrappery) ---
#   emulator://callstack                       - V1 shadow callstack snapshot
#   emulator://profiler                        - per-funkce profile data
#   emulator://symbols                         - sym_db list (capped 10k)
#   emulator://stack/history                   - SP ring buffer
#   emulator://stack/regions                   - definované stack regions
#
# Žádné Resources subscribe / `notifications/resources/updated`
# (= V1.A scope, vyžaduje EVENT infrastructure).


@mcp.resource("emulator://state")
async def resource_state() -> str:
    """Current emulator state (paused, frame, cycles, transport).

    Lightweight read-only snapshot. Returns JSON with the same fields as
    the ``emu_status`` tool plus a ``transport`` field naming the active
    backend transport (``pipe`` or ``tcp``). If the transport has not
    been connected yet, returns ``{"connected": false, "transport": ...}``.
    """
    # Pokud transport ještě nedrží konexi, vrátíme jen meta info -
    # nechceme triggerovat lazy spawn jen kvůli Resource readu (= AI
    # klient by mohl Resource číst opakovaně a nechtěně spustit emu).
    if _transport is None or not _transport.is_alive():
        return json.dumps({
            "connected": False,
            "transport": TRANSPORT_KIND,
        })
    resp = await _send_request("get_state")
    data = resp.get("data", {}) or {}
    data["transport"] = TRANSPORT_KIND
    return json.dumps(data)


@mcp.resource("emulator://run/history")
async def resource_run_history() -> str:
    """Newest stop records of the emulator (oldest first, up to 32).

    Same content as ``emu_get_stop_history``: why and where each run
    stopped, who started it and how long it ran.
    """
    resp = await _send_request("get_stop_history", {"limit": 32})
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://cpu/registers")
async def resource_cpu_registers() -> str:
    """Full Z80 register snapshot (PC, SP, AF, BC, DE, HL, IX, IY,
    alternate registers AF_/BC_/DE_/HL_, combined IR with I in the high
    byte). Reading the resource does NOT pause the emulator; for a
    coherent snapshot pause via the ``emu_pause`` tool first.
    """
    # Resource read mimo connected transport vrátí prázdný objekt -
    # AI klient se z chybějících polí dozví, že emu není dostupný.
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_registers")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://memory/{addr_hex}/{length}")
async def resource_memory(addr_hex: str, length: str) -> str:
    """Read bytes from Z80 memory at a hex address.

    URI template: ``emulator://memory/{addr_hex}/{length}``. Example:
    ``emulator://memory/4000/16`` reads 16 bytes starting at 0x4000.
    The address is interpreted via the current Z80 memory map (banking
    state applies). Returns JSON ``{"addr": <int>, "len": <int>,
    "data_b64": "..."}`` on success or ``{"error": "..."}`` on bad input.

    Args:
        addr_hex: hex string of the start address (e.g. ``"4000"`` or
            ``"E000"``; case-insensitive, no ``0x`` prefix).
        length: decimal byte count, range 1..4096.
    """
    # Resource URI parametry jsou stringy - parsujeme je lokálně před
    # transportem, aby zjevně malformovaný request šetřil roundtrip.
    try:
        addr = int(addr_hex, 16)
    except (TypeError, ValueError):
        return json.dumps({"error": "addr_hex must be a hex string"})
    try:
        n = int(length)
    except (TypeError, ValueError):
        return json.dumps({"error": "length must be a decimal integer"})
    if addr < 0 or addr > 0xFFFF:
        return json.dumps({"error": "addr must be in range 0..65535"})
    if n < 1 or n > 4096:
        return json.dumps({"error": "length must be in range 1..4096"})
    if _transport is None or not _transport.is_alive():
        return json.dumps({"error": "emulator not connected"})
    resp = await _send_request("mem_read", {"addr": addr, "len": n})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error", "mem_read failed")})
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://breakpoints")
async def resource_breakpoints() -> str:
    """List of active breakpoints with id, address, type, hit counter
    and enabled flag. Mirror of the ``emu_bp_list`` tool intended for
    AI clients that want to browse current debugger state without
    invoking a tool.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False, "breakpoints": []})
    resp = await _send_request("bp_list")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://platform/info")
async def resource_platform_info() -> str:
    """Platform metadata: MZ architecture, mode, clocks and scanline geometry.

    Returns top-level fields plus three sub-objects (``capabilities``,
    ``clocks``, ``scanline``) describing everything the client needs to
    reason about timing and HW after MCP connect.

    Top-level fields:

    - ``platform``: ``mz700-pal`` / ``mz700-ntsc`` / ``mz800`` / ``mz1500``
      (= compile-time MZARCH_NAME, includes PAL/NTSC variant for MZ-700)
    - ``full_name``: human-readable label (= "MZ-700 (PAL)", "MZ-800", ...)
    - ``mode``: ``native`` / ``compat700`` (= runtime from GDG regDMD bit;
      MZ-700 always native, MZ-800 bit 3 = native_800)
    - ``tv_system``: ``PAL`` / ``NTSC`` (= compile-time MZTVSYS)
    - ``framerate_hz``: 50 / 60 (= derived from TV system)
    - ``pxclk_hz``: GDG base clock (= simulated, integer multiple of frame)
    - ``mzarch``: same as ``platform`` (legacy V0.B.7 compat)
    - ``mzarch_numeric``: 700 / 800 / 1500 (= numeric platform variant)
    - ``rom_version``: ``unknown`` (= future V2 from ROM header)

    ``capabilities`` sub-object - compile-time HW support (what is in the
    binary, not what is runtime attached):

    - ``has_pioz80`` (bool), ``psg_count`` (int)
    - ``hwext_fdc`` / ``hwext_ide8`` / ``hwext_ramdisk`` / ``hwext_qdisk``
    - ``cpu_model`` (= "Z80")

    ``clocks`` sub-object - per-platform Hz frequencies + dividers:

    - ``gdg_base_hz``, ``gdg_real_base_hz`` (= simulated vs crystal)
    - ``cpu_hz``, ``cpu_divider``
    - ``ctc0_input_hz``, ``ctc0_divider``
    - ``psg_input_hz``, ``psg_divider`` (null for MZ-700, HAVE_PSG=0)
    - ``ctc1_input_hz``, ``ctc2_input_hz`` = null (cascade-driven, read
      ``emulator://periph/i8253`` for runtime state)

    ``scanline`` sub-object - raster timing constants (= sync + porch +
    display, plus canvas + border geometry).

    Backed by the ``get_platform_info`` dispatch handler.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_platform_info")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://config/mcp")
async def resource_config_mcp() -> str:
    """MCP server configuration from the emulator INI ``[MCP]`` section.

    Backed by the dispatch ``get_mcp_config`` handler added in V0.B.7.
    Returns JSON with fields ``tcp_port``, ``bind_address``, ``profile``,
    ``auto_start_tcp`` and ``tcp_enabled`` (= false when the emulator
    was built without ``MZ800EMU_CFG_MCP_TCP_ENABLED``).
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_mcp_config")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://config/peripherals")
async def resource_config_peripherals() -> str:
    """List of attached peripherals (V0.B.7 placeholder).

    Returns a stub payload with an empty ``attached`` array. The full
    per-peripheral state Resources (``emulator://periph/i8255``,
    ``emulator://periph/i8253``, FDC, GDG, ...) are deferred to V1.D
    when the emu backend exports a dedicated peripheral introspection
    API.
    """
    # Stub V0 - nepoužíváme transport ani _send_request, aby Resource
    # mohlo být přečteno i bez běžícího emu (= AI klient si přečte
    # `note` a ví, že full implementace přijde V1.D).
    return json.dumps({
        "attached": [],
        "note": "Placeholder in V0.B.7. Per-peripheral Resources in V1.D.",
    })


# === V1.D.1 - Core + CPU extras Resources (8 new endpoints) ==========
# Backed by dbgapi handlers added in V1.D.1; together with the 7 V0
# Resources above this gives 15 Resources total.


@mcp.resource("emulator://config/settings")
async def resource_config_settings() -> str:
    """Live emulator configuration values (audio, video, ...).

    Returns a hierarchical JSON ``{"profile": str, "filtered": bool,
    "sections": {section: {key: value}}}``. Section / key names mirror the
    cfgmain INI registry. Only a curated whitelist of live-settable keys
    is exposed (no filesystem paths, no boot-time keys).

    When the current MCP security profile is ``observer``, ``filtered`` is
    true and ``sections`` is an empty object - the AI client cannot read
    even the whitelisted keys.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_config_settings")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://media/state")
async def resource_media_state() -> str:
    """Current state of attached media devices (CMT, FDC, QD, IDE8).

    Returns ``{"slots": [{slot, inserted, path, ro}, ...], "count": N,
    "note": "..."}``. The slot list mirrors the ``media_state`` Tool
    payload (V1.B.1). Extended fields such as motor state or head position
    are planned for V1.D.2 and indicated in ``note``.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_media_state")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://speed")
async def resource_speed() -> str:
    """Current emulation speed state (throttle / warp).

    Returns ``{"current_percent": int, "max_speed": bool, "mode": str,
    "status": str}`` where ``mode`` is ``"max"`` (warp), ``"custom"``
    (non-100% throttle) or ``"normal"`` (100%). ``status`` is an
    informational UI text. Read-only, no side effects.

    Use the ``emu_set_speed`` / ``emu_speed_step`` tools to change the
    speed (e.g. warp through a slow boot or tape load).
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_speed")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://cpu/im2_vector")
async def resource_cpu_im2_vector() -> str:
    """Z80 Interrupt Mode 2 vector snapshot.

    Returns ``{"im": int, "i": int, "vec": int, "available": bool,
    "isr_addr": int, "isr_target": int}``. When the CPU is not in IM 2,
    ``available`` is false and ``isr_addr`` / ``isr_target`` are zero
    (the AI client should ignore them).

    ``isr_addr`` = ``(i << 8) | vec``; ``isr_target`` is the 16-bit
    little-endian word read from memory at ``isr_addr`` (= actual ISR
    entry point). No side effects on the emulator state.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_cpu_im2_vector")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://cpu/interrupt_bus")
async def resource_cpu_interrupt_bus() -> str:
    """Full IRQ subsystem snapshot (Z80 core + per-chip placeholders).

    Returns ``{"z80_state": {iff1, iff2, im, halted, int_line, nmi_line,
    i, ei_pending}, "platform_note": str, "daisy_chain": {available,
    reason}, "non_chain_sources": {...}, "nmi_sources": {...},
    "recent_acks": []}``.

    V1.D.1 ships with the Z80 core fields fully populated and a
    per-platform note; the per-chip sub-objects (daisy chain, non-chain
    sources, NMI sources) carry ``available=false`` plus a textual reason
    and will be implemented in V1.D.2. ``recent_acks`` is reserved for a
    ring buffer of recent IRQ acknowledgments (planned V1.D.2).
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_cpu_interrupt_bus")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://cooperation/policy")
async def resource_cooperation_policy() -> str:
    """Cooperation hint policy currently set by the AI client.

    Returns ``{"mode": "free|read_only|paused_only", "until": str|null,
    "set_by": "ai", "set_at_us": int}``. The cooperation hint is a
    self-binding instruction the AI sets via the ``cooperation_hint_set``
    Tool; V1.A.1 stores it but does not enforce it.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_cooperation_policy")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://security/profile")
async def resource_security_profile() -> str:
    """MCP security profile + derived capabilities.

    Returns ``{"profile": "wild|confined|sandboxed|observer",
    "capabilities": [str, ...], "file_access_paths": [str, ...],
    "auth": {"required": bool}, "enforcement_note": str}``. V0.B.4 only
    stores the profile value; full enforcement (whitelist, capability
    gating, authentication) is deferred to V2 - the AI client is
    informed via ``enforcement_note``.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_security_profile")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://memory/map")
async def resource_memory_map() -> str:
    """Current 16 x 4 KB banking snapshot for the active platform.

    Returns ``{"platform": "mz700|mz800|mz1500", "mode_note": str,
    "slots": [{addr_start, addr_end, source, ro_rw, slot_offset}, ...]}``.

    ``source`` is one of ``unknown / rom / cgrom / sram / vram /
    memext_ram / memext_flash``; ``ro_rw`` is ``"r"`` or ``"rw"``. V1.D.1
    populates MEMEXT slots fully (Luftner / PEHU); other slots are marked
    as ``unknown`` until V1.D.2 wires up per-platform banking detection.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_memory_map")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://memext/info")
async def resource_memext_info() -> str:
    """Memory expansion adapter info (Luftner / PEHU / none).

    Returns ``{"type": str, "connected": bool, "ram_banks": int,
    "ram_bank_size": int, "flash_banks": int|null, "flash_bank_size":
    int|null, "current_map": [16 ints], "map_available": bool}``.

    For Luftner (= default MZ-800 memext) ``flash_banks`` and
    ``flash_bank_size`` are populated; for PEHU they are ``null`` (PEHU
    has no flash). ``current_map`` mirrors the live ``g_memext.map[]``
    register state - 16 raw bank indices currently mapped into the Z80
    address space.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_memext_info")
    return json.dumps(_data_or_error(resp))


# === V1.D.2.A - Easy reuse Resources (5 new endpoints) ===============
# Backed by dispatch handlers added in V1.D.2.A that wrap the existing
# DBGAPI_CMD_GET_CALLSTACK / GET_PROFILER / SYMBOL_LIST /
# STACK_HISTORY_GET / STACK_REGIONS_LIST commands shipped in V1.A. No
# new dbgapi enum entries; pure JSON serialization layer. Together with
# the 15 V0+V1.D.1 Resources this gives 20 Resources total.


@mcp.resource("emulator://callstack")
async def resource_callstack() -> str:
    """Current Z80 call stack snapshot from the V1 single-shadow tracker.

    Returns ``{"active": bool, "current_depth": int,
    "max_depth_reached": int, "divergence_count": int,
    "overflow_count": int, "cycles_now": int, "count": int,
    "frames": [{"depth", "return_addr", "call_site_addr", "target_addr",
    "sp_at_entry", "cycles_at_entry", "kind"}]}``.

    ``frames`` are ordered with ``depth=0`` at the top of the stack.
    ``kind`` is one of ``"call" | "rst" | "irq_im0" | "irq_im1" |
    "irq_im2" | "nmi" | "synthetic"``.

    The full callstack subsystem is gated by the cfgmain ``[CALLSTACK]
    active`` flag; when inactive, ``active=false`` and ``frames`` is
    empty. The Resource read is non-destructive and does not pause the
    emulator (snapshot is taken on an EMU safe-point).
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_callstack")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://profiler")
async def resource_profiler() -> str:
    """Per-function profile aggregate (call count + inclusive / exclusive
    cycles per unique CALL target).

    Returns ``{"active": bool, "entry_count": int, "total_cycles_64": int,
    "total_calls": int, "irq_entries": int, "unmatched_returns": int,
    "max_depth_reached": int, "overflow_count": int, "entries": [{"addr",
    "kind", "calls", "excl_cycles", "incl_cycles", "min_cycles",
    "max_cycles", "avg_cycles"}]}``.

    ``entries`` is unsorted - the client may sort by any field. The
    profiler subsystem is gated by ``[PROFILER] active`` (cfgmain INI)
    or by the ``profiler_start`` Tool; when inactive, ``active=false``
    and ``entries`` is empty.

    The full unpaginated list is returned (capped by profiler hash map
    capacity); use the ``profiler_get`` Tool with an explicit ``limit``
    if you need pagination.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_profiler")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://symbols")
async def resource_symbols() -> str:
    """Symbol table snapshot (sym_db) - addresses, names, comments and
    source origin.

    Returns ``{"count": int, "truncated": bool, "max_returned": int,
    "symbols": [{"addr", "name", "comment", "source"}]}``. ``source`` is
    one of ``"user" | "map_file" | "noi_file" | "sym_file"`` matching the
    en_SYM_SOURCE values (3=user, 2=map, 1=noi, 0=sjasmplus sym).

    The Resource caps the response at 10000 symbols; ``truncated`` is
    true when the cap was hit and the client should fall back to the
    ``symbol_list`` Tool with an explicit ``prefix`` filter for full
    pagination.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_symbols")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://stack/history")
async def resource_stack_history() -> str:
    """SP (stack pointer) ring buffer for stack creep diagnostics.

    Returns ``{"enabled": bool, "count": int, "slope_window": int,
    "slope": float, "samples": [{"cycles", "sp"}]}``. Samples are ordered
    oldest first; ``slope`` is a linear regression of SP vs cycles over
    the last ``slope_window`` samples (default 256) - negative slope
    means SP is decreasing over time (= stack growing).

    Recording is gated by the ``[STACK_HISTORY] active`` cfgmain flag.
    When disabled, ``enabled=false`` and ``samples`` is empty. The ring
    buffer holds up to 4096 samples (= DBGAPI_STACK_HISTORY_MAX).
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_stack_history")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://stack/regions")
async def resource_stack_regions() -> str:
    """List of user-defined stack regions with watermarks and counters.

    Returns ``{"count": int, "sp_now": int, "regions": [{"name", "base",
    "limit", "watermark", "push_count", "pop_count",
    "current_sp_in_region"}]}``. ``base`` is the top (highest address)
    and ``limit`` is the bottom of the region; the region matches when
    ``limit <= SP <= base``.

    Up to 8 regions can be defined at once (= DBGAPI_STACK_REGIONS_MAX).
    ``watermark`` tracks the lowest SP ever observed in the region since
    the last reset, useful for sizing stack allocations.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_stack_regions")
    return json.dumps(_data_or_error(resp))


# === V1.D.2.B medium debug Resources ==================================


@mcp.resource("emulator://watch")
async def resource_watch() -> str:
    """List of active watch rows (expression watches and address watches).

    Returns ``{"count": int, "watches": [{"index": int, "name": str|null,
    "mode": str, "type": str, "addr": int, "expr": str|null,
    "value": str}]}``. ``mode`` is ``"address"`` for literal addr+type
    watches and ``"expr_scalar"`` / ``"expr_deref"`` for expression-driven
    rows. ``type`` covers the supported integer / string flavours
    (``u8``, ``i8``, ``u16le``, ``u16be``, ``u32le``, ``ascii``,
    ``mzascii``, ``bytes`` etc.).

    ``value`` is the most recent formatted value (string form, exactly
    what the watch panel would render). ``addr`` is meaningful only for
    ``mode="address"`` rows. The list reflects the in-memory watch
    storage and is capped at 256 rows; clients needing more should use
    the ``watch_list`` Tool with explicit pagination.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_watch")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://watch/snapshot/{name}")
async def resource_watch_snapshot(name: str) -> str:
    """Per-watch snapshot statistics looked up by case-sensitive name.

    URI template: ``emulator://watch/snapshot/{name}``. Returns the
    EMU-side mirror snapshot for the watch row matching ``name``. The
    payload bundles snapshot baseline, current value, delta, observed
    min/max and the change counter as accumulated by the watch cache:

    - ``found`` (bool): true if a row with this exact name exists in
      the mirror; if false, the rest of the payload is omitted.
    - ``row_id`` (int): stable identifier from the watch storage.
    - ``type`` (str): the watch type at the time of last publish
      (``u8`` / ``i8`` / ``u16le`` / ... / ``ascii`` / ``bytes``).
    - ``snapshot_active`` (bool): true when a snapshot baseline is set
      (i.e. ``snap_int`` and ``delta_int`` are meaningful).
    - ``min_max_valid`` (bool): true only for integer-typed rows.
    - ``snap_int`` / ``cur_int`` / ``delta_int`` (int, sign-extended)
    - ``min_int`` / ``max_int`` (int): observed extremes since the
      last reset (only when ``min_max_valid``).
    - ``change_count`` (int): number of value changes recorded.

    The mirror is published by the UI thread once per frame, so the
    payload may be up to one frame stale relative to live emulation.
    Anonymous watch rows (no name) are intentionally not addressable
    via this URI - use ``emulator://watch`` for the full listing.

    Args:
        name: case-sensitive watch row name (e.g. ``"PLAYER_HP"``).
    """
    if name is None or name == "":
        return json.dumps({"error": "name must be a non-empty string"})
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_watch_snapshot", {"name": name})
    if not resp.get("success", False):
        return json.dumps({"error": resp.get("error",
                                              "get_watch_snapshot failed")})
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://stack")
async def resource_stack() -> str:
    """Hex dump of memory starting at the current Z80 stack pointer.

    Returns ``{"sp_now": int, "sp_odd": bool, "count_words": int,
    "words": [{"addr": int, "value": int}]}``. The default window covers
    32 words (= 64 bytes) starting at ``sp_now`` and growing towards
    higher addresses (= the order in which values were pushed; ``words[0]``
    is the most recent push, sitting at ``sp_now``).

    Each ``value`` is a little-endian 16-bit word read from
    ``mem[addr] | mem[addr+1] << 8``. When ``sp_odd`` is true, the SP is
    not word-aligned; clients should switch to a byte-oriented view or
    re-align before interpreting the data as call return addresses.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_stack")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://vars")
async def resource_vars() -> str:
    """List of user-defined ``$name`` variables from smart breakpoints.

    Returns ``{"count": int, "truncated": bool, "vars": [{"name": str,
    "value": int, "comment": str, "has_comment": bool,
    "persist_value": bool}]}``. The list reflects the in-memory ``bp_vars``
    storage that is updated by breakpoint action executors (``set $foo =
    expr`` and similar).

    ``value`` is a 32-bit signed integer; the default domain is plain
    counters / scalars. ``persist_value=true`` means the value survives
    save/load of the ``.bpt`` file; ``has_comment=true`` indicates a
    non-empty comment string. Capped at 256 records; if more exist,
    ``truncated=true``.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_vars")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://bookmarks")
async def resource_bookmarks() -> str:
    """List of named address bookmarks defined in the debugger.

    Returns ``{"count": int, "truncated": bool, "bookmarks": [{"id": int,
    "input": str, "comment": str, "has_comment": bool, "addr": int|null,
    "addr_resolved": bool, "owner": str}]}``. Each bookmark stores a
    ``user_input`` string (hex literal like ``0x1234`` or a symbol name);
    ``addr`` is the resolved 16-bit CPU address or ``null`` if the input
    cannot be resolved at snapshot time (= unknown symbol, malformed
    literal). ``addr_resolved`` mirrors that boolean.

    ``owner`` is the V1.C.3 cmd_origin tag of the bookmark
    (``"user"``, ``"mcp"``, ``"test"``, ``"internal"``). The snapshot is
    taken under an internal mutex, so concurrent UI mutations are safe.
    Capped at 1024 records.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_bookmarks")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://periph/i8255")
async def resource_periph_i8255() -> str:
    """Intel 8255 PPI state snapshot.

    Returns ``{"port_a": int, "port_b": int, "port_c": int,
    "control_word": int, "cw_decoded": bool, "mode_group_a": int,
    "mode_group_b": int, "pa_dir": "input"|"output", "pb_dir": ...,
    "pc_upper_dir": ..., "pc_lower_dir": ..., "signal_pc00": int,
    "signal_pc01": int, "signal_pc02": int, "signal_pc03": int,
    "signal_pc04": int, "pa_keyboard_column": int,
    "pa_joy1_enabled": bool, "pa_joy2_enabled": bool}``.

    ``control_word`` is the last byte the CPU wrote to the PPI control
    port (8255 hardware cannot read it back; this is an emulator-side
    mirror). ``cw_decoded=true`` means the mirror byte was a Mode Set
    word (bit 7 = 1), so ``mode_group_*`` and ``*_dir`` fields are
    valid. When the last write was a Bit Set/Reset operation
    (``cw_decoded=false``) those fields stay at 0 and must not be
    interpreted.

    PC signal bits expose: ``pc00`` (CTC0 audio gate; MZ-700 always 1),
    ``pc01`` (CMT data out), ``pc02`` (CTC2 IRQ enable; 0 = blocked),
    ``pc03`` (CMT motor control), ``pc04`` (CMT motor status read).
    Reading this resource does not modify the chip.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_periph_i8255")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://periph/i8253")
async def resource_periph_i8253() -> str:
    """Intel 8253 CTC (three programmable counters) state snapshot.

    Returns ``{"last_cw_byte": int, "channels": [{"index": int,
    "value": int, "preset_value": int, "preset_latch": int,
    "read_latch": int, "out": int, "gate": int, "mode": str,
    "bcd": bool, "rlf": str, "state": str, "load_done": bool,
    "latch_op": bool, "rl_byte": int}]}``.

    ``channels`` always has three entries (CTC0, CTC1, CTC2 in
    ``index`` order). ``mode`` is one of ``"mode0".."mode5"`` per the
    8253 datasheet; ``rlf`` is ``"lsb"``/``"msb"``/``"lsb_msb"``/
    ``"none"`` (Read/Load Format selecting how many bytes the data
    port transfers); ``state`` is the internal sequencer state
    (``"init"``, ``"load_done"``, ``"countdown"``, ...).

    ``last_cw_byte`` is the last Control Word the CPU wrote to the
    CTC CWREG port; 8253 hardware cannot read it back, this is an
    emulator-side mirror useful for debugging Mode Set transitions.
    Reading this resource is side-effect free.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_periph_i8253")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://periph/z80_pio")
async def resource_periph_z80_pio() -> str:
    """Zilog Z80 PIO state snapshot (MZ-800 and MZ-1500 only).

    Returns ``{"available": bool, "reason": str?, "interrupt": str,
    "interrupt_port_id": int|null, "port_a": {...}, "port_b": {...}}``
    on platforms with the chip. On MZ-700 (no Z80 PIO present) the
    payload is just ``{"available": false, "reason": "platform has
    no Z80 PIO"}``; clients must check ``available`` before reading
    the port fields.

    Each port object contains: ``data_output`` (Data Output Register
    byte), ``masked_input`` (input snapshot after Mode 3 mask),
    ``io_mask`` (per-bit direction in Mode 3: 1=in, 0=out),
    ``mode`` (``"output"``/``"input"``/``"bidir"``/``"user"`` per
    en_PIOZ80_PORT_MODE), ``int_vec`` (IM2 vector base),
    ``icmask`` (interrupt control mask byte), ``int_enable``
    (boolean from ICW ENA bit), ``icfnc`` (0=OR, 1=AND),
    ``iclvl`` (0=LOW, 1=HIGH active), ``port_int``
    (``"none"``/``"pending"``/``"received"``/``"repending"``),
    ``last_ctrl_byte`` (sequencer mirror of the last CPU write to
    the control port).

    Top-level ``interrupt`` summarises the aggregate state
    (``"none"``/``"pending"``/``"received"``/``"nextprio_
    unemulated"``); ``interrupt_port_id`` points at the port that
    currently owns the pending interrupt (0 = Port A, 1 = Port B),
    or ``null`` when no port has pending IRQ. Reading is side-effect
    free.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_periph_z80_pio")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://periph/sn76489")
async def resource_periph_sn76489() -> str:
    """SN76489 PSG (programmable sound generator) state snapshot.

    Returns ``{"available": bool, "reason": str?, "psg_count": int,
    "stereo": bool, "psg0": {...}, "psg1": {...}?}`` on platforms with
    the chip. On MZ-700 (``HAVE_PSG=0`` at compile time) the payload is
    just ``{"available": false, "reason": "platform has no PSG"}``;
    clients must check ``available`` before reading further fields.

    ``psg_count`` is 1 (mono) or 2 (stereo). MZ-800 boots mono and may
    switch to stereo at runtime when the optional right PSG is enabled
    (``g_psg_module.stereo``); MZ-1500 is natively stereo. ``stereo``
    mirrors the boolean module flag for convenience.

    Each ``psg0`` / ``psg1`` object contains ``latch_cs`` (current
    channel select bits 6-5 of the last LATCH byte, 0..3),
    ``latch_attn`` (true = the next DATA byte updates attenuation,
    false = it updates the tone/noise divider) and ``channels`` array
    (four entries: ``index`` 0..2 are TONE generators, index 3 is the
    NOISE generator).

    Per-channel fields:

      - ``type``: ``"tone"`` or ``"noise"``
      - ``attenuation``: 0..15 (0 = max volume, 15 = silent)
      - ``tone_divider``: 10-bit period divider (meaningful only when
        ``type=="tone"``)
      - ``noise_div_type``: ``"div_16"`` / ``"div_32"`` / ``"div_64"``
        / ``"tone2_controlled"`` (meaningful only when
        ``type=="noise"``)
      - ``noise_type``: ``"periodic"`` or ``"white"`` (meaningful only
        when ``type=="noise"``)

    The PSG is write-only HW; this snapshot uses side-effect free
    ``psg_mirror_*`` accessors and is safe to poll from the MCP client.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_periph_sn76489")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://periph/ay3_8910")
async def resource_periph_ay3_8910() -> str:
    """AY-3-8910 PSG state snapshot (placeholder).

    The AY-3-8910 chip is **not implemented** in the current emulator
    build. This resource is provided as a forward-compatibility stub:
    clients querying the URI receive ``{"available": false, "reason":
    "AY-3-8910 not implemented in this emulator"}`` on every platform
    (MZ-700, MZ-800, MZ-1500). When the chip gets added in a future
    build, the schema will be extended with register fields; the
    ``available`` flag will switch to true and clients with proper
    feature detection will start receiving real state without a wire
    protocol break.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_periph_ay3_8910")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://periph/gdg")
async def resource_periph_gdg() -> str:
    """GDG custom video LSI state snapshot - per-platform palette layout.

    The GDG is the custom display LSI used in Sharp MZ-700, MZ-800 and
    MZ-1500. Each platform has its own ``st_GDG`` struct with shared
    fields (``regDMD``, raster state, ``regct53g7``, ``beam_row``) but
    different palette systems:

      - MZ-800: 16-color palette through ``regPALGRP`` + ``regPAL0..3``,
        plus ``regBOR`` (border color) and ``cksw`` (superimpose bit).
        ``palette_count = 16``, ``has_border_reg = true``,
        ``has_pal_group = true``, ``has_cksw = true``.
      - MZ-700: 8-entry palette via ``mode700_color[8]``, no
        ``regBOR``/``regPALGRP``/``cksw``. ``palette_count = 8``.
      - MZ-1500: 8-entry palette via ``mode1500_color[8]``, no
        ``regBOR``/``regPALGRP``/``cksw``. ``palette_count = 8``.

    Returns ``{"available": true, "platform": "mz800"|"mz700"|"mz1500",
    "palette_count": int, "palette": [int*n], "has_border_reg": bool,
    "has_pal_group": bool, "has_cksw": bool, "regDMD": int,
    "regBOR": int, "regPALGRP": int, "regct53g7": int, "beam_row": int,
    "total_screens": int, "total_ticks": int, "sts_vsync": int,
    "sts_hsync": int, "hbln": int, "vbln": int, "cksw": int,
    "tempo": int, "tempo_divider": int}``.

    For MZ-800 the four ``palette[0..3]`` entries are the raw
    ``regPAL0..3`` bytes; entries 4..15 are zero placeholders and the
    client should reconstruct the full 16-color table by combining
    ``regPAL0..3`` with ``regPALGRP`` (see hw/09-video-mz800-modes.md).
    For MZ-700/MZ-1500 entries 0..7 are the ``mode_color[]`` values
    directly.

    MZ-800 only (the keys are absent on MZ-700/MZ-1500):

      - ``vramctrl``: VRAM access registers of the 800 graphics modes,
        ``{"wf_mode": "single"|"exor"|"or"|"reset"|"replace"|"pset",
        "wf_mode_code": int, "wf_planes": int, "rf_planes": int,
        "rf_search": bool, "vbank": 0|1}``. Plane masks: bit 0 = plane I
        .. bit 3 = plane IV. WF is port 0CCh, RF port 0CDh; both are
        write-only on the real machine, these are mirrors.
      - ``hwscroll``: hardware scroll registers in the units the program
        writes to the ports, ``{"enabled": bool, "sof": 0..3FFh (01CFh +
        02CFh), "ssa": 0..7Fh (04CFh), "sea": 0..7Fh (05CFh), "sw": 0..7Fh
        (03CFh)}``. ``enabled`` is true when the register values activate
        the scroll.

    The handler is side-effect free; no GDG register read/write occurs
    during snapshot. Reading ``beam_row`` returns the instantaneous
    raster line, useful for raster-effect debugging.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_periph_gdg")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://periph/wd1793")
async def resource_periph_wd1793() -> str:
    """WD279x (WD1793) floppy disk controller state snapshot.

    Returns chip register snapshot plus per-drive mount metadata.
    Available on platforms with ``CFG_HWEXT_HAVE_FDC`` (default for all
    three platforms) and only when the chip is runtime connected; in
    other cases the payload is ``{"available": false, "reason": "FDC
    not compiled or detached"}``.

    The Sharp implementation inverts bus data and the SIDE signal
    relative to the WD279x datasheet (see hw/16-floppy.md). The
    ``bus_xlate_invert`` field surfaces whether the runtime translation
    layer is active (Sharp default = true).

    Returns ``{"available": true, "bus_xlate_invert": bool,
    "hd_patch": bool, "reg_status": int, "reg_command": int,
    "reg_track": int, "reg_sector": int, "reg_data": int, "motor": int,
    "side": int, "density": int, "multiblock_rw": int,
    "direction_latch": int, "intrq_active": int,
    "positioned_track": int, "positioned_sector": int,
    "positioned_side": int, "status_mode": int, "buffer_pos": int,
    "data_counter": int, "current_sector_size": int,
    "drives": [{...} x4]}``.

    Each ``drives[]`` entry has ``{"index": 0..3, "present": bool,
    "readonly": bool, "user_readonly": bool, "fs_readonly": bool,
    "storage_mode": 0|1|2, "tracks": int, "sides": int,
    "total_data_bytes": int, "image_basename": str}``. The
    ``image_basename`` is the filename portion only (no directory
    component) per the V1.D.1 security convention; clients that need
    the full host path must use a separate file-access mechanism.

    ``storage_mode`` values: 0=CACHED, 1=DIRECT, 2=DISCARD (see
    fdc.h::en_FDC_STORAGE_MODE). ``readonly`` is the effective state
    used by the chip; ``user_readonly`` and ``fs_readonly`` are the two
    independent inputs.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_periph_wd1793")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://periph/cmt")
async def resource_periph_cmt() -> str:
    """CMT (Cassette Tape) module state snapshot.

    The cassette interface is a standard Sharp MZ peripheral and is
    available on all three platforms (``available`` is always true).
    Returns ``{"available": true, "state": "stop"|"play"|"record",
    "paused": bool, "filled": bool, "polarity_inverted": bool,
    "cmtspeed": int, "cpu_boost": bool, "mzfsize_check": bool,
    "output": int, "playsts": int, "start_time": int,
    "paused_time": int, "image_basename": str,
    "default_pulses_us": [4 floats], "custom_pulses_us": [4 floats]}``.

    ``cmtspeed`` is the default tape speed (en_CMTSPEED, see
    ``emu_cmt_set_speed``; 10 = custom pulse lengths).
    ``default_pulses_us`` are the pulse lengths of the default speed in
    microseconds (long high, long low, short high, short low);
    ``custom_pulses_us`` are the stored custom lengths used when
    ``cmtspeed`` is 10.

    ``state`` and ``paused`` are orthogonal - PLAY + paused=true means
    the playback is suspended. ``filled`` is true when an MZF image is
    loaded (``g_cmt.ext != NULL``). ``output`` is the current outgoing
    bit on the PIO line.

    ``polarity_inverted`` reflects the rear DIP switch (inverted = some
    real machines have flipped wiring; affects bit polarity when
    decoding). ``cpu_boost`` is the runtime acceleration flag for
    long tape loads.

    ``image_basename`` is the filename only (no directory component)
    per V1.D.1 security; for VIRTUAL / no-image states the field is an
    empty string.

    ``start_time`` and ``paused_time`` are GDG total ticks captured at
    the corresponding state transitions; clients computing playback
    time should use the same ``GDGCLK_BASE`` reference.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_periph_cmt")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://periph/cmt/tape")
async def resource_periph_cmt_tape() -> str:
    """CMT tape block listing for the currently loaded tape.

    Lists the blocks of the loaded tape image. SIMPLE_TAPE containers
    (multi-file tapes) expose one entry per block; SINGLE containers
    (e.g. a plain .mzf) expose one block. Returns ``{"available": true,
    "container_type": 0|1, "current_block": int, "count": int,
    "truncated": bool, "blocks": [...]}`` where ``container_type`` is
    0=SINGLE, 1=SIMPLE_TAPE and ``current_block`` is the index of the
    block being played (-1 if none).

    Each block: ``{"block_id": int, "name": str, "cmt_speed": int,
    "type": int, "is_current": bool, "playable": bool,
    "recordable": bool, "block_speed": "none"|"default"|"set",
    "pulses_us": [4 floats]}``. ``block_speed`` "default" follows the
    default tape speed, "set" has its own ``cmt_speed`` (the en_CMTSPEED
    value 1..9, or 10 = custom pulse lengths, see ``emu_cmt_set_speed``).
    ``pulses_us`` (MZ blocks only) are the effective pulse lengths in
    microseconds the block plays with (long high, long low, short high,
    short low). Blocks that follow a UniCMT CMTSPEED header in an .mzt
    have "set" speed with the header's pulse lengths; the header itself
    is not listed. ``type`` is the block type (0=WAV, 1=MZF,
    2=TAPHEADER, 3=TAPDATA). ``playable`` / ``recordable`` are per-tape
    flags repeated on each block for convenience.

    When no tape is loaded (or the container is missing) the payload is
    ``{"available": false, "blocks": []}``. Seek with
    ``emu_cmt_tape_seek`` and override per-block speed with
    ``emu_cmt_tape_set_block_speed``. See ``emulator://docs/cmt_workflow``
    for the full tape workflow.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("cmt_tape_list")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://periph/qd")
async def resource_periph_qd() -> str:
    """Quick Disk (MZ-1F11) module state snapshot.

    Quick Disk is an optional drive available on all three platforms
    when compiled in (``CFG_HWEXT_HAVE_QDISK=1`` by default). Available
    only when the module is runtime connected; otherwise the payload is
    ``{"available": false, "reason": "QDisk not compiled or detached"}``.

    Returns ``{"available": true, "type": "image"|"virtual"|"unicard",
    "status": int, "readonly": bool, "user_readonly": bool,
    "fs_readonly": bool, "storage_mode": 0|1|2, "vrtsts": int,
    "image_position": int, "virt_files_count": int,
    "virt_file_num": int, "virt_mzfbody_size": int,
    "out_crc16": int, "image_basename": str}``.

    ``type`` selects the active QD backend: IMAGE = mounted .mzq image,
    VIRTUAL = on-the-fly synthesized from MZF files, UNICARD = SD card
    backed.

    ``status`` is the QDSIO status byte (``QDSTS_*`` bitfield;
    1=IMG_READY, 2=HEAD_HOME, 4=IMG_SYNC, 8=IMG_READONLY). ``vrtsts``
    is the VIRTUAL-mode state machine position (``st_QDISK_VRTSTS``
    enum: 0=QDHEADER, 1=MZFHEAD, 2=MZFBODY, 3=FREE_FILEAREA,
    4=WR_MZFHEAD, 5=WR_MZFBODY, 6=FORMATING).

    ``image_basename`` is the filename only (no directory component)
    per V1.D.1; for VIRTUAL mode the field is an empty string.
    ``storage_mode`` values match the FDC convention (0=CACHED,
    1=DIRECT, 2=DISCARD).
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_periph_qd")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://input/keyboard/state")
async def resource_input_keyboard_state() -> str:
    """Keyboard matrix snapshot (real + virtual + effective).

    Returns ``{"real_matrix": [int x 10], "virtual_matrix": [int x 10],
    "effective": [int x 10], "pressed_count": int,
    "pressed_truncated": bool, "pressed_keys": [{"col": int, "bit": int,
    "name": str}]}``.

    The Sharp MZ keyboard is wired as a 10-column x 8-row matrix (active-
    low: a pressed key clears the corresponding bit). The emulator
    maintains two parallel matrices:

      - ``real_matrix`` - the hardware-scanned state from the host
        keyboard (``g_pio8255.keyboard_matrix``).
      - ``virtual_matrix`` - the VKBD injection state (``g_pio8255.
        vkbd_matrix``) maintained by MCP HID Tools / autotype.

    ``effective`` is the bitwise AND of both (= what the Z80 sees at
    port read). ``pressed_keys`` decodes the active bit positions into
    symbolic names (``"SHIFT"``, ``"RETURN"``, ...) using the same
    table as ``input/keyboard/matrix_info``. Names are uppercase; if
    a position has no symbolic name, ``name`` is an empty string.

    ``pressed_truncated`` is set to true if more than 32 keys were
    simultaneously held (= ``pressed_keys`` array clipped).

    Reading is side-effect free. The matrix layout is identical across
    MZ-700/MZ-800/MZ-1500 (``iface_keyboard.c`` shares the table).
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_input_keyboard_state")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://input/keyboard/matrix_info")
async def resource_input_keyboard_matrix_info() -> str:
    """Static keyboard matrix mapping table.

    Returns ``{"platform": "mz700"|"mz800"|"mz1500", "key_count": int,
    "keys": [{"name": str, "col": int, "bit": int, "needs_shift":
    bool}]}``.

    The mapping describes every symbolic key name supported by the
    emulator's VKBD layer (``RETURN``, ``ENTER``, ``SPACE``, ``ARROW_UP``,
    ``F1``..``F9``, ``SHIFT``, ``CONTROL``, ``GRAPH``, ``BREAK``, ...).
    Some keys have multiple aliases (RETURN/ENTER/CR) - they appear as
    separate entries pointing to the same (col, bit). ``needs_shift``
    is true for keys that require an implicit SHIFT modifier (e.g.
    ``LIBRA``).

    The table is currently shared across MZ-700/MZ-800/MZ-1500 (per
    ``hid_keymap.c``). The ``platform`` field reflects the build target
    so clients can label the data; if a future revision diverges, the
    backend will return per-platform contents under the same schema.

    The Resource value is static at runtime - clients typically read
    once and cache it.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_input_keyboard_matrix_info")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://input/joystick/state")
async def resource_input_joystick_state() -> str:
    """Per-port joystick state snapshot.

    Returns ``{"ports": [{"index": 0|1, "connected": bool,
    "state_bits": int, "native_state": int, "device_name": str}]}``.

    Two ports (index 0 and 1) are reported. ``connected`` is true when
    the runtime ``g_joy.dev[port].type`` is not ``JOY_TYPE_NONE``.
    Joystick hardware is standard on MZ-800 and MZ-1500; on MZ-700
    builds and on platforms compiled without ``HAVE_JOY`` both ports
    return ``connected=false``.

    ``state_bits`` is an active-HIGH 8-bit mask consistent with the
    HID Tools input API:

      - bit 0: UP
      - bit 1: DOWN
      - bit 2: LEFT
      - bit 3: RIGHT
      - bit 4: FIRE1
      - bit 5: FIRE2
      - bits 6, 7: reserved (= read back as 0)

    ``native_state`` is the raw active-LOW chip byte (``g_joy.dev[].
    state``) - useful for cross-checking the bit decode. ``device_name``
    is one of ``"none"`` / ``"num_keypad"`` / ``"joystick"`` (mapping
    of ``en_JOY_TYPE``).

    Note: the emulator does not currently expose analog axis values or
    deadzone configuration; the digital state above is the full
    runtime model.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_input_joystick_state")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://frame/framebuffer/info")
async def resource_frame_framebuffer_info() -> str:
    """Active framebuffer shape and palette metadata.

    Returns ``{"width": int, "height": int, "last_screen_id": int,
    "framebuffer_state": int, "dirty": bool, "pixel_format": "index8",
    "bytes_per_pixel": 1, "has_palette": true, "palette": [int x N],
    "palette_size": int}``.

    The Sharp MZ framebuffer is INDEX8 - each pixel byte is a palette
    index into a 16-entry color map (``DISPLAY_MZCOLORS``). The palette
    values are 24-bit RGB packed as ``0x00RRGGBB``. Clients that want
    decoded RGBA pixels should read ``emulator://frame/screenshot.raw``
    instead, which performs the INDEX8 -> RGBA expand server-side.

    ``last_screen_id`` is a monotonically increasing frame counter
    (``fbsnapshot_screen_id``) updated each time the emulator finishes
    a screen. ``framebuffer_state`` is an ``en_FBSTATE`` bitmask
    (1=SCREEN_CHANGED, 2=BORDER_CHANGED, 0=NOT_CHANGED). ``dirty`` is
    a convenience flag (``state != NOT_CHANGED``).

    Width / height are compile-time constants per ``MZARCH``; MZ-800
    builds report 928 x 288 (left/right borders 154/134, canvas 640 x
    200, top/bottom borders 46/42).
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_frame_framebuffer_info")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://frame/screenshot.raw")
async def resource_frame_screenshot_raw() -> str:
    """Raw RGBA pixel buffer of the latest framebuffer (base64).

    Returns ``{"available": true, "width": int, "height": int,
    "bytes_per_pixel": 4, "pixel_format": "rgba8888",
    "downscale_factor": 1|2|4, "source_screen_id": int, "data_b64":
    str}`` or ``{"available": false, "reason": str}``.

    The backend reads the INDEX8 framebuffer snapshot under the
    ``fbsnapshot_pixels_mutex`` and expands each pixel via the current
    palette to RGBA8888. For MZ-800 at native 928x288 the payload is
    roughly 1.0 MB raw / 1.4 MB base64. If the per-message MCP wire
    budget is exceeded by a future caller, the backend may auto-bump
    ``downscale_factor`` to 2 or 4 (= every Nth pixel in each axis)
    and reports the value actually used.

    ``source_screen_id`` is the frame counter at the moment of the
    snapshot - lets the client deduplicate identical screenshots. The
    reply also carries ``image_sha256``, ``vram_sha256``, ``beam`` and
    ``paused`` (see ``emu_frame_hash``).

    ``available=false`` is returned only when the emulator has not yet
    emitted a single frame (``fbsnapshot_pixels`` is NULL).
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_frame_screenshot_raw")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://frame/screenshot")
async def resource_frame_screenshot() -> str:
    """Framebuffer snapshot as PNG (base64).

    Returns ``{"available": true, "format": "png", "width": int,
    "height": int, "byte_size": int, "area": "full",
    "scale": "framebuffer", "data_b64": str}`` where
    ``data_b64`` is the base64-encoded PNG file stream. The image is the
    full framebuffer (= same content as ``emulator://frame/screenshot.raw``
    at downscale 1, just a PNG container instead of raw RGBA bytes).
    For a cut-out or another scale use ``emu_screenshot_save_to_file``
    (the underlying JSONL command accepts the same area/scale/crop).

    Returns ``{"available": false, "reason": str}`` only when the
    framebuffer has not yet been rendered, the display is not initialized,
    or the encoder failed.

    The PNG is encoded by ``stb_image_write.h`` (public domain, vendored)
    on the emulator thread - no runtime DLL dependency. For raw RGBA
    pixels (no encoder) use ``emulator://frame/screenshot.raw``.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_frame_screenshot")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_frame_hash() -> str:
    """SHA-256 of the screen picture and of the video memory, without the data.

    ``image_sha256`` hashes the picture from the same source as a
    screenshot: the palette index of every pixel (row by row), then the
    16 palette colours as R, G, B. ``vram_sha256`` hashes the video
    memory (MZ-800: all four planes; MZ-700 / MZ-1500: character and
    attribute VRAM). The picture also depends on GDG registers (palette,
    mode, border), the VRAM hash does not - compare both when needed.

    Paused in the middle of a frame, part of the picture is still from
    the previous frame and the hash depends on the beam position
    (``beam``: ``frame``, ``scanline``, ``column``); compare pictures at a
    frame boundary (after ``emu_run(frames)`` or ``deadline_frames``).
    The screenshot resources return the same ``image_sha256``.

    Returns:
        JSON ``{"available": <bool>, "image_sha256": str, "vram_sha256":
        str, "source_screen_id": int, "image_source": "sdl_snapshot" |
        "gdg_live", "width": int, "height": int, "beam": {...},
        "paused": <bool>}``.
    """
    resp = await _send_request("frame_hash")
    return json.dumps(_data_or_error(resp))


# ---------------------------------------------------------------------------
# Replay: record inputs once, play them back exactly (replay-core)
# ---------------------------------------------------------------------------

#: Seconds the wrapper waits for a replay request (the emulator waits up
#: to 10 s for the emulation thread, a start also saves / loads a snapshot).
_REPLAY_TIMEOUT_S = 20.0


@mcp.tool()
async def emu_replay_record_start(path: str, hash_interval: int = 50,
                                  description: Optional[str] = None,
                                  checkpoint_interval: Optional[int] = None,
                                  checkpoint_count: Optional[int] = None) -> str:
    """Start recording the emulator inputs into a replay file (``.mzr``).

    The recording starts from a complete snapshot of the machine (always
    with the RAM disk) plus the contents of the mounted disks and tape
    (up to 64 MB together) and stores every change of the keyboard matrix
    and joystick state at a frame boundary (host joystick read once per
    frame), resets, autotype, tape control (``emu_cmt_play`` / ``_stop`` /
    ``_pause`` / ``_eject``, ``emu_cmt_tape_seek``, ``emu_cmt_record``)
    and debugger writes (``emu_mem_write``, ``emu_set_register``,
    ``emu_io_write``, region writes, IRQ / NMI injection), plus SHA-256
    hashes of the CPU, RAM, MemExt, VRAM, PCG and peripherals every
    ``hash_interval`` frames. Play it back with ``emu_replay_play``: the
    run repeats exactly, also in a new process without the media files
    and at any speed, so you can repeat a bug with other breakpoints,
    trace or ramlog.

    While recording:

    * keyboard input from the GUI is taken at the next frame boundary,
    * a write or input change while paused **between** frame boundaries
      (after a step or a breakpoint) is refused - pause at a boundary
      with ``emu_run(frames=N)`` or a ``deadline`` first; while running,
      writes are taken at the frame boundary automatically,
    * tape control is recorded like a write (refused between boundaries),
    * loading a snapshot, media, inserting a tape, tape speed / polarity,
      the CMT hack, settings and Freeze Bytes are refused (they cannot be
      repeated).

    When the emulation runs, it pauses at the next frame boundary, the
    recording starts and the emulation runs on; when paused it stays
    paused.

    Every ``checkpoint_interval`` frames a checkpoint (a snapshot in
    memory) is kept, the last ``checkpoint_count`` of them; go back to one
    with ``emu_replay_rewind`` and record a different path from there.

    Args:
        path: Replay file on the emulator host (overwritten on stop).
        hash_interval: Frames between state hashes (1..100000).
        description: Optional text stored in the manifest.
        checkpoint_interval: Frames between checkpoints (0..100000, 0 = no
            checkpoints); default from the ``[REPLAY]`` INI section (250).
        checkpoint_count: Checkpoints kept (1..1000); default from the INI
            (60).

    Returns:
        JSON with the replay status (see ``emu_replay_status``), or an
        error (``recording or playback is already running``, ``autotype
        is running``, ``Freeze Bytes are active``, ``the tape is
        recording``, ...).
    """
    data: dict[str, Any] = {"path": path, "hash_interval": hash_interval}
    if description is not None:
        data["description"] = description
    if checkpoint_interval is not None:
        data["checkpoint_interval"] = checkpoint_interval
    if checkpoint_count is not None:
        data["checkpoint_count"] = checkpoint_count
    resp = await _send_request("replay_record_start", data, timeout_s=_REPLAY_TIMEOUT_S)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_replay_record_stop() -> str:
    """Stop recording and save the replay file.

    Stopped at a frame boundary (running, or paused after
    ``emu_run(frames=N)``), the replay ends with a final state hash.
    Stopped in a pause between boundaries, it ends at the last boundary.

    Returns:
        JSON status with ``outcome: "recorded"`` and ``outcome_message``
        (frames, events, file), or an error (``no recording is
        running``, a write error).
    """
    resp = await _send_request("replay_record_stop", timeout_s=_REPLAY_TIMEOUT_S)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_replay_play(path: str, pause_after_load: bool = False,
                          checkpoint_interval: Optional[int] = None,
                          checkpoint_count: Optional[int] = None) -> str:
    """Play a replay file back from its start snapshot.

    Live input (GUI keyboard, virtual keyboard, MCP keys) is ignored
    during playback. Every recorded hash is compared; the first frame
    where the state differs stops the playback (pause reason
    ``replay_desync``, ``outcome: "desync"``, ``desync_parts`` such as
    ``cpu``, ``ram``, ``vram``, ``periph``, ``time``). The stored disks
    and tape are mounted from copies (original files and INI unchanged,
    the INI media return after the end). At the end the emulation
    pauses with reason ``replay_end`` (``outcome: "ended"``); wait for it
    with ``emu_wait_stop`` or poll ``emu_replay_status``.

    Breakpoints, trace and ramlog may stay on. A write to the machine
    during playback (``emu_mem_write``, a breakpoint ``poke`` / ``set``
    action, Freeze Bytes, reset, loading a snapshot, tape control) marks
    the playback as ``deviated``: it goes on, but hashes are no longer
    checked.

    Jump to any frame with ``emu_replay_seek`` (back via the checkpoints
    kept during this playback).

    Args:
        path: Replay file on the emulator host.
        pause_after_load: True = stay paused after loading the start
            snapshot (set breakpoints, then ``emu_resume``); False = run.
        checkpoint_interval: Frames between checkpoints of this playback
            (0..100000, 0 = none: every seek back starts from the
            beginning); default from the INI (250).
        checkpoint_count: Checkpoints kept (1..1000); default from the INI
            (60).

    Returns:
        JSON status (``mode: "playing"``, ``frames``), or an error (file
        not readable, other platform or TV system, replay with debugger
        events in a build without the debugger, a joystick port connected
        only in one of the two, the CMT hack set differently, an image not
        stored in the replay that differs from the mounted one).
    """
    data: dict[str, Any] = {"path": path, "pause_after_load": pause_after_load}
    if checkpoint_interval is not None:
        data["checkpoint_interval"] = checkpoint_interval
    if checkpoint_count is not None:
        data["checkpoint_count"] = checkpoint_count
    resp = await _send_request("replay_play", data, timeout_s=_REPLAY_TIMEOUT_S)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_replay_play_stop() -> str:
    """Stop a running playback (``outcome: "stopped"``); live input works again.

    Returns:
        JSON status, or an error (``no playback is running``).
    """
    resp = await _send_request("replay_play_stop", timeout_s=_REPLAY_TIMEOUT_S)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_replay_rewind(frames: int = 0) -> str:
    """Go back to a checkpoint of the running recording or playback.

    While recording: the machine returns to the checkpoint, the inputs and
    hashes after it are dropped, ``rerecord_count`` grows by one and the
    recording goes on from there (the emulation stays paused at the
    checkpoint frame boundary; ``emu_resume`` to continue). While playing:
    the same as ``emu_replay_seek`` to that checkpoint.

    A checkpoint after which a disk (FDC, Quick Disk, IDE8) or the tape was
    written or the tape ejected cannot be used: checkpoints do not hold
    media contents. While recording the rewind is then refused; while
    playing the seek starts the playback again from the beginning.

    Args:
        frames: 0 = one checkpoint back (the newest one before the current
            frame; call again to go further); N = the newest checkpoint at
            least N frames back.

    Returns:
        JSON status (``frame`` = checkpoint frame, ``rerecord_count``,
        ``checkpoints``), or an error (``checkpoints are off``, ``no
        checkpoint at or before frame N``, ``a disk or the tape was written
        or ejected after frame N``, ``nothing is recording or playing``).
    """
    resp = await _send_request("replay_rewind", {"frames": frames}, timeout_s=_REPLAY_TIMEOUT_S)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_replay_seek(frame: int) -> str:
    """Jump the running playback to a frame.

    Forward: the playback runs on at MAX SPEED and stops at the frame
    boundary of ``frame`` (pause reason ``replay_seek``; wait with
    ``emu_wait_stop``). Back: the newest usable checkpoint at or before
    ``frame`` is loaded (otherwise the playback starts again from the
    beginning with clean copies of the media) and the rest is played at
    MAX SPEED. Hashes are checked all the way; breakpoints stay active and
    may stop the seek early. The speed before the seek returns at the end.
    A frame past the end of the replay is limited to the end.

    Args:
        frame: Target frame counted from the start of the replay.

    Returns:
        JSON status right after the start: ``seeking: true`` = running to
        ``seek_target``, ``false`` = already standing at the frame; or an
        error (``seek works only during playback``).
    """
    resp = await _send_request("replay_seek", {"frame": frame}, timeout_s=_REPLAY_TIMEOUT_S)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_replay_goto(t: int | None = None,
                          frame: int | None = None,
                          frame_ticks: int | None = None,
                          trace: dict | None = None,
                          observe: dict | None = None,
                          breakpoints: str = "suppress",
                          checkpoint: bool = False,
                          timeout_ms: int = 20000) -> str:
    """Machine state at any moment of the playing replay (time travel).

    The playback jumps to the nearest usable checkpoint (or starts again
    from the beginning), plays on at MAX SPEED and the emulator stops
    itself at the start of the first instruction whose time is >= the
    target. Then the tool reads the state like ``emu_state_observe``.
    A target ahead in the current frame is reached without a seek. Works
    only while a replay is playing (``emu_replay_play``).

    Give exactly one target:

    - ``t``: time in GDG ticks (the ``t`` column of ``emu_trace_read``,
      ``time.ticks`` of ``emu_state_observe``, ``stop_ticks`` of stop
      records, ``Cycle`` in breakpoint conditions);
    - ``frame`` + ``frame_ticks``: absolute frame and tick in the frame as
      ``time.frame`` / ``time.frame_ticks`` of ``emu_state_observe``;
    - ``trace``: ``{"seq": N, "at": "before" | "after", "channel":
      "cputrack"}`` - a record of the current cputrack segment (without a
      PC filter, which removes the time): the state before (default) or
      after that record. Record a trace of any part of the replay during
      playback; the playback produces the same trace as the original run.

    The stop is always at an instruction start. An interrupt acknowledge
    is not a stop point: a target right before it returns the state after
    the acknowledge (``after_int_ack: true`` for ``trace``). With a
    ``trace`` target on a register trace (``regs: true``) the answer has
    ``trace_check``: registers from the trace compared with the reached
    state (``match``, ``mismatch`` ``{reg: [expected, got]}``).

    Args:
        t: Target time in GDG ticks.
        frame: Absolute frame number (with ``frame_ticks``).
        frame_ticks: GDG ticks from the start of ``frame``.
        trace: Target record ``{"seq", "at", "channel"}``.
        observe: ``emu_state_observe`` parameters for the reached state
            (``windows``, ``want_data``, ``include``, ``hashes``); default
            registers and time only.
        breakpoints: ``"suppress"`` (default) = user breakpoints neither
            stop the seek nor run their actions (no ``bplog`` lines);
            ``"keep"`` = they stay active and may stop it early
            (``stopped_by: "breakpoint"``).
        checkpoint: Save a checkpoint at the frame boundary where the
            approach starts, so that further jumps nearby are fast
            (``checkpoint_saved``); newer checkpoints stay.
        timeout_ms: How long to wait for the stop (0..25000, default
            20000). ``0`` or a timeout returns ``done: false`` with
            ``run_seq``; then ``emu_wait_stop`` and ``emu_state_observe``.

    Returns:
        JSON ``{"done": bool, "reached": bool, "stopped_by": str,
        "target_t": int, "t": int, "exact": bool, "after_int_ack": bool,
        "seek_from": "checkpoint" | "restart" | "none", "seek_from_frame":
        int, "checkpoint_saved": bool, "replay_frame": int, "run_seq": int,
        "observe": {...}, "trace_check": {...}}`` or an error (no playback,
        target before the start or after the end of the replay, trace
        record not in the current segment). ``exact`` = an instruction
        started exactly at the target. A running trace keeps recording
        during the jump and starts a new chunk at the time jump.
    """
    data: dict[str, Any] = {"breakpoints": breakpoints, "checkpoint": bool(checkpoint),
                            "timeout_ms": timeout_ms}
    if t is not None:
        data["t"] = t
    if frame is not None:
        data["frame"] = frame
        data["frame_ticks"] = 0 if frame_ticks is None else frame_ticks
    if trace is not None:
        data["trace"] = trace
    if observe is not None:
        data["observe"] = observe
    resp = await _send_request("replay_goto", data,
                               timeout_s=_REPLAY_TIMEOUT_S + max(0, timeout_ms) / 1000.0)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_machine_hash(parts: list[str] | None = None,
                           include_media: bool = False) -> str:
    """Content fingerprint of the whole machine, part by part (SHA-256).

    Answers "is the machine in the same state as before?" - compare two
    fingerprints taken in pause (``stable: true``) with the same
    parameters. Unlike ``state_rev`` it sees only content: a change undone
    gives the same fingerprint (ABA), so for conditional writes use
    ``expected_state_rev``. Details: ``emulator://docs/state_revision``.

    Parts: ``cpu`` (registers and hidden Z80 state), ``ram``, ``memext``
    (MemExt RAM, only when connected), ``vram``, ``pcg`` (MZ-1500),
    ``periph`` (chips and controllers), ``memmap`` (memory mapping, mode
    switch, MemExt map and configuration), ``rom`` (sees the CMT hack
    patch), ``media`` (inserted media identity and the media write
    counter), ``inputs`` (effective keyboard matrix, joysticks, running
    autotype), ``ramdisk`` (RAM disk state), ``cmt`` (tape transport,
    position, settings).

    Args:
        parts: Part names to compute; default all.
        include_media: Also hash the content of disk, Quick Disk, IDE8 and
            tape images, MemExt FLASH and RAM disks (parts ``media`` and
            ``ramdisk``). Can be slow with big IDE8 images.

    Returns:
        JSON ``{"sha256": str, "parts": {name: hex | null}, "covered":
        [names], "not_covered": [names], "include_media": bool,
        "state_rev": int, "run_seq": int, "running": bool, "stable": bool,
        "elapsed_us": int}``. ``null`` = the part does not exist in this
        build or configuration. ``not_covered`` lists what the fingerprint
        never sees (screen, audio, plotter, ...).
    """
    data: dict[str, Any] = {"include_media": bool(include_media)}
    if parts is not None:
        data["parts"] = list(parts)
    resp = await _send_request("machine_hash", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_replay_status() -> str:
    """Status of recording and playback.

    Returns:
        JSON ``{"mode": "off" | "recording" | "playing", "frame": int,
        "frames": int (replay length while playing), "events": int,
        "hashes": int, "hash_interval": int, "warnings": int,
        "deviated": bool, "deviation": str, "path": str, "outcome":
        "none" | "recorded" | "ended" | "desync" | "stopped" | "error",
        "outcome_frame": int, "outcome_message": str, "desync_parts":
        [str], "rerecord_count": int, "seeking": bool, "seek_target": int,
        "goto_active": bool, "goto_target_t": int (last emu_replay_goto),
        "checkpoints": {"interval": int, "capacity": int, "count": int,
        "first_frame": int, "last_frame": int, "bytes": int}}``.
        ``outcome*`` describe the last finished recording or playback;
        ``first_frame`` / ``last_frame`` are present only with ``count`` > 0.
    """
    resp = await _send_request("replay_status")
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_screenshot_save_to_file(path: str, format: str = "png",
                                      area: str = "full",
                                      scale: str = "framebuffer",
                                      crop: Optional[dict] = None) -> str:
    """Render the current frame as PNG and save it directly to a file on the server host.

    This is the robust, self-service path for visual verification. Unlike the
    ``emulator://frame/screenshot`` Resource (which returns base64 PNG into the
    context), this writes the PNG straight to disk server-side, so it avoids
    the large transport payload entirely. The AI client then reads the written
    file from disk.

    The framebuffer has one pixel per GDG clock horizontally and one per
    picture line vertically; the picture area (canvas) is 640 framebuffer
    pixels wide, so in the 320-dot modes one dot is two framebuffer pixels.

    Args:
        path: destination file path on the server host. An absolute path is
            recommended; relative paths resolve against the server process
            working directory.
        format: image format, currently only ``png`` (default). Any other
            value is rejected with an error.
        area: ``full`` (default) - the whole frame including the border;
            ``canvas`` - the picture only, without the border.
        scale: ``framebuffer`` (default) - framebuffer pixels 1:1 (MZ-800
            full frame 928x288); ``native`` - one image pixel per dot of
            the computer: width halved when all pairs of framebuffer
            pixels are equal (320-dot modes, MZ-800 canvas 320x200),
            otherwise kept (640x200), lines 1:1; ``"1"`` to ``"4"`` - the
            aspect ratio of the emulator window, ``"2"`` equals the window
            at 100 % (MZ-800 full frame 928x576). Scale 1 drops every
            second column in the 640-dot modes.
        crop: optional rectangle ``{"x": int, "y": int, "width": int,
            "height": int}`` in output pixels, only with ``framebuffer``
            or ``native``. For a 256x192 ZX Spectrum style viewport in the
            middle of a 320x200 canvas use ``area="canvas"``,
            ``scale="native"``, ``crop={"x": 32, "y": 4, "width": 256,
            "height": 192}``.

    Returns:
        ``{"available": true, "path": str, "format": "png", "width": int,
        "height": int, "byte_size": int, "area": str, "scale": str}`` on
        success (plus ``"hires": bool`` for ``scale="native"``), or
        ``{"available": false, "reason": str}`` when the framebuffer has not
        been rendered yet. Invalid ``area``, ``scale`` or ``crop`` (also a
        crop outside the image) returns an error.

    Note:
        Path access is unrestricted under the V0 ``wild`` security profile;
        path whitelisting lands in V1.A.
    """
    data: dict[str, Any] = {"path": path}
    if format:
        data["format"] = format
    if area:
        data["area"] = area
    if scale:
        data["scale"] = scale
    if crop is not None:
        data["crop"] = crop
    resp = await _send_request("screenshot_save_to_file", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_screenshot_series_start(dir: str = "", area: str = "full",
                                      scale: str = "native",
                                      crop: Optional[dict] = None,
                                      only_changed: bool = True,
                                      frames: int = 0,
                                      max_files: int = 0) -> str:
    """Start saving a PNG of every changed emulated frame - for finding animation bugs.

    At the end of every emulated frame the emulator compares the picture
    (within ``area``, ``scale`` and ``crop``) with the last saved one and
    writes ``frame_NNNNNNN.png`` when it differs. NNNNNNN is the number of
    the emulated frame since the start of the series, so gaps in the
    numbering are unchanged frames; ``index.csv`` lists ``frame,file,
    gdg_screen``. Frames are taken in emulated time - also at MAX SPEED no
    frame is skipped; when writing cannot keep up, the emulation waits.
    The series only captures while the emulation runs (combine with
    ``emu_run(frames=N)`` for an exact stretch).

    Args:
        dir: series directory on the server host; empty = a new
            ``<platform>_frames_<date>_<time>`` folder in the screenshot
            output folder.
        area: ``full`` (default, with border) or ``canvas`` (picture only).
        scale: ``native`` (default) - one image pixel per dot of the
            computer (320 or 640 wide); ``framebuffer``; ``"1"`` to ``"4"``
            (aspect ratio of the emulator window). Same as
            ``emu_screenshot_save_to_file``.
        crop: optional ``{"x", "y", "width", "height"}`` in output pixels
            (``framebuffer`` or ``native`` only). Only changes inside the
            crop count - useful to follow one sprite.
        only_changed: ``True`` (default) = save only frames that differ
            from the last saved one; ``False`` = every frame.
        frames: stop automatically after this many emulated frames
            (0 = until ``emu_screenshot_series_stop``).
        max_files: safety limit of written files (0 = 10000).

    Returns:
        Series status ``{"active", "writing", "frames_seen", "files",
        "unchanged", "dir", "stop_reason", "last_error", "area", "scale",
        "only_changed"}``, or an error (series already running, invalid
        parameters, directory cannot be created).
    """
    data: dict[str, Any] = {"area": area, "scale": scale,
                            "only_changed": only_changed,
                            "frames": frames, "max_files": max_files}
    if dir:
        data["dir"] = dir
    if crop is not None:
        data["crop"] = crop
    resp = await _send_request("screenshot_series_start", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_screenshot_series_stop(wait: bool = True) -> str:
    """Stop the frame series started by ``emu_screenshot_series_start``.

    Args:
        wait: ``True`` (default) = wait (at most 30 s) until all queued
            frames are written; then ``writing`` is false and ``files`` is
            final.

    Returns:
        Series status (see ``emu_screenshot_series_status``), or an error
        when no series is running.
    """
    resp = await _send_request("screenshot_series_stop", {"wait": wait})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_screenshot_series_status() -> str:
    """Status of the running or the last frame series.

    Returns:
        ``{"active": bool (capturing), "writing": bool (files still being
        written), "frames_seen": int (emulated frames observed), "files":
        int (PNG written), "unchanged": int (frames skipped as unchanged),
        "dir": str, "stop_reason": str ("Stopped", "Frame count reached",
        "File limit reached", "Error" or "" while running), "last_error":
        str}``.
    """
    resp = await _send_request("screenshot_series_status", {})
    return json.dumps(_data_or_error(resp))


# ---------------------------------------------------------------------------
# Profiler podle PC - počet vykonání, T-stavy a WAIT pro každou adresu
# instrukce. Data i API v emulátoru: src/emulator/debugger/profiler_pc.h,
# příkaz DBGAPI_CMD_PROFILER_PC.
# ---------------------------------------------------------------------------

@mcp.tool()
async def emu_profiler_pc_start(reset: bool = True) -> str:
    """Start the per-PC profiler: for every instruction address it counts
    executions, T-states (including wait states) and wait states (VRAM and
    PSG WAIT) - to find where a program spends its time.

    Unlike ``emu_profiler_start`` (per function, via the call stack) it
    sees every instruction, also code that is jumped to, not called. The
    key is the logical PC (banks are not told apart). The overhead is one
    function call per instruction while it runs; with the profiler off the
    emulation is not slowed down.

    Time not spent in instructions is counted separately: ``halt_idle``
    (CPU waiting in HALT) and ``other_tstates`` (mainly interrupt
    acknowledge), so ``tstates + halt_idle + other_tstates ==
    elapsed_tstates``.

    Typical use: ``emu_profiler_pc_start()``, ``emu_run(frames=N)``,
    ``emu_profiler_pc_get()``.

    Args:
        reset: ``True`` (default) = clear previous data first; ``False`` =
            continue adding to it.

    Returns:
        Summary ``{"active", "instructions", "tstates", "wait",
        "halt_idle", "other_tstates", "elapsed_tstates", "unique_pcs"}``.
    """
    resp = await _send_request("profiler_pc_start", {"reset": reset})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_profiler_pc_stop() -> str:
    """Stop the per-PC profiler. The data stays for ``emu_profiler_pc_get``
    and ``emu_profiler_pc_export``.

    Returns:
        Summary ``{"active", "instructions", "tstates", "wait",
        "halt_idle", "other_tstates", "elapsed_tstates", "unique_pcs"}``.
    """
    resp = await _send_request("profiler_pc_stop", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_profiler_pc_reset() -> str:
    """Clear the per-PC profiler data. Whether it runs is not changed.

    Returns:
        Summary ``{"active", "instructions", "tstates", "wait",
        "halt_idle", "other_tstates", "elapsed_tstates", "unique_pcs"}``.
    """
    resp = await _send_request("profiler_pc_reset", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_profiler_pc_get(sort: str = "tstates", limit: int = 50,
                              from_addr: int = 0, to_addr: int = 0xFFFF) -> str:
    """Get the busiest instruction addresses of the per-PC profiler.

    Works while it runs and after ``emu_profiler_pc_stop``.

    Args:
        sort: ``tstates`` (default, where time is spent), ``count``
            (executions), ``wait`` (wait states) or ``addr`` (by address).
        limit: rows to return, 1-1000 (default 50).
        from_addr: lowest address (default 0).
        to_addr: highest address, inclusive (default 65535).

    Returns:
        Summary plus ``{"sort", "total_rows" (addresses executed in the
        range), "rows": [{"pc", "count", "tstates", "wait", "tstates_pct"
        (share of all instruction T-states), "avg_tstates", "symbol"
        (nearest symbol at or below pc, "" = none), "sym_offset"}]}``.
    """
    resp = await _send_request("profiler_pc_get",
                               {"sort": sort, "limit": limit,
                                "from": from_addr, "to": to_addr})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_profiler_pc_export(path: str, sort: str = "addr") -> str:
    """Write all executed addresses of the per-PC profiler to a CSV file.

    Columns ``pc,count,tstates,wait,tstates_pct`` (pc as 4 hex digits,
    tstates_pct = share of all instruction T-states in %).

    Args:
        path: output file on the server host (overwritten); the folder
            must exist.
        sort: ``addr`` (default), ``tstates``, ``count`` or ``wait``.

    Returns:
        Summary plus ``{"path", "rows"}``, or an error when the file
        cannot be written.
    """
    resp = await _send_request("profiler_pc_export", {"path": path, "sort": sort})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_profiler_pc_bitmap() -> str:
    """Get the executed instruction addresses of the per-PC profiler as a
    compact 8 KiB bitmap - code coverage for frequent reads, e.g. after
    every step of an automated run.

    Bit ``(addr & 7)`` of byte ``(addr >> 3)`` is set when at least one
    instruction starting at ``addr`` was executed - the same addresses as
    the rows of ``emu_profiler_pc_get`` / ``emu_profiler_pc_export``, so
    the number of set bits equals ``unique_pcs``. Operand bytes get no
    bit, an idle HALT adds nothing, block instructions (LDIR, ...) mark
    their start. The key is the logical PC: memory banks and versions of
    self-modified code at one address are not told apart.

    Works while the profiler runs and after ``emu_profiler_pc_stop``; it
    neither pauses nor advances the emulation. Start collecting with
    ``emu_profiler_pc_start``.

    Returns:
        Summary plus ``{"format": "pc-bitset-lsb0", "map_bytes": 8192,
        "bitmap_base64": str}``.
    """
    resp = await _send_request("profiler_pc_bitmap", {})
    return json.dumps(_data_or_error(resp))


# ---------------------------------------------------------------------------
# Zachycení zvuku do složky - události kanálů, zápisy do PSG, reference.wav
# a sdl.wav. Jádro: src/emulator/audio_capture.h, příkaz DBGAPI_CMD_AUDIO_CAPTURE.
# ---------------------------------------------------------------------------

@mcp.tool()
async def emu_audio_capture_start(dir: str = "", frames: int = 0,
                                  events: bool = True, psg_writes: bool = True,
                                  reference_wav: bool = True,
                                  sdl_wav: bool = True, memory: bool = True,
                                  pcm: list[str] | None = None,
                                  memory_minutes: int = 10,
                                  pcm_seconds: int = 60) -> str:
    """Start capturing what the sound hardware does - for finding sound bugs.

    Files written into ``dir`` in emulated time:

    * ``events.csv`` - ``ticks,time_us,frame,channel,level``: every change
      of the output level (0-15) of a sound channel: ``ctc0`` (speaker,
      0 or 15), ``psg0_0``..``psg0_3`` and on the MZ-1500 or an MZ-800 with
      the second PSG ``psg1_0``..``psg1_3`` (channel 3 = noise). The first
      rows (ticks 0) are the levels at the start.
    * ``psg_writes.csv`` - ``ticks,time_us,frame,psg,byte,channel,reg,value``:
      every write to the PSG (psg = target mask: 1 = PSG0, 2 = PSG1,
      3 = both), decoded from the PSG state after the write: reg
      ``tone_lo`` (latch byte, low 4 bits of the divider), ``tone`` (the
      10-bit divider), ``volume`` (attenuation 0-15, 15 = off) or ``noise``
      ((type << 2) | rate).
    * ``reference.wav`` - the sound rendered exactly in emulated time (the
      renderer of the video recording).
    * ``sdl.wav`` - the sound as produced by the emulator's real audio
      output path (what the user hears). Same format as reference.wav
      (44100 Hz, 16 bit, stereo), but one frame later (without an audio
      device; with a device the lag depends on its buffer). Comparing the
      two shows defects of the output path.
    * ``capture.json`` - platform, clock, channel names, counts, stop
      reason (written at the end).

    With ``memory`` (default) the capture also keeps rings in memory that
    can be read while it runs and after it ends, until the next start -
    no file access needed (also for a remote client): ``emu_trace_read``
    channels ``audio_psg_writes``, ``audio_levels`` (the CSV rows) and
    ``audio_frames`` (one summary per audio frame),
    ``emu_audio_capture_summary`` (what happened: silence, notes, speaker
    tone or PCM, reference vs output path) and ``emu_audio_capture_pcm``
    (a short PCM window). A capture to memory only (all four files off)
    creates no folder.

    ``ticks`` are GDG clock ticks since the start (``gdg_clk_hz`` in
    capture.json), ``frame`` is the audio frame (one video frame) since
    the start. The capture begins at the end of the next audio frame; with
    ``sdl_wav`` after 6 more frames that warm up the output path. It
    captures only while the emulation runs - combine with
    ``emu_run(frames=N)``.

    Args:
        dir: folder on the server host (created); empty = a new
            ``<platform>_audio_<date>_<time>`` folder in the audio capture
            output folder (default ``audio_captures`` next to the
            emulator, Tools -> Audio Capture -> Settings...).
        frames: stop automatically after this many audio frames (0 = until
            ``emu_audio_capture_stop``, at most one hour).
        events, psg_writes, reference_wav, sdl_wav: which files to write.
        memory: keep the capture memory (rings of PSG writes, level
            changes and frame summaries; levels 1M records = 16 MiB,
            PSG writes 256k = 6 MiB).
        pcm: PCM rings in memory: ``["reference"]`` (default), ``["sdl"]``,
            both (needed for ``ref_vs_sdl`` in the summary) or ``[]``;
            10.6 MB per minute and source.
        memory_minutes: length of the frame summary ring (1-120).
        pcm_seconds: length of the PCM rings (1-3600); older audio is
            dropped.

    Returns:
        Capture status (see ``emu_audio_capture_status``) with
        ``pending: true``, or an error (already running, nothing selected,
        folder cannot be created, not enough memory).
    """
    data: dict[str, Any] = {"frames": frames, "events": events,
                            "psg_writes": psg_writes,
                            "reference_wav": reference_wav,
                            "sdl_wav": sdl_wav, "memory": memory,
                            "pcm": ["reference"] if pcm is None else list(pcm),
                            "memory_minutes": memory_minutes,
                            "pcm_seconds": pcm_seconds}
    if dir:
        data["dir"] = dir
    resp = await _send_request("audio_capture_start", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_audio_capture_stop() -> str:
    """Stop the audio capture and finish its files (WAV headers,
    capture.json).

    Returns:
        Final capture status, or an error when no capture is running.
    """
    resp = await _send_request("audio_capture_stop", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_audio_capture_status() -> str:
    """Status of the running or the last audio capture.

    Returns:
        ``{"pending": bool (waiting for the start), "active": bool,
        "frames": int (captured audio frames), "events": int,
        "psg_writes": int, "reference_samples": int, "sdl_blocks": int
        (blocks of the output path; fewer than frames = it skipped frames,
        e.g. at MAX SPEED with an audio device), "sdl_samples": int,
        "dir": str, "stop_reason": str ("Stopped", "Frame count reached",
        "Safety limit reached", "Emulation timeline restarted (reset or
        snapshot load)", "Error", "Emulator exit" or "" while running),
        "last_error": str, "segment": int (capture number since the
        emulator start; a new start replaces the memory), "origin_ticks":
        int | null (absolute GDG ticks of time 0), "channels": [names in
        the order of the audio_frames arrays], "stereo": bool, "memory":
        null | {"bytes": int, "streams": {"psg_writes" | "levels" |
        "frames" | "pcm_reference" | "pcm_sdl": {"record_size",
        "capacity", "first_seq", "next_seq"}}}}``. ``first_seq`` > 0 means
        the ring dropped its oldest records; PCM ``seq`` = sample index
        (44100 Hz) since the start.
    """
    resp = await _send_request("audio_capture_status", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_audio_capture_summary(frame_from: int = 0, frame_to: int = -1,
                                    silence_rms: int = 64,
                                    max_notes: int = 200,
                                    lag_frames: int = -1,
                                    max_lag_frames: int = 8) -> str:
    """Summary of the audio capture memory - what the sound did, as text.

    Needs a capture started with ``memory=True`` (the default of
    ``emu_audio_capture_start``); works while it runs and after it ends.

    Returns ``frames`` [from, to] (audio frames since the capture start),
    ``frames_available``, ``fps``, ``platform``, ``stereo``;
    ``sound_spans`` / ``silent_spans`` ([[from, to], ...], at most 50
    each, ``*_total`` = all; silent = reference RMS below
    ``silence_rms``, ``silence_rule``); ``channels`` (per channel:
    ``active_frames``, ``edges``, ``max_level``, ``on_pct``,
    ``est_hz_median`` = level changes / 2 per second); ``psg_notes``
    (``ch``, ``note`` such as ``"A4"`` or ``"noise"``, ``cents``, ``hz``,
    ``divider``, ``from_frame``, ``to_frame``, ``max_level``; replayed
    from the PSG writes with the emulator's PSG formula;
    ``psg_state_incomplete`` = the oldest writes were dropped, notes at the
    start may be wrong); ``ctc0`` (speaker: ``edges``,
    ``edge_interval_hist_us`` [[bucket_from_us, count]], ``likely``
    ``tone`` / ``pcm`` / ``irregular`` / ``silent`` - a heuristic
    estimate, see ``likely_rule``); ``ref_vs_sdl`` (needs
    ``pcm=["reference", "sdl"]``: ``lag_samples``, ``lag_frames``,
    ``diff_rms`` / ``diff_max`` (-1..1 units, mono), ``reference_rms``,
    ``worst_frames`` [[frame, diff_rms]], ``sdl_missing_blocks``; at most
    2 s are compared).

    Args:
        frame_from: First frame (default 0 = oldest kept).
        frame_to: Last frame (-1 = newest).
        silence_rms: Silence threshold, int16 RMS of the reference (0-32767).
        max_notes: Max notes listed (1-2000).
        lag_frames: Output path delay in frames; -1 = search it.
        max_lag_frames: Largest delay searched (1-60).
    """
    data = {"frame_from": frame_from, "frame_to": frame_to,
            "silence_rms": silence_rms, "max_notes": max_notes,
            "lag_frames": lag_frames, "max_lag_frames": max_lag_frames}
    resp = await _send_request("audio_capture_summary", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_audio_capture_pcm(source: str = "reference", frame_from: int = 0,
                                frames: int = 25, sample_from: int = -1,
                                samples: int = 0, rate: int = 11025,
                                stereo: bool = False,
                                encoding: str = "wav_b64",
                                max_bytes: int = 32768) -> str:
    """A short PCM window from the audio capture memory (WAV or raw, base64).

    Needs ``pcm`` with the source at ``emu_audio_capture_start``
    (``reference`` is kept by default). The default window (25 frames,
    11025 Hz mono, 32 KiB) stays below 50 000 characters. Resampling is
    averaging (an analysis preview); ``rate=44100, stereo=True`` returns
    exactly the samples of reference.wav / sdl.wav. Page through longer
    audio with ``sample_from=next_sample``.

    Args:
        source: ``"reference"`` (render in emulated time) or ``"sdl"``
            (output path).
        frame_from: Start at this audio frame (used when ``sample_from``
            < 0).
        frames: Length in frames (used when ``samples`` is 0).
        sample_from: Start sample (44100 Hz) since the capture start.
        samples: Length in 44100 Hz samples.
        rate: Output rate 1000-44100 Hz.
        stereo: Two channels (default mono = average of L and R).
        encoding: ``"wav_b64"`` (WAV file) or ``"pcm_b64"`` (raw int16
            little endian).
        max_bytes: Data limit before base64 (1024-4194304); a longer window
            is cut (``truncated: true``).

    Returns:
        ``{"source", "segment", "live", "rate", "channels", "from_sample",
        "samples", "frames_out", "next_sample", "end_of_data",
        "truncated", "available": [first, next], "gap", "encoding",
        "data_b64"}`` or ``{"error": ...}``.
    """
    data: dict[str, Any] = {"source": source, "frame_from": frame_from,
                            "frames": frames, "rate": rate, "stereo": stereo,
                            "encoding": encoding, "max_bytes": max_bytes}
    if sample_from >= 0:
        data["sample_from"] = sample_from
    if samples > 0:
        data["samples"] = samples
    resp = await _send_request("audio_capture_pcm", data)
    return json.dumps(_data_or_error(resp))




@mcp.resource("emulator://video/text_dump")
async def resource_video_text_dump() -> str:
    """40x25 text VRAM dump (MZ-700 layout).

    Returns ``{"available": true, "platform": str, "cols": 40,
    "rows": 25, "cell_count": 1000, "chars_b64": str,
    "attributes_b64": str}`` when the emulator is in a text mode that
    matches the MZ-700 D000-D3FF (chars) + D800-DBFF (attributes)
    layout. Returns ``{"available": false, "platform": str, "reason":
    str}`` otherwise.

    Availability per build:

      - MZ-700: always available (text mode is the only mode).
      - MZ-1500: always available (the 700-compat layout is present).
      - MZ-800: available when ``GDG.regDMD`` MZ700-flag is set
        (= running in 700-compat mode). In 800 graphics mode the
        Resource returns ``available=false`` and the reason
        ``"MZ-800 in 800 graphics mode (not text)"``.

    ``chars_b64`` and ``attributes_b64`` are base64-encoded raw byte
    streams (1000 bytes each). ``chars`` is Sharp display code - clients
    can convert to standard ASCII / UTF-8 via the mapping tables. See
    ``emulator://docs/sharp_display_code`` for decoding (display code,
    Sharp MZ ASCII, attribute byte semantics).
    Attribute byte semantics depend on the platform; clients should
    consult the GDG documentation for interpretation.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_video_text_dump")
    return json.dumps(_data_or_error(resp))


@mcp.resource("emulator://periph/beeper")
async def resource_periph_beeper() -> str:
    """Beeper / CTC0 audio path snapshot.

    Returns ``{"available": true, "level": 0|1, "ctc0_out": 0|1,
    "gate0": 0|1, "pc0": 0|1, "source": "PC0"}``.

    The Sharp MZ machines do **not** have a dedicated 1-bit beeper
    like the ZX Spectrum. Instead, audio is routed from the Intel 8253
    CTC channel 0 output through two AND gates (``GATE0`` and
    ``PC0``) to the speaker / audio mixer:

        audible = ctc0_out AND gate0 AND pc0

    ``GATE0`` source depends on the current platform/mode:

      - MZ-800 in 800 mode (``DMD3=0``): GATE0 is hardwired to 1 and
        cannot be changed by software.
      - MZ-700 / MZ-1500 / MZ-800 in 700 mode: GATE0 is bit 0 of the
        memory-mapped register at 0xE008 (mirrored as
        ``g_gdg.regct53g7``).

    ``pc0`` is bit 0 of the 8255 PPI port C (``signal_pc00``). The
    backend returns the raw bits so clients can correlate audio
    silence with which gate is closed. ``source`` is the fixed string
    ``"PC0"`` identifying the primary control bit (kept as a field for
    future extension when alternative audio paths might exist).

    Reading is side-effect free. Note that ``ctc0_out`` is a single
    snapshot - the actual square wave at the speaker is generated by
    CTC0 toggling at the programmed frequency; clients that need the
    audio frequency should read CTC0 ``preset_value`` from
    ``emulator://periph/i8253`` instead.
    """
    if _transport is None or not _transport.is_alive():
        return json.dumps({"connected": False})
    resp = await _send_request("get_periph_beeper")
    return json.dumps(_data_or_error(resp))


# === memsearch - Freeze Bytes a postupné hledání proměnných ============
# Freeze Bytes přes MCP (dřív jen GUI) a stavový vyhledávač proměnných:
# sezení drží kandidáty a vzorky paměti, filtr je zužuje. Kandidát se pak
# řeší existujícími nástroji (emu_watch_add, emu_bp_create_with_init,
# emu_freeze_add) podle polí z emu_memsearch_list.


@mcp.tool()
async def emu_freeze_add(kind: str, offset: int, value: int = -1,
                         bytes: list[int] | None = None, sub_id: int = 0) -> str:
    """Freeze a byte (or consecutive bytes) of a memory region.

    WARNING: This is a destructive operation (it writes into emulated
    memory). The frozen value is written back once per emulated frame
    (Freeze Bytes, at most 256 bytes in total), so the program can see its
    own write until the end of the frame: a game that decrements lives and
    tests them in the same frame still sees the change. An existing entry
    only gets the new value.

    Args:
        kind: Region kind as in ``emu_regions_list`` (``"ram"``,
            ``"memext_ram"``, ``"vram_phys_plane"``, ...). The region must be
            present and writable.
        offset: Offset inside the region (the ``region_offset`` of an
            ``emu_memsearch_list`` item).
        value: Byte value 0-255 (give either ``value`` or ``bytes``).
        bytes: Values 0-255 from ``offset`` on (1-64 items), e.g. both bytes
            of a 16-bit counter in address order.
        sub_id: Region disambiguator (bank, plane), default 0.

    Returns:
        JSON ``{"added": n, "frozen": total}`` or ``{"error": ...}``.
    """
    data: dict[str, Any] = {"kind": kind, "sub_id": sub_id, "offset": offset}
    if bytes is not None:
        data["bytes"] = list(bytes)
    else:
        data["value"] = value
    resp = await _send_request("freeze_add", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_freeze_remove(kind: str = "", offset: int = -1, length: int = 1,
                            sub_id: int = 0, all: bool = False) -> str:
    """Unfreeze bytes frozen by ``emu_freeze_add`` or the GUI.

    Args:
        kind: Region kind as in ``emu_regions_list``.
        offset: First offset inside the region.
        length: Number of consecutive bytes (1-256).
        sub_id: Region disambiguator, default 0.
        all: True = unfreeze everything (other arguments are ignored).

    Returns:
        JSON ``{"removed": n}`` or ``{"error": ...}``.
    """
    if all:
        data: dict[str, Any] = {"all": True}
    else:
        data = {"kind": kind, "sub_id": sub_id, "offset": offset, "length": length}
    resp = await _send_request("freeze_remove", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_freeze_list() -> str:
    """List the frozen bytes (also those frozen in the GUI Memory Browser).

    Returns:
        JSON ``{"count", "max": 256, "entries": [{"kind", "sub_id",
        "offset", "value"}]}``.
    """
    resp = await _send_request("freeze_list", {})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_memsearch_start(space: str | list[dict[str, Any]] = "ram",
                              type: str = "u8", width: int = 0,
                              endian: str = "", aligned: bool = False,
                              digit_encoding: str = "display",
                              leading_space: bool = True,
                              op: str = "", value: int | None = None,
                              value2: int | None = None, mask: int | None = None,
                              negate: bool = False,
                              allow_large: bool = False) -> str:
    """Start a variable search (cheat search): take a memory sample, every
    position is a candidate, optionally keep only those equal to a value.

    The search finds lives, score, timers and coordinates by narrowing:
    change the value in the game (``emu_input_send_keys``, ``emu_run``),
    then ``emu_memsearch_filter`` (e.g. ``op="dec_by", value=1`` after
    losing a life) until few candidates remain, then ``emu_memsearch_list``.
    Samples are taken by the emulator thread at the next frame boundary
    (or at once when paused), consistently for the whole space. Sessions
    are separate from the GUI Memory Search window; at most 8 are open.

    Args:
        space: Preset ``"ram"`` (User RAM 64 KB, default), ``"logical"``
            (what the CPU sees now, incl. ROM and VRAM), ``"vram"`` (VRAM
            700 text/attributes and MZ-800 VRAM planes), ``"memext"``,
            ``"ramdisk"``, ``"all_writable"``; or a list of
            ``{"kind", "sub_id", "offset", "length"}`` (kinds as in
            ``emu_regions_list``, length defaults to the region end).
            With a MemExt that switches banks, User RAM shows whatever is
            mapped at sample time - search the ``memext`` banks instead.
        type: ``u8`` (default), ``s8``, ``u16``, ``s16``, ``u24``, ``s24``,
            ``u32``, ``s32``, ``bcd`` (packed, 2 digits per byte, a nibble
            above 9 = not a candidate) or ``digits`` (one digit per byte).
        width: Bytes for ``bcd`` (1-4, default 2) and ``digits`` (1-8,
            default 4); integers take the width from the type name.
        endian: ``"le"`` (default for integers and BCD, Z80 order) or
            ``"be"`` (default for digits = print order, highest first).
        aligned: Only offsets divisible by the width.
        digit_encoding: ``"raw"`` (00h-09h), ``"ascii"`` (30h-39h) or
            ``"display"`` (Sharp MZ display code 20h-29h, default).
        leading_space: Digits: leading spaces count as zeros (ASCII 20h,
            display code 00h).
        op, value, value2, mask, negate: Optional first filter, it can only
            compare with a value (e.g. ``op="eq", value=3``). See
            ``emu_memsearch_filter``.
        allow_large: Allow a space over 4 MiB (up to 32 MiB; the session
            keeps three samples plus undo history).

    Returns:
        JSON ``{"session_id", "space_size", "spans": [{"kind", "sub_id",
        "offset", "length", "connected"}], "candidates", "step",
        "sample_frame", "undo_depth", "redo_depth"}`` or ``{"error": ...}``.
    """
    data: dict[str, Any] = {"space": space, "type": type, "aligned": aligned,
                            "digit_encoding": digit_encoding,
                            "leading_space": leading_space,
                            "allow_large": allow_large}
    if width:
        data["width"] = width
    if endian:
        data["endian"] = endian
    if op:
        data["op"] = op
        data["negate"] = negate
        for k, v in (("value", value), ("value2", value2), ("mask", mask)):
            if v is not None:
                data[k] = v
    resp = await _send_request("memsearch_start", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_memsearch_filter(session_id: int, op: str, value: int | None = None,
                               compare_to: str = "", value2: int | None = None,
                               mask: int | None = None, negate: bool = False,
                               sample: bool = True) -> str:
    """Narrow the candidates of a search session.

    By default a new sample is taken first and compared with the previous
    sample (``compare_to="previous"``), with the first sample of the search
    (``"first"``) or with ``value`` (``"value"``). ``sample=False`` compares
    the last sample with the one before it (e.g. after
    ``emu_memsearch_sample``). Arithmetic is on the decoded values without
    wrap-around: an u8 going 00h -> FFh is not ``dec_by 1``. Candidates in a
    region that got disconnected stay unchanged.

    Args:
        session_id: From ``emu_memsearch_start``.
        op: ``eq``, ``ne``, ``lt``, ``le``, ``gt``, ``ge``, ``between``
            (``value`` <= x <= ``value2``), ``inc``, ``dec``, ``inc_by``,
            ``dec_by``, ``changed``, ``unchanged``, ``changed_by``
            (difference ``value``), ``mask_eq`` (bits under ``mask``,
            integer types only).
        value: Value, difference or lower bound.
        compare_to: ``"value"``, ``"previous"`` or ``"first"``. Default:
            ``"previous"`` for inc/dec/changed/..., otherwise ``"value"``
            when ``value`` is given.
        value2: Upper bound for ``between``.
        mask: Bit mask for ``mask_eq``.
        negate: Invert the condition (an invalid BCD / digit value is
            removed anyway).
        sample: Take a new sample first (default True).

    Returns:
        JSON ``{"removed", "candidates", "step", "sample_frame",
        "undo_depth", "redo_depth"}`` or ``{"error": ...}``.
    """
    data: dict[str, Any] = {"session_id": session_id, "op": op,
                            "negate": negate, "sample": sample}
    if compare_to:
        data["compare_to"] = compare_to
    for k, v in (("value", value), ("value2", value2), ("mask", mask)):
        if v is not None:
            data[k] = v
    resp = await _send_request("memsearch_filter", data)
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_memsearch_sample(session_id: int) -> str:
    """Take a new sample without filtering (it becomes the "previous" value
    for the next filter with ``sample=False``).

    Args:
        session_id: From ``emu_memsearch_start``.

    Returns:
        JSON session state (``candidates``, ``step``, ``sample_frame``, ...).
    """
    resp = await _send_request("memsearch_sample", {"session_id": session_id})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_memsearch_undo(session_id: int) -> str:
    """Undo the last step of a search session (candidates and the reference
    samples; undoing the start leaves the session without a sample).

    Args:
        session_id: From ``emu_memsearch_start``.

    Returns:
        JSON session state with ``started`` or ``{"error": "nothing to undo"}``.
    """
    resp = await _send_request("memsearch_undo", {"session_id": session_id})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_memsearch_redo(session_id: int) -> str:
    """Redo a step undone by ``emu_memsearch_undo``.

    Args:
        session_id: From ``emu_memsearch_start``.

    Returns:
        JSON session state or ``{"error": "nothing to redo"}``.
    """
    resp = await _send_request("memsearch_redo", {"session_id": session_id})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_memsearch_list(session_id: int, offset: int = 0, limit: int = 100) -> str:
    """List candidates of a search session (one page).

    Each item tells where the value lives and how to act on it:
    ``kind`` / ``sub_id`` / ``region_offset`` (for ``emu_freeze_add`` and
    ``emu_region_read``), ``z80_addr`` (CPU address for ``emu_watch_add``,
    null when the cell is not mapped to the CPU or is a VRAM plane /
    RAM disk) and ``bp_hint`` (``addr``, ``fields``, ``values`` ready for
    ``emu_bp_create_with_init`` - a write breakpoint in the right zone:
    ``RAM`` for User RAM, ``VRAM_FB`` for VRAM 700, ``MMEXT_BANK`` with
    ``bank_offset`` for a PEHU bank; null where a write breakpoint cannot
    match the cell). ``cur`` / ``prev`` / ``first`` are the decoded values
    (null = invalid BCD / digits or disconnected region), ``cur_hex`` the
    bytes in address order.

    Args:
        session_id: From ``emu_memsearch_start``.
        offset: Index of the first candidate.
        limit: Items per page (1-1000).

    Returns:
        JSON ``{"total", "offset", "items": [{"index", "space_offset",
        "kind", "sub_id", "region_offset", "connected", "cur", "prev",
        "first", "cur_hex", "z80_addr", "bp_hint"}], "candidates", "step",
        ...}``.
    """
    resp = await _send_request("memsearch_list", {"session_id": session_id,
                                                  "offset": offset, "limit": limit})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_memsearch_eliminate(session_id: int, space_offsets: list[int]) -> str:
    """Remove chosen candidates by hand (undoable).

    Args:
        session_id: From ``emu_memsearch_start``.
        space_offsets: ``space_offset`` values from ``emu_memsearch_list``
            (at most 1000).

    Returns:
        JSON ``{"removed", "candidates", "step", ...}``.
    """
    resp = await _send_request("memsearch_eliminate", {"session_id": session_id,
                                                       "space_offsets": list(space_offsets)})
    return json.dumps(_data_or_error(resp))


@mcp.tool()
async def emu_memsearch_close(session_id: int) -> str:
    """Close a search session and free its memory.

    Args:
        session_id: From ``emu_memsearch_start``.

    Returns:
        JSON ``{"closed": true}`` or ``{"error": ...}``.
    """
    resp = await _send_request("memsearch_close", {"session_id": session_id})
    return json.dumps(_data_or_error(resp))


# === MCP docs Resources (autoregistrace) =============================
# AI-reference dokumenty se registrují AUTOMATICKY ze složek - žádná
# ruční ``@mcp.resource`` funkce per soubor (dříve to bylo duplikováno
# na 4 místech a snadno se zapomnělo zaregistrovat nový dokument). Dva
# zdroje, oba volitelné a tolerantní k chybějícím cestám:
#
#   1. Built-in projektová reference: ``docs/agent/*.md`` (sourozenec
#      mcp-server/, verzováno v gitu, kopíruje se do dist přes make dist).
#      Každý soubor kromě README.md -> ``emulator://docs/<stem>``.
#      Plochá struktura (bez rekurze do podsložek).
#
#   2. Uživatelská knowledge base: volitelná složka MIMO repo, nastavená
#      přes env ``MZ800EMU_USER_KB_DIR`` (wrapper-side konfigurace,
#      typicky v ``env`` bloku ``.mcp.json``; NE v [MCP] sekci INI - tu
#      vlastní C jádro a přepisuje). Rekurzivně všechny ``*.md`` ->
#      ``emulator://kb/<relativní cesta bez .md>`` (oddělovač ``/``).
#      Nenastaveno / neexistuje = žádné kb resources (bez chyby).
#
# Title + description Resource se získají z dokumentu (viz
# ``_docs_extract_meta``): volitelný frontmatter má přednost, jinak
# fallback na H1 nadpis + první textový odstavec. Discovery
# (``resources/list``) je instantní = jen lokální čtení adresářů při
# startu serveru, žádné volání emu transportu.

_DOCS_DIR = Path(__file__).resolve().parent.parent / "docs" / "agent"

# Maximální délka auto-generovaného description (ve znacích) pro
# resources/list. Delší úvodní odstavce se oříznou.
_DOCS_DESC_MAXLEN = 600


def _docs_extract_meta(text: str, fallback_name: str) -> tuple[str, str]:
    """Získá ``(title, description)`` z markdown dokumentu pro MCP Resource.

    Args:
        text: Celý obsah markdown souboru (UTF-8).
        fallback_name: Náhradní title, pokud dokument nemá H1 nadpis ani
            frontmatter ``title:`` (typicky stem názvu souboru).

    Returns:
        Dvojici ``(title, description)``. Title = frontmatter ``title:``,
        jinak první ``# H1`` nadpis, jinak ``fallback_name``. Description =
        frontmatter ``description:``, jinak první textový odstavec za H1
        (řádky spojené mezerou, oříznuto na ``_DOCS_DESC_MAXLEN`` znaků).
        Frontmatter má vždy přednost před tělem dokumentu.

    Note:
        Frontmatter parser je záměrně minimalistický (řádky ``key: value``
        mezi úvodními ``---`` na prvním neprázdném řádku), bez závislosti
        na PyYAML. Rozpoznává jen klíče ``title`` a ``description``;
        hodnoty se neescapují a ``#`` v hodnotě není komentář.
    """
    lines = text.splitlines()
    fm_title = ""
    fm_desc = ""
    body_start = 0
    # Volitelný frontmatter: první řádek souboru je přesně '---'.
    if lines and lines[0].strip() == "---":
        for i in range(1, len(lines)):
            s = lines[i].strip()
            if s == "---":
                body_start = i + 1
                break
            key, sep, val = s.partition(":")
            if sep:
                key = key.strip().lower()
                if key == "title":
                    fm_title = val.strip()
                elif key == "description":
                    fm_desc = val.strip()
    # H1 nadpis + první textový odstavec z těla dokumentu.
    h1 = ""
    para: list[str] = []
    in_para = False
    for ln in lines[body_start:]:
        s = ln.strip()
        if not h1:
            if s.startswith("# "):
                h1 = s[2:].strip()
            continue
        if not in_para:
            if s:
                in_para = True
                para.append(s)
        elif s:
            para.append(s)
        else:
            break
    title = fm_title or h1 or fallback_name
    desc = fm_desc or " ".join(para)
    if len(desc) > _DOCS_DESC_MAXLEN:
        desc = desc[: _DOCS_DESC_MAXLEN - 3].rstrip() + "..."
    return title, desc


def _docs_add_resource(uri: str, path: Path) -> None:
    """Zaregistruje jeden .md soubor jako ``TextResource`` pod daným URI.

    Args:
        uri: Cílový MCP resource URI (např. ``emulator://docs/bp_dsl``).
        path: Absolutní cesta k existujícímu markdown souboru.

    Side effects:
        Volá ``mcp.add_resource``. Při chybě čtení souboru se resource
        přeskočí a chyba zaloguje (start serveru nesmí spadnout kvůli
        jednomu nečitelnému dokumentu).

    Note:
        Obsah se načte VŽDY jako UTF-8 a předá ``TextResource`` jako text
        (drží se v paměti). Záměrně se nepoužívá ``FileResource``, který
        čte v locale kódování (na Windows cp1250) a padá na UTF-8 znaky
        v dokumentech (např. šipkové glyfy v keyboard referenci).
        Dokumenty jsou malé (jednotky až desítky KB), takže eager držení
        v RAM je zanedbatelné; obsah se zafixuje při startu serveru.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as e:
        log.warning("docs resource skip %s: %s", path, e)
        return
    name = path.stem
    title, desc = _docs_extract_meta(text, name)
    mcp.add_resource(TextResource(
        uri=uri,
        name=name,
        title=title,
        description=desc,
        mime_type="text/markdown",
        text=text,
    ))


def _register_docs_resources() -> None:
    """Autoregistrace AI-reference dokumentů jako MCP Resources.

    Built-in: ``docs/agent/*.md`` (mimo README.md) ->
    ``emulator://docs/<stem>`` (ploše).
    User KB: rekurzivně ``<user_kb_dir>/**/*.md`` ->
    ``emulator://kb/<relativní cesta>``. ``user_kb_dir`` je čistě
    wrapper-side konfigurace přes env ``MZ800EMU_USER_KB_DIR`` (typicky
    nastavená v ``env`` bloku ``.mcp.json``, kterým MCP klient spouští
    server). Záměrně se NEčte z [MCP] sekce ``mz800emu.ini`` - tu vlastní
    C cfgmain a při uložení přepisuje (truncate), takže neregistrovaný
    klíč by zmizel. Nenastaveno / neexistující adresář = jen built-in docs.

    Volá se jednou při importu modulu (po vytvoření ``mcp`` instance).
    Tolerantní: chybějící adresáře jen zaloguje, nikdy nevyhodí výjimku.
    """
    # 1. Built-in projektová reference (plochá, verzovaná v gitu).
    if _DOCS_DIR.is_dir():
        for p in sorted(_DOCS_DIR.glob("*.md")):
            if p.name.lower() == "readme.md":
                continue
            _docs_add_resource(f"emulator://docs/{p.stem}", p)
    else:
        log.warning("docs/agent dir not found: %s", _DOCS_DIR)

    # 2. Uživatelská knowledge base (rekurzivní, volitelná, mimo repo).
    kb_dir_str = os.environ.get("MZ800EMU_USER_KB_DIR", "").strip()
    if not kb_dir_str:
        return
    kb_dir = Path(kb_dir_str).expanduser()
    if not kb_dir.is_dir():
        log.warning("user_kb_dir is not a directory: %s", kb_dir)
        return
    for p in sorted(kb_dir.rglob("*.md")):
        if not p.is_file():
            continue
        rel = p.relative_to(kb_dir).with_suffix("")
        topic = "/".join(rel.parts)
        _docs_add_resource(f"emulator://kb/{topic}", p)
    log.info("user KB resources registered from %s", kb_dir)


_register_docs_resources()


# === Entry point =====================================================

def main() -> None:
    """Spustí FastMCP server na stdio MCP transportu.

    FastMCP řídí vlastní asyncio event loop. Transport bridge state je
    inicializován lazily při prvním tool callu (= ``_ensure_connected``).

    Cleanup transportu při exit-u je best-effort - FastMCP ``run()``
    blokuje až do EOF na stdinu, takže shutdown se provede až Claude
    klient ukončí MCP session.
    """
    log.info("mcp_server.py starting, TRANSPORT=%s, EMU_EXE=%s, "
             "TCP=%s:%d", TRANSPORT_KIND, EMU_EXE, TCP_HOST, TCP_PORT)
    try:
        mcp.run()
    except KeyboardInterrupt:
        log.info("interrupted, exiting")
    finally:
        # Best-effort cleanup. Pokud transport žije, pokusíme se ho
        # v separate event loopu uzavřít.
        if _transport is not None and _transport.is_alive():
            try:
                asyncio.run(_shutdown_emu())
            except Exception as e:
                log.warning("cleanup _shutdown_emu failed: %s", e)
        log.info("mcp_server.py exited")


if __name__ == "__main__":
    main()
