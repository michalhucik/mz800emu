#!/usr/bin/env python3
"""Test běhu ukončeného podmínkou: návratové kódy, result.json, akce exit.

Spouští binárky s ``--headless`` v izolovaném prostředí (dočasný
``--cfg-dir`` / ``--work-dir``, ``--no-save-ini``, cwd = dočasná složka)
a testovací program vyrobený v testu (MZF na 1200h):

    1200  DI
    1201  LD A,40h ; LD (3000h),A           zápis 40h
    1206  LD HL,0                            vnější smyčka
    1209  INC HL ; LD A,H ; CP 8 ; JR NZ     vnitřní smyčka (asi 1 snímek)
    120F  CALL 1230h                         značka (5 zásahů)
    1212  LD A,(3000h) ; INC A ; LD (3000h),A   zápisy 41h-45h
    1219  CP 45h ; JR NZ,1206h
    121D  LD A,0A5h ; OUT (0F2h),A           výstup pro --exit-on-out
    1221  JR $                               konec
    1230  RET

Případy:
  1. --exit-after-frames + --automation-dir: kód 0, result.json (schéma,
     snímky, registry, hashe), screen.png;
  2. --timeout-frames bez jiné podmínky: kód 2;
  3. --exit-on-pc se skip: kód 0, trigger.hits = skip + 1, option;
  4. --fail-on-pc: kód 1;
  5. --exit-on-out: kód = zapsaná hodnota (0A5h);
  6. --exit-on-write ADDR=VAL: kód = VAL, code=C přebije;
  7. dvě podmínky: vyhraje dřívější (PC před timeoutem i naopak);
  8. --bpt se sadou s akcí exit; uživatelův .bpt se při konci nepřepíše;
  9. chyby voleb: v automatizaci kód 3 + result.json reason error, bez
     automatizace dál 1; chybějící --run-mzf = 3;
 10. řízený konec s nenulovým kódem dopíše artefakty (N5): timeout
     s --audio-capture (capture.json) a --exit-snapshot;
 11. determinismus: dva stejné běhy dají stejné snímky, PC a hashe;
 12. MZ-700 PAL / NTSC a MZ-1500: --exit-on-write a --exit-after-frames;
 13. --mcp-pipe: akce exit z MCP jen pozastaví (stopped_by "exit"),
     proces žije; akce stop zastaví jako breakpoint;
 14. headless s MCP TCP bez automatizační volby: exit jen pauza; s
     automatizační volbou proces skončí s kódem;
 15. automatický běh nenačte ani nepřepíše výchozí .bpt (zbytek z GUI
     s akcí exit 9 výsledek neovlivní).

MCP TCP port: volný od systému (nikdy 23800). Výstup anglicky.
Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_snapshot_gdg_event import (  # noqa: E402
    PipeEmu, TestFailure, _find_exe, check)
from test_cli_speed import TcpEmu  # noqa: E402 - headless TCP s vlastními volbami

PROG = 0x1200
PROG_BIN = bytes.fromhex(
    "F3" "3E40" "320030"
    "210000"
    "23" "7C" "FE08" "20FA"
    "CD3012"
    "3A0030" "3C" "320030"
    "FE45" "20E9"
    "3EA5" "D3F2"
    "18FE") + bytes(0x1230 - 0x1223) + bytes.fromhex("C9")
MARK = 0x1230
END = 0x1221
#: Snímků na doběhnutí programu do konce je asi 5; timeout pro "nic nenastalo".
PLATFORMS = ("mz700emu-pal", "mz700emu-ntsc", "mz1500emu")


def make_mzf(path):
    """Zapíše testovací program jako MZF (typ 01h, načtení i start na 1200h)."""
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"EXITTEST\x0d"
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(PROG_BIN).to_bytes(2, "little")
    hdr[0x14:0x16] = PROG.to_bytes(2, "little")
    hdr[0x16:0x18] = PROG.to_bytes(2, "little")
    path.write_bytes(bytes(hdr) + PROG_BIN)


class Run:
    """Jeden headless běh do konce procesu.

    Aktuální adresář procesu je ``self.dir``; ``--work-dir`` je ``self.dir``,
    nebo jeho podadresář ``work_dir`` (relativní cesty voleb se pak dají
    odlišit od pracovního adresáře).
    """

    def __init__(self, exe, tmp, name, extra, mzf=True, timeout=60,
                 ini_text="[BREAKPOINTS]\nauto_load = 0\n", cfg_files=None, work_dir=None):
        self.dir = tmp / name
        self.dir.mkdir()
        wdir = self.dir
        if work_dir:
            wdir = self.dir / work_dir
            wdir.mkdir()
        cfg = self.dir / "cfg"
        cfg.mkdir()
        for fname, text in (cfg_files or {}).items():
            (cfg / fname).write_text(text, encoding="utf-8")
        ini = cfg / "emu.ini"
        ini.write_text(ini_text, encoding="utf-8")
        args = [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
                f"--cfg-dir={cfg}", f"--work-dir={wdir}", f"--config={ini}",
                "--speed", "max"]
        if mzf:
            args += ["--run-mzf", str(tmp / "exittest.mzf")]
        args += [str(a) for a in extra]
        self.log = self.dir / "emu.log"
        t0 = time.monotonic()
        with open(self.log, "w", encoding="utf-8", errors="replace") as log:
            proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, cwd=str(self.dir))
            try:
                self.code = proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                emu_test_proc.kill_proc(proc)
                raise TestFailure(f"{name}: emulator did not exit within {timeout} s")
        self.seconds = time.monotonic() - t0
        res = self.dir / "result.json"
        self.result = json.loads(res.read_text(encoding="utf-8")) if res.is_file() else None

    def tail(self):
        """Konec logu emulátoru pro chybové hlášky."""
        return self.log.read_text(encoding="utf-8", errors="replace")[-600:]


def run(exe, tmp, name, *extra, **kw):
    """Běh s --result-json do jeho složky."""
    return Run(exe, tmp, name, ["--result-json", "result.json"] + list(extra), **kw)


def case_frames(exe, tmp):
    print("Case 1: --exit-after-frames with --automation-dir")
    r = Run(exe, tmp, "frames", ["--exit-after-frames", "20", "--automation-dir", "out"])
    out = r.dir / "out"
    res = json.loads((out / "result.json").read_text(encoding="utf-8"))
    check(r.code == 0, f"exit code 0 ({r.code})")
    check(res["schema"] == "mz800emu-result/1" and res["reason"] == "frames"
          and res["exit_code"] == 0, f"schema, reason frames ({res['reason']})")
    check(res["time"]["frames"] == 20, f"frames since start = 20 ({res['time']['frames']})")
    check(res["trigger"] == {"kind": "frames", "option": "--exit-after-frames"},
          f"trigger ({res['trigger']})")
    cpu = res["cpu"]
    check(isinstance(cpu["pc"], int) and 0x1200 <= cpu["pc"] <= 0x1240, f"PC in the test program ({cpu['pc']})")
    h = res["hashes"]
    check(h["algorithm"] == "sha256" and all(len(h[k]) == 64 for k in ("ram", "vram", "screen", "cpu")),
          f"hashes ram/vram/screen/cpu ({sorted(h)})")
    check((out / "screen.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", "screen.png written")
    check(res["artifacts"]["screenshot"] and Path(res["artifacts"]["screenshot"]).name == "screen.png",
          "artifacts.screenshot")
    check(res["inputs"]["run_mzf"].endswith("exittest.mzf"), "inputs.run_mzf")
    return res


def case_timeout(exe, tmp):
    print("Case 2: --timeout-frames without another condition")
    r = run(exe, tmp, "timeout", "--timeout-frames", "15")
    check(r.code == 2 and r.result["reason"] == "timeout" and r.result["time"]["frames"] == 15,
          f"exit code 2, reason timeout, 15 frames ({r.code}, {r.result and r.result['reason']})")
    r = run(exe, tmp, "timeout_s", "--timeout-seconds", "0.21")
    check(r.code == 2 and r.result["time"]["frames"] == 11,
          f"--timeout-seconds 0.21 = 11 frames (rounded up) ({r.result['time']['frames']})")


def case_exit_on_pc(exe, tmp):
    print("Case 3/4: --exit-on-pc with skip, --fail-on-pc")
    r0 = run(exe, tmp, "pc0", "--exit-on-pc", f"{MARK:04X}h")
    r = run(exe, tmp, "pc", "--exit-on-pc", f"{MARK:04X}h,skip=2", "--timeout-frames", "100")
    t = r.result["trigger"]
    check(r.code == 0 and r.result["reason"] == "exit_action", f"exit code 0 ({r.code})")
    # Zásahy breakpointu se počítají až po přeskočených (skip_count).
    check(t["kind"] == "breakpoint" and t["option"] == "--exit-on-pc" and t["hits"] == 1
          and t["bp_type"] == "PC_EXEC" and t["bp_name"] == "cli-exit-on-pc-1",
          f"trigger: option, name, 1 counted hit ({t})")
    check(r.result["cpu"]["pc"] == MARK, f"stopped at {MARK:04X}h ({r.result['cpu']['pc']:04X})")
    t0 = r0.result["time"]["gdg_ticks"]
    t2 = r.result["time"]["gdg_ticks"]
    check(t2 > 2 * t0, f"skip=2 ends at the third pass, later than skip=0 ({t2} vs {t0} ticks)")
    r = run(exe, tmp, "pc_code", "--exit-on-pc", f"0x{MARK:04X},code=99")
    check(r.code == 99, f"code=99 ({r.code})")
    r = run(exe, tmp, "fail", "--fail-on-pc", f"{MARK}")
    check(r.code == 1 and r.result["trigger"]["option"] == "--fail-on-pc", f"--fail-on-pc: exit code 1 ({r.code})")


def case_exit_on_out_write(exe, tmp):
    print("Case 5/6: --exit-on-out, --exit-on-write")
    r = run(exe, tmp, "out", "--exit-on-out", "0F2h")
    check(r.code == 0xA5 and r.result["trigger"]["bp_type"] == "IORQ_W",
          f"--exit-on-out: exit code = written value A5h ({r.code})")
    r = run(exe, tmp, "write", "--exit-on-write", "3000h=43h")
    check(r.code == 0x43, f"--exit-on-write 3000h=43h: exit code 43h ({r.code})")
    r = run(exe, tmp, "write_code", "--exit-on-write", "0x3000=0x44,code=200")
    check(r.code == 200 and r.result["trigger"]["hits"] == 1,
          f"code=200 overrides the value, condition Value == 44h hit once ({r.code}, {r.result['trigger']})")


def case_two_conditions(exe, tmp):
    print("Case 7: two conditions - the earlier one wins")
    r = run(exe, tmp, "first_pc", "--exit-on-pc", f"{MARK:04X}h", "--timeout-frames", "50")
    check(r.code == 0 and r.result["reason"] == "exit_action", f"PC before timeout ({r.code})")
    r = run(exe, tmp, "first_timeout", "--exit-on-pc", f"{END:04X}h", "--timeout-frames", "2")
    check(r.code == 2 and r.result["reason"] == "timeout", f"timeout before PC ({r.code})")


def case_bpt(exe, tmp):
    print("Case 8: --bpt with an exit action; the user's .bpt is not overwritten")
    src = tmp / "set.bpt"
    src.write_text(json.dumps({
        "breakpoints": [{"id": 1, "name": "done", "addr": END, "type": "PC_EXEC",
                         "enabled": True, "skip_count": 0, "hit_count": 0, "expr": None,
                         "action": "exit 77, \"reached %X\", PC", "edge_triggered": False}],
        "groups": []}), encoding="utf-8")
    r = run(exe, tmp, "bpt", "--bpt", str(src))
    if r.code != 77:
        # Formát .bpt se mohl změnit: ukaž log pro diagnózu.
        print(r.tail())
    check(r.code == 77 and r.result["message"] == f"reached {END:X}",
          f"exit 77 with message from the action ({r.code}, {r.result and r.result['message']})")
    check(r.result["inputs"]["bpt"].endswith("set.bpt"), "inputs.bpt")
    check(not any(p.suffix in (".bpt", ".vars") for p in (r.dir / "cfg").iterdir()),
          "no breakpoint or variable file saved into the config directory at exit")


def case_default_bpt_ignored(exe, tmp):
    print("Case 15: unattended run ignores the automatically loaded breakpoint set")
    # Zbytek z GUI relace: výchozí .bpt s breakpointem exit 9 na značce
    # a automatické načtení zapnuté. Automatický běh ho nesmí načíst ani přepsat.
    bpt = json.dumps({
        "breakpoints": [{"id": 1, "name": "leftover", "addr": MARK, "type": "PC_EXEC",
                         "enabled": True, "skip_count": 0, "hit_count": 0, "expr": None,
                         "action": "exit 9", "edge_triggered": False}],
        "groups": []})
    r = run(exe, tmp, "default_bpt", "--exit-after-frames", "20",
            ini_text="[BREAKPOINTS]\nauto_load = 1\nauto_save = 1\n",
            cfg_files={"mz800-breakpoints.bpt": bpt})
    check(r.code == 0 and r.result["reason"] == "frames",
          f"leftover breakpoint not loaded: exit code 0, reason frames ({r.code}, {r.result['reason']})")
    saved = (r.dir / "cfg" / "mz800-breakpoints.bpt").read_text(encoding="utf-8")
    check(saved == bpt, "the default breakpoint file is not overwritten at exit")


def case_errors(exe, tmp):
    print("Case 9: option errors")
    r = run(exe, tmp, "bad", "--exit-on-pc", "12G4", mzf=False)
    check(r.code == 3 and r.result["reason"] == "error" and r.result["exit_code"] == 3
          and "cpu" not in r.result, f"invalid spec: exit code 3, result.json reason error ({r.code})")
    r = run(exe, tmp, "unknown", "--no-such-option", mzf=False)
    check(r.code == 3 and r.result["reason"] == "error", f"unknown option in automation mode: 3 ({r.code})")
    r = Run(exe, tmp, "compat", ["--no-such-option"], mzf=False)
    check(r.code == 1, f"unknown option without automation: still 1 ({r.code})")
    r = Run(exe, tmp, "nomzf", ["--result-json", "result.json", "--run-mzf", "missing.mzf",
                                "--exit-after-frames", "5"], mzf=False)
    check(r.code == 3 and r.result["reason"] == "error", f"missing --run-mzf file: 3 ({r.code})")


def case_artifacts(exe, tmp):
    print("Case 10: exit with a non-zero code completes artifacts (N5)")
    r = run(exe, tmp, "artifacts", "--timeout-frames", "10", "--audio-capture", "audio",
            "--exit-snapshot", "final.mzs")
    check(r.code == 2, f"exit code 2 ({r.code})")
    cap = r.dir / "audio" / "capture.json"
    check(cap.is_file(), "audio capture finished (capture.json written at exit)")
    check((r.dir / "final.mzs").is_file() and r.result["artifacts"]["snapshot"],
          "final snapshot written and listed")
    check(r.result["artifacts"]["audio_capture"].endswith("audio"), "artifacts.audio_capture")


def case_determinism(exe, tmp, first):
    print("Case 11: determinism of two equal runs")
    r = Run(exe, tmp, "frames2", ["--exit-after-frames", "20", "--automation-dir", "out"])
    res = json.loads((r.dir / "out" / "result.json").read_text(encoding="utf-8"))
    for key in ("time", "cpu", "hashes", "raster"):
        a = {k: v for k, v in first[key].items() if k != "wall_seconds"}
        b = {k: v for k, v in res[key].items() if k != "wall_seconds"}
        check(a == b, f"{key} equal")


def case_platforms(tmp):
    print("Case 12: MZ-700 PAL / NTSC and MZ-1500")
    for name in PLATFORMS:
        exe = _find_exe(name)
        if not exe:
            raise TestFailure(f"{name} binary not found")
        r = run(exe, tmp, f"{name}_write", "--exit-on-write", "3000h=42h")
        check(r.code == 0x42, f"{name}: --exit-on-write exit code 42h ({r.code})")
        r = run(exe, tmp, f"{name}_frames", "--exit-after-frames", "7")
        check(r.code == 0 and r.result["time"]["frames"] == 7
              and r.result["emulator"]["screens_per_second"] == (60 if "ntsc" in name or "1500" in name else 50),
              f"{name}: 7 frames ({r.result['time']['frames']}, {r.result['emulator']['screens_per_second']} fps)")


def _load_prog(emu):
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_BIN.hex()})
    emu.call("set_register", {"reg": "PC", "value": PROG})


def case_pipe(exe, tmp):
    print("Case 13: --mcp-pipe - exit only pauses, stop stops")
    d = tmp / "pipe"
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        _load_prog(emu)
        res = emu.call("bp_create_with_init", {"fields": ["type", "addr", "action"],
                                               "type": "PC_EXEC", "addr": MARK,
                                               "action": "exit 66, \"pipe %d\", 1"})
        check(res.get("created") is True, "breakpoint with exit action created")
        r = emu.call("run", {"frames": 50})
        check(r.get("stopped_by") == "exit", f"run stopped_by exit ({r.get('stopped_by')})")
        st = emu.call("get_state")
        check(st.get("paused") is True and emu.proc.poll() is None,
              f"process alive and paused after exit ({st.get('paused')})")
        emu.call("bp_remove", {"id": res["id"]})
        res = emu.call("bp_create_with_init", {"fields": ["type", "addr", "action"],
                                               "type": "PC_EXEC", "addr": END,
                                               "action": "stop \"end reached\"; log \"not run\""})
        r = emu.call("run", {"frames": 50})
        check(r.get("stopped_by") == "breakpoint", f"stop: stopped_by breakpoint ({r.get('stopped_by')})")
    finally:
        emu.close()
    check(emu.proc.returncode == 0, f"pipe process ended by shutdown with 0 ({emu.proc.returncode})")


def case_tcp(exe, tmp):
    print("Case 14: headless with MCP TCP")
    d = tmp / "tcp"
    d.mkdir()
    ini = d / "emu.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load = 0\n", encoding="utf-8")
    emu = TcpEmu(exe, d, ini, None, [])
    try:
        _load_prog(emu)
        emu.call("bp_create_with_init", {"fields": ["type", "addr", "action"],
                                         "type": "PC_EXEC", "addr": MARK, "action": "exit 70"})
        r = emu.call("run", {"frames": 50})
        check(r.get("stopped_by") == "exit", f"TCP without automation option: only paused ({r.get('stopped_by')})")
        check(emu.proc.poll() is None, "process still running")
    finally:
        emu.close()
    d2 = tmp / "tcp_auto"
    d2.mkdir()
    port = emu_test_proc.free_tcp_port()
    r = Run(exe, tmp, "tcp_auto_run", [f"--mcp-tcp-port={port}", "--exit-on-pc", f"{MARK:04X}h,code=71"])
    check(r.code == 71, f"TCP with an automation option: process ends with the code ({r.code})")


def main():
    emu_test_proc.install(deadline_s=280)
    exe = _find_exe("mz800emu")
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_headless_exit_"))
    make_mzf(tmp / "exittest.mzf")
    ok = False
    try:
        first = case_frames(exe, tmp)
        case_timeout(exe, tmp)
        case_exit_on_pc(exe, tmp)
        case_exit_on_out_write(exe, tmp)
        case_two_conditions(exe, tmp)
        case_bpt(exe, tmp)
        case_default_bpt_ignored(exe, tmp)
        case_errors(exe, tmp)
        case_artifacts(exe, tmp)
        case_determinism(exe, tmp, first)
        case_platforms(tmp)
        case_pipe(exe, tmp)
        case_tcp(exe, tmp)
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        shutil.rmtree(tmp, ignore_errors=True)
        print("PASS")
    else:
        print(f"Artifacts kept in {tmp}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
