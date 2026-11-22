# MCP-Server veröffentlichen

Der MCP-Server (`mcp/unidaw_mcp.py`) ist der Weg, über den Agent-Nutzer
unidaw finden (`docs/vertrieb-und-verkauf.md`, „Kanal“). Er geht auf drei
Wegen raus. Bauen und prüfen macht der Agent; die Konten und das
Veröffentlichen macht Matthias.

| Weg | Was | Datei im Repo |
| --- | --- | --- |
| Claude Code | Plugin aus dem Marketplace dieses Repos | `.claude-plugin/marketplace.json`, `mcp/.claude-plugin/plugin.json` (+ `icon.png`) |
| Claude Desktop und andere | MCP-Bundle `.mcpb` im GitHub-Release | `mcp/manifest.json`, gebaut von `tools/build_mcpb.py` |
| MCP-Registry | Eintrag `server.json`, zeigt auf das Bundle | erzeugt von `tools/build_mcpb.py` |

Quellen (geprüft 2026-11-19):
[Claude Code: Create a marketplace](https://code.claude.com/docs/en/plugin-marketplaces),
[Plugin manifest reference](https://code.claude.com/docs/en/plugins-reference),
[MCPB Manifest](https://github.com/modelcontextprotocol/mcpb/blob/main/MANIFEST.md),
[Registry: Package types](https://github.com/modelcontextprotocol/registry/blob/main/docs/modelcontextprotocol-io/package-types.mdx),
[Registry: Authentication](https://github.com/modelcontextprotocol/registry/blob/main/docs/modelcontextprotocol-io/authentication.mdx).

## Aufbau

- **Plugin**: Plugin-Ordner ist `mcp/`. Der Server steht direkt in
  `plugin.json` (`mcpServers`, `python3 ${CLAUDE_PLUGIN_ROOT}/unidaw_mcp.py`).
  Der Marketplace liegt in der Wurzel des Repos, Name `unihad`, Eintrag
  `unidaw` mit `source: ./mcp`. Nutzer: `claude plugin marketplace add
  unihad/unidaw`, dann `claude plugin install unidaw@unihad`. Claude Code holt
  beim Klonen nur Submodule, die im Plugin-Ordner liegen — die Tracktion
  Engine also nicht.
- **Bundle**: Zip mit `manifest.json`, `unidaw_mcp.py` und `LICENSE`, feste
  Zeitstempel: gleicher Stand = gleiche SHA-256. Name
  `unidaw-mcp-<version>.mcpb` (die Registry verlangt „mcp“ im Dateinamen
  oder Repo-Namen).
- **Registry**: Name `io.github.unihad/unidaw`, Paket `registryType: mcpb`
  mit URL ins GitHub-Release `v<version>` und `fileSha256`. Beschreibung
  höchstens 100 Zeichen.
- **Version**: eine Quelle, `project(unidaw VERSION …)` in
  `dawd/CMakeLists.txt`. `VERSION` in `unidaw_mcp.py`, `manifest.json` und
  `plugin.json` müssen gleich sein; `build_mcpb.py` bricht sonst ab, die
  Suite prüft es (`mcp_package_flow`).
- **unidaw starten**: Plugin und Bundle haben nur den MCP-Server, nicht
  unidaw. Der Nutzer startet das AppImage selbst — oder setzt `UNIDAW_BIN`
  auf den Pfad des AppImage, dann startet `unidaw_start` den Daemon
  (`<AppImage> unidaw --port 7400`, Arbeitsordner `~/.local/state/unidaw`).
  Ohne beides meldet `unidaw_start` das mit Hinweis auf `UNIDAW_BIN`.

## Bauen

```bash
python3 tools/build_mcpb.py            # nach dawd/build/mcpb
claude plugin validate .               # Marketplace
claude plugin validate ./mcp           # Plugin
```

Ein anderes GitHub-Repo als `unihad/unidaw`: `UNIDAW_GITHUB_REPO=<org>/<repo>`
beim Bauen setzen und den Namen in `README.md`, `plugin.json`,
`manifest.json` und `marketplace.json` mitziehen.

## Veröffentlichen (Matthias)

Erst nach dem Release auf GitHub (Todo
`2026-11-18-erstes-release-veroeffentlichen`), Reihenfolge:

1. **Release**: `unidaw-mcp-<version>.mcpb` (und `.sha256`) ans Release
   `v<version>` hängen — neben das AppImage. Bundle aus demselben Stand
   wie das Release bauen, sonst passt die Prüfsumme in `server.json` nicht.
2. **Claude Code**: nichts zu tun — sobald das Repo öffentlich ist,
   funktioniert `claude plugin marketplace add unihad/unidaw`. Optional
   zusätzlich ins Verzeichnis von Anthropic einreichen
   ([Submit to Anthropic's directory](https://code.claude.com/docs/en/plugins/publish.md#submit-to-anthropics-directory)).
3. **MCP-Registry**: `mcp-publisher` installieren, mit Token anmelden,
   `server.json` aus `dawd/build/mcpb` veröffentlichen. `mcp-publisher
   login github` (Geräte-Login) gibt nur den persönlichen Namensraum
   (`io.github.<nutzer>/*`), nie den einer Organisation (Registry-Issue
   #1468). Für `io.github.unihad/*` braucht es Owner-Rechte in `unihad`
   und ein Personal Access Token (classic, nur `read:org`; fine-grained:
   Organization permissions → Members → Read-only). Fehlt das Recht,
   veröffentlicht die Registry still nur im persönlichen Namensraum
   (Quelle: Registry, Authentication und Quickstart, gelesen 2026-11-19).

Befehle (Linux x86_64; dieselben Schritte stehen als Rollout im Todo
`2026-11-18-mcp-server-fuer-agenten-veroeffentlichen`):

```bash
# Release steht, Repo ist öffentlich
gh release view v0.1.0 -R unihad/unidaw
# Bundle aus dem Release-Stand bauen und anhängen
python3 tools/build_mcpb.py
cd dawd/build/mcpb
gh release upload v0.1.0 unidaw-mcp-0.1.0.mcpb unidaw-mcp-0.1.0.mcpb.sha256 -R unihad/unidaw
# mcp-publisher installieren
curl -fsSL https://github.com/modelcontextprotocol/registry/releases/latest/download/mcp-publisher_linux_amd64.tar.gz \
  | tar xz mcp-publisher && install -m 755 mcp-publisher ~/.local/bin/ && rm mcp-publisher
# Token (classic, read:org) anlegen: GitHub → Settings → Developer settings
# → Personal access tokens → Tokens (classic). Dann:
read -rsp 'Token: ' T; echo; mcp-publisher login github --token "$T"
mcp-publisher validate
mcp-publisher publish
curl "https://registry.modelcontextprotocol.io/v0.1/servers?search=io.github.unihad/unidaw"
```

Bei jeder neuen Version: Version in den vier Dateien hochsetzen, Bundle
neu bauen, ans Release hängen, `mcp-publisher publish` erneut.
