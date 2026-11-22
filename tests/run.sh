#!/usr/bin/env bash
# unidaw: bauen und die Custom-Feature-Tests laufen lassen.
# Vor jedem Commit ausführen — die Suite fängt Regressionen über die
# Befehls-API (das, was das Flow-UI benutzt).
#
# Die Suite läuft in zwei Gruppen auf zwei dawd-Instanzen parallel (GROUP_B in
# tests/run_tests.py, docs/decisions/2026-10-25-suite-parallel.md): Gruppe A
# auf UNIDAW_TEST_PORT (Standard: erster freier aus 7411, 7441 …), Gruppe B auf UNIDAW_TEST_PORT + 2 (+ 1 ist
# die Session-Instanz der Gruppe A). Eine Instanz wie bisher mit
# UNIDAW_TESTS=a,b (Teilmenge) oder UNIDAW_SERIAL=1 (Notausgang).
set -e
cd "$(dirname "$0")/.."
python3 tests/check_literals.py
bash tests/check_patches.sh
bash dawd/patches/apply.sh
cmake --build dawd/build -j"$(nproc)"

# Basis-Port P: der erste aus 7411, 7441, 7471 …, dessen Ports frei sind
# (tests/ports_free.py) und den keine andere Suite gesperrt hat. Die Sperre
# (flock auf fd 9) hält der Lauf samt Kindern bis zum Ende — zwei Suiten
# gleichzeitig (Worktrees, pre-push-Hook) teilen sich so nie eine Instanz.
# UNIDAW_TEST_PORT legt P fest; ist er belegt, bricht der Lauf ab.
port=""
for p in ${UNIDAW_TEST_PORT:-$(seq 7411 30 7981)}; do
    exec 9>"${TMPDIR:-/tmp}/dawd-test-port-$p.lock"
    if flock -n 9 && python3 tests/ports_free.py "$p"; then port=$p; break; fi
    exec 9>&-
done
if [ -z "$port" ]; then
    echo "kein freier Test-Port${UNIDAW_TEST_PORT:+ (UNIDAW_TEST_PORT=$UNIDAW_TEST_PORT ist belegt)}" >&2
    exit 1
fi
[ "$port" != "${UNIDAW_TEST_PORT:-7411}" ] && echo "Test-Port $port (7411 belegt)"
export UNIDAW_TEST_PORT=$port

if [ -n "$UNIDAW_TESTS" ] || [ "$UNIDAW_SERIAL" = 1 ]; then
    exec python3 tests/run_tests.py
fi

logs="$(mktemp -d "${TMPDIR:-/tmp}/dawd-suite-XXXXXX")"
pids=()
stop () {   # beide Gruppen beenden: SIGTERM -> run_tests.py räumt seine Instanz
    trap - INT TERM
    for p in "${pids[@]}"; do kill -TERM "$p" 2>/dev/null || true; done
    wait
    echo "Suite abgebrochen (Logs: $logs)"
    exit 130
}
trap stop INT TERM

start=$SECONDS
for g in A B; do
    p=$port; [ "$g" = B ] && p=$((port + 2))
    UNIDAW_GROUP=$g UNIDAW_TEST_PORT=$p python3 tests/run_tests.py > "$logs/$g.log" 2>&1 &
    pids+=($!)
    # Ausgabe live mit Präfix; tail endet mit dem Testlauf
    tail --pid="$!" -n +1 -f "$logs/$g.log" | sed -u "s/^/[$g] /" &
done
rc=0
wait "${pids[0]}" || rc=1
wait "${pids[1]}" || rc=1
wait   # tail/sed

echo
echo "Suite parallel: $(( (SECONDS - start) / 60 )) min $(( (SECONDS - start) % 60 )) s"
for g in A B; do
    echo "Gruppe $g: $(grep -E '^[0-9]+/[0-9]+ tests ok$' "$logs/$g.log" || echo 'keine Zusammenfassung')"
done
if [ "$rc" != 0 ]; then
    echo
    echo "Failures:"
    for g in A B; do
        sed -n '/^Failures:$/,$p' "$logs/$g.log" | grep '^  - ' | sed "s/^  - /  - [$g] /" || true
    done
    # ohne Zusammenfassung (Absturz): das Ende des Logs
    for g in A B; do
        grep -qE '^[0-9]+/[0-9]+ tests ok$' "$logs/$g.log" || { echo "[$g] Ende des Logs:"; tail -5 "$logs/$g.log"; }
    done
    echo "Logs: $logs"
    exit 1
fi
rm -rf "$logs"
