#!/usr/bin/env bash
# Build OurNotes for macOS and package it as a disk image:
#   dist/OurNotes-<version>-macos-<arch>.dmg
# holding OurNotes.app (with its own Qt, OpenSSL and MicroTeX fonts; ournotes-cli and
# ournotesd are in OurNotes.app/Contents/MacOS, AGENTS.md in Contents/Resources), a link
# to /Applications, and AGENTS.md (how an AI agent can work with OurNotes).
#
# Usage: scripts/build-dmg.sh [options]
#   --qt DIR         Qt 6 prefix (default: $QT_ROOT_DIR, else Homebrew's qt)
#   --build-dir DIR  build directory (default: build-dmg/)
#   --out DIR        where the disk image is written (default: dist/)
#   --no-build       package what was built before
#
# The app is signed ad hoc, which macOS accepts after right-click > Open. Set
# CODESIGN_IDENTITY to a "Developer ID Application: ..." identity to sign it properly,
# and NOTARY_PROFILE to a profile saved with `xcrun notarytool store-credentials` to
# also notarize and staple the disk image.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$SRC_DIR/build-dmg"
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

[ "$(uname -s)" = Darwin ] || die "this script runs on macOS"
if [ -z "$QT_DIR" ] && command -v brew >/dev/null; then
    QT_DIR="$(brew --prefix qt 2>/dev/null || true)"
fi
[ -d "$QT_DIR/lib/cmake/Qt6" ] || die "no Qt 6 found${QT_DIR:+ in $QT_DIR} (use --qt DIR, or: brew install qt)"
MACDEPLOYQT="$QT_DIR/bin/macdeployqt"
[ -x "$MACDEPLOYQT" ] || die "macdeployqt not found in $QT_DIR/bin"

# --- build -------------------------------------------------------------------------------
if [ "$BUILD" = 1 ]; then
    command -v cmake >/dev/null || die "cmake is required"
    info "Using Qt from $QT_DIR"
    args=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_DIR" -DOURNOTES_BUILD_TESTS=OFF)
    [ ! -f "$BUILD_DIR/CMakeCache.txt" ] && command -v ninja >/dev/null && args+=(-G Ninja)
    if command -v brew >/dev/null && brew --prefix openssl@3 >/dev/null 2>&1; then
        args+=(-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)")
    fi
    info "Configuring"
    cmake -S "$SRC_DIR" -B "$BUILD_DIR" "${args[@]}" >/dev/null
    info "Building (this takes a few minutes the first time)"
    cmake --build "$BUILD_DIR" --parallel "$(sysctl -n hw.ncpu)"
fi
BUILT_APP="$BUILD_DIR/src/gui/ournotes.app"
[ -d "$BUILT_APP" ] || die "nothing built in $BUILD_DIR (run without --no-build)"
VERSION="$(grep -m1 'OURNOTES_VERSION ' "$BUILD_DIR/generated/ournotes/version.h" | cut -d'"' -f2)"
ARCH="$(lipo -archs "$BUILT_APP/Contents/MacOS/ournotes" | tr ' ' '-')"

# --- assemble the bundle ----------------------------------------------------------------
STAGE="$BUILD_DIR/dmg-root"
APP="$STAGE/OurNotes.app"
info "Assembling OurNotes.app"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$BUILT_APP" "$APP"
cp "$BUILD_DIR/src/cli/ournotes-cli" "$BUILD_DIR/src/server/ournotesd" "$APP/Contents/MacOS/"
cp -R "$BUILD_DIR/share/ournotes/microtex" "$APP/Contents/Resources/microtex"
# for AI agents: what ournotes-cli can do (in the app, and beside it on the disk image)
cp "$SRC_DIR/docs/agents.md" "$APP/Contents/Resources/AGENTS.md"
cp "$SRC_DIR/docs/agents.md" "$STAGE/AGENTS.md"
ln -s /Applications "$STAGE/Applications"

info "Copying the Qt runtime into the bundle"
"$MACDEPLOYQT" "$APP" -verbose=1 \
    -executable="$APP/Contents/MacOS/ournotes-cli" -executable="$APP/Contents/MacOS/ournotesd"

# Homebrew's Qt keeps the plugins of all its modules in one folder, so macdeployqt also
# copies plugins whose frameworks it cannot deploy (QtPdf, QtVirtualKeyboard, ...) and
# what they pull in (QtQuick, QtNetwork, ...). Drop such plugins, then whatever no
# program or plugin loads any more.
CONTENTS="$(cd "$APP/Contents" && pwd -P)"
macho_id() { { otool -D "$1" | grep -v ':$' | head -n1; } || true; }
bundle_deps() {  # the files in the bundle a Mach-O file loads, whether present or not
    local id
    id="$(macho_id "$1")"
    otool -L "$1" | awk '/^\t/ {print $1}' | sort -u | while read -r dep; do
        [ "$dep" = "$id" ] && continue
        case "$dep" in
            @rpath/*) echo "$CONTENTS/Frameworks/${dep#@rpath/}" ;;
            @executable_path/*) echo "$CONTENTS/MacOS/${dep#@executable_path/}" ;;
            @loader_path/*) echo "$(dirname "$1")/${dep#@loader_path/}" ;;
        esac
    done
}
info "Removing what the application does not use"
while IFS= read -r plugin; do
    while IFS= read -r dep; do
        if [ ! -e "$dep" ]; then
            echo "    ${plugin#"$CONTENTS/"} (needs ${dep#"$CONTENTS/Frameworks/"})"
            rm -f "$plugin"
            break
        fi
    done < <(bundle_deps "$plugin")
done < <(find "$CONTENTS/PlugIns" -name '*.dylib')
queue=("$CONTENTS"/MacOS/*)
while IFS= read -r plugin; do queue+=("$plugin"); done < <(find "$CONTENTS/PlugIns" -name '*.dylib')
used="|"
i=0
while [ "$i" -lt "${#queue[@]}" ]; do
    f="${queue[$i]}"
    i=$((i + 1))
    while IFS= read -r dep; do
        [ -e "$dep" ] || die "${f#"$CONTENTS/"} needs ${dep#"$CONTENTS/"}, which is not in the bundle"
        dep="$(cd "$(dirname "$dep")" && pwd -P)/$(basename "$dep")"
        top="${dep#"$CONTENTS/Frameworks/"}"
        top="${top%%/*}"
        case "$used" in *"|$top|"*) ;; *) used="$used$top|"; queue+=("$dep") ;; esac
    done < <(bundle_deps "$f")
done
for entry in "$CONTENTS"/Frameworks/*; do
    case "$used" in *"|$(basename "$entry")|"*) ;; *) echo "    Frameworks/$(basename "$entry")"; rm -rf "$entry" ;; esac
done

# Nothing may load from the build machine: drop absolute rpaths, then check every binary.
macho_files() { find "$APP/Contents" -type f -exec sh -c 'file -b "$1" | grep -q Mach-O' _ {} \; -print; }
for exe in "$APP"/Contents/MacOS/*; do
    otool -l "$exe" | awk '/LC_RPATH/ {getline; getline; print $2}' | { grep -v '^@' || true; } | while read -r rpath; do
        install_name_tool -delete_rpath "$rpath" "$exe" 2>/dev/null
    done
done
bad=""
minos="0"
while IFS= read -r f; do
    id="$(macho_id "$f")"
    case "$id" in
        ""|@*) ;;
        *) install_name_tool -id "@rpath/${f#"$APP/Contents/Frameworks/"}" "$f" 2>/dev/null; id="$(macho_id "$f")" ;;
    esac
    foreign="$(otool -L "$f" | awk -v id="$id" '/^\t/ && $1 != id {print $1}' | { grep -Ev '^(@|/usr/lib/|/System/)' || true; } | sort -u | tr '\n' ' ')"
    [ -z "$foreign" ] || bad="$bad\n  ${f#"$APP/"}: $foreign"
    v="$(otool -l "$f" | awk '/LC_BUILD_VERSION/ {b=1} b && $1 == "minos" && !v {v=$2} /LC_VERSION_MIN_MACOSX/ {m=1} m && $1 == "version" && !v {v=$2} END {print v}')"
    [ -n "$v" ] && [ "$(printf '%s\n%s\n' "$minos" "$v" | sort -V | tail -n1)" = "$v" ] && minos="$v"
done < <(macho_files)
[ -z "$bad" ] || die "the bundle still uses libraries from this machine:$(printf '%b' "$bad")"

# Finder shows "requires macOS N" instead of a crash on older systems.
chmod u+w "$APP/Contents/Info.plist"
plutil -replace LSMinimumSystemVersion -string "$minos" "$APP/Contents/Info.plist"

# --- sign --------------------------------------------------------------------------------
IDENTITY="${CODESIGN_IDENTITY:--}"
if [ "$IDENTITY" = "-" ]; then
    info "Signing ad hoc"
    codesign --force --deep --sign - "$APP"
else
    info "Signing as $IDENTITY"
    codesign --force --deep --options runtime --timestamp --sign "$IDENTITY" "$APP"
fi
codesign --verify --deep --strict "$APP"

# --- disk image --------------------------------------------------------------------------
mkdir -p "$OUT_DIR"
DMG="$OUT_DIR/OurNotes-$VERSION-macos-$ARCH.dmg"
info "Creating $(basename "$DMG")"
rm -f "$DMG"
hdiutil create -volname "OurNotes $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG" >/dev/null
if [ "$IDENTITY" != "-" ]; then
    codesign --force --timestamp --sign "$IDENTITY" "$DMG"
    if [ -n "${NOTARY_PROFILE:-}" ]; then
        info "Notarizing (this can take a few minutes)"
        xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
        xcrun stapler staple "$DMG"
    fi
fi
info "Done: $DMG ($(du -h "$DMG" | cut -f1 | tr -d ' '), macOS $minos or newer)"
