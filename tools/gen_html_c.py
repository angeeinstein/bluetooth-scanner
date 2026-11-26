#!/usr/bin/env python3
"""Regenerates the embedded-HTML .c/.h pairs from their source .html files.

Run this after editing src/dashboard.html or src/live.html:

    python3 tools/gen_html_c.py src

We embed each page as a plain C string constant rather than via CMake's
EMBED_TXTFILES, because EMBED_TXTFILES hits a path-resolution bug in
PlatformIO's ESP-IDF integration (the generated .S source's path gets
double-prefixed with the build dir and CMake/SCons can't find it).
"""
import sys

# (html filename, generated C identifier) -- add a tuple here for each new
# embedded page; everything else (output .c/.h filenames, header content)
# is derived from the identifier.
PAGES = [
    ("dashboard.html", "dashboard_html"),
    ("live.html", "live_html"),
]

src_dir = sys.argv[1] if len(sys.argv) > 1 else "src"


def generate(html_name, ident):
    with open(src_dir + "/" + html_name, "r", encoding="utf-8", newline=None) as f:
        lines = f.readlines()

    out = []
    out.append(f"// Auto-generated from {html_name} -- do not hand-edit.\n")
    out.append("// Edit the .html and regenerate with tools/gen_html_c.py.\n\n")
    out.append(f'#include "{ident}.h"\n\n')
    out.append(f"const char {ident}[] =\n")
    for line in lines:
        line = line.rstrip("\n").rstrip("\r")
        esc = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append('"' + esc + '\\n"\n')
    out.append(";\n")

    with open(src_dir + "/" + ident + ".c", "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(out))

    header = (
        f"// Generated from {html_name} -- see tools/gen_html_c.py / {ident}.c.\n\n"
        "#pragma once\n\n"
        f"extern const char {ident}[];\n"
    )
    with open(src_dir + "/" + ident + ".h", "w", encoding="utf-8", newline="\n") as f:
        f.write(header)

    print("wrote", len(lines), "lines from", html_name)


for html_name, ident in PAGES:
    generate(html_name, ident)
