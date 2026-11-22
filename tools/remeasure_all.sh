#!/usr/bin/env bash
# Steckbriefe und Klang-Kataloge der Instrumente mit der aktuellen Testphrase
# (profile_plugin.VERSION, seit 2026-10-27 Version 3)
# neu messen (Todo 2026-10-18 P2, Entscheidung Matthias 2026-10-26: nachts,
# mit nice/taskset, eigene Instanzen).
#
#   nice -n 15 taskset -c 8-15 tools/remeasure_all.sh            # ganzer Lauf
#   JOBS=3 PORT_BASE=7600 tools/remeasure_all.sh                  # Vorgaben
#   ONLY="Helm" LIMIT=3 QUERY=cutoff tools/remeasure_all.sh       # Probelauf: ein Plugin, je Katalog 3 Einträge, ein Regler
#   EFFECTS=1 tools/remeasure_all.sh                              # auch die Effekte (profile_plugin.EFFECT_VERSION, seit 2026-10-27 4: Bursts; eine Aufgabe, seriell)
#   RESET=1 tools/remeasure_all.sh                                # Fertig-Marken löschen, alles neu prüfen
#
# Aufgaben (je eine eigene dawd --no-audio unter bwrap ohne /dev/snd, eigener
# Port PORT_BASE + 10 * Worker, eigener Config-Ordner — kein Audio-Gerät, kein
# PipeWire): Steckbrief je Instrument (profile_plugin.py --skip-current),
# Preset-Katalog (profile_presets.py, Einträge ohne phrase = VERSION gelten als offen)
# und erkundete Klänge (profile_presets.py --sounds). JOBS Worker holen sich
# die nächste Aufgabe aus einer Warteschlange, die größten zuerst.
#
# Ergebnisse: dawd/plugin-profiles/ DIESES Checkouts (die Werkzeuge schreiben
# neben sich; mit UNIDAW_PROFILE_OUT=<Ordner> woandershin). Lauf-Ordner
# remeasure-run/ (unversioniert): run.log (Gesamt), logs/<Aufgabe>.log,
# work/<Worker>/ (Config, Renders), done/<Aufgabe> (fertig).
#
# Abbruch-sicher: Ctrl-C/SIGTERM beendet die Worker, die Werkzeuge beenden
# ihre dawd per quit (die Plugin-Hosts enden mit ihr). Ein neuer Start setzt
# fort: fertige Aufgaben werden übersprungen, Steckbriefe setzen aus
# <slug>.partial.json fort, Kataloge messen nur Einträge ohne phrase = VERSION.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUN="${RUN:-$ROOT/remeasure-run}"
JOBS="${JOBS:-3}"
PORT_BASE="${PORT_BASE:-7600}"
# Token der localhost-API: die Instanzen der Worker (profile_plugin.py) erben ihn
export UNIDAW_TOKEN="${UNIDAW_TOKEN:-$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')}"
ONLY="${ONLY:-}"
LIMIT="${LIMIT:-}"
QUERY="${QUERY:-}"
EFFECTS="${EFFECTS:-0}"
PY="${PYTHON:-python3}"

mkdir -p "$RUN/logs" "$RUN/work" "$RUN/done"
[ "${RESET:-0}" = 1 ] && rm -f "$RUN/done/"*
LOG="$RUN/run.log"
say () { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

if [ ! -x "$ROOT/dawd/build/unidaw_artefacts/Release/unidaw" ]; then
    say "no dawd binary in $ROOT/dawd/build — build first"
    exit 1
fi

# Aufgaben: id|Werkzeug-Argumente (größte zuerst — Surge ~3000 Presets)
tasks () {
    cat <<'EOF'
presets-surge|tools/profile_presets.py|Surge XT|VST3|
presets-diva|tools/profile_presets.py|Diva|VST3|
presets-dexed|tools/profile_presets.py|Dexed|VST3|
sounds-helm|tools/profile_presets.py|Helm|LV2|--sounds
sounds-jx10|tools/profile_presets.py|MDA JX10|LV2|--sounds
sounds-dexed|tools/profile_presets.py|Dexed|VST3|--sounds
sounds-surge|tools/profile_presets.py|Surge XT|VST3|--sounds
profile-surge|tools/profile_plugin.py|Surge XT|VST3|--skip-current
profile-diva|tools/profile_plugin.py|Diva|VST3|--skip-current
profile-helm|tools/profile_plugin.py|Helm|LV2|--skip-current
profile-dexed|tools/profile_plugin.py|Dexed|VST3|--skip-current
profile-jx10|tools/profile_plugin.py|MDA JX10|LV2|--skip-current
profile-4osc|tools/profile_plugin.py|4OSC||--skip-current
presets-helm|tools/profile_presets.py|Helm|LV2|
profile-epiano|tools/profile_plugin.py|MDA ePiano|LV2|--skip-current
profile-piano|tools/profile_plugin.py|MDA Piano|LV2|--skip-current
presets-jx10|tools/profile_presets.py|MDA JX10|LV2|
presets-epiano|tools/profile_presets.py|MDA ePiano|LV2|
presets-piano|tools/profile_presets.py|MDA Piano|LV2|
presets-sfizz|tools/profile_presets.py|sfizz|VST3|
EOF
    if [ "$EFFECTS" = 1 ]; then
        echo "profile-effects|tools/profile_plugin.py|||--core --only effects --skip-current"
    fi
}

QUEUE="$RUN/queue"
: > "$QUEUE"
while IFS='|' read -r id tool name fmt extra; do
    [ -z "$id" ] && continue
    [ -e "$RUN/done/$id" ] && continue
    if [ -n "$ONLY" ] && [ "$name" != "$ONLY" ]; then continue; fi
    echo "$id|$tool|$name|$fmt|$extra" >> "$QUEUE"
done < <(tasks)

pop () {   # nächste Aufgabe (flock: mehrere Worker)
    exec 9>"$QUEUE.lock"
    flock 9
    local line
    line="$(head -n 1 "$QUEUE")"
    [ -n "$line" ] && sed -i '1d' "$QUEUE"
    flock -u 9
    echo "$line"
}

worker () {
    local w="$1" port=$((PORT_BASE + 10 * $1))
    local work="$RUN/work/w$w"
    mkdir -p "$work"
    while :; do
        local line
        line="$(pop)"
        [ -z "$line" ] && break
        IFS='|' read -r id tool name fmt extra <<< "$line"
        local args=()
        [ -n "$name" ] && args+=("$name")
        [ -n "$fmt" ] && args+=(--format "$fmt")
        # shellcheck disable=SC2206
        [ -n "$extra" ] && args+=($extra)
        [ -n "$LIMIT" ] && [[ "$tool" == *presets* ]] && args+=(--limit "$LIMIT")
        [ -n "$QUERY" ] && [[ "$tool" == *profile_plugin* ]] && args+=(--query "$QUERY")
        say "[w$w] start $id (port $port)"
        local t0=$SECONDS
        "$PY" -u "$ROOT/$tool" "${args[@]}" --start-port "$port" --workdir "$work" > "$RUN/logs/$id.log" 2>&1 &
        local pid=$!
        echo "$pid" > "$RUN/work/w$w.pid"
        wait "$pid"
        local rc=$?
        rm -f "$RUN/work/w$w.pid"
        # "skip …" heißt Fehler, außer ein Steckbrief ist schon aktuell
        if [ "$rc" = 0 ] && ! grep '^skip ' "$RUN/logs/$id.log" | grep -qv 'has version'; then
            touch "$RUN/done/$id"
            say "[w$w] done $id in $(( (SECONDS - t0) / 60 )) min — $(tail -n 1 "$RUN/logs/$id.log")"
        else
            say "[w$w] FAILED $id (exit $rc) after $(( (SECONDS - t0) / 60 )) min — $(tail -n 1 "$RUN/logs/$id.log")"
        fi
        [ -e "$RUN/stop" ] && break
    done
}

stop () {
    trap - INT TERM
    say "stopping: workers get SIGTERM, their dawd quit"
    touch "$RUN/stop"
    for f in "$RUN"/work/w*.pid; do
        [ -e "$f" ] && kill -TERM "$(cat "$f")" 2>/dev/null
    done
    wait
    cleanup
    rm -f "$RUN/stop"
    say "stopped — start again to continue"
    exit 130
}

# Sicherheitsnetz: eine dawd, die auf einem der Lauf-Ports noch antwortet,
# per quit beenden (nie per Muster-kill)
cleanup () {
    for ((w = 0; w < JOBS; w++)); do
        for ((p = PORT_BASE + 10 * w; p < PORT_BASE + 10 * w + 10; p++)); do
            if curl -s -m 2 "http://127.0.0.1:$p/health" > /dev/null 2>&1; then
                say "dawd still up on $p — quit"
                curl -s -m 10 -X POST -H 'Content-Type: application/json' -H "Authorization: Bearer $UNIDAW_TOKEN" -d '{"op":"quit"}' "http://127.0.0.1:$p/command" > /dev/null 2>&1
            fi
        done
    done
}

trap stop INT TERM
rm -f "$RUN/stop"
say "remeasure: $(wc -l < "$QUEUE") tasks, $JOBS workers, ports $PORT_BASE.., results in ${UNIDAW_PROFILE_OUT:-$ROOT/dawd/plugin-profiles}"
start=$SECONDS
for ((w = 0; w < JOBS; w++)); do
    worker "$w" &
done
wait
cleanup
say "finished in $(( (SECONDS - start) / 60 )) min — $(ls "$RUN/done" | wc -l) tasks done in total, FAILED lines in $LOG"
