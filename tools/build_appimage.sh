#!/usr/bin/env bash
# Baut unidaw als AppImage fuer Linux x86_64 (docs/appimage.md).
#
#   bash tools/build_appimage.sh [<ausgabe-ordner>]      (Standard dawd/build/appimage)
#
# - baut unidaw auf der aelteren Basis Ubuntu 22.04 (glibc 2.35, g++-12;
#   tools/buildroot.sh) im Build-Ordner dawd/build-ubuntu-22.04, damit das
#   AppImage auch auf aelteren Distributionen startet. UNIDAW_HOST_BUILD=1
#   baut stattdessen auf diesem Rechner in dawd/build (wie tests/run.sh) —
#   dann braucht das AppImage die glibc dieses Rechners.
#   JOBS=4 Compiler, mehr frisst den Speicher. ONNX Runtime und Rubberband
#   muessen da sein (tools/fetch_ml.sh), sonst fehlen dem Release ML-Ops
#   und Timestretch
# - AppDir: Binary, Bibliotheken ausser glibc und alsa-lib, Ressourcen aus
#   dem Git-Stand (HEAD, ohne Diva-Katalog), Lizenztexte; keine Modelle
#   (die laedt `<AppImage> fetch-models`)
# - AppImage = Runtime (AppImage type2-runtime, gepinnt mit SHA-256) +
#   SquashFS (mksquashfs, Paket squashfs-tools)
# UNIDAW_SOURCE_URL: Ort des Quellcodes fuer diese Version (AGPLv3), landet in
# usr/share/doc/unidaw/SOURCE.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mkdir -p "${1:-$ROOT/dawd/build/appimage}" && cd "${1:-$ROOT/dawd/build/appimage}" && pwd)"

# auf der aelteren Basis: dieses Skript dort noch einmal starten (Quellbaum,
# Git-Ordner und Ausgabe an denselben Pfaden)
if [ "${UNIDAW_HOST_BUILD:-0}" != 1 ] && [ -z "${UNIDAW_IN_BUILDROOT:-}" ]; then
    binds=(--bind "$ROOT" --bind "$OUT")
    common="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
    case "$common" in "$ROOT"/*) ;; *) binds+=(--bind "$common") ;; esac
    exec bash "$ROOT/tools/buildroot.sh" exec build "${binds[@]}" -- \
        /usr/bin/env UNIDAW_IN_BUILDROOT=1 CC=gcc-12 CXX=g++-12 JOBS="${JOBS:-4}" \
        UNIDAW_SOURCE_URL="${UNIDAW_SOURCE_URL:-}" /bin/bash "$ROOT/tools/build_appimage.sh" "$OUT"
fi
if [ -n "${UNIDAW_IN_BUILDROOT:-}" ]; then
    BUILD="$ROOT/dawd/build-ubuntu-22.04"
else
    BUILD="$ROOT/dawd/build"
fi
BIN="$BUILD/unidaw_artefacts/Release/unidaw"
APPDIR="$OUT/AppDir"
ORT_DIR="$ROOT/dawd/third_party/onnxruntime-linux-x64-1.31.0"
RB_DIR="$ROOT/dawd/third_party/rubberband"

RUNTIME_VERSION=20251108
RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
RUNTIME_DIR="$ROOT/dawd/third_party/appimage-runtime-$RUNTIME_VERSION"
RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_VERSION/runtime-x86_64"
RUNTIME_LICENSE_URL="https://raw.githubusercontent.com/AppImage/type2-runtime/$RUNTIME_VERSION/LICENSE"

die () { echo "build_appimage: $*" >&2; exit 1; }

[ "$(uname -m)" = x86_64 ] || die "only x86_64"
command -v mksquashfs >/dev/null || die "mksquashfs missing (package squashfs-tools)"
[ -f "$ORT_DIR/lib/libonnxruntime.so" ] || die "ONNX Runtime missing — run tools/fetch_ml.sh"
[ -f "$RB_DIR/single/RubberBandSingle.cpp" ] || die "Rubberband missing — run tools/fetch_ml.sh"

# 1. unidaw bauen
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    cmake -S "$ROOT/dawd" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
fi
grep -q '^CMAKE_BUILD_TYPE:STRING=Release$' "$BUILD/CMakeCache.txt" || die "$BUILD is not a Release build"
cmake --build "$BUILD" -j"${JOBS:-4}"
# beide Bausteine muessen in der Binary sein (fehlten sie beim Configure,
# baut CMake ohne sie): neu konfigurieren mit cmake -S unidaw -B dawd/build
readelf -d "$BIN" | grep -q 'libonnxruntime' || die "unidaw was built without ONNX Runtime — reconfigure $BUILD"
grep -q -a 'RubberBand' "$BIN" || die "unidaw was built without Rubberband — reconfigure $BUILD"

VERSION="$(sed -n 's/^project(unidaw VERSION \([0-9.]*\).*/\1/p' "$ROOT/dawd/CMakeLists.txt")"
[ -n "$VERSION" ] || die "no version in dawd/CMakeLists.txt"
COMMIT="$(git -C "$ROOT" rev-parse --short=12 HEAD)"
DIRTY=""
git -C "$ROOT" diff --quiet HEAD -- dawd/source dawd/launcher dawd/appimage dawd/ui dawd/kits dawd/plugin-profiles recipes \
    || DIRTY=" (with uncommitted changes)"

# 2. Runtime laden (einmal, gepinnt)
if [ ! -s "$RUNTIME_DIR/runtime-x86_64" ]; then
    mkdir -p "$RUNTIME_DIR"
    curl -fsSL -o "$RUNTIME_DIR/runtime-x86_64.part" "$RUNTIME_URL"
    echo "$RUNTIME_SHA256  $RUNTIME_DIR/runtime-x86_64.part" | sha256sum -c --quiet - \
        || { rm -f "$RUNTIME_DIR/runtime-x86_64.part"; die "AppImage runtime: SHA-256 does not match"; }
    mv "$RUNTIME_DIR/runtime-x86_64.part" "$RUNTIME_DIR/runtime-x86_64"
fi
[ -s "$RUNTIME_DIR/LICENSE" ] || curl -fsSL -o "$RUNTIME_DIR/LICENSE" "$RUNTIME_LICENSE_URL"

# 3. AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/unidaw" "$APPDIR/usr/share/doc/unidaw/licenses"
install -m 755 "$BIN" "$APPDIR/usr/bin/unidaw"
install -m 755 "$ROOT/dawd/launcher/unidaw-start" "$ROOT/dawd/launcher/unidaw-fetch-models" "$APPDIR/usr/bin/"
install -m 755 "$ROOT/dawd/appimage/unidaw-exec" "$APPDIR/usr/lib/unidaw-exec"
install -m 755 "$ROOT/dawd/appimage/AppRun" "$APPDIR/AppRun"
install -m 644 "$ROOT/dawd/appimage/unidaw.desktop" "$ROOT/dawd/appimage/unidaw.svg" "$APPDIR/"
ln -s unidaw.svg "$APPDIR/.DirIcon"
echo "$VERSION" > "$APPDIR/usr/share/unidaw/VERSION"

# Ressourcen aus dem Git-Stand: nur versionierte Dateien (keine
# preview.wav, keine Laufreste). Der Diva-Preset-Katalog bleibt draussen
# (docs/decisions/2026-11-18-oeffentlicher-stand-ohne-historie.md).
git -C "$ROOT" archive --format=tar HEAD -- dawd/ui dawd/kits dawd/plugin-profiles recipes \
    ':(exclude)dawd/plugin-profiles/vst3-357abb5a-presets.json' ':(exclude)dawd/kits/*.py' \
    | tar -x -C "$APPDIR/usr/share/unidaw"
mv "$APPDIR/usr/share/unidaw/dawd/"* "$APPDIR/usr/share/unidaw/"
rmdir "$APPDIR/usr/share/unidaw/dawd"

# Bibliotheken: alles, was unidaw und ONNX Runtime laden, ausser glibc (muss
# vom System kommen) und alsa-lib (muss zu den ALSA-Plugins und der
# Konfiguration des Systems passen, z. B. pipewire-alsa)
declare -A lib_pkgs=()
while read -r name path; do
    case "$name" in
        linux-vdso*|ld-linux*|libc.so.*|libm.so.*|libdl.so.*|librt.so.*|libpthread.so.*|libasound.so.*) continue ;;
    esac
    [ -f "$path" ] || die "library $name not found ($path)"
    cp -L "$path" "$APPDIR/usr/lib/$name"
    if pkg="$(dpkg -S "$(readlink -f "$path")" 2>/dev/null | head -1 | cut -d: -f1)" && [ -n "$pkg" ]; then
        lib_pkgs[$pkg]=1
    fi
done < <(LD_LIBRARY_PATH="$ORT_DIR/lib" ldd "$BIN" | awk '$2 == "=>" && $3 ~ /^\// { print $1, $3 }')
[ -f "$APPDIR/usr/lib/libonnxruntime.so.1" ] || die "libonnxruntime.so.1 not bundled"
# C++-Laufzeit eigener Ordner: unidaw-exec nimmt sie nur, wenn die des Systems
# aelter ist (neuere Plugins brauchen die neuere libstdc++)
mkdir -p "$APPDIR/usr/lib/cxx"
mv "$APPDIR/usr/lib/libstdc++.so.6" "$APPDIR/usr/lib/libgcc_s.so.1" "$APPDIR/usr/lib/cxx/"

# 4. Lizenzen (docs/lizenzen-agpl.md, Pflichten fuer die Veroeffentlichung)
DOC="$APPDIR/usr/share/doc/unidaw"
LIC="$DOC/licenses"
install -m 644 "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_LICENSES.md" "$DOC/"
copy_license () {   # copy_license <pfad relativ zum Quellbaum>
    [ -f "$ROOT/$1" ] || die "license text missing: $1"
    mkdir -p "$LIC/$(dirname "$1")"
    cp "$ROOT/$1" "$LIC/$1"
}
J=tracktion_engine/modules/juce/modules
T=tracktion_engine/modules
for f in \
    tracktion_engine/LICENSE.md \
    tracktion_engine/modules/juce/LICENSE.md \
    "$J/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt" \
    "$J/juce_audio_processors_headless/format_types/LV2_SDK/lv2/COPYING" \
    "$J/juce_audio_processors_headless/format_types/pslextensions/ipslcontextinfo.h" \
    "$J/juce_audio_formats/codecs/flac/Flac Licence.txt" \
    "$J/juce_audio_formats/codecs/oggvorbis/Ogg Vorbis Licence.txt" \
    "$J/juce_core/zip/zlib/README" \
    "$J/juce_graphics/image_formats/pnglib/LICENSE" \
    "$J/juce_graphics/image_formats/jpglib/README" \
    "$J/juce_graphics/fonts/harfbuzz/COPYING" \
    "$J/juce_graphics/unicode/sheenbidi/LICENSE" \
    "$T/3rd_party/choc/LICENSE.md" \
    "$T/3rd_party/crill/LICENSE.txt" \
    "$T/3rd_party/doctest/doctest.h" \
    "$T/3rd_party/expected/expected.hpp" \
    "$T/3rd_party/libsamplerate/samplerate.h" \
    "$T/3rd_party/magic_enum/magic_enum.hpp" \
    "$T/3rd_party/rigtorp/MPMCQueue.h" \
    "$T/3rd_party/rpmalloc/rpmalloc.c" \
    "$T/tracktion_graph/3rd_party/farbot/LICENSE" \
    "$T/tracktion_graph/3rd_party/concurrentqueue.h" \
    "$T/tracktion_graph/3rd_party/lightweightsemaphore.h" \
    dawd/third_party/rubberband/COPYING \
    dawd/third_party/onnxruntime-linux-x64-1.31.0/LICENSE \
    dawd/third_party/onnxruntime-linux-x64-1.31.0/ThirdPartyNotices.txt \
    dawd/ui/vendor/webawesome/LICENSE.md; do
    copy_license "$f"
done
# Airwindows: die Dateien im Engine-Baum tragen keine Lizenz; der Text aus
# dem Upstream-Repo liegt im Repo (docs/lizenzen-agpl.md)
copy_license dawd/appimage/licenses/airwindows-LICENSE
mkdir -p "$LIC/appimage-runtime"
cp "$RUNTIME_DIR/LICENSE" "$LIC/appimage-runtime/LICENSE"
cat > "$LIC/appimage-runtime/SOURCE" <<EOF
AppImage type2-runtime $RUNTIME_VERSION (the executable part of this AppImage)
Source: https://github.com/AppImage/type2-runtime/tree/$RUNTIME_VERSION
Statically linked: musl libc (MIT), libfuse 3 (LGPL-2.1), squashfuse (BSD-2-Clause);
build scripts with the exact versions are in the source above.
EOF
mkdir -p "$LIC/system"
for pkg in "${!lib_pkgs[@]}"; do
    [ -f "/usr/share/doc/$pkg/copyright" ] && cp "/usr/share/doc/$pkg/copyright" "$LIC/system/$pkg.copyright"
done

cat > "$DOC/SOURCE" <<EOF
unidaw $VERSION, commit $COMMIT$DIRTY
Source code: ${UNIDAW_SOURCE_URL:-comes with the release this AppImage was published in}
EOF
cat > "$DOC/LICENSE-NOTICE" <<'EOF'
unidaw is free software: you can redistribute it and/or modify it under the
terms of the GNU Affero General Public License, version 3 (AGPL-3.0-only).
It comes with ABSOLUTELY NO WARRANTY. Full text: LICENSE.
Third-party components and their licenses: THIRD_PARTY_LICENSES.md and the
folder licenses/ (license texts; system/ holds the Debian/Ubuntu copyright
files of the bundled libraries). unidaw ships no ML models; fetch-models
downloads them from their source and names their licenses.
EOF

# 5. AppImage
FILE="$OUT/unidaw-$VERSION-x86_64.AppImage"
rm -f "$OUT/payload.squashfs" "$FILE"
mksquashfs "$APPDIR" "$OUT/payload.squashfs" -root-owned -noappend -comp zstd -quiet -no-progress
cat "$RUNTIME_DIR/runtime-x86_64" "$OUT/payload.squashfs" > "$FILE"
chmod 755 "$FILE"
rm -f "$OUT/payload.squashfs"
( cd "$OUT" && sha256sum "$(basename "$FILE")" > "$(basename "$FILE").sha256" )

[ -n "${UNIDAW_SOURCE_URL:-}" ] || echo "build_appimage: note — UNIDAW_SOURCE_URL not set, SOURCE names no URL" >&2
echo "ok: $FILE ($(du -h "$FILE" | cut -f1)), needs glibc $(objdump -T "$BIN" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 | cut -d_ -f2)+"
