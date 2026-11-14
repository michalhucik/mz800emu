"""Kontrola jmen kazetových nástrojů, resource a polí v dokumentaci pro agenty.

Dokumenty ``docs/agent/*.md`` (bez ``README.md`` v libovolné velikosti písmen,
který wrapper nevystavuje a který konvenci popisuje) jmenují nástroje
``emu_cmt_*``, resource ``emulator://periph/cmt*``, parametry nástrojů a pole
JSON odpovědí. Test je ověří proti kódu, aby překlep nebo přejmenování v kódu
neprošlo bez povšimnutí:

- každé jméno ``emu_cmt_<x>`` musí být nástroj (``@mcp.tool()``) v
  ``mcp-server/mcp_server.py``; ``emu_cmt_<předpona>*`` musí odpovídat aspoň
  jednomu nástroji;
- každé URI ``emulator://periph/cmt...`` musí být resource (``@mcp.resource``)
  wrapperu;
- v zápisu volání ``emu_<x>(jméno=...)`` musí být každý pojmenovaný argument
  parametrem toho nástroje (všechny nástroje ``emu_*`` wrapperu; volání
  neznámého jména jiného než ``emu_cmt_*`` se nehlásí);
- pole v tabulkách označených konvencí níže musí nastavovat handler
  JSONL příkazu v ``src/emulator/mcp/dispatch.c`` (volání
  ``json_object_set_*_member(obj, "pole", ...)``); pole ``state_rev``
  doplňuje obecná odpověď dispatch a je vždy povolené;
- pole výsledku nástroje v result-table musí navíc jmenovat docstring toho
  nástroje (nástroje se sdíleným handlerem, např. ``emu_cmt_play`` /
  ``emu_cmt_stop`` / ``emu_cmt_eject``, tak nepřevezmou pole, které vrací jen
  jiný z nich);
- v blocích kódu (```` ``` ````) se ověří indexy ``proměnná["pole"]`` u
  proměnných přiřazených z kazetového nástroje (``r = emu_cmt_x(...)``) nebo
  resource (``t = emulator://periph/cmt/tape``), u proměnných cyklu nad nimi
  (``b in t["blocks"]``) a přímo za URI resource; pole musí nastavovat
  handler toho nástroje / resource (všechny úrovně dohromady). Vazba platí
  podle pořadí v bloku: nové přiřazení téže proměnné (i z jiného nástroje)
  ji nahradí. Dokumenty musí obsahovat aspoň jeden takto ověřený index.

Konvence značení polí v dokumentu (pro autory dokumentů popsaná bez cest
v ``docs/agent/README.md``, pro vývojáře v
``devdoc/testing/testy_integracni.md``, oddíl 6):

- ``<!-- field-table: <URI resource nebo jméno nástroje> [<člen>] -->`` na
  samostatném řádku před tabulkou Markdown: v prvním sloupci každého řádku
  těla tabulky jsou jména polí v obrácených apostrofech (jedno nebo víc).
  Bez ``<člen>`` jde o vrchní úroveň odpovědi, s ním o objekt v členu
  ``<člen>`` (u pole objektů o jeho prvky), např. ``blocks``.
- ``<!-- result-table -->`` před tabulkou: v prvním sloupci jméno nástroje
  v obrácených apostrofech, ve druhém jména polí jeho odpovědi (všechny
  úrovně).

Vazba nástroj / resource -> handler: wrapper posílá příkaz
``_send_request("<příkaz>", ...)``, tabulka ``g_cmd_map[]`` v dispatch.c
mapuje příkaz na ``_handle_<...>``. Úroveň odpovědi se pozná podle proměnné
v prvním argumentu ``json_object_set_*_member``: vrchní úroveň je objekt
předaný ``_ok_response()``, člen je objekt nebo pole vložené do ní
``json_object_set_(object|array)_member(<vrchní>, "<člen>", <proměnná>)``
(pole: prvky z ``json_array_add_object_element(<proměnná>, <prvek>)``).

Neověřuje: poziční argumenty volání, výchozí hodnoty v zápisu signatur,
volání nástrojů s neznámým jménem mimo ``emu_cmt_*``, URI
``emulator://docs/*``, pole v próze mimo označené tabulky a bloky kódu,
klíče, jejichž jméno v dispatch.c není literál (deset textů informací
o pásce z ``cmttmz_tape_info_key()``), texty chyb, čísla a meze.

Nakonec test ověří sám sebe: tytéž kontroly nad uměle pokaženým textem
(překlep nástroje, parametru, pole, URI, pole jiné úrovně, pole jiného
nástroje se sdíleným handlerem, pole v bloku kódu) musí hlásit chybu, jinak
by kontrola mohla tiše přestat cokoli hlídat.

Wrapper nejde v testu importovat (potřebuje balík ``mcp`` z .venv), čte se
proto jen jeho zdrojový text modulem ``ast``.

Použití: python test_agent_docs_names.py <docs/agent> <mcp_server.py> <dispatch.c>
Návrat 0 = vše prošlo, 1 = selhání (výpis na stderr).
"""

import ast
import os
import re
import sys

# Pole doplněné obecnou odpovědí dispatch (_ok_response) u příkazů, které mění stav.
UNIVERSAL_FIELDS = {"state_rev"}

TOOL_RE = re.compile(r"\bemu_cmt_[a-z0-9_]*(\*?)")
URI_RE = re.compile(r"emulator://periph/cmt[a-z0-9_/]*")
CALL_RE = re.compile(r"\b(emu_[a-z0-9_]+)\(")
KWARG_RE = re.compile(r"(?<![\w.])([A-Za-z_]\w*)\s*=(?!=)")
SPAN_RE = re.compile(r"`([^`]*)`")
IDENT_RE = re.compile(r"^[a-z_][a-z0-9_]*$")
FIELD_MARK_RE = re.compile(r"^<!--\s*field-table:\s*(\S+)(?:\s+([a-z_][a-z0-9_]*))?\s*-->\s*$")
RESULT_MARK_RE = re.compile(r"^<!--\s*result-table\s*-->\s*$")
FENCE_RE = re.compile(r"^```[^\n]*\n(.*?)^```", re.S | re.M)
ASSIGN_TOOL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*=\s*(emu_cmt_[a-z0-9_]+)\(")
ASSIGN_URI_RE = re.compile(r"\b([A-Za-z_]\w*)\s*=\s*(emulator://periph/cmt[a-z0-9_/]*)(?![\w/\[])")
LOOP_RE = re.compile(r"\b([A-Za-z_]\w*)\s+in\s+([A-Za-z_]\w*)\[")
ASSIGN_ANY_RE = re.compile(r"(?:^|;)[ \t]*([A-Za-z_]\w*)[ \t]*=(?!=)", re.M)
SUBSCRIPT_RE = re.compile(r"(emulator://periph/cmt[a-z0-9_/]*|\b[A-Za-z_]\w*)((?:\[\"[^\"]*\"\])+)")


def _decorator_info(dec):
    """Vrátí ("tool", None) pro @mcp.tool(), ("resource", uri) pro @mcp.resource("uri"), jinak None."""
    if not isinstance(dec, ast.Call) or not isinstance(dec.func, ast.Attribute):
        return None
    if not (isinstance(dec.func.value, ast.Name) and dec.func.value.id == "mcp"):
        return None
    if dec.func.attr == "tool":
        return ("tool", None)
    if dec.func.attr == "resource" and dec.args and isinstance(dec.args[0], ast.Constant):
        return ("resource", dec.args[0].value)
    return None


def _sent_commands(func):
    """Jména příkazů JSONL, které funkce wrapperu posílá přes _send_request("<příkaz>", ...)."""
    cmds = set()
    for node in ast.walk(func):
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "_send_request"
                and node.args and isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str)):
            cmds.add(node.args[0].value)
    return cmds


class Tool:
    """Nástroj wrapperu: parametry, příkazy JSONL a slova docstringu."""

    def __init__(self, func):
        a = func.args
        self.params = {x.arg for x in a.posonlyargs + a.args + a.kwonlyargs}
        self.commands = _sent_commands(func)
        self.doc_words = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", ast.get_docstring(func) or ""))


def load_wrapper(path):
    """Nástroje {jméno: Tool} a resource {uri: příkazy} z mcp_server.py."""
    with open(path, encoding="utf-8") as fh:
        tree = ast.parse(fh.read(), filename=path)
    tools, resources = {}, {}
    for node in tree.body:
        if not isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef)):
            continue
        for dec in node.decorator_list:
            info = _decorator_info(dec)
            if not info:
                continue
            if info[0] == "tool":
                tools[node.name] = Tool(node)
            else:
                resources[info[1]] = _sent_commands(node)
    return tools, resources


def _match_brace(text, start):
    """Index za '}' párovou ke '{' na pozici start; přeskakuje řetězce, znaky a komentáře C."""
    depth, i, n = 0, start, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n and text[i] != q:
                i += 2 if text[i] == "\\" else 1
        elif text.startswith("/*", i):
            i = text.find("*/", i + 2)
            if i < 0:
                return n
            i += 1
        elif text.startswith("//", i):
            i = text.find("\n", i)
            if i < 0:
                return n
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def _function_body(text, name):
    """Tělo definice funkce C `name` (definice začíná na začátku řádku), nebo None."""
    m = re.search(r"^[A-Za-z_][\w \t\*]*?\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text, re.M)
    if not m:
        return None
    return text[m.end() - 1:_match_brace(text, m.end() - 1)]


def _set_members(body, var=None):
    """Jména polí z json_object_set_*_member(<var>, "pole", ...) v těle (var None = libovolný objekt)."""
    obj = r"\w+" if var is None else re.escape(var)
    return set(re.findall(r'json_object_set_\w+_member\s*\(\s*' + obj + r'\s*,\s*"([A-Za-z0-9_]+)"', body))


class Dispatch:
    """Mapa příkazů JSONL na handlery a pole JSON, která handlery nastavují."""

    def __init__(self, path):
        with open(path, encoding="utf-8") as fh:
            self.text = fh.read()
        self.cmd_map = dict(re.findall(r'\{\s*"([a-z0-9_]+)"\s*,\s*\w+\s*,\s*(_handle_\w+)\s*\}', self.text))
        self._cache = {}

    def _body(self, name):
        body = _function_body(self.text, name)
        if body is None:
            raise SystemExit("function %s not found in dispatch.c" % name)
        return body

    def fields_of_function(self, name, seen=None):
        """Pole všech úrovní z těla funkce a z pomocných funkcí _cmt_*, které volá."""
        if name in self._cache:
            return self._cache[name]
        seen = set() if seen is None else seen
        seen.add(name)
        body = self._body(name)
        fields = _set_members(body)
        for helper in set(re.findall(r"\b(_cmt_\w+)\s*\(", body)):
            if helper not in seen and _function_body(self.text, helper) is not None:
                fields |= self.fields_of_function(helper, seen)
        self._cache[name] = fields
        return fields

    def level_fields(self, handler, member):
        """Pole jedné úrovně odpovědi handleru: vrchní (member None) nebo objekt / prvky pole v členu member."""
        body = self._body(handler)
        m = re.search(r"_ok_response\s*\(\s*\w+\s*,\s*(\w+)\s*,", body)
        if not m:
            raise SystemExit("%s: no _ok_response(req_id, <object>, ...) found" % handler)
        root = m.group(1)
        if member is None:
            fields = _set_members(body, root)
            h = re.search(r"\b" + re.escape(root) + r"\s*=\s*(_cmt_\w+)\s*\(", body)
            if h:
                fields |= self.fields_of_function(h.group(1))
            return fields
        m = re.search(r'json_object_set_(object|array)_member\s*\(\s*' + re.escape(root) + r'\s*,\s*"'
                      + re.escape(member) + r'"\s*,\s*(\w+)\s*\)', body)
        if not m:
            return None
        var = m.group(2)
        if m.group(1) == "array":
            e = re.search(r"json_array_add_object_element\s*\(\s*" + re.escape(var) + r"\s*,\s*(\w+)\s*\)", body)
            if not e:
                return None
            var = e.group(1)
        return _set_members(body, var)

    def handlers(self, cmds):
        """Handlery daných příkazů JSONL; neznámý příkaz = SystemExit."""
        out = []
        for cmd in sorted(cmds):
            handler = self.cmd_map.get(cmd)
            if handler is None:
                raise SystemExit("command %s not found in g_cmd_map of dispatch.c" % cmd)
            out.append(handler)
        return out

    def fields_of_commands(self, cmds):
        """Sjednocení polí všech úrovní handlerů daných příkazů JSONL."""
        fields = set()
        for h in self.handlers(cmds):
            fields |= self.fields_of_function(h)
        return fields


def _call_kwargs(text, pos):
    """Pojmenované argumenty volání, jehož '(' je na pozici pos (jen nejvyšší úroveň závorek, mimo řetězce)."""
    depth, i, n, top = 0, pos, len(text), []
    limit = min(n, pos + 2000)
    while i < limit:
        c = text[i]
        if c in "\"'":
            q = c
            i += 1
            while i < limit and text[i] != q:
                i += 2 if text[i] == "\\" else 1
            if depth == 1:
                top.append(" ")
        elif c in "([{":
            depth += 1
            if depth == 2:
                top.append(" ")
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                break
        elif depth == 1:
            top.append(c)
        i += 1
    return KWARG_RE.findall("".join(top))


def _tables_after_marks(lines):
    """Seznam (značka, řádek značky, řádky těla tabulky) pro tabulky za značkami konvence.

    Značka je ("field", cíl, člen nebo None) nebo ("result", None, None); řádek těla je
    (číslo řádku, seznam buněk).
    """
    out = []
    i = 0
    while i < len(lines):
        fm = FIELD_MARK_RE.match(lines[i].strip())
        rm = RESULT_MARK_RE.match(lines[i].strip())
        if not fm and not rm:
            i += 1
            continue
        mark = ("field", fm.group(1), fm.group(2)) if fm else ("result", None, None)
        j = i + 1
        while j < len(lines) and not lines[j].strip():
            j += 1
        rows = []
        # záhlaví a oddělovač tabulky přeskočit
        if j + 1 < len(lines) and lines[j].lstrip().startswith("|") and lines[j + 1].lstrip().startswith("|"):
            k = j + 2
            while k < len(lines) and lines[k].lstrip().startswith("|"):
                cells = [c.strip() for c in lines[k].strip().strip("|").split("|")]
                rows.append((k + 1, cells))
                k += 1
        out.append((mark, i + 1, rows))
        i = j + 1
    return out


def _target_commands(target, tools, resources):
    """Příkazy JSONL resource nebo nástroje; None = neznámý cíl."""
    if target in resources:
        return resources[target]
    if target in tools:
        return tools[target].commands
    return None


#: Počet indexů proměnná["pole"] v blocích kódu, které kontrola skutečně ověřila
#: (main() ho po průchodu dokumentů vyžaduje nenulový, jinak by kontrola tiše
#: přestala hlídat recepty, např. po přejmenování proměnných).
CODE_SUBSCRIPTS_CHECKED = [0]


def _binding_at(bindings, var, pos, line_of):
    """Cíl proměnné var platný v místě pos.

    Platí poslední vazba (přiřazení nebo cyklus) před pos; není-li žádná,
    vazba cyklu na témž řádku (generátorový zápis ``[b["x"] for b in t["blocks"]]``
    má index před ``for``).

    @param bindings Seznam (pozice, proměnná, cíl) v pořadí textu.
    @return Cíl (URI nebo jméno nástroje), nebo None.
    """
    before = [t for (p, v, t) in bindings if v == var and p < pos]
    if before:
        return before[-1]
    same = [t for (p, v, t) in bindings if v == var and line_of(p) == line_of(pos)]
    return same[0] if same else None


def _check_code_blocks(name, text, tools, resources, dispatch):
    """Indexy proměnná["pole"] v blocích kódu u proměnných z kazetových nástrojů a resource.

    Vazba proměnné na nástroj / resource se bere podle pořadí v bloku: tatáž
    proměnná přiřazená znovu jinému nástroji platí od nového přiřazení.
    """
    errors = []
    for fence in FENCE_RE.finditer(text):
        code = fence.group(1)
        base_ln = text.count("\n", 0, fence.start(1)) + 1

        def line_of(p, code=code):
            return code.count("\n", 0, p)

        # každé přiřazení vazbu zruší; přiřazení z kazetového nástroje / resource ji
        # na stejné pozici nastaví (v seznamu je za obecným, řazení je stabilní)
        bindings = [(m.start(1), m.group(1), None) for m in ASSIGN_ANY_RE.finditer(code)]
        for m in ASSIGN_TOOL_RE.finditer(code):
            bindings.append((m.start(1), m.group(1), m.group(2) if m.group(2) in tools else None))
        for m in ASSIGN_URI_RE.finditer(code):
            bindings.append((m.start(1), m.group(1), m.group(2) if m.group(2) in resources else None))
        bindings.sort(key=lambda b: b[0])
        for m in LOOP_RE.finditer(code):
            src = _binding_at(bindings, m.group(2), m.start(), line_of)
            bindings.append((m.start(1), m.group(1), src))
            bindings.sort(key=lambda b: b[0])
        for m in SUBSCRIPT_RE.finditer(code):
            head = m.group(1)
            target = head if head.startswith("emulator://") else _binding_at(bindings, head, m.start(), line_of)
            if target is None:
                continue
            cmds = _target_commands(target, tools, resources)
            if cmds is None:
                continue
            allowed = dispatch.fields_of_commands(cmds) | UNIVERSAL_FIELDS
            ln = base_ln + code.count("\n", 0, m.start())
            for f in re.findall(r'\["([^"]*)"\]', m.group(2)):
                CODE_SUBSCRIPTS_CHECKED[0] += 1
                if f not in allowed:
                    errors.append("%s:%d: code block field %s not in the result of %s (dispatch.c)" % (name, ln, f, target))
    return errors


def check_text(name, text, tools, resources, dispatch):
    """Vrátí seznam chyb jednoho dokumentu (prázdný = v pořádku)."""
    errors = []
    lines = text.split("\n")
    for ln, line in enumerate(lines, 1):
        for m in TOOL_RE.finditer(line):
            token, star = m.group(0).rstrip("*"), m.group(1)
            if star:
                if not any(t.startswith(token) for t in tools):
                    errors.append("%s:%d: no tool matches %s*" % (name, ln, token))
            elif token not in tools:
                errors.append("%s:%d: unknown tool %s" % (name, ln, token))
        for m in URI_RE.finditer(line):
            if m.group(0) not in resources:
                errors.append("%s:%d: unknown resource %s" % (name, ln, m.group(0)))
    for m in CALL_RE.finditer(text):
        tool = m.group(1)
        if tool not in tools:
            continue
        ln = text.count("\n", 0, m.start()) + 1
        for kw in _call_kwargs(text, m.end() - 1):
            if kw not in tools[tool].params:
                errors.append("%s:%d: %s has no parameter %s" % (name, ln, tool, kw))
    for (kind, target, member), ln, rows in _tables_after_marks(lines):
        if not rows:
            errors.append("%s:%d: marker without a table" % (name, ln))
            continue
        if kind == "field":
            cmds = _target_commands(target, tools, resources)
            if cmds is None:
                errors.append("%s:%d: field-table target %s is neither a resource nor a tool" % (name, ln, target))
                continue
            allowed = set(UNIVERSAL_FIELDS)
            missing_level = False
            for h in dispatch.handlers(cmds):
                lf = dispatch.level_fields(h, member)
                if lf is None:
                    missing_level = True
                else:
                    allowed |= lf
            if missing_level and allowed == UNIVERSAL_FIELDS:
                errors.append("%s:%d: %s has no member %s" % (name, ln, target, member))
                continue
            where = target + ((" " + member) if member else "")
            for rln, cells in rows:
                spans = SPAN_RE.findall(cells[0])
                if not spans:
                    errors.append("%s:%d: field-table row without a field name" % (name, rln))
                for f in spans:
                    if not IDENT_RE.match(f) or f not in allowed:
                        errors.append("%s:%d: field %s not set for %s in dispatch.c" % (name, rln, f, where))
        else:
            for rln, cells in rows:
                spans = SPAN_RE.findall(cells[0])
                if len(spans) != 1 or spans[0] not in tools:
                    errors.append("%s:%d: result-table row does not name one tool: %s" % (name, rln, cells[0]))
                    continue
                tool = spans[0]
                allowed = dispatch.fields_of_commands(tools[tool].commands) | UNIVERSAL_FIELDS
                if len(cells) < 2 or not SPAN_RE.findall(cells[1]):
                    errors.append("%s:%d: result-table row without result fields" % (name, rln))
                    continue
                for f in SPAN_RE.findall(cells[1]):
                    if not IDENT_RE.match(f) or f not in allowed:
                        errors.append("%s:%d: field %s not in the result of %s (dispatch.c)" % (name, rln, f, tool))
                    elif f not in UNIVERSAL_FIELDS and f not in tools[tool].doc_words:
                        errors.append("%s:%d: field %s not named in the docstring of %s" % (name, rln, f, tool))
    errors += _check_code_blocks(name, text, tools, resources, dispatch)
    return errors


SELF_TEST_CASES = [
    ("misspelled tool", "Call `emu_cmt_tape_seak` here."),
    ("misspelled wildcard", "All `emu_cmt_sett_*` tools."),
    ("misspelled resource", "Read `emulator://periph/cmt/tapes`."),
    ("misspelled parameter", "emu_cmt_tape_seek(blok_id=2)"),
    ("misspelled parameter of another tool", "emu_run_until_addr(adress=0x24)"),
    ("misspelled resource field",
     "<!-- field-table: emulator://periph/cmt/tape -->\n| Field | x |\n|--|--|\n| `curent_block` | x |\n"),
    ("field of the other resource",
     "<!-- field-table: emulator://periph/cmt -->\n| Field | x |\n|--|--|\n| `blocks` | x |\n"),
    ("field of another level",
     "<!-- field-table: emulator://periph/cmt/tape blocks -->\n| Field | x |\n|--|--|\n| `count` | x |\n"),
    ("unknown member",
     "<!-- field-table: emulator://periph/cmt/tape blokcs -->\n| Field | x |\n|--|--|\n| `name` | x |\n"),
    ("misspelled result field",
     "<!-- result-table -->\n| Tool | Fields |\n|--|--|\n| `emu_cmt_tape_import` | `imported`, `new_tapes` |\n"),
    ("result field of another tool of the same handler",
     "<!-- result-table -->\n| Tool | Fields |\n|--|--|\n| `emu_cmt_stop` | `action`, `warning` |\n"),
    ("misspelled field in a code block", "```python\nr = emu_cmt_stop()\nx = r[\"recordd\"][\"mzf\"]\n```\n"),
    ("misspelled field of a loop variable in a code block",
     "```python\nt = emulator://periph/cmt/tape\nn = [b[\"blok_id\"] for b in t[\"blocks\"]]\n```\n"),
    ("field of the tool assigned earlier to a reused variable",
     "```python\nr = emu_cmt_tape_info()\nr = emu_cmt_tape_check()\nx = r[\"storable\"]\n```\n"),
    ("misspelled resource field after another assignment",
     "```python\nt = emu_get_registers()\nt = emulator://periph/cmt/tape\nx = t[\"curent_block\"]\n```\n"),
    ("misspelled field after a resource URI in a code block",
     "```python\nx = emulator://periph/cmt/tape[\"last_recordng\"]\n```\n"),
]

SELF_TEST_GOOD = (
    "`emu_cmt_tape_seek`, `emu_cmt_set_*`, `emulator://periph/cmt/tape`, emu_cmt_tape_seek(block_id=2)\n\n"
    "<!-- field-table: emulator://periph/cmt/tape -->\n| Field | x |\n|--|--|\n| `current_block`, `state_rev` | x |\n\n"
    "<!-- field-table: emulator://periph/cmt/tape blocks -->\n| Field | x |\n|--|--|\n| `block_id`, `size` | x |\n\n"
    "<!-- field-table: emulator://periph/cmt/tape last_recording -->\n| Field | x |\n|--|--|\n| `serial` | x |\n\n"
    "<!-- result-table -->\n| Tool | Fields |\n|--|--|\n| `emu_cmt_tape_import` | `imported`, `new_tape` |\n"
    "| `emu_cmt_eject` | `action`, `warning` |\n\n"
    "```python\nr = emu_cmt_stop()\nx = r[\"recorded\"][\"mzf\"]\nregs = emu_get_registers(); y = regs[\"SP\"]\n"
    "r = emu_get_registers(); z = r[\"PC\"]\n"
    "t = emulator://periph/cmt/tape\nn = [b[\"block_id\"] for b in t[\"blocks\"]]\n"
    "emu_run_until_addr(addr=0x24)\n```\n"
)


def self_test(tools, resources, dispatch):
    """Ověří, že kontrola na pokaženém textu selže a na správném projde. Vrací seznam chyb samotného testu."""
    problems = []
    good = check_text("self-test", SELF_TEST_GOOD, tools, resources, dispatch)
    if good:
        problems.append("self-test: a correct text was reported: %s" % good)
    for label, text in SELF_TEST_CASES:
        if not check_text("self-test", text, tools, resources, dispatch):
            problems.append("self-test: %s was not detected" % label)
    return problems


def main():
    if len(sys.argv) != 4:
        print("usage: test_agent_docs_names.py <docs/agent> <mcp_server.py> <dispatch.c>", file=sys.stderr)
        return 1
    docs_dir, wrapper, dispatch_c = sys.argv[1:]
    tools, resources = load_wrapper(wrapper)
    dispatch = Dispatch(dispatch_c)
    cmt_tools = [t for t in tools if t.startswith("emu_cmt_")]
    if len(cmt_tools) < 20 or "emulator://periph/cmt" not in resources or not dispatch.cmd_map:
        print("FAIL: wrapper or dispatch.c not parsed (tools %d, commands %d)"
              % (len(cmt_tools), len(dispatch.cmd_map)), file=sys.stderr)
        return 1

    errors, field_tables, result_tables = [], set(), 0
    for fname in sorted(os.listdir(docs_dir)):
        # README.md popisuje adresář pro správce a wrapper ho jako resource nevystavuje
        if not fname.endswith(".md") or fname.lower() == "readme.md":
            continue
        with open(os.path.join(docs_dir, fname), encoding="utf-8") as fh:
            text = fh.read()
        errors += check_text(fname, text, tools, resources, dispatch)
        for (kind, target, member), _, _ in _tables_after_marks(text.split("\n")):
            if kind == "field":
                field_tables.add((target, member))
            else:
                result_tables += 1
    # konvence nesmí z dokumentu tiše zmizet (pak by se pole nekontrolovala)
    for need in (("emulator://periph/cmt", None), ("emulator://periph/cmt/tape", None),
                 ("emulator://periph/cmt/tape", "blocks")):
        if need not in field_tables:
            errors.append("no field-table for %s in docs/agent" % " ".join(x for x in need if x))
    if result_tables == 0:
        errors.append("no result-table in docs/agent")
    real_subscripts = CODE_SUBSCRIPTS_CHECKED[0]
    if real_subscripts == 0:
        errors.append("no field subscript in code blocks of docs/agent was checked")
    errors += self_test(tools, resources, dispatch)

    for e in errors:
        print("FAIL: " + e, file=sys.stderr)
    if errors:
        return 1
    print("agent docs: %d cassette tools, %d field tables, %d result tables, %d code-block fields checked: PASS"
          % (len(cmt_tools), len(field_tables), result_tables, real_subscripts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
