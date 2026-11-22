# MP3-Decoder von JUCE bleibt im öffentlichen Release

- **Status:** gilt — Matthias hat am 2026-11-19 entschieden (Todo
  `todos/2026-11-18-lizenzen-der-abhaengigkeiten-fuer-agpl-pruefen.md`,
  Frage „mp3“: bleibt).
- **Datum:** 2026-11-19, Entwurf Claude Code

## Kontext

dawd baut den MP3-Decoder von JUCE mit `JUCE_USE_MP3AUDIOFORMAT=1`
(`dawd/CMakeLists.txt`). Damit liest dawd MP3-Dateien, vor allem
Referenz-Songs für `compareReference` und MP3 als Clip.

Die Lizenz passt: der Decoder ist JUCE-Code unter AGPLv3
(`docs/lizenzen-agpl.md`). Der Kopf von `juce_MP3AudioFormat.cpp` lehnt
aber jede Haftung für Patente und Urheberrechte Dritter ab und rät zu
eigener Prüfung.

## Entscheidung

Der MP3-Decoder bleibt im öffentlichen Release. `JUCE_USE_MP3AUDIOFORMAT`
bleibt an.

## Alternativen

- **Decoder abschalten:** verworfen — dawd läse keine MP3-Referenzen
  mehr; Nutzer müssten sie vorher selbst nach WAV wandeln.

## Folgen

- `THIRD_PARTY_LICENSES.md` nennt den Decoder samt Haftungsausschluss von
  JUCE (unverändert).
- Keine Code-Änderung.
