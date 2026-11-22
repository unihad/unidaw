# Demo-Video: Agent arrangiert, Mensch nimmt auf

Drehbuch für das erste Demo-Video von unidaw (Todo
`2026-11-18-demo-video-aufnehmen`, Ziel
`agent-daw-bereit-fuer-den-vertrieb-machen`). Verwendet in README und
Ankündigung. Matthias nimmt nur noch auf; Ablauf und Prompts sind einmal
mit dem AppImage durchgespielt (Trockenlauf unten).

## Ziel und Länge

- **Aussage:** Ein Agent baut über MCP einen Song in unidaw, ein Mensch
  nimmt eine Spur dazu auf, beide sehen denselben Song. Der Song-Baum hält
  jede Variante.
- **Länge:** fertig 90–120 s. Rohmaterial etwa 10 min; die Wartezeit des
  Agenten wird schneller abgespielt (4–8×).
- **Format:** 1920×1080, 30 fps, H.264 + AAC, `.mp4`. Ablage zum Ansehen:
  `~/StorageBox/agent-daw/agent-daw-demo-<name>.mp4`.
- **Alles im Bild ist Englisch:** Prompts, Antworten, UI, Dateinamen,
  Spurnamen.

## Bildaufbau

Ein Bildschirm, zwei Fenster nebeneinander:

- links (etwa 40 %): Terminal mit Claude Code,
- rechts (etwa 60 %): Browser mit der unidaw-UI (`http://127.0.0.1:7400/ui`).

Die UI zeigt links den Song-Baum, in der Mitte Sections und Spuren, unten
die Routing-Kette. Neue Spuren und Nodes erscheinen dort, während der Agent
arbeitet — das ist das Bild, das das Video trägt.

## Vorbereitung

1. **AppImage:** `unidaw-0.1.0-x86_64.AppImage` aus `tools/build_appimage.sh`
   (`docs/appimage.md`) bzw. aus dem Release. Getestet: Stand dieses
   Branches, Version 0.1.0. Keine Modelle nötig — kein Prompt braucht
   ML-Ops.
2. **Kein anderes unidaw auf Port 7400:** die Entwickler-Instanz vorher
   beenden (über die API mit `quit`, `AGENTS.md`, Tests). Das AppImage
   startet sonst nicht selbst, sondern öffnet nur die UI der alten Instanz.
3. **Leerer Projektordner:** `~/music/unidaw-demo` darf noch nicht
   existieren (sonst öffnet `treeSeed` nichts Neues). Bei einem zweiten
   Versuch den Ordner umbenennen.
4. **MCP in Claude Code:** Plugin oder Server von Hand, wie im README
   („First steps with an agent“):

   ```bash
   claude mcp add unidaw -- python3 /pfad/zu/agent-daw/mcp/unidaw_mcp.py
   ```

   Die beiden Tools vorab erlauben, damit keine Rückfragen ins Bild kommen
   (`/permissions` oder `--allowedTools "mcp__unidaw__unidaw_command
   mcp__unidaw__unidaw_start"`).
5. **Claude Code ohne unihad-Kontext starten:** die globale
   `~/.claude/CLAUDE.md` (Todos, Sessions, Firmenregeln) gehört nicht ins
   Bild. Im Trockenlauf reichte `--setting-sources project,local`; ohne
   den Schalter nannte der Agent am Ende „not linked to a todo“. Start in
   einem leeren Ordner, z. B. `~/music`:

   ```bash
   cd ~/music && claude --setting-sources project,local \
     --allowedTools "mcp__unidaw__unidaw_command mcp__unidaw__unidaw_start"
   ```

   Vorher prüfen, dass `/mcp` den Server `unidaw` als verbunden zeigt
   (bei Plugin-Installation liegt er in den Nutzer-Einstellungen — dann
   den Server von Hand mit `--mcp-config` übergeben oder mit `claude mcp
   add -s local` im Ordner `~/music` anlegen).
6. **Audio:** Fireface UC als Gerät, Kopfhörer am Fireface. Mikrofon oder
   Instrument am Eingang, den unidaw als System-Eingang nimmt (in der UI
   unten „INPUT“). Pegel vorher einmal mit Monitor prüfen.
7. **Terminal:** Schrift groß (≥ 16 pt), dunkles Schema wie die UI,
   Fenster ohne Namen/Pfade aus dem Firmenalltag (Prompt-Zeile
   neutral, z. B. `PS1='$ '`).
8. **Bildschirmaufnahme:** GNOME-Bildschirmaufnahme
   (Strg+Umschalt+Alt+R oder Bildschirmfoto-Dialog → Video). Sie nimmt
   ohne Ton auf, als `.webm`. Den Ton liefert der Render am Ende (siehe
   Schnitt). Pro Szene eine eigene Datei — das macht den Schnitt einfach.

## Ablauf

Die Prompts stehen wörtlich da; im Video werden sie getippt (oder per
Zwischenablage eingefügt und dann Enter). Pfade in den Prompts nutzen
`~/music/unidaw-demo`.

### Szene 1 — Start (5–10 s im Video)

- Bild: Terminal, darin der Start des AppImage; der Browser öffnet die
  leere UI.
- Matthias: im Terminal

  ```bash
  ./unidaw-0.1.0-x86_64.AppImage
  ```

  Dann das Terminal-Fenster mit Claude Code nach vorn holen.
- Texteinblendung (Schnitt): „unidaw — a DAW for people and AI agents“.

### Szene 2 — Der Agent baut das Gerüst (Rohzeit ~70 s, im Video ~15 s)

Prompt 1:

```text
Start a new song in ~/music/unidaw-demo at 96 BPM. Make two sections: Intro (4 bars) and Verse (8 bars). Add a drum beat with the built-in drum kit and a simple bass line in A minor with the 4OSC synth, both playing through both sections. Then add an empty audio track called Take for my recording.
```

- Erwartet (Trockenlauf): Sections Intro/Verse bei 96 BPM, Spuren Drums
  (eingebautes Drum Kit), Bass (4OSC, Am–F–C–G als Achtel, Klang als Bass
  eingestellt), leere Audio-Spur Take; der Agent misst den Pegel und senkt
  Drums/Bass, falls es clippt.
- Bild: rechts erscheinen Sections, Spuren, Clips und Nodes im Song-Baum.
- Am Ende kurz Play in der UI drücken (2 Takte), damit man den Beat hört.

### Szene 3 — Der Mensch nimmt auf (Rohzeit ~40 s, im Video ~20 s)

- Matthias in der UI: Spur **Take** armen (Knopf ● in der Spur), die
  Section **Verse** anklicken (die Aufnahme geht in die gewählte Section,
  sonst in die unter dem Playhead), Record (● oben oder Taste R). 8 Takte spielen oder singen — eine Melodie in a-Moll über
  Am–F–C–G. Stopp.
- Bild: Aufnahme läuft, die Wellenform wächst auf der Spur Take.
- Was eingespielt wird, entscheidet Matthias (Stimme, Gitarre, Keyboard
  über Audio). Es muss in a-Moll sein, damit Prompt 2 passt.

### Szene 4 — Der Agent hört zu und ergänzt (Rohzeit ~60 s, im Video ~15 s)

Prompt 2:

```text
I just recorded a take on the track Take, in the Verse. What key is it in? Then add a soft pad that plays chords in that key under the Verse, and put a little reverb on my take.
```

- Erwartet: der Agent nennt die Tonart der Aufnahme (Trockenlauf:
  „A minor“), legt eine Spur Pad (4OSC, weicher Klang) mit Am–F–C–G in
  der Verse an und setzt einen eingebauten Reverb auf Take (leise, etwa
  −14 dB Wet).
- Bild: Antwort mit der Tonart im Terminal groß lesbar; rechts die neue
  Spur Pad und das Modul „Take Reverb“ in der Routing-Kette.

### Szene 5 — Varianten im Song-Baum (Rohzeit ~2 min, im Video ~20 s)

Prompt 3:

```text
Now try a more energetic version of the Verse in a new branch: sixteenth-note hi-hats and a busier bass line. Keep the current version as it is, so I can compare both.
```

- Erwartet: der Agent benennt den aktuellen Node („Verse calm …“), legt
  einen Branch an („Verse energetic …“) und ändert dort Hi-Hats (16tel)
  und Bass; er rendert beide Fassungen zum Vergleich.
- Matthias: danach in der UI den Node „Verse calm“ anklicken, Play in der
  Verse, dann den Node „Verse energetic“, Play — der Unterschied ist zu
  hören (im Schnitt je 4 s).

### Szene 6 — Mix und Render (Rohzeit ~40 s, im Video ~15 s)

Prompt 4:

```text
Keep the energetic version. Mix it for streaming: about -14 LUFS and no clipping. Render the whole song to ~/music/unidaw-demo/demo.wav and tell me the loudness.
```

- Erwartet: Favorit auf dem Node, Limiter auf Master („Master Limiter“),
  Fader angepasst, Render, Messung der Datei: etwa −14 LUFS, True Peak
  unter −1 dB.
- Bild: Antwort mit „−14.0 LUFS“ im Terminal; rechts der Limiter in der
  Master-Kette.

### Szene 7 — Ende (5 s)

- Bild: UI mit dem ganzen Song-Baum, Play läuft.
- Texteinblendung: „unidaw — free and open source (AGPLv3), Linux“ und
  die Repo-Adresse.

## Schnitt

Ton für das ganze Video: der fertige Render `demo.wav` (läuft im Hintergrund
leise, am Ende voll). Die Aufnahme in Szene 3 hört man darin ohnehin.

Szenen beschleunigen und zusammenfügen, ohne Schnittprogramm (Dateien je
Szene `s1.webm` … `s7.webm`; Faktor je Szene anpassen):

```bash
# eine Szene 6x schneller, 30 fps, 1080p
ffmpeg -i s2.webm -vf "setpts=PTS/6,fps=30,scale=1920:1080" -an -c:v libx264 -crf 20 -pix_fmt yuv420p s2.mp4
# alle Szenen hintereinander
printf "file '%s'\n" s1.mp4 s2.mp4 s3.mp4 s4.mp4 s5.mp4 s6.mp4 s7.mp4 > list.txt
ffmpeg -f concat -safe 0 -i list.txt -c copy video.mp4
# Ton darunter, auf die Länge des Videos gekürzt
ffmpeg -i video.mp4 -i ~/music/unidaw-demo/demo.wav -map 0:v -map 1:a -c:v copy -c:a aac -b:a 192k -shortest agent-daw-demo-v1.mp4
ffprobe agent-daw-demo-v1.mp4   # erwartet: h264 und aac
```

Der Render des Trockenlaufs ist nur 30 s lang (12 Takte bei 96 BPM). Für
ein Video von 90–120 s den Render mehrfach hintereinander legen oder im
letzten Prompt den Song verlängern lassen (z. B. „duplicate the Verse
twice“) — dann vorher einmal ausprobieren.

## Fallstricke

- **Wartezeit des Agenten:** 40 s bis 2 min je Prompt (Trockenlauf: 66,
  56, 114, 39 s). Im Schnitt beschleunigen; nicht live warten.
- **Kosten:** der Trockenlauf mit Opus kostete laut Claude Code zusammen
  etwa 5 USD für vier Prompts. Ein zweiter Durchgang kostet wieder so viel.
- **Antworten des Agenten sind lang.** Im Video nur die erste Zeile und
  das Ergebnis zeigen (Tonart, LUFS); den Rest im Schnitt überspringen.
  Die Antworten weichen bei jedem Lauf etwas ab — Tonart und Spuren
  bleiben gleich, Formulierungen und Node-Namen nicht.
- **Node-Namen im Song-Baum:** Die automatischen Namen sind englisch
  („Drums: Notes“, „Bass: Sound“, Todo
  `2026-11-21-auto-node-namen-auf-englisch`). Ein Projekt von davor behält
  seine deutschen Namen — für die Aufnahme ein frisches Projekt nehmen.
- **Name des Eingangs:** Die Zeile „INPUT“ in der UI zeigt den Namen des
  System-Eingangs von PipeWire in der Systemsprache (Trockenlauf:
  „Fireface UC ADAT-Eingang 7/8“). Für die Aufnahme den Eingang auf
  einen englisch benannten stellen oder den Dock unten eingeklappt lassen.
- **Latenz bei der Aufnahme:** mit Kopfhörer am Fireface aufnehmen, nicht
  über Lautsprecher (sonst kommt der Beat mit in die Aufnahme und die
  Tonart-Erkennung hört den Bass mit). Buffer klein halten
  (`audioSettings {bufferSize}`), Monitor über das Fireface.
- **Erst die Aufnahme, dann Prompt 2:** der Agent liest die Aufnahme von
  der Spur Take. Im Trockenlauf (ohne Audio-Eingang) lag dort eine
  eingefügte Datei; `lastTake` meldete „no recorded take“, der Agent
  nahm den Clip trotzdem. Mit echter Aufnahme entfällt das.
- **Rückfragen von Claude Code:** ohne vorab erlaubte Tools fragt Claude
  Code bei jedem Befehl nach. Im Trockenlauf versuchte der Agent einmal
  `Bash` (für ein Noten-Skript); ohne Erlaubnis lief er ohne weiter.
- **Fenster:** keine Benachrichtigungen (GNOME „Nicht stören“), kein
  zweiter Bildschirm im Bild, Browser ohne Lesezeichenleiste und Tabs.
- **Port und Token:** das AppImage nimmt Port 7400, der MCP-Server liest
  den Token aus `~/.config/unidaw/api-token-7400` — kein Schalter nötig.
  Läuft schon ein anderes unidaw auf 7400, arbeitet der Agent dort.

## Trockenlauf (2026-11-21)

- AppImage `unidaw-0.1.0-x86_64.AppImage`, gebaut aus diesem Branch,
  gestartet ohne Audio (`--no-audio`) auf Port 7480 mit eigenem
  Config-Ordner; MCP-Server als Kopie mit Port 7480.
- Claude Code (`claude -p`, Opus, `--setting-sources project,local`, nur
  die beiden unidaw-Tools erlaubt), eine Session über alle vier Prompts.
- Die Aufnahme des Menschen ersetzt durch eine erzeugte Datei: 8 Takte
  Melodie in a-Moll bei 96 BPM (gezupfter Ton, selbst erzeugt), per
  `addClip` auf Take in der Verse.
- Ergebnis: jeder Prompt lief ohne Fehler durch und brachte das
  erwartete Ergebnis (oben je Szene unter „Erwartet“). Render
  `demo.wav`: −14,0 LUFS, True Peak −1,1 dB.
- Nicht geprüft: echte Aufnahme über das Fireface, Bildschirmaufnahme,
  Schnitt.
