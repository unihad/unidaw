#!/usr/bin/env bash
# Aeltere Linux-Basis fuer das AppImage (docs/appimage.md, Abschnitt
# "Aeltere Basis"): Ubuntu 22.04 (glibc 2.35) als Ordner, ohne sudo und ohne
# Container-Dienst — bubblewrap mit eigenem User-Namespace (root nur darin).
#
#   bash tools/buildroot.sh build            Basis zum Bauen anlegen/aktualisieren, Pfad auf stdout
#   bash tools/buildroot.sh run              schlanke Basis zum Pruefen (python3, curl, libasound2)
#   bash tools/buildroot.sh exec <art> [--bind <pfad>]... -- <befehl>...
#                                            Befehl in der Basis (Netz und /dev wie draussen)
#
# Ablage: ${UNIDAW_BUILDROOT:-~/.cache/dawd-buildroot}/ubuntu-22.04-<art>
# (Build ~0,8 GB, Run ~0,1 GB). Ein zweiter Lauf aendert nichts, solange
# sich die Paketliste nicht aendert.
set -euo pipefail

BASE_TAR=ubuntu-base-22.04.5-base-amd64.tar.gz
BASE_URL="https://cdimage.ubuntu.com/ubuntu-base/releases/22.04/release/$BASE_TAR"
BASE_SHA256=242cd8898b33ea806ef5f13b1076ed7c76f9f989d18384452f7166692438ff1a
STORE="${UNIDAW_BUILDROOT:-$HOME/.cache/dawd-buildroot}"

# g++-12: C++20 fuer dawd und JUCE 8; Header fuer JUCE (ALSA, JACK, X11,
# Freetype); git/dpkg/mksquashfs/curl fuer tools/build_appimage.sh
BUILD_PKGS="g++-12 gcc-12 make cmake git ca-certificates curl squashfs-tools binutils pkg-config file
libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxrandr-dev
libxinerama-dev libxext-dev libxcursor-dev libxcomposite-dev"
# was tests/check_appimage.sh und der Starter auf dem Zielsystem brauchen;
# alles andere bringt das AppImage mit
RUN_PKGS="python3 curl libasound2"

die () { echo "buildroot: $*" >&2; exit 1; }
command -v bwrap >/dev/null || die "bwrap missing (package bubblewrap)"

# in_root <ordner> [bwrap-optionen...] -- <befehl>...
in_root () {
    local root="$1"; shift
    local opts=()
    while [ "$1" != "--" ]; do opts+=("$1"); shift; done
    shift
    bwrap --unshare-user --uid 0 --gid 0 --bind "$root" / --dev /dev --proc /proc --tmpfs /tmp \
        --ro-bind /etc/resolv.conf /etc/resolv.conf --die-with-parent \
        --clearenv --setenv HOME /root --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin \
        --setenv DEBIAN_FRONTEND noninteractive --setenv LANG C.UTF-8 \
        "${opts[@]}" "$@"
}

prepare () {   # prepare <art> <pakete>
    local root="$STORE/ubuntu-22.04-$1" pkgs
    pkgs="$(echo $2)"
    if [ -f "$root/.dawd-buildroot" ] && [ "$(cat "$root/.dawd-buildroot")" = "$pkgs" ]; then
        echo "$root"; return
    fi
    if [ ! -x "$root/usr/bin/apt-get" ]; then
        mkdir -p "$STORE/dl"
        if ! echo "$BASE_SHA256  $STORE/dl/$BASE_TAR" | sha256sum -c --quiet - 2>/dev/null; then
            curl -fsSL -o "$STORE/dl/$BASE_TAR.part" "$BASE_URL"
            echo "$BASE_SHA256  $STORE/dl/$BASE_TAR.part" | sha256sum -c --quiet - \
                || { rm -f "$STORE/dl/$BASE_TAR.part"; die "$BASE_TAR: SHA-256 does not match"; }
            mv "$STORE/dl/$BASE_TAR.part" "$STORE/dl/$BASE_TAR"
        fi
        rm -rf "$root.part"
        mkdir -p "$root.part"
        tar -xzf "$STORE/dl/$BASE_TAR" -C "$root.part" --exclude='dev/*'
        # apt laeuft als root im Namespace; sein Wechsel auf _apt geht dort nicht
        echo 'APT::Sandbox::User "root";' > "$root.part/etc/apt/apt.conf.d/00dawd-sandbox"
        mv "$root.part" "$root"
    fi
    # dpkg kann im Namespace nicht auf fremde Nutzer chownen (Logdatei von
    # apt): die Warnung ist harmlos; ein abgebrochener Lauf wird mit
    # --configure -a fertig
    in_root "$root" -- /bin/bash -c "apt-get update -qq && apt-get install -y -qq --no-install-recommends $pkgs >/dev/null \
        || dpkg --configure -a; apt-get install -y -qq --no-install-recommends $pkgs >/dev/null; apt-get clean" >&2
    in_root "$root" -- /usr/bin/dpkg -s $pkgs >/dev/null || die "packages missing in $root"
    echo "$pkgs" > "$root/.dawd-buildroot"
    echo "$root"
}

case "${1:-}" in
    build) prepare build "$BUILD_PKGS" ;;
    run)   prepare run "$RUN_PKGS" ;;
    exec)
        kind="${2:-}"; shift 2 || die "usage: exec build|run [--bind <path>]... -- <command>"
        case "$kind" in
            build) root="$(prepare build "$BUILD_PKGS")" ;;
            run)   root="$(prepare run "$RUN_PKGS")" ;;
            *) die "usage: exec build|run [--bind <path>]... -- <command>" ;;
        esac
        binds=()
        while [ $# -gt 0 ] && [ "$1" != "--" ]; do
            [ "$1" = "--bind" ] && [ -n "${2:-}" ] || die "usage: exec build|run [--bind <path>]... -- <command>"
            binds+=(--bind "$2" "$2"); shift 2
        done
        [ "${1:-}" = "--" ] || die "usage: exec build|run [--bind <path>]... -- <command>"
        shift
        in_root "$root" "${binds[@]}" -- "$@" ;;
    *) die "usage: buildroot.sh build|run|exec" ;;
esac
