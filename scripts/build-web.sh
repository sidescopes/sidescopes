#!/bin/sh
# Builds the browser lab: the application, in WebAssembly.
#
# The web is a platform here, not a side project. src/platform/web answers the
# same seams macOS and Windows answer, the lab links the same libraries the
# desktop executables link, and CMake owns the source lists for all of them —
# so a new src/app unit reaches this build without anyone remembering to add
# it. This script only configures that build and assembles the page around it.
#
# Single-threaded on purpose. The analysis is bit-exact regardless of how
# many chunks run - proven on every push across runners with different core
# counts - so one chunk is a tested configuration rather than a degraded
# one. Threads would need SharedArrayBuffer, which needs cross-origin
# isolation, which changes what the whole document may embed.
#
# The page around it is src/web/index.html, in this repo: the web build's
# shell, as cmake/Info.plist.in is the macOS one. So the browser version can
# be built and looked at from a clean checkout with no website involved.
#
#   scripts/build-web.sh [outdir]
#   scripts/build-web.sh --serve [outdir]        builds, then serves it
#   scripts/build-web.sh --standalone [outdir]   also emits one self-contained
#                                                file that opens from disk
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERVE=0
STANDALONE=0
while :; do
    case "${1:-}" in
        --serve) SERVE=1; shift ;;
        --standalone) STANDALONE=1; shift ;;
        *) break ;;
    esac
done
OUT=${1:-$ROOT/build-web}
BUILD=$ROOT/build-web-cmake
PORT=${SIDESCOPES_WEB_PORT:-8099}

command -v emcmake >/dev/null || {
    echo "build-web: no emcmake on PATH. brew install emscripten" >&2
    exit 2
}

mkdir -p "$OUT"

# Refresh compiler and toolchain paths after SDK upgrades. Disconnected
# updates reuse fetched dependencies while still permitting initial population.
emcmake cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
cmake --build "$BUILD"

# Anything named for the lab goes first. The whole directory is what gets
# uploaded, so a renamed artefact - the module was .mjs before it became a
# classic .js - would otherwise sit there being published indefinitely.
rm -f "$OUT"/sidescopes-lab.*
cp "$BUILD/sidescopes-lab.js" "$BUILD/sidescopes-lab.wasm" "$OUT/"

# Sample photographs, small enough to fetch on a page.
#
# The set is scripts/scenarios/photos.json — public domain works of the US
# federal government (17 U.S.C. 105), digest-pinned there and fetched below.
#
# face-portrait.jpg is deliberately NOT among them, and the distinction is
# worth keeping straight: right of publicity is separate from copyright, and a
# LIVING person's official portrait on a lab page invites the reading that
# she endorses this. skin-and-colour.jpg carries a face too and is shown,
# because it is an archival photograph from 1942 and nothing about it suggests
# its subject endorses software made eighty years later. The test for a face
# here is that reading, not the presence of a face.
CACHE=${SIDESCOPES_PHOTO_CACHE:-$HOME/.cache/sidescopes/scenarios}
SAMPLES="skin-and-colour neutral-detail wide-tonal-range flat-field"
# Browser CI fulfils these requests with a deterministic in-memory pixel: it
# is testing touch and embedding, not the availability of four external
# archives. Production and local builds keep the real public-domain set.
[ "${SIDESCOPES_WEB_SKIP_SAMPLES:-0}" = "1" ] && SAMPLES=""

# Fetched here rather than assumed: the manifest is the citable artefact and
# checks every download against its digest, so this needs nothing committed to
# the repository and nobody to have run the scenario harness first. A failure
# is not fatal - the lab falls back to generated patterns and says so, which
# is the same contract the harness degrades under.
for name in $SAMPLES; do
    python3 "$ROOT/scripts/scenarios/content.py" "$CACHE" "$name.jpg" || true
done

# The same mark the website wears, taken from this repository's own brand
# assets: nothing new is committed, and the lab does not reach into the
# website repo to dress itself.
cp "$ROOT/assets/brand/icons/linux/sidescopes-32.png" "$OUT/favicon-32.png"
cp "$ROOT/assets/brand/icons/linux/sidescopes-256.png" "$OUT/apple-touch-icon.png"

mkdir -p "$OUT/licenses"
rm -f "$OUT/licenses/"*.txt
cp "$BUILD/licenses/"*.txt "$OUT/licenses/"

mkdir -p "$OUT/samples"
# Anything the set no longer names goes, because the whole directory is what
# gets uploaded: a sample dropped from the list would otherwise linger here
# and be published for as long as nobody noticed.
for stale in "$OUT/samples"/*.jpg; do
    [ -e "$stale" ] || continue
    case " $SAMPLES " in
        *" $(basename "$stale" .jpg) "*) ;;
        *) rm -f "$stale" ;;
    esac
done

made=0
for name in $SAMPLES; do
    [ -f "$CACHE/$name.jpg" ] || continue
    [ -f "$OUT/samples/$name.jpg" ] && { made=$((made + 1)); continue; }
    magick "$CACHE/$name.jpg" -resize 1100x1100\> -quality 82 -strip "$OUT/samples/$name.jpg"
    made=$((made + 1))
done
if [ "$made" -eq 0 ]; then
    echo "build-web: no sample photographs at $CACHE, and none could be fetched."
    echo "  The lab falls back to generated patterns. Check the network, or the"
    echo "  digests in scripts/scenarios/photos.json if a source has changed."
else
    echo "build-web: $made sample photograph(s) in $OUT/samples"
fi

# The loader is stamped with the build's own digest. Without it a browser
# happily keeps the previous module - the page changes, the WebAssembly does
# not, and the result reads as "my change did nothing" rather than as a
# cache. Cost nothing on R2, where the whole directory is already hashed.
DIGEST=$(shasum -a 256 "$OUT/sidescopes-lab.wasm" | cut -c1-10)
sed "s|src=\"sidescopes-lab.js\"|src=\"sidescopes-lab.js?b=$DIGEST\"|" \
    "$ROOT/src/web/index.html" > "$OUT/index.html"

# --standalone: one file that can be downloaded and opened by double-clicking
# it, with no server anywhere. A page loaded from file:// may not import a
# module, may not fetch a sibling .wasm, and may not fetch a sibling JPEG - so
# the engine goes inline with its WebAssembly inside it, and the photographs
# go in as data: URIs, which fetch WILL read from a file:// page.
if [ "$STANDALONE" -eq 1 ]; then
    SINGLE=$ROOT/build-web-single
    emcmake cmake -S "$ROOT" -B "$SINGLE" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DFETCHCONTENT_UPDATES_DISCONNECTED=ON -DSIDESCOPES_WEB_SINGLE_FILE=ON
    cmake --build "$SINGLE"
    python3 "$ROOT/scripts/web-standalone.py" \
        --page "$ROOT/src/web/index.html" \
        --engine "$SINGLE/sidescopes-lab.js" \
        --samples "$OUT/samples" \
        --icon "$ROOT/assets/brand/icons/linux/sidescopes-32.png" \
        --licenses "$OUT/licenses" \
        --out "$OUT/sidescopes-lab.html"
    ls -lh "$OUT/sidescopes-lab.html" | awk '{print "standalone:", $5, $9}'
fi

echo
echo "built into $OUT"
echo "  index.html            needs a web server - it loads the .wasm beside it"
if [ -f "$OUT/sidescopes-lab.html" ]; then
    echo "  sidescopes-lab.html  self-contained; open it straight from disk"
else
    echo "  (pass --standalone for a single file that opens from disk)"
fi

if [ "$SERVE" -eq 1 ]; then
    echo
    echo "serving $OUT at http://127.0.0.1:$PORT/  (Ctrl-C to stop)"
    cd "$OUT" && exec python3 -m http.server "$PORT" --bind 127.0.0.1
fi
