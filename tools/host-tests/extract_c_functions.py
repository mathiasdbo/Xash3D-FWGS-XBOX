#!/usr/bin/env python3
"""Copy named function definitions out of a C file, verbatim.

    python tools/host-tests/extract_c_functions.py <source.c> <out.c> NAME [NAME...]

A host test that needs a few functions of a large engine file (one whose
other functions pull in half the engine) compiles them from the real file
instead of a hand copy that would drift: each definition - the line at
column 0 that names the function and opens its parameter list, through
the first line after it that is exactly "}" - is written to <out.c>, in
the source's order, each behind a #line directive so a compile error
points into the real file. Everything between them (the file's own
includes, statics, other functions) is left out; the test supplies those.

Exits 1, writing nothing, when a name is not defined exactly once: a
renamed or deleted function must fail the test's build, not drop a check.
"""

import re
import sys


def extract(lines, name):
    head = re.compile(r"^(?!\s)(?!#)[^;(]*\b" + re.escape(name) + r"\s*\(")
    found = []
    for i, line in enumerate(lines):
        if head.match(line) and not line.rstrip().endswith(";"):
            found.append(i)
    if len(found) != 1:
        raise SystemExit("extract_c_functions: %s defined %d times" % (name, len(found)))
    start = found[0]
    for end in range(start + 1, len(lines)):
        if lines[end].rstrip("\r\n") == "}":
            return start, end
    raise SystemExit("extract_c_functions: %s has no closing brace" % name)


def main(argv):
    if len(argv) < 4:
        raise SystemExit(__doc__)
    src, out, names = argv[1], argv[2], argv[3:]
    with open(src, encoding="utf-8", newline="") as f:
        lines = f.readlines()
    spans = sorted(extract(lines, n) for n in names)
    text = []
    for start, end in spans:
        text.append('#line %d "%s"\n' % (start + 1, src.replace("\\", "/")))
        text.extend(l.rstrip("\r\n") + "\n" for l in lines[start:end + 1])
        text.append("\n")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
