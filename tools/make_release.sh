#!/bin/sh
# Builds the distributable player for Linux and Windows and packages it.
#
#   tools/make_release.sh VERSION
#
# Environment:
#   TINYCC_DIR      TinyCC 0.9.27 source tree (downloaded if unset)
#   SDL2_MINGW_DIR  SDL2 MinGW dev package root (SDL2-2.32.x); Windows is
#                   skipped if unset and third_party/sdl2-mingw is absent
#   WB_ROM        ROM used only to check the packages contain none of it
set -eu

VERSION=${1:?usage: tools/make_release.sh VERSION}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIST=$ROOT/dist
CACHE=$DIST/cache
TCC_TAR=tcc-0.9.27.tar.bz2
TCC_SHA256=de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c
ROM=${WB_ROM:-$ROOT/Wario Blast Featuring Bomberman! (USA, Europe) (SGB Enhanced).gb}

mkdir -p "$CACHE"
if [ ! -f "$CACHE/$TCC_TAR" ]; then
    curl -fsSL -o "$CACHE/$TCC_TAR" "https://download.savannah.gnu.org/releases/tinycc/$TCC_TAR"
fi
echo "$TCC_SHA256  $CACHE/$TCC_TAR" | sha256sum -c --quiet
if [ -z "${TINYCC_DIR:-}" ]; then
    rm -rf "$CACHE/tcc-src" && mkdir -p "$CACHE/tcc-src"
    tar -xjf "$CACHE/$TCC_TAR" -C "$CACHE/tcc-src"
    TINYCC_DIR=$CACHE/tcc-src/tcc-0.9.27
fi
SDL2_MINGW_DIR=${SDL2_MINGW_DIR:-$ROOT/third_party/sdl2-mingw/SDL2-2.32.10}

stage_common() {   # $1 = staging dir
    mkdir -p "$1/licenses" "$1/source" "$1/mods"
    sed "s/@VERSION@/$VERSION/" "$ROOT/tools/release/README.txt" > "$1/README.txt"
    cp "$ROOT/MODDING.md" "$1/MODDING.md"
    printf 'Put mod packs here, one folder per pack. See MODDING.md.\n' > "$1/mods/README.txt"
    cp -r "$ROOT/examples/mods/WarioBlast-Example" "$1/mods/"
    cp "$ROOT/LICENSE" "$1/LICENSE.txt"
    cp "$TINYCC_DIR/COPYING" "$1/licenses/TinyCC-COPYING.txt"
    cp "$CACHE/$TCC_TAR" "$1/source/"
}

PACKAGES=""

# ---- Linux ----
B=$ROOT/build-release-linux
cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWB_PLAYER=ON -DTINYCC_DIR="$TINYCC_DIR" >/dev/null
ninja -C "$B" warioblast >/dev/null
NAME=WarioBlast-Recomp-$VERSION-linux-x86_64
S=$DIST/stage/$NAME
rm -rf "$S" && mkdir -p "$S"
cp "$B/warioblast" "$B/libtcc.so" "$S/"
strip "$S/warioblast" "$S/libtcc.so"
cp "$ROOT/assets/icon/warioblast-256.png" "$S/warioblast.png"
cp "$ROOT/tools/release/install-desktop-entry.sh" "$S/"
stage_common "$S"
PACKAGES="$PACKAGES linux:$S"

# ---- Windows ----
if [ -d "$SDL2_MINGW_DIR" ] && command -v x86_64-w64-mingw32-gcc >/dev/null; then
    B=$ROOT/build-release-win
    cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWB_PLAYER=ON \
          -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64.cmake" \
          -DSDL2_DIR="$SDL2_MINGW_DIR/x86_64-w64-mingw32/lib/cmake/SDL2" \
          -DTINYCC_DIR="$TINYCC_DIR" >/dev/null
    ninja -C "$B" warioblast >/dev/null
    NAME=WarioBlast-Recomp-$VERSION-windows-x64
    S=$DIST/stage/$NAME
    rm -rf "$S" && mkdir -p "$S"
    cp "$B/warioblast.exe" "$B/libtcc.dll" "$B/SDL2.dll" "$S/"
    x86_64-w64-mingw32-strip "$S/warioblast.exe" "$S/libtcc.dll"
    stage_common "$S"
    cp "$SDL2_MINGW_DIR/LICENSE.txt" "$S/licenses/SDL2-LICENSE.txt"
    PACKAGES="$PACKAGES windows:$S"
else
    echo "skipping Windows: no MinGW-w64 or SDL2 MinGW package" >&2
fi

# ---- check, then archive ----
for p in $PACKAGES; do
    S=${p#*:}
    if [ -f "$ROM" ]; then
        find "$S" -type f ! -name "*.tar.bz2" -exec python3 "$ROOT/tools/release/check_release.py" "$ROM" {} +
    else
        echo "warning: ROM not found, package contents not checked for game data" >&2
    fi
done
cd "$DIST/stage"
for p in $PACKAGES; do
    S=${p#*:}
    N=$(basename "$S")
    case $p in
    windows:*) rm -f "$DIST/$N.zip"; zip -qr "$DIST/$N.zip" "$N" ;;
    linux:*)   tar -czf "$DIST/$N.tar.gz" "$N" ;;
    esac
done
cd "$DIST"
sha256sum WarioBlast-Recomp-"$VERSION"-* > "SHA256SUMS-$VERSION.txt"
cat "SHA256SUMS-$VERSION.txt"
