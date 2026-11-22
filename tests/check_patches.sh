#!/usr/bin/env bash
# Prüft dawd/patches/apply.sh an Wegwerf-Repos in einem Temp-Ordner: frisch
# anwenden, zweiter Lauf still, neue Fassung (alte zurück, neue drauf), alte
# Fassung nicht zurücknehmbar, Engine-Kopie ohne .git mit "diff --git"-Patch,
# Patch passt nicht. Läuft in tests/run.sh vor apply.sh; Exit 1 bei Fehlern.
set -e
here="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/dawd-patches-test-XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
source "$here/dawd/patches/apply.sh"   # nur die Funktionen (git_apply, apply)
set +e
root="$tmp"                            # apply sucht Repos und Patches hier

checks=0 fails=0
check () {   # check <Name> <Bedingung …>; meldet nur Fehler
    checks=$((checks + 1))
    "${@:2}" || { echo "  FAIL $repo: $1"; fails=$((fails + 1)); }
}
run () {     # run <Repo> <Patch>: apply in einer Subshell, setzt out und rc
    out="$( (apply "$1" "$2") 2>&1 )"; rc=$?
}
has () { [[ "$out" == *"$1"* ]]; }

# Aufbau wie im Worktree: äußeres Repo (root), darin ein Repo mit eigenem
# .git (Submodul) und eine Kopie ohne .git (Engine-Kopie).
git init -q "$tmp"
mkdir -p "$tmp/dawd/patches" "$tmp/gen/src" "$tmp/sub/src" "$tmp/copy/src"
orig="$tmp/gen/orig.txt"
printf 'eins\nzwei\ndrei\nvier\nfünf\nsechs\nsieben\n' > "$orig"
cp "$orig" "$tmp/sub/src/f.txt"
cp "$orig" "$tmp/copy/src/f.txt"
git init -q "$tmp/sub"
git init -q "$tmp/gen"
mkpatch () {   # mkpatch <Name> <sed-Ausdruck>: "diff --git"-Patch auf f.txt
    cp "$orig" "$tmp/gen/src/f.txt"
    git -C "$tmp/gen" add src/f.txt
    sed -i "$2" "$tmp/gen/src/f.txt"
    git -C "$tmp/gen" diff > "$tmp/dawd/patches/$1"
}
mkpatch p.patch 's/^drei$/drei (v1)/'
grep -q '^diff --git' "$tmp/dawd/patches/p.patch" || { echo "Testaufbau: kein diff --git-Kopf"; exit 1; }

for repo in sub copy; do
    f="$tmp/$repo/src/f.txt"
    mkpatch p.patch 's/^drei$/drei (v1)/'
    run $repo p.patch
    check "frisch angewendet" eval '[ $rc = 0 ] && has "p.patch angewendet" && grep -qx "drei (v1)" "$f"'
    check "Fassung vermerkt" cmp -s "$tmp/$repo/.dawd-patches/p.patch" "$tmp/dawd/patches/p.patch"
    run $repo p.patch
    check "zweiter Lauf still" eval '[ $rc = 0 ] && [ -z "$out" ]'

    mkpatch p.patch 's/^drei$/drei (v2)/; s/^sechs$/sechs (v2)/'
    run $repo p.patch
    check "neue Fassung: alte zurück, neue drauf" eval '[ $rc = 0 ] && has "alte Fassung zurückgenommen" && has "p.patch angewendet"'
    check "neue Fassung: nur v2 in der Datei" eval '! grep -q "v1" "$f" && grep -qx "drei (v2)" "$f" && grep -qx "sechs (v2)" "$f"'
    run $repo p.patch
    check "neue Fassung: danach still" eval '[ $rc = 0 ] && [ -z "$out" ]'

    sed -i 's/^drei (v2)$/drei (von Hand)/' "$f"       # alte Fassung kaputt
    mkpatch p.patch 's/^sechs$/sechs (v3)/'
    run $repo p.patch
    check "alte Fassung nicht zurücknehmbar: Abbruch mit Anleitung" eval '[ $rc = 1 ] && has "lässt sich nicht zurücknehmen" && has "Von Hand"'
    check "alte Fassung nicht zurücknehmbar: Datei unverändert" grep -qx "drei (von Hand)" "$f"

    cp "$orig" "$f"                                      # zurücksetzen
    rm -r "$tmp/$repo/.dawd-patches"
    mkpatch p.patch 's/^drei$/drei (v1)/'
    sed -i 's/^drei$/drei (v1)/' "$f"                   # schon drin, ohne Vermerk
    run $repo p.patch
    check "ohne Vermerk schon drin: vermerkt" eval '[ $rc = 0 ] && has "schon drin, Fassung vermerkt" && [ -f "$tmp/$repo/.dawd-patches/p.patch" ]'
    run $repo p.patch
    check "ohne Vermerk schon drin: danach still" eval '[ $rc = 0 ] && [ -z "$out" ]'

    mkpatch q.patch 's/^vier$/vier (q)/'
    sed -i 's/^vier$/VIER/' "$f"                         # Kontext weg
    run $repo q.patch
    check "passt nicht: Fehler" eval '[ $rc = 1 ] && has "q.patch passt nicht mehr" && [ ! -f "$tmp/$repo/.dawd-patches/q.patch" ]'
done

repo=git
# Vermerk unversioniert: das äußere Repo sieht ihn nicht (Kopie ohne .git),
# das Submodul auch nicht.
check "Vermerk nicht in git status (äußeres Repo)" eval '! git -C "$tmp" status --porcelain --untracked-files=all | grep -q dawd-patches'
check "Vermerk nicht in git status (Submodul)" eval '! git -C "$tmp/sub" status --porcelain --untracked-files=all | grep -q dawd-patches'

if [ "$fails" != 0 ]; then echo "check_patches: $fails Fehler"; exit 1; fi
echo "check_patches: $checks Prüfungen ok"
