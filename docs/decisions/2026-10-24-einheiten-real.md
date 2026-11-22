# Einheiten: `real` ist die angezeigte Zahl in der gemeldeten Einheit

- **Status:** gilt — umgesetzt in 5e21a41 (Branch `q3-einheiten`)
- **Datum:** 2026-10-24, Matthias

## Kontext

Die Praxistests stolperten mehrfach über Einheiten (1g P7, 1h P12, 1j P8,
1l P8, 1m P11). Fakten (Agent `kleinkram`, 2026-10-24): bei 4OSC ist die
Amp-Hüllkurve in ms, Filter- und Mod-Hüllkurve in Sekunden, obwohl
`listParameters` „ms“ meldet; Prozent-Regler rechnen 0..100; Rezepte
speichern den internen Engine-Wert ohne Einheit; `setAutomation` kennt für
Plugin-Parameter nur 0..1.

## Entscheidung

`real` heißt überall die angezeigte Zahl in der gemeldeten Einheit: Zeiten
in ms, Pegel in dB, Anteile in % (0..100). Fehlermeldungen nennen die
Einheit. Rezept-Zusammenfassungen zeigen `real` und `unit` wie
`listParameters`. `setAutomation` nimmt je Punkt zusätzlich `real`; `value`
0..1 bleibt.

## Folgen

- 4OSC Filter-/Mod-Hüllkurve: `real` wechselt von Sekunden auf ms — Bruch
  für Aufrufer, die heute Sekunden schicken.
- Gespeicherte Rezepte: der Klang steckt im Plugin-Zustand, nur die
  Anzeige-Zahlen ändern sich.

## Nachtrag 2026-10-25 (Entscheidungen E1, E5)

- 4OSC Pan: real −100..100 % (L negativ), Text „-50%“ statt „50L“/„50R“
  (vorher meldete listParameters für beide Seiten real 50).
- 4OSC Level (Master) in dB, Fine Tune in ct.
- Umgesetzt als Engine-Patch `dawd/patches/tracktion-fourosc-units.patch`;
  Prozent-Texte (Pan, Detune, Pulse Width) rechnen real direkt um statt über
  die Textsuche (die traf die Kante: −50 → „-51%“).
