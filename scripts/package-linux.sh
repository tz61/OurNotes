#!/usr/bin/env bash
# Build OurNotes for Linux and package it:
#   dist/OurNotes-<version>-<arch>.AppImage       the desktop application in one file
#   dist/ournotes-<version>-linux-<arch>.tar.gz   the same as a folder, with bin/ournotes,
#                                                 bin/ournotes-cli and bin/ournotesd
# Both bring their own Qt. They use the system's OpenSSL 3 (libssl3), which knows where
# the distribution keeps its certificates.
#
# Usage: scripts/package-linux.sh [options]
#   --qt DIR         Qt 6 prefix (default: $QT_ROOT_DIR, else newest ~/Qt/6.*/gcc_64, else the system Qt)
#   --build-dir DIR  build directory (default: build-appimage/)
#   --out DIR        where the packages are written (default: dist/)
#   --no-build       package what was built before
#
# linuxdeploy and linuxdeploy-plugin-qt are downloaded into the build directory, unless
# LINUXDEPLOY names a linuxdeploy that has the Qt plugin next to it.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$SRC_DIR/build-appimage"
OUT_DIR="$SRC_DIR/dist"
QT_DIR="${QT_ROOT_DIR:-}"
BUILD=1

die() { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --qt) QT_DIR="$2"; shift ;;
        --build-dir) BUILD_DIR="$2"; shift ;;
        --out) OUT_DIR="$2"; shift ;;
        --no-build) BUILD=0 ;;
        -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
    shift
done

[ "$(uname -s)" = Linux ] || die "this script runs on Linux"
ARCH="$(uname -m)"
if [ -z "$QT_DIR" ]; then
    QT_DIR="$(ls -d "$HOME"/Qt/6.*/gcc_64 2>/dev/null | sort -V | tail -n1 || true)"
fi
[ -z "$QT_DIR" ] || [ -d "$QT_DIR/lib/cmake/Qt6" ] || die "no Qt 6 found in $QT_DIR"

# --- build -------------------------------------------------------------------------------
if [ "$BUILD" = 1 ]; then
    command -v cmake >/dev/null || die "cmake is required"
    args=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
          -DOURNOTES_BUILD_TESTS=OFF -DOURNOTES_DEPLOY_QT=OFF)
    if [ -n "$QT_DIR" ]; then
        info "Using Qt from $QT_DIR"
        args+=(-DCMAKE_PREFIX_PATH="$QT_DIR")
    else
        info "Using the system Qt 6"
    fi
    [ ! -f "$BUILD_DIR/CMakeCache.txt" ] && command -v ninja >/dev/null && args+=(-G Ninja)
    info "Configuring"
    cmake -S "$SRC_DIR" -B "$BUILD_DIR" "${args[@]}" >/dev/null
    info "Building (this takes a few minutes the first time)"
    cmake --build "$BUILD_DIR" --parallel "$(nproc 2>/dev/null || echo 4)"
fi
[ -x "$BUILD_DIR/src/gui/ournotes" ] || die "nothing built in $BUILD_DIR (run without --no-build)"
VERSION="$(grep -m1 'OURNOTES_VERSION ' "$BUILD_DIR/generated/ournotes/version.h" | cut -d'"' -f2)"

APPDIR="$BUILD_DIR/AppDir"
info "Installing into $APPDIR"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" >/dev/null

# --- linuxdeploy -------------------------------------------------------------------------
if [ -z "${LINUXDEPLOY:-}" ]; then
    tools="$BUILD_DIR/linuxdeploy"
    mkdir -p "$tools"
    for tool in linuxdeploy linuxdeploy-plugin-qt; do
        if [ ! -x "$tools/$tool-$ARCH.AppImage" ]; then
            info "Downloading $tool"
            curl -fsSL -o "$tools/$tool-$ARCH.AppImage" \
                "https://github.com/linuxdeploy/$tool/releases/download/continuous/$tool-$ARCH.AppImage"
            chmod +x "$tools/$tool-$ARCH.AppImage"
        fi
    done
    LINUXDEPLOY="$tools/linuxdeploy-$ARCH.AppImage"
fi

export APPIMAGE_EXTRACT_AND_RUN=1  # no FUSE needed (containers, CI)
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"
if [ -n "$QT_DIR" ]; then
    QMAKE="$QT_DIR/bin/qmake"
    [ -x "$QMAKE" ] || QMAKE="$QT_DIR/bin/qmake6"
    export QMAKE
    export LD_LIBRARY_PATH="$QT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

info "Bundling Qt and creating the AppImage"
mkdir -p "$OUT_DIR"
rm -f "$BUILD_DIR"/OurNotes-*.AppImage
(
    cd "$BUILD_DIR"
    "$LINUXDEPLOY" --appdir "$APPDIR" \
        --executable "$APPDIR/usr/bin/ournotes" \
        --executable "$APPDIR/usr/bin/ournotes-cli" \
        --executable "$APPDIR/usr/bin/ournotesd" \
        --desktop-file "$APPDIR/usr/share/applications/ournotes.desktop" \
        --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/ournotes.png" \
        --exclude-library 'libssl.so*' --exclude-library 'libcrypto.so*' \
        --plugin qt --output appimage
)
APPIMAGE="OurNotes-$VERSION-$ARCH.AppImage"
[ -f "$BUILD_DIR/$APPIMAGE" ] || die "linuxdeploy did not create $APPIMAGE"
mv -f "$BUILD_DIR/$APPIMAGE" "$OUT_DIR/"

# The AppDir's usr/ runs as it is: linuxdeploy points the programs at ../lib and writes a
# qt.conf for the plugins.
NAME="ournotes-$VERSION-linux-$ARCH"
info "Creating $NAME.tar.gz"
rm -rf "${BUILD_DIR:?}/$NAME"
cp -a "$APPDIR/usr" "$BUILD_DIR/$NAME"
tar -C "$BUILD_DIR" -czf "$OUT_DIR/$NAME.tar.gz" "$NAME"

info "Done:"
echo "    $OUT_DIR/$APPIMAGE"
echo "    $OUT_DIR/$NAME.tar.gz"
