# Lizenzen der Abhängigkeiten für AGPLv3

Stand: 2026-11-19 (Systembibliotheken, MP3-Decoder (bleibt) und Plugin-Steckbriefe
ergänzt; Rest 2026-11-18). Prüfung für die Veröffentlichung von unidaw als Open
Source unter AGPLv3 (Ziel `agent-daw-bereit-fuer-den-vertrieb-machen`,
Todo `2026-11-18-lizenzen-der-abhaengigkeiten-fuer-agpl-pruefen`).
Die Liste für Nutzer (Englisch, mit Pfaden zu den Lizenztexten):
`THIRD_PARTY_LICENSES.md`. Lizenztext von unidaw: `LICENSE` (AGPLv3, Text
von gnu.org).

Keine Rechtsberatung: die Prüfung liest die Lizenzangaben der Quellen,
sie ersetzt keinen Anwalt.

## Ergebnis

- **Alles, was in die Binary oder neben sie kommt, ist mit AGPLv3
  vereinbar.** Kein Baustein verlangt eine kommerzielle Lizenz, solange
  unidaw unter AGPLv3 erscheint.
- **AGPL-3.0-only, nicht „or later“**: JUCE nennt „AGPLv3“ ohne „or later“
  (`tracktion_engine/modules/juce/LICENSE.md`, Kopf von
  `juce_core/juce_core.h`). Das Gesamtwerk mit JUCE steht damit unter
  AGPLv3. Eigener Code könnte „or later“ tragen, das Gesamtwerk nicht.
- **Keine Modelle mitliefern.** Die Modelle lädt `unidaw-fetch-models`
  (im AppImage `fetch-models`, im Repo über `tools/fetch_ml.sh`) aus
  der Quelle, mit Lizenzangabe und Prüfsumme; das AppImage enthält keine. Grund: unklare Trainingsdaten
  (Mega 53) und unklare Wirkung von Spenden auf Lizenzen mit
  NC-Klausel. Heute hat keines der geladenen Modelle eine NC-Lizenz
  (Stand der Modellkarten 2026-11-18, erneut geprüft 2026-11-19).
- **MP3-Decoder bleibt.** Lizenz passt (JUCE), JUCE übernimmt aber keine
  Haftung für Patente. Matthias hat am 2026-11-19 entschieden, dass er im
  Release bleibt (Decision
  [2026-11-19-mp3-decoder-bleibt](decisions/2026-11-19-mp3-decoder-bleibt.md)).

## Bausteine in der Binary

| Baustein | Lizenz | Mit AGPLv3? | Quelle der Angabe |
| --- | --- | --- | --- |
| JUCE 8.0.13 | AGPLv3 / JUCE-Lizenz | ja, ist die Basis | `tracktion_engine/modules/juce/LICENSE.md` |
| Tracktion Engine (e42d82f) | GPL-3.0-or-later / kommerziell | ja: GPLv3 § 13 erlaubt die Verbindung mit AGPLv3 | `tracktion_engine/LICENSE.md` |
| Rubberband 4.0.0 | GPL-2.0-or-later | ja, als GPLv3 genommen | `dawd/third_party/rubberband/COPYING`, `README.md` |
| JUCE-Teile: VST3 SDK (MIT), LV2 (ISC), FLAC/Ogg Vorbis (BSD), zlib, libpng, jpeglib (IJG), HarfBuzz (Old MIT), SheenBidi (Apache-2.0), pslextensions (gemeinfrei) | frei | ja | Liste in `juce/LICENSE.md`, Abschnitt „Dependencies“ |
| Engine-Teile: CHOC (ISC), crill (BSL-1.0), doctest/magic_enum/MPMCQueue/farbot (MIT), expected (CC0), libsamplerate (BSD-2), moodycamel ConcurrentQueue/LightweightSemaphore (BSD-2, Teil zlib), rpmalloc (gemeinfrei), Airwindows (MIT) | frei | ja | Kopf bzw. `LICENSE` je Ordner unter `tracktion_engine/modules/` |

Welche JUCE-Module unidaw baut: `dawd/build/compile_commands.json` (keine
OpenGL-, JavaScript-, Box2D-Module, kein ARA, kein AU/AAX/ASIO/Oboe).
SoundTouch (LGPL-2.1) und Signalsmith Stretch sind in der Engine
abgeschaltet (`tracktion_engine.h`, `TRACKTION_ENABLE_TIMESTRETCH_*` = 0).

MP3-Decoder: unidaw baut ihn mit `JUCE_USE_MP3AUDIOFORMAT=1`
(`dawd/CMakeLists.txt`). Er ist JUCE-Code unter derselben Lizenz; der Kopf
von `juce_MP3AudioFormat.cpp` lehnt jede Haftung für Patente und
Urheberrechte Dritter ab und rät zu eigener Prüfung. Er bleibt im
Release (Decision
[2026-11-19-mp3-decoder-bleibt](decisions/2026-11-19-mp3-decoder-bleibt.md)).

Airwindows: die Dateien im Engine-Baum tragen nur „Copyright (c) 2016
airwindows, All rights reserved“; die Lizenz steht im Upstream-Repo
`github.com/airwindows/airwindows`, Datei `LICENSE`: MIT, Copyright (c)
2018 Chris Johnson (abgerufen 2026-11-18).

## Systembibliotheken

Die Binary lädt diese Bibliotheken vom Betriebssystem
(`readelf -d` auf `dawd/build/unidaw_artefacts/Release/unidaw`; JACK per
`dlopen` in `juce_JackAudio.cpp`). Lizenz laut
`/usr/share/doc/<paket>/copyright` unter Ubuntu 26.04:

| Bibliothek | Paket | Lizenz | Mit AGPLv3? |
| --- | --- | --- | --- |
| `libasound.so.2` | libasound2t64 | LGPL-2.1+ | ja |
| `libfreetype.so.6` | libfreetype6 | FTL | ja (Systembibliothek, nicht im Repo) |
| `libfontconfig.so.1` | libfontconfig1 | freie Erlaubnis im MIT-Stil | ja |
| `libstdc++`, `libgcc_s`, `libatomic` | GCC | GPL-3 mit Runtime Library Exception | ja |
| `libjack.so.0` (nur wenn installiert) | libjack-jackd2-0 | LGPL-2.1+ | ja |

Das AppImage bündelt die GCC-Runtime, FreeType und Fontconfig samt
dem, was sie laden (Expat, zlib, bzip2, libpng, Brotli — alle frei, ja);
die Copyright-Dateien der Pakete legt `tools/build_appimage.sh` nach
`usr/share/doc/unidaw/licenses/system/`. alsa-lib, JACK und glibc kommen
immer vom System (`docs/appimage.md`).

Die AppImage-Runtime (type2-runtime 20251108) ist MIT und linkt musl
(MIT), libfuse 3 (LGPL-2.1) und squashfuse (BSD-2) statisch. Sie wird
unverändert weitergegeben; Lizenz und Quelle (Repo und Tag mit den
Build-Skripten) liegen im AppImage unter `licenses/appimage-runtime/`.

## Neben der Binary

| Baustein | Lizenz | Mit AGPLv3? | Pflicht beim Ausliefern |
| --- | --- | --- | --- |
| ONNX Runtime 1.31.0 (`libonnxruntime.so`) | MIT | ja | `LICENSE` und `ThirdPartyNotices.txt` mitgeben |
| Web Awesome 3.14.0 (`dawd/ui/vendor/webawesome`) | MIT | ja | `LICENSE.md` mitgeben (liegt im Ordner) |
| Drum-Kit `basic` | eigenes Werk | ja | — (synthetisiert, `dawd/kits/make_basic_kit.py`) |

Web Awesome kann Icons von Font Awesome aus dem Netz laden
(`fontawesome.com` in einigen Chunks). `flow.html` nutzt kein `wa-icon`;
es wird also nichts geladen. Kommt später ein Icon dazu, gilt die
Lizenz der Font-Awesome-Icons mit.

MCP-Server (`mcp/unidaw_mcp.py`): nur Python-Standardbibliothek, keine
Fremdpakete. `tools/export_mega53.py` braucht torch und einops; es läuft
nur beim Entwickler (`MEGA53=1`) und kommt nicht ins AppImage.

Plugin-Steckbriefe (`dawd/plugin-profiles/`): eigene Messungen an
Plugins Dritter (Parameter, Klangwerte), keine Teile der Plugins. Die
Preset-Kataloge (`*-presets.json`, `*-sounds.json`) tragen Preset-Namen
der Hersteller. Der Diva-Katalog bleibt aus dem öffentlichen Stand
(Decision `docs/decisions/2026-11-18-oeffentlicher-stand-ohne-historie.md`).
Die übrigen gehören zu Surge XT, Dexed, Helm, sfizz und MDA (frei laut
Decision) und zu vier AVL Drumkits von Robin Gareus (Plugin GPL-2+,
Samples CC-BY-SA-3.0 laut `/usr/share/doc/avldrums.lv2/copyright`; der
Katalog enthält nur Kit- und Preset-Namen, keine Samples).

## Modelle (nicht mitliefern)

| Modell | Lizenz | Quelle der Angabe | Mitliefern? |
| --- | --- | --- | --- |
| basic-pitch (`nmp.onnx`) | Apache-2.0 (Lizenz des Repos; keine eigene Datei im Modellordner) | `github.com/spotify/basic-pitch`, README | nein, Download |
| HT-Demucs v4 (Export adowu) | MIT | Modellkarte `huggingface.co/adowu/htdemucs-onnx` | nein, Download |
| HT-Demucs 6s (Export StemSplit) | MIT | Modellkarte `huggingface.co/StemSplitio/htdemucs-6s-onnx` | nein, Download |
| CLAP `larger_clap_general` (Export Xenova) | Apache-2.0 laut Basismodell; der Export selbst nennt keine | `huggingface.co/laion/larger_clap_general`, `huggingface.co/Xenova/larger_clap_general` | nein, Download |
| MVSep Mega 53 | Gewichte MIT laut Autor; Rechte an den Trainingsdaten hat er nicht alle | `github.com/ZFTurbo/Music-Source-Separation-Training`, Issue #245 (2026-09-25) | nein, nur mit `MEGA53=1` |

Die frühere Notiz in `tools/fetch_ml.sh`, manche HT-Demucs-Exporte
nennten CC BY-NC, trifft auf die geladenen Exporte nicht zu: beide
Modellkarten sagen MIT (Kommentar dort angepasst). Das Upstream-Repo `facebookresearch/demucs` nennt
MIT für den Code und keine eigene Lizenz für die Gewichte.

## Pflichten aus der AGPLv3 für die Veröffentlichung

- `LICENSE` und `THIRD_PARTY_LICENSES.md` ins Repo und ins AppImage.
- Quellcode zum AppImage anbieten: Link auf das GitHub-Repo und den Tag
  des Release.
- Lizenztexte der Bausteine ins AppImage legen (ONNX-Runtime-Notices,
  Web-Awesome-Lizenz, Rubberband `COPYING`, JUCE- und Engine-Lizenzen):
  erledigt `tools/build_appimage.sh` (`usr/share/doc/unidaw/`, Liste in
  `docs/appimage.md`). Den Ort des Quellcodes nennt `UNIDAW_SOURCE_URL`
  beim Bauen (Datei `SOURCE`, Befehl `licenses`).
- AGPLv3 § 13: unidaw bedient die Flow-UI übers Netz (HTTP). Wer unidaw
  verändert und anderen über das Netz anbietet, muss den Quellcode
  anbieten. Ein Link „Source code“ mit Lizenzhinweis in der UI deckt
  das ab — eigenes Todo.
- Die eigenen Engine-Korrekturen liegen als Patches in `dawd/patches/`;
  sie sind Teil des veröffentlichten Quellcodes.

## Nicht mehr nötig gegenüber dem Closed-Source-Plan

`docs/vertrieb-und-verkauf.md` plante kommerzielle Lizenzen für JUCE und
Tracktion und den Ersatz von Rubberband. Unter AGPLv3 entfällt das:
JUCE und Tracktion laufen unter ihrer freien Lizenz (ohne
Tracktion-Branding-Pflicht, die gehört zur kommerziellen Personal-Stufe),
Rubberband darf bleiben.
