#!/usr/bin/env bash
# Frischen Worktree baubar machen: nach `git worktree add …` einmal laufen
# lassen, danach geht `bash tests/run.sh` ohne Handarbeit.
#
#   bash tools/worktree_setup.sh [<worktree>]   (Standard: das Repo dieses Skripts)
#
# Holt aus dem Haupt-Checkout (git-common-dir), was ein Worktree nicht mitbringt:
#   - tracktion_engine/: Kopie ohne alle .git-Dateien (das Submodul ist im
#     Worktree leer; der gitlink modules/juce/.git zeigt auf ein .git/modules,
#     das es dort nicht gibt — mit ihm passen die JUCE-Patches nicht), samt
#     .dawd-patches (Vermerk der angewendeten Fassungen, dawd/patches/apply.sh).
#   - dawd/third_party/ (ML-Bausteine, unversioniert): Kopie.
#   - dawd/models/ (1,1 GB, unversioniert, nur gelesen): Symlink auf den
#     Haupt-Checkout. Ohne Modelle fallen nicht alle ML-Tests sauber weg.
#   - dawd/build/: CMake-Configure (Release, ~3,5 min; wendet die Patches an).
# Was schon da ist, bleibt unberührt — ein zweiter Lauf ändert nichts.
set -e
wt="$(cd "${1:-$(dirname "$0")/..}" && pwd)"
main="$(cd "$(git -C "$wt" rev-parse --git-common-dir)/.." && pwd)"
if [ "$wt" = "$main" ]; then
    echo "worktree_setup: $wt ist der Haupt-Checkout, nichts zu tun" >&2
    exit 1
fi
echo "worktree_setup: $wt (Quelle: $main)"

if [ -z "$(ls -A "$wt/tracktion_engine" 2>/dev/null)" ]; then
    [ -f "$main/tracktion_engine/CMakeLists.txt" ] || {
        echo "worktree_setup: $main/tracktion_engine ist leer (git submodule update --init --recursive)" >&2
        exit 1
    }
    rsync -a --exclude=.git "$main/tracktion_engine/" "$wt/tracktion_engine/"
    echo "  tracktion_engine kopiert (ohne .git)"
else
    echo "  tracktion_engine schon da"
fi

if [ -d "$wt/dawd/third_party" ]; then
    echo "  dawd/third_party schon da"
elif [ -d "$main/dawd/third_party" ]; then
    rsync -a "$main/dawd/third_party/" "$wt/dawd/third_party/"
    echo "  dawd/third_party kopiert"
else
    echo "  dawd/third_party fehlt auch im Haupt-Checkout (tools/fetch_ml.sh) — ohne ML-Bausteine"
fi

if [ -e "$wt/dawd/models" ] || [ -L "$wt/dawd/models" ]; then
    echo "  dawd/models schon da"
elif [ -d "$main/dawd/models" ]; then
    ln -s "$main/dawd/models" "$wt/dawd/models"
    echo "  dawd/models -> $main/dawd/models"
else
    echo "  dawd/models fehlt auch im Haupt-Checkout (tools/fetch_ml.sh) — ohne Modelle"
fi

if [ -f "$wt/dawd/build/CMakeCache.txt" ]; then
    echo "  dawd/build schon konfiguriert"
else
    cmake -S "$wt/dawd" -B "$wt/dawd/build" -DCMAKE_BUILD_TYPE=Release
fi
echo "worktree_setup: fertig — weiter mit bash tests/run.sh"
