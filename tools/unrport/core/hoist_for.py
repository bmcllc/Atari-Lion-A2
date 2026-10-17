#!/usr/bin/env python3
"""Old MSVC lets a variable declared in a for statement outlive the loop; clang scopes it to the loop.
Driven by clang's diagnostics: for every "use of undeclared identifier X", the nearest preceding
`for( T X = ...` is rewritten as `T X; for( X = ...`; a duplicate hoisted declaration is dropped.
Usage: hoist_for.py <source.cpp> <clang args...>   (edits the work copy, headers included)"""
import re, subprocess, sys, pathlib

src, flags = sys.argv[1], sys.argv[2:]
decl = re.compile(r'for\s*\(\s*((?:unsigned\s+|const\s+)*[A-Za-z_][\w:]*(?:<[^>;]*>)?[\s\*&]+)(\w+)\s*=')
ctor = re.compile(r'for\s*\(\s*([A-Za-z_][\w:]*<[^>;]*>\s+)(\w+)\s*(\([^;]*\))\s*;')
hoisted = re.compile(r'^(\s*)((?:unsigned\s+|const\s+)*[A-Za-z_][\w:]*(?:<[^>;]*>)?[\s\*&]+)(\w+); for\( (\w+)\s*=')

for round_ in range(60):
    p = subprocess.run(["clang++", *flags, "-fsyntax-only", src], capture_output=True, text=True)
    jobs = {}  # file -> {line: ("hoist", name) | ("dup", name)}
    for m in re.finditer(r"^(.+?):(\d+):\d+: error: (use of undeclared identifier|redefinition of) '(\w+)'", p.stderr, re.M):
        path, line, kind, name = m.group(1), int(m.group(2)), m.group(3), m.group(4)
        jobs.setdefault(path, {})[(line, name)] = kind
    if not jobs:
        break
    changed = False
    for path, errs in jobs.items():
        f = pathlib.Path(path)
        lines = f.read_text(encoding="latin-1").split("\n")
        edits = {}
        for (line, name), kind in errs.items():
            if kind.startswith("redefinition"):
                m = hoisted.match(lines[line - 1])
                if m and m.group(3) == name:
                    edits[line - 1] = m.group(1) + "for( " + name + " =" + lines[line - 1][m.end():]
                continue
            for i in range(line - 1, max(line - 400, -1), -1):
                m = decl.search(lines[i])
                if m and m.group(2) == name:
                    if i not in edits:
                        edits[i] = lines[i][:m.start()] + m.group(1) + name + "; for( " + name + " =" + lines[i][m.end():]
                    break
                m = ctor.search(lines[i])
                if m and m.group(2) == name:
                    if i not in edits:
                        edits[i] = lines[i][:m.start()] + m.group(1) + name + m.group(3) + "; for( ;" + lines[i][m.end():]
                    break
        for i, new in edits.items():
            lines[i] = new
            changed = True
        f.write_text("\n".join(lines), encoding="latin-1")
    if not changed:
        break
print(f"hoist_for: {src}: {round_ + 1} rounds")
