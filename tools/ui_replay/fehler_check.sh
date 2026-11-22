#!/bin/bash
# Gegenprobe der UI-Fehler aus dem Nachstell-Lauf 1 (fehler_check.js) mit
# eigener dawd-Instanz: frische --config, kein Audiogerät, Song im tmpfs.
#   tools/ui_replay/fehler_check.sh <port> [weitere fehler_check.js-Argumente]
# Umgebung: DAWD (Binary, Standard: Build dieses Checkouts), UI_FILE (Flow-UI,
# Standard: dieses Checkout), WORK (Arbeitsordner, Standard: /tmp/…), NODE_PATH.
set -uo pipefail
PORT=$1; shift
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DAWD=${DAWD:-$REPO/dawd/build/unidaw_artefacts/Release/unidaw}
WORK=${WORK:-/tmp/dawd-fehler-check-$PORT}
rm -rf "$WORK"; mkdir -p "$WORK/config" "$WORK/recipes"
# Token der localhost-API: die Instanz erbt ihn, curl und node schicken ihn
export UNIDAW_TOKEN="${UNIDAW_TOKEN:-$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')}"
"$DAWD" --port "$PORT" --config "$WORK/config" --recipes "$WORK/recipes" --log "$WORK/events.jsonl" \
  --session "$WORK/session.json" --ui "${UI_FILE:-$REPO/dawd/ui/flow.html}" --interface-sim --no-audio > "$WORK/dawd.log" 2>&1 &
PID=$!
for i in $(seq 1 60); do curl -s -m 2 -X POST -H "Authorization: Bearer $UNIDAW_TOKEN" "127.0.0.1:$PORT/command" -d '{"op":"ping"}' >/dev/null && break; sleep 1; done
NODE_PATH=${NODE_PATH:-$HOME/dev/unspill/node_modules} node "$HERE/fehler_check.js" --port "$PORT" --dir "$WORK/song" "$@"
RC=$?
curl -s -m 5 -X POST -H "Authorization: Bearer $UNIDAW_TOKEN" "127.0.0.1:$PORT/command" -d '{"op":"quit"}' >/dev/null
sleep 2; kill "$PID" 2>/dev/null
rm -rf "$WORK"
exit $RC
