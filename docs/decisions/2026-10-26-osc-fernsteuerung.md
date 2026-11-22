# OSC-Fernsteuerung: feste Adressen, standardmäßig nur dieser Rechner

- **Status:** gilt — umgesetzt (Todo `todos/2026-10-26-osc-fernsteuerung.md`)
- **Datum:** 2026-10-26, Matthias

## Kontext

Live-Spiel per OSC (Feature-Longlist, ältere DAW-Decision). OSC hat keine
Anmeldung: Wer Pakete an den Port schicken kann, steuert dawd.

## Entscheidung

- OSC ist **standardmäßig aus** und lauscht nur auf **127.0.0.1**. Das Netz
  öffnet nur, wer `oscSettings {address}` ausdrücklich setzt.
- Aus dem Netz gelten nur Absender auf der **Liste erlaubter IPs**
  (`oscSettings {allow}`); 127.0.0.1 ist immer erlaubt, leer = nur dieser
  Rechner. Fremde Pakete werden verworfen und gezählt.
- Es gibt nur **feste Adressen unter `/dawd/`**: Transport, Section/Marker,
  Spur-Mix, Plugin-Parameter. Keine allgemeine Op-Durchreiche, nichts mit
  Dateien, Projekten oder dem Daemon.
- Namen stehen im Pfad, `_` steht für ein Leerzeichen, Spuren gehen auch per
  Index.
- Werte haben die Einheiten der API (dB, −1..1, Parameter 0..1); dazu
  `/dawd/track/<n>/fader 0..1` mit der Kurve des UI-Faders, weil OSC-Apps
  meist 0..1 senden.
- Jede Adresse läuft über `executeOp`: gleiche Guards, gleiches Undo,
  gleiches Log, Auto-Branch wie bei der UI. Eine Fader-Geste ist ein
  Undo-Schritt.
- Auto-Branch bei OSC-Fadern bleibt wie in der UI; im ersten Live-Test
  beobachten.

## Folgen

- Kein Rückkanal zur App (Motorfader o. ä.), bis er gebraucht wird.
- Eine neue OSC-Adresse braucht eine Zeile in `Osc.cpp`, die Doku und einen
  Test.
