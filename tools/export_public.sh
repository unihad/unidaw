#!/usr/bin/env bash
# Öffentlichen Stand exportieren (Decision
# docs/decisions/2026-11-18-oeffentlicher-stand-ohne-historie.md): Baum eines
# Commits (Standard main) ohne die internen Pfade als neuer Commit auf dem
# lokalen Branch `public`. Der erste Export hat keinen Vorgänger (frischer
# Stand ohne Historie), jeder weitere hängt am vorigen Export.
#
#   bash tools/export_public.sh [<ref>] [<Commit-Text>]   (Standard: main, "unidaw <Version>")
#
# Arbeitet nur mit Git-Objekten (eigener Index in einer Temp-Datei): der
# Checkout, main und das Submodul tracktion_engine (gitlink) bleiben, wie sie
# sind. Pusht nicht — das Veröffentlichen von `public` macht ein Mensch bzw.
# opencode (`git push <öffentlich> public:main`).
#
# Bricht ab, wenn im exportierten Baum noch ein ausgeschlossener Pfad oder ein
# absoluter Pfad ins eigene Home ($HOME/) steht. Ohne Änderung am Baum gegen
# den letzten Export entsteht kein Commit.
set -euo pipefail

# Intern, nie im öffentlichen Stand
EXCLUDE=(
    todos                                            # Agent-Logs, Session-IDs, private Pfade
    APP.md                                           # Steckbrief fürs interne Cockpit
    docs/vertrieb-und-verkauf.md                     # alte Closed-Source-Planung
    docs/decisions/2026-10-24-vertrieb-ruht.md
    dawd/plugin-profiles/vst3-357abb5a-presets.json  # Diva-Katalog (u-he, kommerziell)
    config                                           # Einstellungen einer Test-Instanz
    play-src.wav                                     # Test-Rest
)

ref="${1:-main}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"
commit="$(git rev-parse --verify "$ref^{commit}")"
version="$(git show "$commit:dawd/CMakeLists.txt" | sed -n 's/^project *([a-z]* VERSION \([0-9.]*\).*/\1/p')"
message="${2:-unidaw ${version:-$(git rev-parse --short "$commit")}}"

index="$(mktemp)"
trap 'rm -f "$index"' EXIT
export GIT_INDEX_FILE="$index"
git read-tree "$commit"
git rm -r -q --cached --ignore-unmatch -- "${EXCLUDE[@]}"
tree="$(git write-tree)"
unset GIT_INDEX_FILE

fail=0
for p in "${EXCLUDE[@]}"; do
    if git cat-file -e "$tree:$p" 2>/dev/null; then
        echo "export_public: $p steht noch im Baum" >&2
        fail=1
    fi
done
if hits="$(git grep -I -l -F "$HOME/" "$tree" -- . 2>/dev/null)"; then
    echo "export_public: absolute Pfade ins Home ($HOME/) in:" >&2
    echo "${hits//$tree:/  }" >&2
    fail=1
fi
[ "$fail" = 0 ] || exit 1

parent=()
if last="$(git rev-parse -q --verify refs/heads/public)"; then
    if [ "$(git rev-parse "$last^{tree}")" = "$tree" ]; then
        echo "export_public: public ($(git rev-parse --short "$last")) hat schon diesen Stand"
        exit 0
    fi
    parent=(-p "$last")
fi

new="$(git commit-tree "$tree" "${parent[@]}" -m "$message")"
git update-ref -m "export_public: $ref" refs/heads/public "$new" ${last:+"$last"}
echo "export_public: public = $(git rev-parse --short "$new") ($message, aus $ref $(git rev-parse --short "$commit"))"
