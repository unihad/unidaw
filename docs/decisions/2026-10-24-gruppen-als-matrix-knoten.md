# Gruppen sind Knoten der Routing-Matrix

- **Status:** gilt (Branch `gruppen`, Merge folgt)
- **Datum:** 2026-10-24, Matthias

## Kontext

Gruppen-/Submix-Spuren aus der Longlist. Die Engine hat Folder- und
Submix-Spuren; dawd führt Signale aber über die Routing-Matrix (AuxSends,
alle Track-Ausgänge in der Void-Spur).

## Entscheidung

Eine Gruppe ist ein Knoten `g<N>` der Routing-Matrix — ein Bus wie Main
(versteckte Spur mit Fader und Sends). Mitglied ist jede Spur, deren Weg
nach Main durch die Gruppe läuft; die Mitgliedschaft wird aus den Kanten
gelesen, keine zweite Datenhaltung. Keine Folder/Submix-Spuren der Engine.

## Folgen

- Ops `createGroup`, `setGroup`, `removeGroup`, `groupMix`; eigene Kette der
  Gruppe; Stems je Gruppe.
- Gruppen-Mute/Solo sind Abhör-Hilfen wie bei Spuren (Decision 2026-10-20).
- Verschachtelte Gruppen nur über die Matrix; Rezepte kennen Gruppen noch
  nicht.
