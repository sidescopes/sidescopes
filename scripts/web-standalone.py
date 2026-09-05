#!/usr/bin/env python3
"""Folds the browser lab into ONE file that opens straight from disk.

A page loaded over file:// is treated as its own opaque origin, which rules
out three of the things the served lab does: it may not import an ES module,
may not fetch the sibling .wasm the engine asks for, and may not fetch the
sample photographs. Each has an answer that costs only size:

  * the engine is built with -sSINGLE_FILE, which carries the WebAssembly
    inside it as base64, and is inlined here rather than linked;
  * the photographs go in as data: URIs, which fetch reads happily from a
    file:// page even though a sibling JPEG is refused;
  * the icon goes in the same way, so the tab wears the mark offline too.

The PAGE is the same src/web/index.html the served build uses. Keeping one
page rather than a second copy is the point: a standalone variant maintained
beside the real one drifts from it, and the drift is invisible until somebody
downloads the file.
"""

import argparse
import base64
import json
import pathlib
import re
import sys


def data_uri(path, media_type):
    return f"data:{media_type};base64," + base64.b64encode(path.read_bytes()).decode("ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--page", required=True, type=pathlib.Path)
    parser.add_argument("--engine", required=True, type=pathlib.Path)
    parser.add_argument("--samples", required=True, type=pathlib.Path)
    parser.add_argument("--icon", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--licenses", required=True, type=pathlib.Path)
    args = parser.parse_args()

    page = args.page.read_text(encoding="utf-8")

    # The engine, inline. The script tag is matched rather than assumed, so a
    # rename in the page is a loud failure here instead of a file that builds
    # and then loads nothing.
    engine_tag = re.search(r'<script src="sidescopes-lab\.js"[^>]*></script>', page)
    if engine_tag is None:
        print("web-standalone: the page no longer loads sidescopes-lab.js by that name", file=sys.stderr)

        return 1
    engine = args.engine.read_text(encoding="utf-8")
    if "SINGLE_FILE" not in engine and ".wasm" in engine:
        # The giveaway that the engine was built without -sSINGLE_FILE: it
        # still means to fetch a sibling binary, which from disk it cannot.
        print(f"web-standalone: {args.engine} still expects a separate .wasm", file=sys.stderr)

        return 1
    page = page.replace(engine_tag.group(0), "<script>\n" + engine + "\n</script>")

    # The photographs, inline. Whatever the samples directory holds, so the
    # set is the build's own answer rather than a list repeated here.
    pictures = {path.stem: data_uri(path, "image/jpeg") for path in sorted(args.samples.glob("*.jpg"))}

    # __STANDALONE tells the page not to refuse to run: it carries a guard
    # for being opened from disk, and this is the build that may be.
    inline = ("<script>\nwindow.__STANDALONE = true;\nwindow.__SAMPLES = "
              + json.dumps(pictures) + ";\n</script>\n")
    page = page.replace("<script>\n" + engine, inline + "<script>\n" + engine, 1)

    # The icon, so the tab is not a blank document offline either.
    page = page.replace('<link rel="icon" href="favicon-32.png" type="image/png" sizes="32x32">',
                        f'<link rel="icon" href="{data_uri(args.icon, "image/png")}" type="image/png" sizes="32x32">')
    page = page.replace('<link rel="apple-touch-icon" href="apple-touch-icon.png">', "")

    notices = [path.read_bytes().decode("utf-8") for path in sorted(args.licenses.glob("*.txt"))]
    if not notices:
        print("web-standalone: distribution license notices are missing", file=sys.stderr)
        return 1
    page = "<!--\n" + "\n\n".join(notices) + "\n-->\n" + page
    args.out.write_bytes(page.encode("utf-8"))
    print(f"web-standalone: {len(pictures)} photograph(s) inline, {args.out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
