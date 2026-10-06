#!/usr/bin/env python3
"""E2E test: rozsahové breakpointy a jejich výpis (MCP pipe).

Breakpoint vytvořený s ``addr_end``, ale bez ``addr_match_mode`` RANGE,
hlídá jen ``addr`` (výchozí režim SINGLE). Dříve to ``bp_list`` neukázal
(chyběla pole ``addr_end`` / ``addr_match_mode``) a vytvoření proběhlo
bez upozornění - vypadalo to jako chyba MEM_W breakpointu. Ověřuje:

  - ``bp_create_with_init`` s ``addr_end`` != ``addr`` bez RANGE vrátí
    ``warning`` a BP vznikne (chování beze změny),
  - s ``addr_match_mode`` RANGE warning není,
  - ``bp_list`` u obou ukazuje ``addr_end``, ``addr_match_mode``
    a ``addr_mask``,
  - ``bp_list`` ukazuje i bankový rozsah (``bank_id_end``,
    ``bank_match_mode``, ``bank_id_mask``) MEM_W BP v zóně MMEXT_BANK.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402


def _create(emu, fields, values):
    """Vytvoří BP přes bp_create_with_init a vrátí data odpovědi."""
    data = {"fields": fields}
    data.update(values)
    res = emu.call("bp_create_with_init", data)
    check(res.get("created") is True and res.get("id", -1) >= 0,
          f"breakpoint created: {res}")
    return res


def _find_bp(emu, bp_id):
    """Vrátí záznam BP z bp_list podle id."""
    for bp in emu.call("bp_list").get("breakpoints") or []:
        if bp.get("id") == bp_id:
            return bp
    raise TestFailure(f"breakpoint {bp_id} not in bp_list")


def run(emu):
    """Ověří warning při vytvoření a pole rozsahu v bp_list."""
    single = _create(emu, ["type", "addr", "addr_end"],
                     {"type": "MEM_W", "addr": 0x2000, "addr_end": 0x20FF})
    check("warning" in single and "RANGE" in single["warning"],
          "addr_end without RANGE returns a warning")
    bp = _find_bp(emu, single["id"])
    check(bp.get("type") == "MEM_W", "type is MEM_W")
    check(bp.get("addr_match_mode") == "SINGLE" and bp.get("addr_end") == 0x20FF,
          f"bp_list shows SINGLE mode with addr_end: {bp}")
    check("addr_mask" in bp, "bp_list shows addr_mask")

    rng = _create(emu, ["type", "addr", "addr_end", "addr_match_mode"],
                  {"type": "MEM_W", "addr": 0x3000, "addr_end": 0x30FF,
                   "addr_match_mode": "RANGE"})
    check("warning" not in rng, "RANGE breakpoint has no warning")
    bp = _find_bp(emu, rng["id"])
    check(bp.get("addr_match_mode") == "RANGE" and bp.get("addr") == 0x3000
          and bp.get("addr_end") == 0x30FF,
          f"bp_list shows RANGE 0x3000..0x30FF: {bp}")
    check(bp.get("bank_match_mode") == "SINGLE" and "bank_id_end" in bp
          and "bank_id_mask" in bp, f"bp_list shows default bank fields: {bp}")

    bank = _create(emu, ["type", "addr", "zone", "bank_id", "bank_id_end",
                         "bank_match_mode"],
                   {"type": "MEM_W", "addr": 0x4000, "zone": "MMEXT_BANK",
                    "bank_id": 2, "bank_id_end": 5, "bank_match_mode": "RANGE"})
    bp = _find_bp(emu, bank["id"])
    check(bp.get("zone") == "MMEXT_BANK" and bp.get("bank_id") == 2
          and bp.get("bank_id_end") == 5 and bp.get("bank_match_mode") == "RANGE",
          f"bp_list shows bank RANGE 2..5: {bp}")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_bp_addr_range_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n",
                       encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            emu.call("pause")
            run(emu)
            ok = True
        finally:
            emu.close()
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        shutil.rmtree(tmp, ignore_errors=True)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
