#!/usr/bin/env python3
"""E2E test psaní textu přes ``input_send_keys`` (kódování ascii): CR a nenapsatelné znaky.

Klávesu CR v tabulce znaků napíše ``\\n``. Dřív se ``\\r`` (i ve vlastním
příkladu docstringu ``"RUN\\r"``) tiše zahodil a povel se nepotvrdil.

  A  ``"A\\r"``, ``"A\\r\\n"`` a ``"A\\n"`` napíšou totéž: dvě klávesy, A a CR
     (stejné ``keys_sent`` i ``total_frames``), ``skipped`` 0, bez varování.
  B  Program v RAM čte sloupec klávesy CR a při stisku uloží značku: ``"\\r"``
     ho stiskne stejně jako ``"\\n"``.
  C  Nenapsatelné znaky (řídicí znak BEL, ``č``) se přeskočí: ``skipped`` je
     jejich počet a ``warning`` jmenuje kódy (U+0007, U+010D); ostatní znaky
     se napíšou (tabulátor MZ-800 napíše, má klávesu TAB).
  D  ``key_names`` má ``skipped`` 0; neznámé jméno je dál chyba.
  E  Text nad mez 256 kláves: zbytek se nenapíše a ohlásí (``truncated``,
     ``warning``), i pro dvojici CR LF a pro ``key_names``.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, ``--no-save-ini``),
bez TCP portu; zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (bez binárky mz800emu).
"""

import base64
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_record import PipeEmu, TestFailure, check, _find_exe  # noqa: E402

#: Adresa programu, který čeká na klávesu CR.
PROG = 0x6000
#: Značka stisku CR (program sem zapíše 55h).
MARK = 0x6080
#: DI; loop: LD A,0F0h+0 (sloupec 0, LED vypnuto); LD (0E000h),A; LD A,(0E001h);
#: BIT 0,A; JR NZ,loop; LD A,55h; LD (6080h),A; done: JR done
#: CR je v matici sloupec 0, řádek 0 (pio8255_autotype_get_matrix(): {'\n', 0, 0, 0}).
PROG_BYTES = bytes([0xF3,
                    0x3E, 0xF0, 0x32, 0x00, 0xE0, 0x3A, 0x01, 0xE0,
                    0xCB, 0x47, 0x20, 0xF4,
                    0x3E, 0x55, 0x32, 0x80, 0x60, 0x18, 0xFE])


def _keys(emu, text, encoding="ascii"):
    """Pošle text a vrátí odpověď (data)."""
    return emu.call("input_send_keys", {"text": text, "encoding": encoding, "frame_per_key": 3})


def phase_a(emu):
    """\\r, \\r\\n a \\n napíšou totéž."""
    print("Phase A: CR typed by \\r, \\r\\n and \\n alike")
    results = [_keys(emu, t) for t in ("A\r", "A\r\n", "A\n")]
    for t, r in zip(("A\\r", "A\\r\\n", "A\\n"), results):
        check(r.get("keys_sent") == 2 and r.get("skipped") == 0 and "warning" not in r,
              f"{t}: two keys, nothing skipped ({r})")
    check(len({r.get("total_frames") for r in results}) == 1, "the same number of frames for all three")
    r = _keys(emu, "\r\n\r\n")
    check(r.get("keys_sent") == 2 and r.get("skipped") == 0, f"two CRLF pairs = two CR keys ({r})")


def phase_b(emu):
    """Program v RAM uvidí CR z \\r stejně jako z \\n."""
    print("Phase B: a program reading the CR key sees \\r as CR")
    for text in ("\r", "\n"):
        emu.call("pause")
        emu.call("mem_write", {"addr": MARK, "data_hex": "00"})
        emu.call("mem_write", {"addr": PROG, "data_hex": PROG_BYTES.hex()})
        emu.call("set_register", {"reg": "PC", "value": PROG})
        emu.call("run", {"frames": 3})
        mark = base64.b64decode(emu.call("mem_read", {"addr": MARK, "len": 1})["data_b64"])[0]
        check(mark == 0, f"no key yet ({mark:#04x})")
        _keys(emu, text)
        emu.call("run", {"frames": 2})
        mark = base64.b64decode(emu.call("mem_read", {"addr": MARK, "len": 1})["data_b64"])[0]
        check(mark == 0x55, f"{text!r} pressed the CR key ({mark:#04x})")


def phase_c(emu):
    """Nenapsatelné znaky se přeskočí a ohlásí."""
    print("Phase C: characters that cannot be typed are skipped and reported")
    r = _keys(emu, "A\x07Bč")
    check(r.get("keys_sent") == 2 and r.get("skipped") == 2, f"A and B typed, two skipped ({r})")
    w = r.get("warning") or ""
    check(w.startswith("Skipped 2 character(s) that cannot be typed:") and "U+0007" in w and "U+010D" in w,
          f"warning names the code points ({w!r})")


def phase_d(emu):
    """key_names: skipped 0, neznámé jméno je chyba."""
    print("Phase D: key_names")
    r = _keys(emu, json.dumps(["A", "RETURN"]), "key_names")
    check(r.get("keys_sent") == 2 and r.get("skipped") == 0 and r.get("truncated") == 0 and "warning" not in r,
          f"key_names ({r})")
    resp = emu.call_raw("input_send_keys", {"text": json.dumps(["NOPE"]), "encoding": "key_names"})
    check(not resp.get("success") and "Unknown key" in (resp.get("error") or ""), f"unknown name ({resp.get('error')})")


def phase_e(emu):
    """Text nad mez 256 kláves: zbytek se nenapíše a ohlásí (truncated, warning).

    Čtyři sekvence po 256 klávesách (300 znaků, 255 + CR LF, 256 + CR LF,
    260 jmen kláves) a jedna s 10 klávesami jsou přes 4000 emulovaných snímků; při
    reálném čase (50 snímků/s) by fáze trvala přes 80 s. Počty kláves na
    rychlosti emulace nezávisí (snímky jsou emulované), proto fáze běží na
    maximální rychlost a pak ji vrátí na normální.
    """
    print("Phase E: text over the limit of 256 keys")
    emu.call("set_speed", {"mode": "max"})
    try:
        _phase_e_checks(emu)
    finally:
        # jen úklid (fáze E je poslední): chyba návratu rychlosti nesmí
        # překrýt původní selhání fáze, proto krátký timeout a bez kontroly
        try:
            emu.call_raw("set_speed", {"mode": "normal"}, timeout=5.0)
        except (TestFailure, OSError):
            pass


def _phase_e_checks(emu):
    """Kontroly fáze E (běží na maximální rychlost, viz phase_e())."""
    r = _keys(emu, "A" * 300, "ascii")
    check(r.get("keys_sent") == 256 and r.get("truncated") == 44
          and "44 character(s) after the first 256 keys were not typed" in (r.get("warning") or ""),
          f"300 characters: 256 typed, 44 truncated ({ {k: r.get(k) for k in ('keys_sent', 'truncated', 'warning')} })")
    r = _keys(emu, "A" * 255 + "\r\n")
    check(r.get("keys_sent") == 256 and r.get("truncated") == 0 and "warning" not in r,
          f"a CR LF pair as the 256th key is one key, nothing truncated ({ {k: r.get(k) for k in ('keys_sent', 'truncated')} })")
    r = _keys(emu, "A" * 256 + "\r\n")
    check(r.get("keys_sent") == 256 and r.get("truncated") == 2,
          f"a CR LF pair after the limit is counted ({ {k: r.get(k) for k in ('keys_sent', 'truncated')} })")
    r = _keys(emu, "A" * 10)
    check(r.get("truncated") == 0, f"truncated is 0 under the limit ({r.get('truncated')})")
    r = _keys(emu, json.dumps(["A"] * 260), "key_names")
    check(r.get("keys_sent") == 256 and r.get("truncated") == 4 and "4 " in (r.get("warning") or ""),
          f"260 key names: 4 truncated ({ {k: r.get(k) for k in ('keys_sent', 'truncated', 'warning')} })")


def main():
    emu_test_proc.install(deadline_s=120)
    exe = _find_exe("mz800emu")
    if exe is None:
        print("SKIP: mz800emu binary not found")
        return 77
    failures = []
    with tempfile.TemporaryDirectory(prefix="mz_send_keys_") as td:
        tmp = Path(td)
        emu = PipeEmu(exe, tmp / "cfg")
        try:
            emu.call("run", {"frames": 100})
            for label, fn in (("A", phase_a), ("B", phase_b), ("C", phase_c), ("D", phase_d), ("E", phase_e)):
                try:
                    fn(emu)
                except TestFailure as exc:
                    failures.append(f"phase {label}: {exc}")
                    print(f"  FAIL: {exc}")
                    break
        finally:
            emu.close()
    if failures:
        print("FAILED:")
        for f in failures:
            print(f"  {f}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
