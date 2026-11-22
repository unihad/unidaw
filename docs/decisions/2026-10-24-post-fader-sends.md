# Sends hinter der Kette sind Post-Fader-Sends

- **Status:** gilt — umgesetzt in 8ae438c (Branch `folge-routing`)
- **Datum:** 2026-10-24, Matthias

## Kontext

Der Fader einer Spur sitzt am Ende ihrer eigenen Kette (`faderTrackOf`,
Decision aus Befunde 1m). Ein Send hinter dem letzten Modul machte die Kette
bisher „complex“; dann rutschte der Fader vor die Kette, und jeder Send aus
dem Mixer war pre-fader (Bericht Mixer-Agent).

## Entscheidung

Ein Send hinter dem letzten Modul einer Spur zählt nicht als Verzweigung.
Der Fader bleibt am Kettenende, der Send geht hinter dem Fader ab
(Post-Fader-Send).

## Folgen

- `deriveChain`/`faderTrackOf` und die Simple-Sicht anpassen; der Mixer
  zeigt solche Sends als POST.
- Bestehende Projekte: der Fader wandert beim Laden an das Kettenende
  (`syncFaders`), Pegel bleiben.
