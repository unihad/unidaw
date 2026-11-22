# AppImage für Linux

Die erste öffentliche Version von unidaw kommt als AppImage im
GitHub-Release (Todo `2026-11-18-appimage-bauen`, Ziel
`agent-daw-bereit-fuer-den-vertrieb-machen`).

## Bauen

```bash
bash tools/fetch_ml.sh                 # einmal: ONNX Runtime, Rubberband (und Modelle für die Suite)
UNIDAW_SOURCE_URL=<Repo-URL mit Tag> bash tools/build_appimage.sh
```

Ergebnis: `dawd/build/appimage/unidaw-<version>-x86_64.AppImage` und
`.sha256` daneben. Version aus `project(unidaw VERSION …)` in
`dawd/CMakeLists.txt`.

Was das Skript tut:

- baut unidaw auf der älteren Basis Ubuntu 22.04 (glibc 2.35, g++-12),
  im Build-Ordner `dawd/build-ubuntu-22.04` — siehe „Ältere Basis“.
  `UNIDAW_HOST_BUILD=1` baut stattdessen auf diesem Rechner in `dawd/build`
  (schneller, aber nur für Systeme mit derselben glibc oder neuer).
  `-j4`, `JOBS=` ändert das; mehr Compiler sprengen den Speicher. Ohne
  ONNX Runtime oder Rubberband in der Binary bricht es ab — das Release
  soll ML-Ops und Timestretch haben.
- legt das AppDir an:
  - `AppRun` (`dawd/appimage/AppRun`) — Einstieg, siehe unten
  - `usr/bin/dawd`, `unidaw-start`, `unidaw-fetch-models`
  - `usr/lib/` — alle Bibliotheken, die unidaw und ONNX Runtime laden,
    außer glibc (muss vom System kommen) und alsa-lib (muss zur
    ALSA-Konfiguration und den ALSA-Plugins des Systems passen, z. B.
    pipewire-alsa). `usr/lib/unidaw-exec` setzt `LD_LIBRARY_PATH` nur für
    unidaw, nicht für den Browser, den der Starter öffnet.
  - `usr/lib/cxx/` — libstdc++ und libgcc_s der älteren Basis.
    `unidaw-exec` nimmt sie nur, wenn die libstdc++ des Systems nicht
    neuer ist (höchste `GLIBCXX_3.4.N` verglichen). Grund: Plugins, die
    auf einem neueren System gebaut sind, brauchen dessen libstdc++ —
    Dexed, sfizz und TestGain auf dem Build-Rechner (Ubuntu 26.04)
    verlangen `GLIBCXX_3.4.32`, die libstdc++ von 22.04 hat 3.4.30.
  - `usr/share/unidaw/` — UI, Kits, Plugin-Profile, Rezepte aus dem
    Git-Stand (`git archive HEAD`, keine unversionierten Dateien), ohne
    den Diva-Preset-Katalog
    ([Decision](decisions/2026-11-18-oeffentlicher-stand-ohne-historie.md)).
    unidaw findet sie über `<bin>/../share/unidaw/` (`dawd/source/Resources.h`).
  - `usr/share/doc/unidaw/` — `LICENSE`, `THIRD_PARTY_LICENSES.md`,
    `SOURCE` (Version, Commit, `UNIDAW_SOURCE_URL`), `LICENSE-NOTICE`,
    `licenses/` mit den Lizenztexten der Bausteine (Pfade wie im
    Quellbaum; `system/` = Copyright-Dateien der Ubuntu-Pakete der
    gebündelten Bibliotheken; `appimage-runtime/`).
- keine Modelle (`docs/lizenzen-agpl.md`): die lädt der Nutzer mit
  `fetch-models` nach `~/.local/share/unidaw/models`.
- AppImage = AppImage type2-runtime (Release `20251108`, SHA-256 im
  Skript, liegt danach in `dawd/third_party/appimage-runtime-*`) +
  SquashFS (`mksquashfs`, zstd). Kein appimagetool nötig.

## Ältere Basis

`tools/buildroot.sh` legt Ubuntu 22.04 als Ordner an — ohne sudo, ohne
podman/docker: das offizielle `ubuntu-base`-Archiv (22.04.5, SHA-256 im
Skript), darin `apt-get` als root in einem eigenen User-Namespace von
bubblewrap. Ablage `~/.cache/dawd-buildroot/` (`UNIDAW_BUILDROOT=` ändert
das):

- `ubuntu-22.04-build` (~0,8 GB): g++-12, CMake 3.22, Header für JUCE
  (ALSA, JACK, X11, Freetype), git, mksquashfs. `build_appimage.sh`
  startet sich darin selbst noch einmal; Quellbaum, Git-Ordner und
  Ausgabe liegen an denselben Pfaden.
- `ubuntu-22.04-run` (~0,1 GB): nur python3, curl, libasound2 — die
  Prüfung auf der älteren Basis (unten).

Ein zweiter Lauf ändert nichts, solange die Paketliste im Skript gleich
bleibt. Warum so: podman/docker sind nicht installiert (bräuchte sudo),
der Forgejo-Runner braucht für fremde Images ebenfalls einen
Container-Dienst; bubblewrap ist da (auch für `check_appimage.sh`).
Warum 22.04: g++-12 baut C++20 und JUCE 8, CMake 3.22 ist genau das
Minimum von unidaw und JUCE; glibc 2.35 deckt Ubuntu 22.04 und jede
Distribution mit glibc 2.35 oder neuer ab. ONNX Runtime 1.31 braucht selbst nur glibc 2.28.

## Benutzen

```bash
./unidaw-0.1.0-x86_64.AppImage                     # Starter: unidaw + UI im Browser, Ctrl+C beendet
./unidaw-0.1.0-x86_64.AppImage --port 7401 --no-open -- --no-audio
./unidaw-0.1.0-x86_64.AppImage unidaw --port 7400    # nur der Daemon (Server, MCP)
./unidaw-0.1.0-x86_64.AppImage fetch-models        # Modelle laden (~1,2 GB), mit Lizenzen
./unidaw-0.1.0-x86_64.AppImage licenses            # Lizenz, Commit, Quellcode
```

Das Dateisystem des AppImage gibt es nur, solange der Starter läuft.
Darum startet `AppRun` unidaw im Vordergrund (`unidaw-start --foreground`);
läuft auf dem Port schon ein unidaw, öffnet es nur die UI. Ohne FUSE:
`APPIMAGE_EXTRACT_AND_RUN=1` oder `--appimage-extract-and-run`.

`unidaw-fetch-models` liegt in `dawd/launcher/`; `tools/fetch_ml.sh` ruft
es mit `dawd/models` auf. Es nennt jedes Modell mit Lizenz und Quelle
und prüft jede Datei per SHA-256 (Stand der Lizenzprüfung 2026-11-19).

## Prüfen

```bash
bash tests/check_appimage.sh [<AppImage>]
```

Läuft in einer Umgebung aus bubblewrap: nur `/usr` und `/etc` des
Systems (nur lesen), leeres Home und `/tmp`, eigenes Netz und eigene
PIDs, kein Audio-Gerät, kein Quellordner. Geprüft: Hilfe und Lizenzen,
UI samt Vendor-Dateien, Kits/Plugin-Profile/Rezepte aus dem AppImage,
Modelle im Nutzerordner gesucht (Fehler nennt `fetch-models`), Render
mit 4OSC hörbar, jede gebündelte Bibliothek kommt aus dem AppImage (was
alsa-lib und JACK per dlopen nachladen, z. B. PipeWire, kommt vom
System), libstdc++ vom System, wenn sie neuer ist, sonst aus dem
AppImage, Ctrl+C beendet unidaw.
Mit FUSE auf dem System läuft vorher `--help` im Mount-Modus.

Mit `UNIDAW_CHECK_BASE=ubuntu-22.04` läuft dieselbe Prüfung mit `/usr`
und `/etc` der schlanken älteren Basis (`tools/buildroot.sh run`) statt
denen dieses Rechners: das ist der Start auf Ubuntu 22.04 selbst.

Nicht Teil von `tests/run.sh` (Bauen dauert, lädt die Runtime).

## Grenzen

- **glibc:** die Binary braucht glibc 2.35 oder neuer (das Skript nennt
  die Version am Ende). Systeme mit älterer glibc starten das
  AppImage nicht. Mit `UNIDAW_HOST_BUILD=1` gilt die glibc des
  Build-Rechners (Ubuntu 26.04: 2.43).
- Geprüft ist der Start auf Ubuntu 22.04 (schlanke Basis) und auf dem
  Build-Rechner; andere Distributionen (Fedora, Arch, Debian) prüft die
  Suite nicht.
- Der Starter braucht `curl` vom System (Prüfung, ob unidaw antwortet).
- Rezepte mit Status „checked“ speichert unidaw in `usr/share/unidaw/recipes`
  — im AppImage nur lesbar, Speichern schlägt dort fehl. Entwürfe
  (`<config>/recipes/`) gehen.
