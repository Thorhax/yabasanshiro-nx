#!/usr/bin/env python3
"""Removes Launcher member functions (definition and class declaration) from
DolphinSwitch/Launcher.cpp. Used when adapting NaGa's Dolphin launcher: the
listed members are GameCube/Wii specific or features we don't ship.

Usage: strip_members.py Launcher.cpp Name [Name ...]
"""
import re
import sys


def strip(source, name):
    lines = source.split("\n")

    # Definition: starts at column 0 with "... Launcher::Name(" and ends at the
    # first line that is exactly "}".
    start = None
    for i, line in enumerate(lines):
        if not line[:1].isspace() and re.search(r"\bLauncher::%s\(" % re.escape(name), line):
            start = i
            break
    if start is None:
        raise SystemExit("no definition for %s" % name)
    # Return types split onto their own line ("bool\nLauncher::X(")
    while start > 0 and lines[start - 1].strip() and not lines[start - 1].rstrip().endswith((";", "}", "{")) \
            and not lines[start - 1].startswith("//"):
        start -= 1
    end = start
    while lines[end] != "}":
        end += 1
    # Drop the blank line that separated it from the next definition
    if end + 1 < len(lines) and not lines[end + 1].strip():
        end += 1
    del lines[start:end + 1]

    # Declaration inside the class: an indented line naming the member, up to
    # the line that ends with ';'.
    decl = None
    for i, line in enumerate(lines):
        if line.startswith("  ") and not line.startswith("   ") and re.search(r"[\s*&]%s\(" % re.escape(name), line) \
                and "Launcher::" not in line:
            decl = i
            break
    if decl is not None:
        end = decl
        while not lines[end].rstrip().endswith(";"):
            end += 1
        del lines[decl:end + 1]

    return "\n".join(lines)


def main():
    path = sys.argv[1]
    with open(path) as f:
        source = f.read()
    for name in sys.argv[2:]:
        source = strip(source, name)
        print("removed", name)
    with open(path, "w") as f:
        f.write(source)


if __name__ == "__main__":
    main()
