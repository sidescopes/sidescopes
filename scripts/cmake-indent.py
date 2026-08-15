#!/usr/bin/env python3
"""Refuses a CMake argument list whose lines do not agree on their indent.

WHY THIS AND NOT A FORMATTER. A full CMake formatter was measured first:
gersemi rewrites 362 of this file's 628 lines and explodes hand-written
one-liners like `option(NAME "help" OFF)` into four lines apiece. That is a
worse file and an unreviewable diff, to catch a fault it is not even aimed at.

WHY NOT AN INDENT-UNIT RULE. The fault this exists for was four source lines
added at 8 spaces inside a list indented 12. Eight is a multiple of four, so
"every indent is a multiple of four" passes it happily. What is wrong is not
the number - it is that the lines DISAGREE WITH THEIR NEIGHBOURS.

So: inside one command's parentheses, no argument line may be SHALLOWER than
the list it is in. Shallower is the mistake - a line that fell out of its
list. DEEPER is left alone on purpose, because it is real style here: the
operands of a COMMAND keyword are indented under it, and a rule that called
those wrong would cry wolf on correct code, which is how a check comes to be
ignored.

    scripts/cmake-indent.py CMakeLists.txt tests/CMakeLists.txt
"""

import pathlib
import re
import sys


SOURCE_LINE = re.compile(r'^\s*(?:\$\{[A-Z_]+\}/)?[\w./${}-]*\.(?:cpp|h|hpp|mm|c)\s*$')


def indent_of(line):
    return len(line) - len(line.lstrip(' '))


def offences(path):
    """Source-file lines inside one list that do not share an indent.

    ONLY source-file lines, and that narrowness is the point. A CMake list
    mixes kinds: keywords like COMMAND or CACHE take operands indented under
    them, deliberately and correctly, so a rule over every argument line either
    misses the fault or calls that style wrong - and a check that cries wolf on
    correct code is one that gets ignored.

    Source paths are homogeneous. Nothing legitimately indents one of them
    differently from its neighbours, so any disagreement among them is a
    mistake - which is exactly the fault this exists for: four sources spliced
    into a target at 8 spaces inside a list that sits at 12.
    """
    lines = path.read_text().split('\n')
    blocks = {}
    order = []
    stack = []
    depth = 0
    for number, raw in enumerate(lines, start=1):
        line = raw.rstrip()

        if depth > 0 and stack and SOURCE_LINE.match(line):
            key = stack[-1]
            if key not in blocks:
                blocks[key] = []
                order.append(key)
            blocks[key].append((number, indent_of(line)))

        for character in line:
            if character == '(':
                depth += 1
                stack.append((depth, number))
            elif character == ')':
                depth = max(0, depth - 1)
                if stack:
                    stack.pop()

    found = []
    for key in order:
        entries = blocks[key]
        if len(entries) < 2:
            continue
        counts = {}
        for _, indent in entries:
            counts[indent] = counts.get(indent, 0) + 1
        agreed = max(counts, key=lambda indent: (counts[indent], -indent))
        first = min(number for number, indent in entries if indent == agreed)
        for number, indent in entries:
            if indent != agreed:
                found.append((number, indent, agreed, first, lines[number - 1].strip()[:60]))

    return sorted(found)


def main(argv):
    paths = [pathlib.Path(a) for a in argv[1:]]
    if not paths:
        paths = sorted(pathlib.Path('.').rglob('CMakeLists.txt'))
        paths = [p for p in paths if 'build' not in p.parts and '_deps' not in p.parts]
    if not paths:
        print('cmake-indent: no CMakeLists.txt found - refusing to report success', file=sys.stderr)
        return 2

    total = 0
    for path in paths:
        for number, got, want, first, text in offences(path):
            total += 1
            print(f'{path}:{number}: source indented {got}, but this target lists its sources at {want} (line {first})')
            print(f'  {text}')
    print(f'cmake-indent: {len(paths)} file(s) checked, {total} disagreement(s)')

    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
