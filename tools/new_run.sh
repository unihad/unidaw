#!/usr/bin/env bash
# Lauf-Ordner für einen Praxistest anlegen: der Agent darf nur die
# dawd-MCP-Tools benutzen (wie ein Mensch, der nur seine DAW hat).
#
#   tools/new_run.sh <name> <auftrag.md>
#
# Legt ~/dev/agent-daw-runs/<name>/ an mit AUFTRAG.md (= Auftrag), .mcp.json
# (nur unidaw), .claude/settings.json und recipes-version.json (Stand des
# Rezept-Katalogs, tools/recipes.py freeze). Den Auftrag bringt ein
# SessionStart-Hook in den Kontext (cat AUFTRAG.md): Eine CLAUDE.md im
# Lauf-Ordner kam in den Praxistests 1, 1b und 1c nie im Kontext an — das
# deny "Read" unterdrückt auch das Laden der CLAUDE.md (nachgestellt
# 2026-10-18, Claude Code 2.1.301, undokumentiert). Der Hook läuft auch ohne
# Vertrauensdialog, und die deny-Liste gilt nicht für ihn.
# Die Rechte:
# - defaultMode dontAsk: alles, was nicht erlaubt ist, wird ohne Nachfrage
#   abgelehnt.
# - deny mit nackten Tool-Namen: die Tools sind gar nicht im Kontext. Nötig,
#   weil dontAsk lesende Shell-Befehle und Datei-Lesen trotzdem zulässt.
#   Monitor steht eigens drin: es führt Shell-Befehle aus und wird von "Bash"
#   nicht erfasst (Praxistest 1b: CLAUDE.md per Monitor + cat gelesen).
# - ToolSearch bleibt: darüber lädt der Agent die dawd-Tools.
set -euo pipefail

name="${1:?Name des Laufs, z. B. remix-1}"
task="${2:?Auftrag als Markdown-Datei}"
runs="${AGENT_DAW_RUNS:-$HOME/dev/agent-daw-runs}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
dir="$runs/$name"

if [ -e "$dir" ]; then
    echo "gibt es schon: $dir" >&2
    exit 1
fi

mkdir -p "$dir/.claude"
cp "$task" "$dir/AUFTRAG.md"

cat > "$dir/.mcp.json" <<EOF
{
  "mcpServers": {
    "unidaw": {
      "command": "python3",
      "args": ["$repo/mcp/unidaw_mcp.py"]
    }
  }
}
EOF

cat > "$dir/.claude/settings.json" <<EOF
{
  "enableAllProjectMcpServers": true,
  "hooks": {
    "SessionStart": [
      { "matcher": "startup|clear|compact",
        "hooks": [ { "type": "command", "command": "cat '$dir/AUFTRAG.md'" } ] }
    ]
  },
  "permissions": {
    "defaultMode": "dontAsk",
    "allow": [
      "mcp__unidaw__unidaw_start",
      "mcp__unidaw__unidaw_command"
    ],
    "deny": [
      "Bash", "Monitor", "PowerShell",
      "Read", "Write", "Edit", "NotebookEdit", "Glob", "Grep", "LSP",
      "WebFetch", "WebSearch",
      "Agent", "Task", "Skill", "SendMessage", "ListAgents",
      "CronCreate", "CronDelete", "CronList", "ScheduleWakeup", "RemoteTrigger",
      "EnterWorktree", "ExitWorktree", "Workflow",
      "Artifact", "ArtifactComments", "ArtifactData",
      "ListMcpResourcesTool", "ReadMcpResourceTool",
      "mcp__claude_ai_*", "mcp__claude-vscode__*"
    ]
  }
}
EOF

# Rezept-Katalog einfrieren: welcher Stand der geprüften Rezepte galt
python3 "$repo/tools/recipes.py" freeze "$dir"
if python3 -c 'import json,sys; sys.exit(json.load(open(sys.argv[1]))["count"] != 0)' "$dir/recipes-version.json"; then
    echo "Achtung: Rezept-Katalog leer (0 geprüfte Rezepte) — im Auftrag nicht als Hilfe nennen." >&2
fi

echo "Lauf-Ordner: $dir"
echo
echo "Start:   code $dir   → neue Claude-Code-Session, erste Nachricht:"
echo "         Dein Auftrag steht schon in deinem Kontext (AUFTRAG.md). Leg los."
echo "Prüfen:  in der Session nach den verfügbaren Tools fragen — nur unidaw_start,"
echo "         unidaw_command (und ToolSearch) dürfen erscheinen — und nach der"
echo "         Überschrift des Auftrags (muss ohne Nachfrage bekannt sein)."
