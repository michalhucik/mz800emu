#!/usr/bin/env python3
"""Regression test pro bug 0011 - region_read/write wedge v TCP módu.

Testuje skutečný `_TcpTransport` + `_stdout_reader_task` + `_send_request`
z `mcp_server.py` proti MOCK TCP serveru (BEZ emulátoru). Reprodukuje
root cause:

  Defekt 1+2 (Test A): velký region_read response (>64 KiB JSONL řádek)
    shodí reader task přes nezachycený ValueError z readline() ->
    bridge se zasekne. Test ověří, že velký response projde A že bridge
    zůstane živý pro následný mem_read.

  Defekt 3 (Test B): když backend vrátí error response (bez `data`),
    `emu_region_read` musí surfacovat error, ne vrátit holé `{}`.

Pipe transport tímto netrpí (file.readline() bez limitu) - proto test
běží přes TCP mock. Spouštět venv interpretem MCP serveru (FastMCP):

  ./mcp-server/.venv/Scripts/python.exe tests/mcp/test_tcp_region_wedge.py

Exit 0 = vše PASS, 1 = aspoň jeden FAIL.
"""
import asyncio
import base64
import json
import os
import subprocess
import sys
from pathlib import Path

# Exit kód pro ctest "skip", když chybí FastMCP venv. Musí odpovídat
# SKIP_RETURN_CODE v tests/mcp/CMakeLists.txt.
SKIP_EXIT = 77

REPO_ROOT = Path(__file__).resolve().parents[2]
MCP_DIR = REPO_ROOT / "mcp-server"


def _find_venv_python():
    """Najde venv interpret MCP serveru (priorita: env, .venv, žádný)."""
    env = os.environ.get("MZ_MCP_VENV_PY")
    if env and Path(env).is_file():
        return Path(env)
    for c in (MCP_DIR / ".venv" / "Scripts" / "python.exe",
              MCP_DIR / ".venv" / "bin" / "python",
              MCP_DIR / ".venv" / "bin" / "python3"):
        if c.is_file():
            return c
    return None


# mcp_server importuje FastMCP. Když běžíme pod systémovým Pythonem (= ctest
# launcher) bez FastMCP, re-execneme se pod venv interpretem. Když venv není,
# test skipneme (exit 77).
#
# Spouští se přes subprocess.call s předáním exit kódu, ne os.execv: na
# Windows os.execv proces nenahradí - rodič hned skončí s kódem 0, výstup
# testu se ztratí a ctest hlásí falešný PASS.
#
# Smyčku opakovaného spuštění hlídá sentinel ``MZ_MCP_REEXEC`` v prostředí
# (dítě dědí aktuální env), ne porovnání cest interpretů: na Linuxu je
# ``.venv/bin/python`` symlink na systémový python, takže jeho ``resolve()``
# je shodný se ``sys.executable`` a porovnání by re-exec chybně potlačilo
# (na MSYS2 je venv python ``.exe`` kopie s odlišnou cestou, proto tam
# porovnání fungovalo). Sentinel zaručí právě jeden re-exec a funguje
# stejně na obou platformách.
try:
    import mcp  # noqa: F401
except ImportError:
    _venv = _find_venv_python()
    if _venv is not None and not os.environ.get("MZ_MCP_REEXEC"):
        os.environ["MZ_MCP_REEXEC"] = "1"
        sys.exit(subprocess.call(
            [str(_venv), str(Path(__file__).resolve()), *sys.argv[1:]]))
    print("SKIP: FastMCP/mcp není dostupný (.venv chybí nebo nemá mcp)",
          file=sys.stderr)
    sys.exit(SKIP_EXIT)

# Import mcp_server.py z mcp-server/ adresáře (sourozenec tests/).
sys.path.insert(0, str(MCP_DIR))
# mcp_server čte MZ800EMU_TRANSPORT při importu; pipe se neconnectne,
# transport přepíšeme až za běhu na tcp + mock port.
os.environ.setdefault("MZ800EMU_TRANSPORT", "pipe")
import mcp_server as m  # noqa: E402


# Region 1 (User RAM) má 65536 bajtů; base64 ~87.4 KB -> jeden JSONL
# řádek přes default 64 KiB limit StreamReaderu.
BIG_REGION_LEN = 65536


async def _mock_emu_server(host: str, port_holder: list):
    """Mock TCP server simulující emu MCP backend.

    Pošle hello, pak na každý request odpoví JSONL response. region_read
    vrací data_b64 dané `length` (velký řádek), mem_read malý.
    """
    async def handler(reader, writer):
        hello = {"type": "hello", "version": "test",
                 "commands": ["region_read", "mem_read"]}
        writer.write((json.dumps(hello) + "\n").encode("utf-8"))
        await writer.drain()
        try:
            while True:
                try:
                    line = await reader.readline()
                except Exception:
                    break
                if not line:
                    break
                try:
                    req = json.loads(line)
                except json.JSONDecodeError:
                    continue
                cmd = req.get("cmd")
                rid = req.get("req_id")
                d = req.get("data", {}) or {}
                if cmd == "region_read":
                    n = int(d.get("length", 256))
                    b64 = base64.b64encode(bytes(n)).decode("ascii")
                    resp = {"req_id": rid, "success": True,
                            "data": {"region_id": d.get("region_id"),
                                     "offset": d.get("offset", 0),
                                     "length": n, "data_b64": b64}}
                elif cmd == "mem_read":
                    n = int(d.get("len", 8))
                    b64 = base64.b64encode(bytes(n)).decode("ascii")
                    resp = {"req_id": rid, "success": True,
                            "data": {"addr": d.get("addr", 0), "len": n,
                                     "data_b64": b64}}
                else:
                    resp = {"req_id": rid, "success": True, "data": {}}
                writer.write((json.dumps(resp) + "\n").encode("utf-8"))
                await writer.drain()
        finally:
            # Uzavři connection writer, jinak na Pythonu 3.12+ čeká
            # Server.wait_closed() navždy na dokončení tohoto spojení
            # (změna chování oproti <3.12, kde wait_closed na spojení
            # nečekal - proto hang nevznikal na MSYS2 se starším Pythonem).
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass

    server = await asyncio.start_server(handler, host, 0)
    port_holder.append(server.sockets[0].getsockname()[1])
    return server


def _reset_bridge_globals():
    """Vynuluje globální stav bridge mezi testy."""
    m._transport = None
    m._reader_task = None
    m._response_queue = None
    m._event_queue = None
    m._send_lock = None
    m._next_req_id = 1


async def test_a_big_region_read_does_not_wedge() -> None:
    """Velký region_read projde a bridge zůstane živý (Defekt 1+2)."""
    host = "127.0.0.1"
    ph: list = []
    server = await _mock_emu_server(host, ph)
    port = ph[0]

    m.TRANSPORT_KIND = "tcp"
    m.TCP_HOST = host
    m.TCP_PORT = port
    m.SEND_TIMEOUT_S = 5.0  # rychlý fail při zaseknutém readeru
    _reset_bridge_globals()

    try:
        await m._ensure_connected()

        # Velký region read - s bugem shodí reader task -> timeout.
        resp1 = await m._send_request(
            "region_read",
            {"region_id": 1, "offset": 0, "length": BIG_REGION_LEN})
        data1 = resp1.get("data", {})
        assert "data_b64" in data1, \
            f"velký region_read nevrátil data: {resp1!r}"
        got = len(base64.b64decode(data1["data_b64"]))
        assert got == BIG_REGION_LEN, \
            f"velký region_read: {got} != {BIG_REGION_LEN} bajtů"

        # Bridge musí být dál živý - následný mem_read nesmí být zaseknutý.
        resp2 = await m._send_request("mem_read", {"addr": 0, "len": 8})
        data2 = resp2.get("data", {})
        assert "data_b64" in data2, \
            f"bridge zaseknutý po velkém region_read: {resp2!r}"
    finally:
        try:
            await m._shutdown_emu()
        except Exception:
            pass
        server.close()
        await server.wait_closed()


async def test_b_error_response_is_surfaced() -> None:
    """emu_region_read surfacuje error response, ne holé {} (Defekt 3)."""
    orig = m._send_request

    async def _fake_send(cmd, data=None):
        return {"req_id": 1, "success": False,
                "error": "REGIONS_READ failed (bad region_id / offset)"}

    m._send_request = _fake_send
    try:
        out = await m.emu_region_read(region_id=1, offset=0, length=8)
    finally:
        m._send_request = orig
    obj = json.loads(out)
    assert "error" in obj, \
        f"error response se nesurfacoval, dostal jsem: {out!r}"


async def test_c_send_request_self_heals_desync() -> None:
    """_send_request přeskočí stale response a vrátí matching req_id.

    Reprodukuje off-by-one desync (Defekt 2): pozdní response z dříve
    vytimeoutovaného requestu zůstane ve frontě. Další _send_request ji
    musí zahodit a počkat na response se správným req_id, ne ji vrátit.
    """
    host = "127.0.0.1"
    ph: list = []
    server = await _mock_emu_server(host, ph)
    port = ph[0]

    m.TRANSPORT_KIND = "tcp"
    m.TCP_HOST = host
    m.TCP_PORT = port
    m.SEND_TIMEOUT_S = 5.0
    _reset_bridge_globals()

    try:
        await m._ensure_connected()
        # Naseed stale response s req_id nižším než příští (_next_req_id).
        stale_rid = m._next_req_id - 1
        await m._response_queue.put(
            {"req_id": stale_rid, "success": True,
             "data": {"stale": True}})
        # Tento request dostane req_id == _next_req_id; mock odpoví se
        # správným req_id. Stale ve frontě se musí zahodit.
        expected_rid = m._next_req_id
        resp = await m._send_request("mem_read", {"addr": 0, "len": 8})
        assert resp.get("req_id") == expected_rid, \
            f"vrácen stale response: očekáván req_id {expected_rid}, " \
            f"dostal {resp.get('req_id')} ({resp!r})"
    finally:
        try:
            await m._shutdown_emu()
        except Exception:
            pass
        server.close()
        await server.wait_closed()


async def test_d_class_a_tools_surface_error() -> None:
    """Reprezentativní tooly dříve třídy (a) surfacují error, ne {}.

    Regresní guard pro H1 (jednotná návratová konvence): po hromadné
    záměně ``resp.get("data", {})`` -> ``_data_or_error(resp)`` nesmí
    žádný tool na backend error (``success==false``, bez ``data``)
    vrátit holé ``{}``. Testuje vzorek napříč rodinami (V0, CPU-details,
    eventlog) přes monkeypatch ``_send_request`` na error response.
    """
    orig = m._send_request

    async def _fake_send(cmd, data=None):
        return {"req_id": 1, "success": False,
                "error": f"backend error for cmd={cmd}"}

    # Vzorek toolů bez transport-guardu (volají rovnou _send_request) a
    # bez povinných argumentů. Pokrývá rodiny V0 / V1.E.2 / eventlog.
    sample = [
        ("emu_pause", m.emu_pause),
        ("emu_get_registers", m.emu_get_registers),
        ("emu_reset", m.emu_reset),
        ("emu_eventlog_start", m.emu_eventlog_start),
    ]
    m._send_request = _fake_send
    try:
        for name, fn in sample:
            out = await fn()
            obj = json.loads(out)
            assert "error" in obj, \
                f"{name} nesurfacoval error, dostal jsem: {out!r}"
            assert obj != {}, \
                f"{name} vrátil holé {{}} na chybu backendu"
    finally:
        m._send_request = orig


async def _amain() -> int:
    tests = [
        ("A: velký region_read nezasekne bridge",
         test_a_big_region_read_does_not_wedge),
        ("B: error response se surfacuje (ne holé {})",
         test_b_error_response_is_surfaced),
        ("C: _send_request self-heal off-by-one desync",
         test_c_send_request_self_heals_desync),
        ("D: tooly třídy (a) surfacují error (H1 konvence)",
         test_d_class_a_tools_surface_error),
    ]
    failed = 0
    for name, fn in tests:
        try:
            await fn()
            print(f"OK   {name}")
        except Exception as e:
            failed += 1
            print(f"FAIL {name}: {e}", file=sys.stderr)
    if failed:
        print(f"\n{failed}/{len(tests)} testů selhalo", file=sys.stderr)
        return 1
    print(f"\nvšech {len(tests)} testů PASS")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(_amain()))
