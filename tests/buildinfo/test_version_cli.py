#!/usr/bin/env python3
"""Test CLI volby ``--version`` všech binárek emulátoru.

Argumenty: dvojice ``<jméno>=<cesta k binárce>`` (předává je ctest přes
generator expression ``$<TARGET_FILE:...>``), např.
``mz800emu=C:/.../build/build-mz800emu/mz800emu.exe``.

Pro každou binárku ověří:

  1. ``--version`` skončí s kódem 0 do 30 s (bez spuštění emulace/okna),
  2. výstup obsahuje verzi z ``src/emulator/cfgmain.h`` (číslo + tag),
  3. první řádek nese jméno binárky, emulovaný počítač a TV normu
     (např. ``mz700emu-ntsc - Sharp MZ-700 emulator (NTSC)``),
  4. přítomnost všech řádků výpisu (Revision, Build date, Source, Branch,
     Commit, Compiler, Toolchain, Features, SDL, GLib, Host OS),
  5. řádky Source a Commit odpovídají vygenerovanému
     ``src/build_revision/build_revision.c`` (původ zdrojů a hash commitu).

U ``mz800emu`` navíc ověří ``--mcp-pipe --version`` (volba musí fungovat
i s pipe transportem a nesmí spustit JSONL protokol).

Výstup anglicky. Exit code: 0 = PASS, 1 = FAIL.
"""

import re
import subprocess
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent.parent
_CFGMAIN_H = _REPO_ROOT / "src" / "emulator" / "cfgmain.h"
_BUILD_REVISION_C = _REPO_ROOT / "src" / "build_revision" / "build_revision.c"

# Očekávaný první řádek podle jména binárky.
_EXPECTED_HEAD = {
    "mz800emu": "mz800emu - Sharp MZ-800 emulator (PAL)",
    "mz700emu-pal": "mz700emu-pal - Sharp MZ-700 emulator (PAL)",
    "mz700emu-ntsc": "mz700emu-ntsc - Sharp MZ-700 emulator (NTSC)",
    "mz1500emu": "mz1500emu - Sharp MZ-1500 emulator (NTSC)",
}

# Klíče, které musí výpis obsahovat (každý na začátku řádku).
_REQUIRED_KEYS = [
    "Version:", "Revision:", "Build date:", "Source:", "Branch:", "Commit:",
    "Compiler:", "Toolchain:", "Target:", "Features:", "SDL:", "GLib:",
    "Host OS:",
]

_failures = []


def check(cond, what):
    """Vyhodnotí jednu kontrolu, vypíše PASS/FAIL a zapamatuje selhání."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        _failures.append(what)


def _expected_version():
    """Vrátí "<číslo> <tag>" z aktivních #define v cfgmain.h."""
    text = _CFGMAIN_H.read_text(encoding="utf-8")
    num = re.search(r'^\s*#define\s+CFGMAIN_EMULATOR_VERSION_NUM_STRING\s+"([^"]+)"', text, re.M)
    tag = re.search(r'^\s*#define\s+CFGMAIN_EMULATOR_VERSION_TAG\s+"([^"]+)"', text, re.M)
    if not num or not tag:
        print("ERROR: cannot parse version from cfgmain.h", file=sys.stderr)
        sys.exit(1)
    return f"{num.group(1)} {tag.group(1)}"


def _generated_values():
    """Vrátí (origin_name, commit) z vygenerovaného build_revision.c, nebo None."""
    try:
        text = _BUILD_REVISION_C.read_text(encoding="utf-8")
    except OSError:
        return None
    origin = re.search(r'build_revision_get_repo_origin_name\(void\) \{ return "([^"]*)"; \}', text)
    commit = re.search(r'build_revision_get_commit\(void\) \{ return "([^"]*)"; \}', text)
    if not origin or not commit:
        return None
    return origin.group(1), commit.group(1)


def _run(exe, args):
    """Spustí binárku a vrátí CompletedProcess (stdout/stderr jako text)."""
    return subprocess.run([str(exe)] + args, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=30,
                          stdin=subprocess.DEVNULL)


def _check_output(name, out, version, generated):
    """Zkontroluje obsah výpisu --version jedné binárky."""
    lines = out.splitlines()
    check(bool(lines) and lines[0] == _EXPECTED_HEAD[name],
          f"{name}: first line is '{_EXPECTED_HEAD[name]}'")
    check(re.search(r"^Version:\s+" + re.escape(version) + r"$", out, re.M) is not None,
          f"{name}: version '{version}' present")
    for key in _REQUIRED_KEYS:
        check(re.search(r"^" + re.escape(key), out, re.M) is not None,
              f"{name}: line '{key}' present")
    if generated:
        origin, commit = generated
        expected_source = {"NAS1": "NAS1 repository",
                           "GitHub": "GitHub (official upstream)"}.get(origin, "unknown")
        check(re.search(r"^Source:\s+" + re.escape(expected_source) + r"$", out, re.M) is not None,
              f"{name}: Source matches build_revision.c ({expected_source})")
        check(re.search(r"^Commit:\s+" + re.escape(commit) + r"( \(dirty: uncommitted changes\))?$",
                        out, re.M) is not None,
              f"{name}: Commit matches build_revision.c ({commit})")


def main():
    """Vstupní bod testu."""
    exes = {}
    for arg in sys.argv[1:]:
        name, _, path = arg.partition("=")
        if name not in _EXPECTED_HEAD or not path:
            print(f"ERROR: bad argument '{arg}' (expected <name>=<path>)", file=sys.stderr)
            return 1
        exes[name] = Path(path)
    if not exes:
        print("ERROR: no binaries given", file=sys.stderr)
        return 1

    version = _expected_version()
    generated = _generated_values()
    if generated is None:
        print("NOTE: build_revision.c not found or not parseable - origin/commit cross-check skipped")

    for name, exe in exes.items():
        print(f"[{name}] {exe}")
        if not exe.is_file():
            check(False, f"{name}: binary exists")
            continue
        try:
            res = _run(exe, ["--version"])
        except subprocess.TimeoutExpired:
            check(False, f"{name}: --version finished within 30 s")
            continue
        check(res.returncode == 0, f"{name}: exit code 0 (got {res.returncode})")
        _check_output(name, res.stdout, version, generated)

        if name == "mz800emu":
            try:
                res = _run(exe, ["--mcp-pipe", "--version"])
            except subprocess.TimeoutExpired:
                check(False, "mz800emu: --mcp-pipe --version finished within 30 s")
                continue
            check(res.returncode == 0, f"mz800emu: --mcp-pipe --version exit code 0 (got {res.returncode})")
            check(res.stdout.startswith(_EXPECTED_HEAD["mz800emu"]),
                  "mz800emu: --mcp-pipe --version prints version info (no JSONL hello)")

    if _failures:
        print(f"FAIL: {len(_failures)} check(s) failed")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
