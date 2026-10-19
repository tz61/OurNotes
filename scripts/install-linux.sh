#!/usr/bin/env bash
# Build freePen and install it for the current user:
#   - the application (with its own copy of the Qt runtime) in ~/.local/opt/freepen
#   - freepen, freepen-cli and freepend linked into ~/.local/bin
#   - an application-menu entry, icons, and a launcher on the desktop
#
# Usage: scripts/install-linux.sh [options]
#   --qt DIR               Qt 6 prefix (default: newest ~/Qt/6.*/gcc_64, else the system Qt)
#   --prefix DIR           install location (default: ~/.local/opt/freepen)
#   --no-desktop-shortcut  do not put a launcher on the desktop
#   --no-build             install what was built before (in build-release/)
#   --uninstall            remove everything a previous run installed
#
# Your library (~/freePen) and settings (~/.config/freepen) are never touched.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$SRC_DIR/build-release"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
PREFIX="${FREEPEN_PREFIX:-$HOME/.local/opt/freepen}"
BIN_DIR="${FREEPEN_BIN_DIR:-$HOME/.local/bin}"
APPS_DIR="$DATA_HOME/applications"
ICONS_DIR="$DATA_HOME/icons/hicolor"
DESKTOP_DIR="${FREEPEN_DESKTOP_DIR:-$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")}"
QT_DIR="${QT_DIR:-}"
SHORTCUT=1
BUILD=1
UNINSTALL=0

die() { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --qt) QT_DIR="$2"; shift ;;
        --prefix) PREFIX="$2"; shift ;;
        --no-desktop-shortcut) SHORTCUT=0 ;;
        --no-build) BUILD=0 ;;
        --uninstall) UNINSTALL=1 ;;
        -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
    shift
done

MARKER="$PREFIX/.freepen-install"
MANIFEST="$PREFIX/install-manifest.txt"

refresh_caches() {
    command -v update-desktop-database >/dev/null && update-desktop-database -q "$APPS_DIR" 2>/dev/null || true
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t -f "$ICONS_DIR" 2>/dev/null || true
    for k in kbuildsycoca6 kbuildsycoca5; do
        if command -v "$k" >/dev/null; then "$k" >/dev/null 2>&1 || true; break; fi
    done
}

uninstall() {
    [ -f "$MARKER" ] || die "no freePen installation found in $PREFIX"
    info "Removing freePen from $PREFIX"
    if [ -f "$MANIFEST" ]; then
        while IFS= read -r f; do
            [ -n "$f" ] && { [ -e "$f" ] || [ -L "$f" ]; } && rm -f "$f"
        done < "$MANIFEST"
    fi
    rm -rf "$PREFIX"
    refresh_caches
}

if [ "$UNINSTALL" = 1 ]; then
    uninstall
    info "Done. Your library and settings were kept."
    exit 0
fi

# --- build -------------------------------------------------------------------------------
if [ "$BUILD" = 1 ]; then
    command -v cmake >/dev/null || die "cmake is required"
    if [ -z "$QT_DIR" ]; then
        QT_DIR="$(ls -d "$HOME"/Qt/6.*/gcc_64 2>/dev/null | sort -V | tail -n1 || true)"
    fi
    args=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DFREEPEN_DEPLOY_QT=ON -DFREEPEN_BUILD_TESTS=OFF)
    if [ -n "$QT_DIR" ]; then
        [ -d "$QT_DIR/lib/cmake/Qt6" ] || die "no Qt 6 found in $QT_DIR"
        info "Using Qt from $QT_DIR"
        args+=(-DCMAKE_PREFIX_PATH="$QT_DIR")
    else
        info "Using the system Qt 6"
    fi
    # reuse dependencies prepared for development builds, if present
    [ -f "$SRC_DIR/.deps/mupdf/lib/libmupdf.a" ] && args+=(-DMUPDF_ROOT="$SRC_DIR/.deps/mupdf")
    [ -f "$SRC_DIR/.deps/sqlite-src/sqlite3.c" ] && args+=(-DFETCHCONTENT_SOURCE_DIR_SQLITE="$SRC_DIR/.deps/sqlite-src")
    [ -d "$SRC_DIR/.deps/microtex-src/src" ] && args+=(-DFETCHCONTENT_SOURCE_DIR_MICROTEX="$SRC_DIR/.deps/microtex-src")
    [ -f "$SRC_DIR/.deps/tinyxml2-src/tinyxml2.cpp" ] && args+=(-DFETCHCONTENT_SOURCE_DIR_TINYXML2="$SRC_DIR/.deps/tinyxml2-src")
    info "Configuring"
    cmake -S "$SRC_DIR" -B "$BUILD_DIR" "${args[@]}" >/dev/null
    info "Building (this takes a few minutes the first time)"
    cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)" -- --no-print-directory >/dev/null
fi
[ -x "$BUILD_DIR/src/gui/freepen" ] || die "nothing built in $BUILD_DIR (run without --no-build)"

# --- install ------------------------------------------------------------------------------
if [ -f "$MARKER" ]; then
    info "Replacing the previous installation"
    uninstall >/dev/null
elif [ -e "$PREFIX" ] && [ -n "$(ls -A "$PREFIX" 2>/dev/null)" ]; then
    die "$PREFIX exists and was not created by this script; choose another --prefix"
fi
info "Installing into $PREFIX"
cmake --install "$BUILD_DIR" --prefix "$PREFIX" >/dev/null
touch "$MARKER"
: > "$MANIFEST"
record() { echo "$1" >> "$MANIFEST"; }

mkdir -p "$BIN_DIR" "$APPS_DIR" "$ICONS_DIR"
for exe in freepen freepen-cli freepend; do
    if [ -x "$PREFIX/bin/$exe" ]; then
        ln -sfn "$PREFIX/bin/$exe" "$BIN_DIR/$exe"
        record "$BIN_DIR/$exe"
    fi
done

# icons into the user's icon theme
while IFS= read -r icon; do
    rel="${icon#"$PREFIX/share/icons/hicolor/"}"
    mkdir -p "$ICONS_DIR/$(dirname "$rel")"
    cp -f "$icon" "$ICONS_DIR/$rel"
    record "$ICONS_DIR/$rel"
done < <(find "$PREFIX/share/icons/hicolor" -type f -name 'freepen.*')

# application menu entry, with an absolute Exec so it works without ~/.local/bin in PATH
entry="$APPS_DIR/freepen.desktop"
sed -e "s|^Exec=.*|Exec=\"$PREFIX/bin/freepen\" %F|" \
    -e "/^Exec=/a TryExec=$PREFIX/bin/freepen" \
    "$PREFIX/share/applications/freepen.desktop" > "$entry"
chmod 644 "$entry"
record "$entry"

if [ "$SHORTCUT" = 1 ] && [ -d "$DESKTOP_DIR" ]; then
    shortcut="$DESKTOP_DIR/freepen.desktop"
    cp -f "$entry" "$shortcut"
    chmod 755 "$shortcut"  # KDE and GNOME only launch executable desktop files
    command -v gio >/dev/null && gio set "$shortcut" metadata::trusted true 2>/dev/null || true
    record "$shortcut"
    info "Desktop launcher: $shortcut"
fi

refresh_caches
info "freePen $(grep -m1 'FREEPEN_VERSION ' "$BUILD_DIR/generated/freepen/version.h" | cut -d'"' -f2) installed."
echo "    Start it from the application menu or the desktop, or run: freepen"
case ":$PATH:" in *":$BIN_DIR:"*) ;; *) echo "    (add $BIN_DIR to PATH to use freepen-cli and freepend)";; esac
echo "    Uninstall with: $SRC_DIR/scripts/install-linux.sh --uninstall"
