#!/usr/bin/env python3
"""Test: MCP wrapper čte čísla sekce [MCP] z INI jako emulátor (N-D5-1).

Emulátor (cfgfile, typ UNSIGNED) ukládá ``wrapper_log_rotate_size_mb``
a ``wrapper_log_rotate_keep`` jako ``0x..`` a čte je vždy šestnáctkově,
i bez prefixu. Wrapper je dřív četl desítkově (``int("0x32")`` selže),
takže nastavení rotace logu z GUI se neprojevilo (vždy 10 MB / 5 souborů).

Test nespouští wrapper ani emulátor (balík ``mcp`` nemusí být nainstalovaný):
z ``mcp-server/mcp_server.py`` přes ``ast`` vezme jen pomocné funkce
čtení INI a ``_build_log_handler()`` a ověří:

  - ``_build_log_handler()`` s ``kind = size``, ``size_mb = 0x32``,
    ``keep = 0x03`` vyrobí RotatingFileHandler 50 MB, 3 zálohy;
  - zápis bez prefixu se čte šestnáctkově (``size_mb = 50`` = 80 MB,
    ``keep = 0a`` = 10), ``#0A`` = 10;
  - neplatný zápis, chybějící klíč a hodnota mimo rozsah emulátoru
    (size 1-10240, keep 0-1000) dají výchozí hodnotu 10 MB / 5.

Výstup anglicky. Exit code: 0 = PASS, 1 = FAIL.
"""

import ast
import logging
import logging.handlers
import sys
import tempfile
from pathlib import Path
from typing import Any, Optional

#: Zdroj wrapperu.
WRAPPER = Path(__file__).resolve().parents[2] / "mcp-server" / "mcp_server.py"
#: Funkce, které test z wrapperu vezme (které existují).
FUNCS = ("_safe_int", "_ini_unsigned", "_build_log_handler")

_failed = 0


def check(cond, what):
    """Vypíše výsledek kontroly a započítá selhání."""
    global _failed
    print(("  PASS: " if cond else "  FAIL: ") + what)
    if not cond:
        _failed += 1


def load_funcs():
    """Vrátí jmenný prostor s vybranými funkcemi wrapperu."""
    tree = ast.parse(WRAPPER.read_text(encoding="utf-8"), filename=str(WRAPPER))
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in FUNCS]
    ns = {"logging": logging, "Any": Any, "Optional": Optional, "Path": Path}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(WRAPPER), "exec"), ns)
    return ns


def handler(ns, tmp, **cfg):
    """(maxBytes, backupCount) handleru z _build_log_handler()."""
    cfg = {"wrapper_log_rotate_kind": "size", **cfg}
    h = ns["_build_log_handler"](cfg, str(tmp / "w.log"))
    try:
        check(isinstance(h, logging.handlers.RotatingFileHandler),
              f"size rotation gives RotatingFileHandler ({type(h).__name__})")
        return getattr(h, "maxBytes", None), getattr(h, "backupCount", None)
    finally:
        h.close()


def main():
    ns = load_funcs()
    mb = 1024 * 1024
    with tempfile.TemporaryDirectory(prefix="mz_wrapper_ini_") as d:
        tmp = Path(d)
        cases = [
            ({"wrapper_log_rotate_size_mb": "0x32", "wrapper_log_rotate_keep": "0x03"},
             (50 * mb, 3), "values written by the emulator (0x32, 0x03)"),
            ({"wrapper_log_rotate_size_mb": "50", "wrapper_log_rotate_keep": "0a"},
             (0x50 * mb, 10), "values without prefix are hexadecimal (50, 0a)"),
            ({"wrapper_log_rotate_size_mb": "0X10", "wrapper_log_rotate_keep": "#0A"},
             (16 * mb, 10), "prefixes 0X and #"),
            ({}, (10 * mb, 5), "missing keys give the defaults"),
            ({"wrapper_log_rotate_size_mb": "zz", "wrapper_log_rotate_keep": ""},
             (10 * mb, 5), "invalid values give the defaults"),
            ({"wrapper_log_rotate_size_mb": "0x0", "wrapper_log_rotate_keep": "0x3e9"},
             (10 * mb, 5), "values out of the emulator range give the defaults"),
            ({"wrapper_log_rotate_size_mb": "0x2800", "wrapper_log_rotate_keep": "0x0"},
             (10240 * mb, 0), "range limits are accepted"),
        ]
        for cfg, want, what in cases:
            got = handler(ns, tmp, **cfg)
            check(got == want, f"{what}: {got} == {want}")
    if _failed:
        print(f"FAIL ({_failed} checks)")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
