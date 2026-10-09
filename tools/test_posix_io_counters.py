#!/usr/bin/env python3
"""Table test for the file layer's kernel-call counters (docs/r3d/divergences.md
#162): compat/xbox/xbox_posix_io.c must make every Win32 or kernel call
through R3D_IO_CALL( <category>, <call> ) (compat/xbox/xbox_io_stats.h), so
a frame-time load stage counts that call's time as io rather than CPU.

The file only builds against nxdk, so this reads its source rather than
running it:

1. Every call of a Win32/kernel function - every identifier in KERNEL_CATEGORY,
   and any other CamelCase or Nt*/Ke*/Mm*/Io*/Ob*/Rtl* name called like a
   function, so a new, unlisted call fails too - is the direct argument of
   R3D_IO_CALL, with the category KERNEL_CATEGORY gives it. GetLastError (a
   read of the thread's error field, no kernel transition) is the one
   exception.
2. Each public operation, followed through the file's own static helpers it
   calls, reaches exactly the categories EXPECTED gives it - the table the
   counters' meaning (xbox_io_stats.h) and the recorder's documentation
   (docs/r3d/frametime-recorder.md) promise.

Run by tools/host-tests/run.sh:
    python tools/test_posix_io_counters.py
"""

import os
import re
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(REPO, "compat", "xbox", "xbox_posix_io.c")
HEADER = os.path.join(REPO, "compat", "xbox", "xbox_io_stats.h")

# Every Win32/kernel function the layer calls, and the counter it belongs in.
KERNEL_CATEGORY = {
    "CreateFileA": "opens",
    "ReadFile": "reads",
    "WriteFile": "writes",
    "SetFilePointer": "seeks",
    "CloseHandle": "closes",
    "GetFileAttributesExA": "probes",
    "GetFileAttributesA": "probes",
    "NtFlushBuffersFile": "meta",
    "SetEndOfFile": "meta",
    "GetFileInformationByHandle": "meta",
    "CreateDirectoryA": "meta",
    "RemoveDirectoryA": "meta",
    "DeleteFileA": "meta",
    "MoveFileA": "meta",
    "NtDuplicateObject": "meta",
    "GetFileSizeEx": "meta",
    "FindFirstFileA": "meta",
    "FindNextFileA": "meta",
    "FindClose": "meta",
}
NOT_KERNEL = {"GetLastError"}
CATEGORIES = {"opens", "reads", "writes", "seeks", "probes", "closes", "meta"}

# Public operation -> the categories it can produce (any path through it).
EXPECTED = {
    "open": {"opens", "seeks", "closes"},       # O_APPEND seek; full-table cleanup close
    "read": {"reads"},
    "write": {"writes"},
    "lseek": {"seeks"},
    "close": {"closes"},
    "fsync": {"meta"},
    "_commit": {"meta"},
    "ftruncate": {"seeks", "meta"},
    "stat": {"probes"},
    "fstat": {"meta"},
    "mkdir": {"meta"},
    "remove": {"probes", "meta"},
    "rename": {"meta"},
    "unlink": {"meta"},
    "rmdir": {"meta"},
    "access": {"probes"},
    "chdir": {"probes"},
    "getcwd": set(),
    "dup": {"meta", "closes"},                  # full-table cleanup close
    "opendir": {"meta", "probes"},              # a failed scan looks the directory up once (#164)
    "readdir": {"meta"},
    "closedir": {"meta"},
    "fileno": set(),                            # registers pdclib's handle, no call
    "_r3d_release_stream_fd": {"closes"},       # through close(), never for a borrowed slot in practice
    "_filelengthi64": {"meta"},
    "_open": {"opens", "seeks", "closes"},
    "_read": {"reads"},
    "_write": {"writes"},
    "_lseek": {"seeks"},
    "_close": {"closes"},
    "_unlink": {"meta"},
    "_mkdir": {"meta"},
    "_rmdir": {"meta"},
    "_access": {"probes"},
    "_dup": {"meta", "closes"},
}

CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
WRAP_RE = re.compile(r"R3D_IO_CALL\(\s*(\w+)\s*,\s*([A-Za-z_]\w*)\s*\(")
KERNELISH_RE = re.compile(r"^(?:[A-Z][a-z0-9]+[A-Z]\w*|(?:Nt|Ke|Mm|Io|Ob|Rtl|Ex|Hal)[A-Z]\w*)$")


def strip_comments_and_strings(text):
    """C source with comments and string/char literals blanked (newlines kept)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * (min(j, n) - i - 1) + (c if j < n else ""))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def functions(code):
    """{name: body} for the file's function definitions (upstream style:
    the opening and closing braces in column 0)."""
    out = {}
    for m in re.finditer(r"^[A-Za-z_][^\n;{}()]*?\b(\w+)\s*\(([^;{}]*)\)\s*\n\{", code, re.M):
        start = m.end()
        end = code.find("\n}", start)
        out[m.group(1)] = code[start:end if end >= 0 else len(code)]
    # one-line definitions: the MSVC CRT aliases
    for m in re.finditer(r"^[A-Za-z_][^\n;{}()]*?\b(\w+)\s*\([^;{}\n]*\)\s*\{([^\n]*)\}[ \t]*$", code, re.M):
        out.setdefault(m.group(1), m.group(2))
    return out


def kernel_calls(body):
    """[(name, category or None)] for every kernel-looking call in a body;
    category is None when the call is not R3D_IO_CALL's direct argument."""
    wrapped = {}
    for m in WRAP_RE.finditer(body):
        wrapped[m.start(2)] = m.group(1)
    out = []
    for m in CALL_RE.finditer(body):
        name = m.group(1)
        if name in NOT_KERNEL or name == "R3D_IO_CALL":
            continue
        if name in KERNEL_CATEGORY or KERNELISH_RE.match(name):
            out.append((name, wrapped.get(m.start(1))))
    return out


def reached_categories(funcs, name, seen=None):
    seen = set() if seen is None else seen
    if name in seen or name not in funcs:
        return set()
    seen.add(name)
    cats = {c for _, c in kernel_calls(funcs[name]) if c}
    for m in CALL_RE.finditer(funcs[name]):
        callee = m.group(1)
        if callee != name and callee in funcs:
            cats |= reached_categories(funcs, callee, seen)
    return cats


class PosixIoCounters(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(SOURCE, encoding="utf-8") as f:
            cls.code = strip_comments_and_strings(f.read())
        cls.funcs = functions(cls.code)

    def test_header_has_every_category(self):
        with open(HEADER, encoding="utf-8") as f:
            header = strip_comments_and_strings(f.read())
        for cat in CATEGORIES:
            self.assertRegex(header, r"\buint32_t\s+%s\b" % cat)
        self.assertIn("R3D_IO_CALL", header)

    def test_every_kernel_call_is_counted_in_its_category(self):
        calls = kernel_calls(self.code)
        self.assertGreater(len(calls), 20)
        for name, cat in calls:
            self.assertIn(name, KERNEL_CATEGORY, "%s: a Win32/kernel call missing from KERNEL_CATEGORY" % name)
            self.assertIsNotNone(cat, "%s is called outside R3D_IO_CALL" % name)
            self.assertEqual(cat, KERNEL_CATEGORY[name], "%s counted as %s" % (name, cat))
        self.assertEqual({n for n, _ in calls}, set(KERNEL_CATEGORY), "a listed call no longer appears")

    def test_each_operation_reaches_its_categories(self):
        for op, want in EXPECTED.items():
            self.assertTrue(op in self.funcs, "%s not found" % op)
            self.assertEqual(reached_categories(self.funcs, op), want, op)

    def test_the_checker_catches_an_unwrapped_call(self):
        # the checker's own failure path (docs/r3d/lessons.md: test a new
        # tool's failure path too)
        body = "x = R3D_IO_CALL( meta, FindNextFileA( h, &d )); FindClose( h ); y = GetLastError();"
        self.assertEqual(kernel_calls(body), [("FindNextFileA", "meta"), ("FindClose", None)])
        self.assertEqual(kernel_calls("NtWhatever( a );"), [("NtWhatever", None)])
        self.assertEqual(kernel_calls("R3D_IO_CALL( seeks, SetFilePointer( h, 0, NULL, 2 ))"), [("SetFilePointer", "seeks")])
        funcs = functions(strip_comments_and_strings(
            "static int helper( int h )\n{\n\tR3D_IO_CALL( closes, CloseHandle( h ));\n}\n"
            "int op( void )\n{\n\t/* CreateFileA( */ helper( 1 );\n}\n"))
        self.assertEqual(reached_categories(funcs, "op"), {"closes"})


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
