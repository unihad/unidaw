#!/bin/bash
# Ein Nachstell-Lauf: frische dawd-Instanz (eigener Port, eigene --config,
# kein Audiogerät), Replay über das Flow-UI, Instanz beenden.
#   tools/ui_replay/run.sh <lauf> <BxH> <port> [weitere replay.js-Argumente]
# Umgebung: OPS_DIR (Ausgabe von extract.py), RESULTS (Ergebnisordner),
# BASE_CONFIG (Vorlage für --config: Settings.xml mit Plugin-Liste,
# libraries.json, index, recipes), DAWD (Binary), RUNS (Lauf-Ordner),
# UI_FILE (Flow-UI, Standard: dieses Checkout).
set -euo pipefail
RUN=$1; SIZE=$2; PORT=$3; shift 3
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DAWD=${DAWD:-$HOME/dev/agent-daw/dawd/build/unidaw_artefacts/Release/unidaw}
RUNS=${RUNS:-$HOME/dev/agent-daw-runs}
RESULTS=${RESULTS:-$HOME/dev/agent-daw-runs/ui-replay-1}
OUT="$RESULTS/$RUN-$SIZE"
CFG="${WORK:-/tmp}/ui-replay-config-$RUN-$SIZE"
rm -rf "$OUT" "$CFG"; mkdir -p "$OUT"
cp -r "$BASE_CONFIG" "$CFG"
# Token der localhost-API: die Instanz erbt ihn, curl und node schicken ihn
export UNIDAW_TOKEN="${UNIDAW_TOKEN:-$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')}"
"$DAWD" --port "$PORT" --config "$CFG" --recipes "$CFG/recipes-repo" --log "$OUT/events.jsonl" \
  --session "$OUT/session.json" --ui "${UI_FILE:-$REPO/dawd/ui/flow.html}" --interface-sim --no-audio > "$OUT/dawd.log" 2>&1 &
PID=$!
for i in $(seq 1 60); do curl -s -m 2 -X POST -H "Authorization: Bearer $UNIDAW_TOKEN" "127.0.0.1:$PORT/command" -d '{"op":"ping"}' >/dev/null && break; sleep 1; done
set +e
NODE_PATH=${NODE_PATH:-$HOME/dev/unspill/node_modules} node "$HERE/replay.js" --ops "$OPS_DIR/$RUN.ops.jsonl" --run "$RUN" \
  --orig "$RUNS/$RUN/projekt" --out "$OUT" --port "$PORT" --size "$SIZE" "$@" 2>&1 | tee "$OUT/replay.log"
RC=${PIPESTATUS[0]}
curl -s -m 5 -X POST -H "Authorization: Bearer $UNIDAW_TOKEN" "127.0.0.1:$PORT/command" -d '{"op":"quit"}' >/dev/null
sleep 3; kill "$PID" 2>/dev/null
rm -rf "$CFG"
exit $RC
