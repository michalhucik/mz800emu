"""Jednotková kontrola převodu rychlosti v Python wrapperu MCP.

Wrapper ``mcp-server/mcp_server.py`` nejde v testu importovat (potřebuje
balík ``mcp`` z .venv, který prostředí testů nemusí mít). Kontrola proto
z jeho zdrojového textu vyjme (modul ``ast``) jen čisté funkce převodu
rychlosti (``_cmt_resolve_speed``, ``_cmt_convert_speed_value``) a jejich
konstanty a spustí je samostatně. Hodnoty ověřuje proti enumu
``en_CMTSPEED`` v ``src/libs/cmtspeed/cmtspeed.h`` (C strana bere u příkazu
``cmt_tape_convert_block`` jen celé číslo 1..9, dispatch.c).

Kontroluje:
- ``2``, ``"2"`` a ``"2:1"`` dají hodnotu CMTSPEED_2_1, ``"3:1"`` CMTSPEED_3_1;
- ``True`` / ``False``, ``"custom"``, ``10``, ``0``, ``"x"`` dají 0 (neplatné);
- anotace parametru ``speed`` nástroje ``emu_cmt_tape_convert_block`` připouští
  ``int`` i ``str`` (původní vada: jen ``str``, celé číslo z dokumentace pydantic
  odmítl).

Použití: python test_mcp_wrapper_speed.py <mcp_server.py> <cmtspeed.h>
Návrat 0 = vše prošlo, 1 = selhání (výpis na stderr).
"""

import ast
import re
import sys


def load_functions(path):
    """Vyjme z wrapperu konstanty a funkce převodu rychlosti a vrátí jmenný prostor po jejich spuštění."""
    with open(path, encoding="utf-8") as fh:
        tree = ast.parse(fh.read(), filename=path)
    wanted_assign = {"_CMT_SPEED_KEYS", "_CMT_SPEED_CUSTOM"}
    wanted_func = {"_cmt_resolve_speed", "_cmt_convert_speed_value"}
    body = []
    convert_tool = None
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id in wanted_assign for t in node.targets):
            body.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name in wanted_func:
            body.append(node)
        elif isinstance(node, ast.AsyncFunctionDef) and node.name == "emu_cmt_tape_convert_block":
            convert_tool = node
    module = ast.Module(body=body, type_ignores=[])
    ns = {}
    exec(compile(module, path, "exec"), ns)
    return ns, convert_tool


def load_enum(path):
    """Hodnoty en_CMTSPEED z hlavičky C (pořadí výčtu od CMTSPEED_NONE = 0)."""
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    m = re.search(r"typedef enum en_CMTSPEED \{(.*?)\} en_CMTSPEED;", text, re.S)
    if not m:
        raise SystemExit("en_CMTSPEED not found in " + path)
    names = re.findall(r"^\s*(CMTSPEED_[A-Z0-9_]+)", m.group(1), re.M)
    return {n: i for i, n in enumerate(names)}


def main():
    if len(sys.argv) != 3:
        print("usage: test_mcp_wrapper_speed.py <mcp_server.py> <cmtspeed.h>", file=sys.stderr)
        return 1
    ns, tool = load_functions(sys.argv[1])
    enum = load_enum(sys.argv[2])
    conv = ns.get("_cmt_convert_speed_value")
    failures = []
    if conv is None:
        failures.append("_cmt_convert_speed_value not found in the wrapper")
    else:
        cases = [
            (2, enum["CMTSPEED_2_1"]),
            ("2", enum["CMTSPEED_2_1"]),
            ("2:1", enum["CMTSPEED_2_1"]),
            (" 3:1 ", enum["CMTSPEED_3_1"]),
            (9, enum["CMTSPEED_25_14"]),
            (True, 0),
            (False, 0),
            ("custom", 0),
            (enum["CMTSPEED_CUSTOM"], 0),
            (0, 0),
            ("x", 0),
            (None, 0),
        ]
        for arg, want in cases:
            got = conv(arg)
            if got != want:
                failures.append(f"_cmt_convert_speed_value({arg!r}) = {got}, expected {want}")
    if tool is None:
        failures.append("emu_cmt_tape_convert_block not found in the wrapper")
    else:
        ann = None
        for a in tool.args.args:
            if a.arg == "speed" and a.annotation is not None:
                ann = ast.unparse(a.annotation)
        if ann is None or "int" not in ann or "str" not in ann:
            failures.append(f"speed annotation of emu_cmt_tape_convert_block is {ann!r}, expected str | int | None")
    for f in failures:
        print("FAIL: " + f, file=sys.stderr)
    if not failures:
        print("wrapper speed conversion: OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
