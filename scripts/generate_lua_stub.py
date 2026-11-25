#!/usr/bin/env python3
"""Generate the LuaLS stub file for the `mu` API from comments in src/Lua.cpp.

Two comment forms are read:

  /*lua ... */                 verbatim Lua stub text (functions, aliases, classes)
  // lua ---@class X           start of a class for the next `xGroup.fields = {`
  // lua ---@field name type   documents the *Field("name", ...) that follows;
                               continuation lines are plain `//` comments

Usage:
  generate_lua_stub.py [-o FILE]   write the stubs (default: lua/mu.lua)
  generate_lua_stub.py --check     verify the comments against the code and that
                                   the committed stub file is up to date
"""

import argparse
import re
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "Lua.cpp"
DEFAULT_OUT = ROOT / "lua" / "mu.lua"

KIND_TYPE = {
    "string": "string",
    "int": "integer",
    "bool": "boolean",
    "color": "mu.Color",
    "float": "number",
    "stringList": "string[]",
}

BLOCK_RE = re.compile(r"/\*lua[ \t]*\n(.*?)\*/", re.DOTALL)
FIELD_CALL_RE = re.compile(r"\b(string|int|bool|float|color|enum|sub|func|stringList)Field\(\s*\"(\w+)\"")
GROUP_RE = re.compile(r"\b(\w+Group)\.fields\s*=\s*\{")


class Problems(list):
    def add(self, line, msg):
        self.append(f"{SRC.name}:{line}: {msg}")


def read_blocks(text):
    """Verbatim /*lua blocks, dedented, with the line they start on."""
    out = []
    for m in BLOCK_RE.finditer(text):
        line = text.count("\n", 0, m.start()) + 1
        out.append((line, textwrap.dedent(m.group(1)).strip("\n")))
    return out


def read_comment_docs(lines):
    """Collect `// lua ...` entries: (first_line, text, index_of_next_code_line)."""
    entries = []
    i = 0
    while i < len(lines):
        m = re.match(r"\s*//\s*lua\s+(---@.*)$", lines[i])
        if not m:
            i += 1
            continue
        first, text = i + 1, m.group(1).rstrip()
        i += 1
        # continuation lines: plain `//` comments directly below
        while i < len(lines):
            c = re.match(r"\s*//\s?(.*)$", lines[i])
            if not c or re.match(r"\s*//\s*lua\b", lines[i]):
                break
            text += " " + c.group(1).strip()
            i += 1
        entries.append((first, text.strip(), i))  # i = next line after comment
    return entries


def parse(text):
    lines = text.splitlines()
    problems = Problems()
    blocks = read_blocks(text)
    entries = read_comment_docs(lines)

    # Registered fields per group, from the code.
    groups = {}  # group var -> {"line": n, "fields": {name: (kind, line)}}
    i = 0
    while i < len(lines):
        m = GROUP_RE.search(lines[i])
        if not m:
            i += 1
            continue
        var, start = m.group(1), i
        i += 1
        fields = {}
        while i < len(lines) and not re.match(r"\s*\};", lines[i]):
            for fm in FIELD_CALL_RE.finditer(lines[i]):
                fields[fm.group(2)] = (fm.group(1), i + 1)
            i += 1
        groups[var] = {"line": start + 1, "fields": fields, "cls": None,
                       "docs": {}}

    # Attach doc entries to groups: a class entry directly above `xGroup.fields`,
    # fields inside the braces.
    group_at = {g["line"]: g for g in groups.values()}
    current = None
    for first, doc, nxt in entries:
        cm = re.match(r"---@class\s+(\S+)", doc)
        if cm:
            code_line = nxt + 1  # 1-based line of the code after the comment
            g = group_at.get(code_line)
            if not g:
                problems.add(first, f"'{cm.group(1)}': ---@class must be directly "
                                    "above a `xGroup.fields = {`")
                continue
            g["cls"] = cm.group(1)
            g["class_line"] = first
            current = g
            continue
        fm = re.match(r"---@field\s+(\w+)\??\s+(\S+)\s*(.*)$", doc)
        if fm and current:
            name, typ = fm.group(1), fm.group(2)
            current["docs"][name] = (typ, doc, first)
            # the code right after the doc must register exactly this name
            code = lines[nxt] if nxt < len(lines) else ""
            cf = FIELD_CALL_RE.search(code)
            if not cf:
                problems.add(first, f"@field '{name}' is not followed by a "
                                    "*Field(...) registration")
            elif cf.group(2) != name:
                problems.add(first, f"@field '{name}' documents the field "
                                    f"registered as '{cf.group(2)}'")
            else:
                want = KIND_TYPE.get(cf.group(1))
                if want and typ != want:
                    problems.add(first, f"@field '{name}' is {typ}, but "
                                        f"{cf.group(1)}Field implies {want}")
                if cf.group(1) == "func" and not typ.startswith("fun("):
                    problems.add(first, f"@field '{name}' must be a fun(...)")

    # Function docs in /*lua blocks: `function mu.name(`.
    block_funcs = {}
    for line, body in blocks:
        for fm in re.finditer(r"^function\s+mu\.(\w+)\s*\(", body, re.M):
            if fm.group(1) in block_funcs:
                problems.add(line, f"mu.{fm.group(1)} is declared twice")
            block_funcs[fm.group(1)] = line

    # Every registered field is documented, every documented field exists.
    for var, g in groups.items():
        if not g["cls"]:
            problems.add(g["line"], f"{var} has no `// lua ---@class` comment")
            continue
        is_root = g["cls"] == "mu"
        for name, (kind, line) in g["fields"].items():
            documented = name in g["docs"]
            via_block = is_root and kind == "func" and name in block_funcs
            if documented and via_block:
                problems.add(line, f"'{name}' is documented twice (@field and "
                                   "a /*lua block)")
            elif not documented and not via_block:
                problems.add(line, f"{g['cls']}.{name} has no doc comment")
        for name, (_, _, line) in g["docs"].items():
            if name not in g["fields"]:
                problems.add(line, f"{g['cls']}: @field '{name}' is not "
                                   "registered in the code")
    known = {n for g in groups.values() for n in g["fields"]}
    for name, line in block_funcs.items():
        if name not in known:
            problems.add(line, f"/*lua documents mu.{name}, which is not registered")

    # Event names and window fields must match the code.
    body_all = "\n".join(b for _, b in blocks)
    docs_events = set(re.findall(r'^---\|\s+"(\w+)"', body_all, re.M))
    code_events = set(re.findall(r'^\s*\{\s*"(\w+)",\s*EventType::', text, re.M))
    if docs_events != code_events:
        problems.add(1, "mu.Event aliases differ from kEventNames: "
                        f"docs-only {sorted(docs_events - code_events)}, "
                        f"code-only {sorted(code_events - docs_events)}")

    wm = re.search(r"---@class mu\.Window\n(.*?)(?:\n\s*\n|\Z)", body_all, re.S)
    # Fields typed `fun(...)` are methods on the "mu.window" metatable
    # (kWindowMethods); the rest are data set by pushWindow.
    win_fields = re.findall(r"---@field\s+(\w+)\s+(\S+)", wm.group(1)) if wm else []
    doc_win = {n for n, t in win_fields if not t.startswith("fun(")}
    doc_methods = {n for n, t in win_fields if t.startswith("fun(")}

    pm = re.search(r"^pushWindow\(.*?^\}", text, re.S | re.M)
    code_win = set(re.findall(r'lua_setfield\(L, -2, "(\w+)"\)', pm.group(0))) \
        if pm else set()
    km = re.search(r"kWindowMethods\[\]\s*=\s*\{(.*?)\};", text, re.S)
    code_methods = set(re.findall(r'\{\s*"(\w+)"', km.group(1))) if km else set()

    if doc_win != code_win:
        problems.add(1, "mu.Window fields differ from pushWindow: "
                        f"docs-only {sorted(doc_win - code_win)}, "
                        f"code-only {sorted(code_win - doc_win)}")
    if doc_methods != code_methods:
        problems.add(1, "mu.Window methods differ from kWindowMethods: "
                        f"docs-only {sorted(doc_methods - code_methods)}, "
                        f"code-only {sorted(code_methods - doc_methods)}")

    return blocks, groups, problems


def render(blocks, groups):
    decls = [b for _, b in blocks if not re.search(r"^function\s", b, re.M)]
    funcs = [b for _, b in blocks if re.search(r"^function\s", b, re.M)]

    out = ["---@meta mu",
           "-- Generated by scripts/generate_lua_stub.py from src/Lua.cpp. "
           "Do not edit.", ""]
    out += [d + "\n" for d in decls]

    ordered = sorted(groups.values(), key=lambda g: g["cls"] == "mu")
    for g in ordered:
        out.append(f"---@class {g['cls']}")
        for name in g["fields"]:
            if name in g["docs"]:
                out.append(g["docs"][name][1])
        # The root class must touch `mu = {}`, or LuaLS won't attach it.
        if g["cls"] != "mu":
            out.append("")
    out.append("mu = {}\n")
    out += [f + "\n" for f in funcs]
    return "\n".join(out).rstrip("\n") + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("-o", "--output", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--check", action="store_true",
                    help="verify comments against the code and the stub file")
    args = ap.parse_args()

    blocks, groups, problems = parse(SRC.read_text())
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1

    stubs = render(blocks, groups)
    if args.check:
        if not args.output.exists() or args.output.read_text() != stubs:
            print(f"{args.output} is out of date; run {Path(sys.argv[0]).name}",
                  file=sys.stderr)
            return 1
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(stubs)
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
