#!/usr/bin/env python3
"""CI gate: frozen preprocessor state of the mzhal conversion (krok 13).

Checks that per-arch / capability preprocessor tokens (MZARCH, MZTVSYS,
HAVE_*, CFG_HWEXT_*) appear OUTSIDE the per-arch subtrees only in files
listed in preproc_whitelist.txt (= the deliberate per-EXE glue / hot
core set). A new file using such a token fails the gate until it is
either converted to a g_mzhal runtime read or consciously whitelisted.

Also verifies that:
  - every TU listed in cmake/emucore_sources.txt exists,
  - the emucore poison header still poisons the expected identifiers,
  - the dead HAVE_JOY toggle is not reintroduced anywhere.

Exit code: 0 = state matches, 1 = violations (printed to stdout).
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WHITELIST_FILE = Path(__file__).resolve().parent / "preproc_whitelist.txt"

TOKEN_RE = re.compile(
    r"\b(MZARCH|MZTVSYS|HAVE_PSG|HAVE_PIOZ80|HAVE_JOY"
    r"|CFG_HWEXT_HAVE_FDC|CFG_HWEXT_HAVE_IDE8|CFG_HWEXT_HAVE_RAMDISK"
    r"|CFG_HWEXT_HAVE_QDISK)\b")
ARCH_DIR_RE = re.compile(r"^src/(emulator/mzarch|ui-imgui)/mz[0-9]+/")
POISONED = ["MZARCH", "MZTVSYS", "MZARCH_NAME", "MZTVSYS_NAME",
            "HAVE_PSG", "HAVE_PIOZ80", "HAVE_JOY",
            "CFG_HWEXT_HAVE_FDC", "CFG_HWEXT_HAVE_IDE8",
            "CFG_HWEXT_HAVE_RAMDISK", "CFG_HWEXT_HAVE_QDISK"]


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub("//[^\n]*", " ", text)
    return text


def live_token_files():
    """Files under src/ (outside per-arch dirs and libs) with live tokens."""
    hits = {}
    for p in sorted(REPO.glob("src/**/*")):
        if p.suffix not in (".c", ".cpp", ".h", ".hpp", ".inc"):
            continue
        rel = p.relative_to(REPO).as_posix()
        if ARCH_DIR_RE.match(rel) or rel.startswith("src/libs/"):
            continue
        body = _strip_comments(p.read_text(encoding="utf-8", errors="replace"))
        toks = sorted(set(TOKEN_RE.findall(body)))
        if toks:
            hits[rel] = toks
    return hits


def main():
    errors = []

    # 1) whitelist per-arch tokenu mimo per-arch stromy
    expected = {}
    for line in WHITELIST_FILE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        path, toks = line.split("|")
        expected[path.strip()] = sorted(t for t in toks.split() if t)

    actual = live_token_files()

    for path in sorted(set(actual) - set(expected)):
        errors.append(f"NEW per-arch token file (convert or whitelist): "
                      f"{path} | {' '.join(actual[path])}")
    for path in sorted(set(expected) - set(actual)):
        errors.append(f"STALE whitelist entry (remove): {path}")
    for path in sorted(set(expected) & set(actual)):
        if expected[path] != actual[path]:
            errors.append(f"TOKEN SET CHANGED: {path} | "
                          f"expected {' '.join(expected[path])} | "
                          f"actual {' '.join(actual[path])}")

    # 2) emucore_sources.txt existence
    for line in (REPO / "cmake/emucore_sources.txt").read_text(
            encoding="utf-8").splitlines():
        line = line.strip()
        if line and not (REPO / line).is_file():
            errors.append(f"emucore_sources.txt: missing file {line}")

    # 3) poison hlavicka
    poison = (REPO / "cmake/emucore_poison.h").read_text(encoding="utf-8")
    for ident in POISONED:
        if not re.search(r"#pragma GCC poison[^\n]*\b" + ident + r"\b", poison):
            errors.append(f"emucore_poison.h: identifier not poisoned: {ident}")

    # 4) HAVE_JOY se nesmi vratit (mrtvy toggle smazan v 11j-8)
    for path, toks in actual.items():
        if "HAVE_JOY" in toks:
            errors.append(f"HAVE_JOY reintroduced: {path}")

    if errors:
        print(f"PREPROC STATE GATE: {len(errors)} violation(s)")
        for e in errors:
            print("  " + e)
        return 1
    print("PREPROC STATE GATE: OK "
          f"({len(expected)} whitelisted files, "
          f"{len(POISONED)} poisoned identifiers)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
