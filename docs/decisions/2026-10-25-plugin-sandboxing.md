# Plugin-Sandboxing: Fremd-Plugins in eigenen Prozessen

- **Status:** gilt — Option C (Matthias, 2026-10-25)
- **Datum:** 2026-10-25 (Vorschlag Agent, entschieden Matthias)
- **Konzept und Belege:** [docs/plugin-sandboxing.md](../plugin-sandboxing.md)

## Kontext

Die Tracktion Engine hostet Fremd-Plugins (VST3, LV2) im Prozess von dawd.
Ein Absturz im Plugin beendet den Daemon. Am 2026-10-25 riss u-he Diva
dawd-7400 dreimal mit (Segfault beim erneuten Öffnen des Editors, behoben
in 0529b9f). systemd startete neu, die Session kam zurück, der Song stand,
die Fenster waren zu.

Befunde (Details und Belege im Konzept):

- Engine und JUCE bieten kein Out-of-Process-Hosting für Audio, nur den
  Plugin-Scan im Kindprozess (dawd nutzt ihn nicht) und IPC-Klassen.
- Einhängepunkt ohne Engine-Patch: `PluginManager::createPluginInstance`.
- Messung `bench/sandbox`: Ein Block kann synchron im selben Block in einen
  anderen Prozess und zurück — keine zusätzliche Latenz; Aufschlag je
  Plugin ~50 µs Median, ~0,1 ms Max (512 Frames, Echtzeit-Priorität);
  Kette aus 8 bei 64 Frames bis 22 % des Budgets.
- Neustart von dawd ist schnell: 0,05 s bis `ping`, `treeOpen` mit 6
  Fremd-Plugins 0,52 s.

## Optionen

| Option | Was | Aufwand |
| --- | --- | --- |
| A | Nur Erholung (Weg d): Absturz dem Plugin zuordnen, Sperrliste gegen Absturzschleifen, Position und Fenster nach dem Neustart zurück; dazu Plugin-Scan im Kindprozess | M + S–M |
| B | A jetzt, Sandbox (Plugins in Host-Prozessen) später bei Bedarf | wie A, später XL |
| C | Sandbox jetzt (Gruppierung als Einstellung, Standard „je Plugin“) | XL |
| D | Nichts ändern (Autosave + Neustart wie heute) | 0 |

## Entscheidung (Matthias, 2026-10-25)

**Option C: Sandbox jetzt.** Fremd-Plugins (VST3, LV2) laufen in eigenen
Host-Prozessen. Umsetzung nach dem Konzept: synchron im Block (keine
zusätzliche Latenz), Host-Prozess = dawd-Binary in einem eigenen Modus,
Einhängepunkt `PluginManager::createPluginInstance`, Gruppierung als
Einstellung (Standard „je Plugin“, Ausnahmen „einzeln“ und „in dawd“ je
Plugin). Die Erholungs-Bausteine aus A (Absturz einem Plugin zuordnen,
Sperrliste gegen Absturzschleifen, Plugin-Scan im Kindprozess) gehören
mit dazu.

**Nach einem Absturz** (Frage 2): dawd spielt nicht von selbst weiter.
Position und offene Fenster kommen zurück, der Transport bleibt stehen.
Stürzt nur ein Plugin-Host ab, läuft dawd weiter; das Plugin wird neu
geladen (Zustand aus dem letzten Schnappschuss) oder bleibt stumm mit
Hinweis — Details in der Umsetzung.

## Empfehlung des Agents (vor der Entscheidung)

**B.** A behebt den Schmerz vom 2026-10-25 ohne Eingriff in Audio-Pfad,
Undo und Render. Die Sandbox kommt, wenn Abstürze trotz A Arbeit kosten,
mehr unsichere Fremd-Plugins dazukommen oder der Vertrieb startet
(Paket 5 in `docs/vertrieb-und-verkauf.md`). Dann: synchron im Block,
Host-Prozess = dawd-Binary, Gruppierung als Einstellung (Standard „je
Plugin“, Ausnahmen „einzeln“ und „in dawd“ je Plugin).

## Folgen (bei C)

- Neues Todo „Absturz-Erholung“ (Zuordnung, Sperrliste, Arbeitsplatz
  zurück, Op zum Entsperren mit UI, Test mit abstürzendem TestGain) und
  „Plugin-Scan im Kindprozess“.
- `ARCHITECTURE.md` (Absturzschutz) und `docs/engine-bewertung.md`
  (Offen) verweisen auf diese Decision.
- Die Sandbox bleibt als Paket 5 und in der Longlist stehen; Aufwand dort
  von M/L auf XL korrigieren.
- Bei C (2026-10-25): Umsetzung in Phasen, Todo
  `todos/2026-10-25-plugin-sandbox-umsetzung.md`. Longlist-Aufwand XL.
