#!/usr/bin/env python3
"""Test generátoru ``tools/generate_build_revision_git.sh``.

Každý scénář si v dočasném adresáři postaví "zdrojový strom" (kopie skriptu
v ``<strom>/tools/``; skript se vztahuje ke kořeni podle svého umístění),
případně z něj udělá git repozitář s danými remoty, spustí generátor a
z vygenerovaného C souboru přečte hodnoty (C literály dekóduje, takže se
ověří i escapování). Ověřované scénáře:

  1. strom bez .git -> hodnoty "unknown", revize -1, exit 0,
  2. git není v PATH -> "unknown", exit 0 (přeskočí se, pokud adresář
     s bash obsahuje i git),
  3. git vždy selhává (falešný git) -> "unknown", exit 0,
  4. repozitář bez remotů -> původ unknown, větev, commit, dirty 0/1,
     detached HEAD -> "detached" + příznak,
  5. přímý remote na nas1 (ssh://, scp tvar, FQDN, port, velká písmena)
     -> NAS1; podobné, ale jiné hosty (nas10, cesta s "nas1") -> unknown,
  6. řetěz lokálních klonů (absolutní i relativní cesta, file://) až k nas1
     -> NAS1; cyklus lokálních remotů skončí (unknown); příliš dlouhý
     řetěz (> 4 úrovně) -> unknown,
  7. GitHub upstream -> revize 250 + počet commitů (beze změny), původ
     GitHub i při současném nas1 remotu,
  8. strom uvnitř cizího repozitáře s nas1 remotem -> unknown,
  9. jméno větve s uvozovkou, apostrofem a diakritikou -> správně
     escapovaný literál,
 10. opakovaný běh beze změny nepřepíše výstupní soubor.

Bez interpretu bash nebo programu git test skončí kódem 77 (SKIP).
Výstup anglicky. Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent.parent
_SCRIPT = _REPO_ROOT / "tools" / "generate_build_revision_git.sh"

_failures = []


def check(cond, what):
    """Vyhodnotí jednu kontrolu, vypíše PASS/FAIL a zapamatuje selhání."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        _failures.append(what)


def _find_bash():
    """Najde bash (přednost má MZ_BASH); bash z System32 (WSL) odmítne."""
    env = os.environ.get("MZ_BASH")
    if env and Path(env).is_file():
        return env
    found = shutil.which("bash")
    if found and "system32" in found.lower():
        return None
    return found


_BASH = _find_bash()
_GIT = shutil.which("git")


def _c_unescape(lit):
    """Dekóduje obsah C řetězcového literálu (\\\\, \\", \\?, \\ooo) na text."""
    out = bytearray()
    i = 0
    while i < len(lit):
        c = lit[i]
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        nxt = lit[i + 1]
        if nxt in "01234567":
            m = re.match(r"[0-7]{1,3}", lit[i + 1:])
            out.append(int(m.group(0), 8))
            i += 1 + len(m.group(0))
        elif nxt in "\\\"?'":
            out += nxt.encode("ascii")
            i += 2
        else:
            raise ValueError(f"unexpected escape \\{nxt}")
    return out.decode("utf-8")


def _parse(c_text):
    """Vrátí slovník {jméno funkce: hodnota} z vygenerovaného build_revision.c."""
    vals = {}
    for m in re.finditer(r"(\w+)\(void\) \{ return (.*?); \}$", c_text, re.M):
        name, expr = m.group(1), m.group(2)
        if expr.startswith('"'):
            vals[name] = _c_unescape(expr[1:-1])
        elif re.fullmatch(r"-?\d+", expr):
            vals[name] = int(expr)
        else:
            vals[name] = expr
    return vals


def _git(cwd, *args):
    """Spustí git v adresáři cwd (s pevnou identitou autora) a vrátí stdout."""
    cmd = [_GIT, "-c", "user.name=Test", "-c", "user.email=test@example.com",
           "-c", "init.defaultBranch=master", "-c", "core.autocrlf=false"] + list(args)
    res = subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True,
                         encoding="utf-8", errors="replace", env=_ENV)
    if res.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {res.stderr.strip()}")
    return res.stdout.strip()


def _make_tree(path):
    """Vytvoří "zdrojový strom": adresář s kopií generátoru v tools/."""
    (path / "tools").mkdir(parents=True, exist_ok=True)
    shutil.copy2(_SCRIPT, path / "tools" / _SCRIPT.name)
    return path


def _make_repo(path, remotes=(), commits=1):
    """Vytvoří git repozitář se zdrojovým stromem, commity a remoty."""
    _make_tree(path)
    (path / ".gitignore").write_text("/out.c\n/out.c.tmp.*\n", encoding="utf-8")
    _git(path, "init", "-q")
    for i in range(commits):
        (path / "file.txt").write_text(f"content {i}\n", encoding="utf-8")
        _git(path, "add", "-A")
        _git(path, "commit", "-q", "-m", f"commit {i}")
    for name, url in remotes:
        _git(path, "remote", "add", name, url)
    return path


def _run_gen(tree, path_override=None):
    """Spustí generátor ve stromu; vrátí (exit kód, hodnoty, výstup, cesta)."""
    out = tree / "out.c"
    env = dict(_ENV)
    if path_override is not None:
        env["PATH"] = path_override
    script = (tree / "tools" / _SCRIPT.name).as_posix()
    res = subprocess.run([_BASH, script, out.as_posix()], cwd=str(tree),
                         capture_output=True, text=True, encoding="utf-8",
                         errors="replace", env=env, timeout=120)
    vals = _parse(out.read_text(encoding="utf-8")) if out.is_file() else {}
    return res.returncode, vals, res.stdout + res.stderr, out


def _expect_unknown(label, rc, vals):
    """Společné kontroly pro scénáře bez git informací."""
    check(rc == 0, f"{label}: exit code 0")
    check(vals.get("build_revision_get_int") == -1, f"{label}: revision -1")
    check(vals.get("build_revision_get_const_char") == "Revision: ???", f"{label}: revision text ???")
    check(vals.get("build_revision_get_repo_origin") == "BUILD_REVISION_ORIGIN_UNKNOWN", f"{label}: origin UNKNOWN")
    check(vals.get("build_revision_get_repo_origin_name") == "unknown", f"{label}: origin name unknown")
    check(vals.get("build_revision_get_branch") == "unknown", f"{label}: branch unknown")
    check(vals.get("build_revision_get_commit") == "unknown", f"{label}: commit unknown")
    check(vals.get("build_revision_get_commit_short") == "unknown", f"{label}: short commit unknown")
    check(vals.get("build_revision_is_dirty") == -1, f"{label}: dirty unknown (-1)")
    check(vals.get("build_revision_is_detached") == 0, f"{label}: not detached")


def _origin(vals):
    """Zkratka: jméno původu z hodnot."""
    return vals.get("build_revision_get_repo_origin_name")


def main():
    """Vstupní bod testu."""
    if not _BASH or not _GIT:
        print("SKIP: bash or git not found")
        return 77

    tmp_root = Path(tempfile.mkdtemp(prefix="mz_buildrev_"))
    global _ENV
    _ENV = dict(os.environ)
    # Git nesmí hledat repozitář nad dočasným adresářem.
    _ENV["GIT_CEILING_DIRECTORIES"] = str(tmp_root.parent)
    _ENV["GIT_CONFIG_NOSYSTEM"] = "1"
    try:
        _scenarios(tmp_root)
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)

    if _failures:
        print(f"FAIL: {len(_failures)} check(s) failed")
        return 1
    print("PASS")
    return 0


def _scenarios(tmp):
    """Projde všechny scénáře (viz docstring modulu)."""
    # 1) strom bez .git
    print("[1] source tree without .git")
    rc, vals, _, _ = _run_gen(_make_tree(tmp / "nogit"))
    _expect_unknown("nogit", rc, vals)
    check(vals.get("build_revision_get_build_msystem") == os.environ.get("MSYSTEM", ""),
          "nogit: MSYSTEM recorded")

    # 2) git není v PATH: PATH jen s adresářem, kde leží bash (coreutils).
    print("[2] git not in PATH")
    bash_dir = str(Path(_BASH).parent)
    if any((Path(bash_dir) / n).exists() for n in ("git", "git.exe")):
        print("  SKIP: git lives next to bash, cannot hide it")
    else:
        repo = _make_repo(tmp / "nogitbin", remotes=[("origin", "ssh://michal@nas1/x.git")])
        rc, vals, _, _ = _run_gen(repo, path_override=bash_dir)
        _expect_unknown("no git binary", rc, vals)

    # 3) git vždy selhává
    print("[3] git always fails")
    fake = tmp / "fakegit"
    fake.mkdir()
    (fake / "git").write_text("#!/bin/sh\nexit 1\n", encoding="utf-8", newline="\n")
    os.chmod(fake / "git", 0o755)
    repo = _make_repo(tmp / "failgit", remotes=[("origin", "ssh://michal@nas1/x.git")])
    rc, vals, _, _ = _run_gen(repo, path_override=str(fake) + os.pathsep + _ENV.get("PATH", ""))
    _expect_unknown("failing git", rc, vals)

    # 4) repozitář bez remotů: větev, commit, dirty, detached
    print("[4] repository without remotes")
    repo = _make_repo(tmp / "noremote", commits=2)
    head = _git(repo, "rev-parse", "HEAD")
    rc, vals, _, _ = _run_gen(repo)
    check(rc == 0, "noremote: exit code 0")
    check(_origin(vals) == "unknown", "noremote: origin unknown")
    check(vals.get("build_revision_get_int") == -1, "noremote: revision -1")
    check(vals.get("build_revision_get_branch") == "master", "noremote: branch master")
    check(vals.get("build_revision_get_commit") == head, "noremote: full commit hash")
    check(head.startswith(str(vals.get("build_revision_get_commit_short"))) and
          len(str(vals.get("build_revision_get_commit_short"))) >= 7, "noremote: short commit hash")
    check(vals.get("build_revision_is_dirty") == 0, "noremote: clean tree (dirty 0)")
    (repo / "file.txt").write_text("modified\n", encoding="utf-8")
    rc, vals, _, _ = _run_gen(repo)
    check(vals.get("build_revision_is_dirty") == 1, "noremote: modified tracked file -> dirty 1")
    _git(repo, "checkout", "-q", "--", "file.txt")
    (repo / "untracked.txt").write_text("x\n", encoding="utf-8")
    rc, vals, _, _ = _run_gen(repo)
    check(vals.get("build_revision_is_dirty") == 0, "noremote: untracked file only -> dirty 0")
    _git(repo, "checkout", "-q", "--detach", "HEAD~1")
    rc, vals, _, _ = _run_gen(repo)
    check(vals.get("build_revision_get_branch") == "detached", "detached: branch 'detached'")
    check(vals.get("build_revision_is_detached") == 1, "detached: flag 1")
    check(vals.get("build_revision_get_commit") == _git(repo, "rev-parse", "HEAD"), "detached: commit hash")

    # 5) přímé remoty
    print("[5] direct remote URLs")
    cases = [
        ("ssh://michal@nas1/volume1/git-repositories/mz800new.git", "NAS1"),
        ("ssh://nas1/volume1/x.git", "NAS1"),
        ("michal@nas1:volume1/x.git", "NAS1"),
        ("nas1:x.git", "NAS1"),
        ("ssh://michal@NAS1.local:2222/x.git", "NAS1"),
        ("ssh://michal@nas10/x.git", "unknown"),
        ("https://example.com/nas1/x.git", "unknown"),
        ("https://github.com/someone/mz800emu.git", "unknown"),
    ]
    for i, (url, expected) in enumerate(cases):
        repo = _make_repo(tmp / f"direct{i}", remotes=[("origin", url)])
        rc, vals, _, _ = _run_gen(repo)
        check(rc == 0 and _origin(vals) == expected, f"remote '{url}' -> {expected}")
        if expected == "NAS1":
            check(vals.get("build_revision_get_int") == -1, f"remote '{url}': revision stays -1")

    # 6) řetěz lokálních klonů
    print("[6] local-path remote chains")
    base = _make_repo(tmp / "chain_a", remotes=[("origin", "ssh://michal@nas1/x.git")])
    mid = _make_repo(tmp / "chain_b", remotes=[("origin", base.as_posix())])
    leaf = _make_repo(tmp / "chain_c", remotes=[("origin", "../chain_b")])
    rc, vals, _, _ = _run_gen(leaf)
    check(rc == 0 and _origin(vals) == "NAS1", "relative -> absolute -> nas1 chain -> NAS1")
    leaf2 = _make_repo(tmp / "chain_d", remotes=[("upstream", "file://" + mid.as_posix())])
    rc, vals, _, _ = _run_gen(leaf2)
    check(rc == 0 and _origin(vals) == "NAS1", "file:// -> absolute -> nas1 chain -> NAS1")
    bare = tmp / "chain_bare.git"
    _git(tmp, "clone", "-q", "--bare", base.as_posix(), bare.as_posix())
    leaf3 = _make_repo(tmp / "chain_e", remotes=[("origin", bare.as_posix())])
    rc, vals, _, _ = _run_gen(leaf3)
    check(rc == 0 and _origin(vals) == "NAS1", "bare clone in chain (its origin -> nas1 chain) -> NAS1")

    cyc_a = _make_repo(tmp / "cycle_a")
    cyc_b = _make_repo(tmp / "cycle_b", remotes=[("origin", cyc_a.as_posix())])
    _git(cyc_a, "remote", "add", "origin", cyc_b.as_posix())
    t0 = time.monotonic()
    rc, vals, _, _ = _run_gen(cyc_a)
    check(rc == 0 and _origin(vals) == "unknown", "cycle of local remotes -> unknown")
    check(time.monotonic() - t0 < 60, "cycle of local remotes terminates quickly")

    prev = _make_repo(tmp / "deep0", remotes=[("origin", "ssh://michal@nas1/x.git")])
    for d in range(1, 7):
        prev = _make_repo(tmp / f"deep{d}", remotes=[("origin", prev.as_posix())])
        rc, vals, _, _ = _run_gen(prev)
        expected = "NAS1" if d <= 4 else "unknown"
        check(_origin(vals) == expected, f"chain depth {d} -> {expected}")

    # 7) GitHub upstream: revize 250 + počet commitů (beze změny)
    print("[7] GitHub upstream revision")
    gh_urls = [
        "https://github.com/michalhucik/mz800emu.git",
        "git@github.com:michalhucik/mz800emu",
        "ssh://git@github.com/michalhucik/mz800emu.git",
        "https://GitHub.com/MichalHucik/mz800emu/",
    ]
    for i, url in enumerate(gh_urls):
        repo = _make_repo(tmp / f"gh{i}", remotes=[("origin", url)], commits=3)
        rc, vals, _, _ = _run_gen(repo)
        check(rc == 0 and vals.get("build_revision_get_int") == 253, f"upstream '{url}': revision 253 (250 + 3)")
        check(vals.get("build_revision_get_const_char") == "Revision: 253", f"upstream '{url}': revision text")
        check(_origin(vals) == "GitHub", f"upstream '{url}': origin GitHub")
    repo = _make_repo(tmp / "gh_nas1", remotes=[("origin", "ssh://michal@nas1/x.git"),
                                                 ("github", "https://github.com/michalhucik/mz800emu.git")],
                      commits=2)
    rc, vals, _, _ = _run_gen(repo)
    check(_origin(vals) == "GitHub" and vals.get("build_revision_get_int") == 252,
          "upstream + nas1 remotes -> GitHub, revision 252")

    # 8) strom uvnitř cizího repozitáře
    print("[8] source tree nested in a foreign repository")
    outer = _make_repo(tmp / "outer", remotes=[("origin", "ssh://michal@nas1/x.git")])
    nested = _make_tree(outer / "sub" / "unpacked")
    rc, vals, _, _ = _run_gen(nested)
    _expect_unknown("nested tree", rc, vals)

    # 9) escapování jména větve
    print("[9] branch name escaping")
    repo = _make_repo(tmp / "escape", remotes=[("origin", "ssh://michal@nas1/x.git")])
    # Uvozovku Windows v názvu souboru (loose ref) nedovolí - ta se ověří
    # přímo funkcí c_escape níže.
    weird = "fix/we'ird-náme-č"
    _git(repo, "checkout", "-q", "-b", weird)
    rc, vals, _, _ = _run_gen(repo)
    check(rc == 0 and vals.get("build_revision_get_branch") == weird, "branch with apostrophe/diacritics round-trips")

    # c_escape přímo: uvozovka, zpětné lomítko, otazník (trigraf), tab,
    # diakritika. Funkce se vyřízne ze skriptu a zavolá v bash.
    sample = "a\"b\\c??=d\teé"
    probe = ("eval \"$(sed -n '/^c_escape()/,/^}/p' \"$1\")\"; c_escape \"$2\"")
    res = subprocess.run([_BASH, "-c", probe, "probe", _SCRIPT.as_posix(), sample],
                         capture_output=True, env=_ENV)
    lit = res.stdout.decode("ascii", errors="replace")
    check(res.returncode == 0 and re.fullmatch(r'(?:[^"\\]|\\.)*', lit, re.S) is not None
          and all(32 <= ord(ch) < 127 for ch in lit), "c_escape output is a plain ASCII C literal body")
    try:
        decoded = _c_unescape(lit)
    except (ValueError, UnicodeDecodeError):
        decoded = None
    check(decoded == sample, "c_escape round-trips quote, backslash, ?, tab and UTF-8")

    # 10) beze změny se výstup nepřepisuje
    print("[10] unchanged output is not rewritten")
    repo = _make_repo(tmp / "stable")
    rc, vals, _, out = _run_gen(repo)
    mtime = out.stat().st_mtime_ns
    time.sleep(1.1)
    rc, vals, log, out = _run_gen(repo)
    check(rc == 0 and out.stat().st_mtime_ns == mtime, "second run keeps the file untouched")
    check("Output unchanged." in log, "second run reports 'Output unchanged.'")


_ENV = dict(os.environ)

if __name__ == "__main__":
    sys.exit(main())
