# UI-Checkliste

Checks für jede UI-Änderung am Flow-UI (`dawd/ui/flow.html`). Vor dem
Commit durchgehen — die Suite testet die API, nicht die Sichtbarkeit.

**Die Checkliste lebt mit**: bringt eine Feature-Änderung neue UI-Muster
(neue Buttons, Drags, Zustände, Wege zurück), kommen die sinnvollen neuen
Checks hier dazu — im selben Commit wie das Feature.

**Meldungen** (`say`) stehen seit Variante B (Decision
`docs/decisions/2026-10-29-ui-seitenleiste-referenz.md`) in der
Statuszeile unten, nicht mehr als Toast — wo unten „Toast“ steht, ist die
Statuszeile gemeint.

## Sichtbarkeit

- [ ] Alle Buttons sichtbar — auch auf farbigem Grund (Sections haben eine
      Farbe; Buttons darauf brauchen Kontrast, feste Größe und z-index)
- [ ] Buttons in klickbaren Elementen nicht hinter Text/Clips versteckt
      (z-index + padding am Inhalt, damit der Name nicht unter die Buttons
      läuft)
- [ ] Hinweise/Hints lesen den aktuellen Stand — kein toter Text
- [ ] Kein Modus-Umschalter (Decision
      `docs/decisions/2026-10-30-modus-umschalter-weg.md`): Record/Monitor/
      Insert, Arm, Automation („A“), Freeze, „+“ der Gruppen-Kette und die
      Sterne sind in jedem Node da; die Sorte eines Nodes kommt aus seinen
      Aktionen, den neuen Node legt Auto-Branch an (geprüft in
      `fehler_check.js`, Checks „M“)
- [ ] Undo/Redo-Buttons im Transport (gesperrt ohne Historie, Tooltip nennt
      den Schritt: "Undo addSection"); Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z und
      Ctrl+Y — in Eingabefeldern bleibt Ctrl+Z beim Feld; ein Undo über einen
      Auto-Node nimmt ihn aus dem Baum, der Toast nennt den Schritt
- [ ] Undo über einen Auto-Node mit Notiz (Button und Ctrl+Z): der Toast
      nennt Label und Notiz ("removed "Keys: Sound" with its note: …") und
      bietet "Redo" an; Redo bringt Node und Notiz zurück
- [ ] Auto-Branch: legt eine Aktion einen Node an, erscheint er im Baum
      (aktuell), der Toast nennt ihn ("new node "process 2" …"); Schalter
      im Song-Tree-Kopf (Zweig-Icon,
      hervorgehoben = an), Tooltip erklärt beide Zustände
- [ ] Timeline behält Platz: das Dock unten hat 42 % der Höhe (Reiter
      wechseln springt nicht), sein Inhalt scrollt im Reiter — Sections,
      Tracks und "+ Audio/MIDI" bleiben bei 900 px Fensterhöhe sichtbar
      (Piano-Roll eingeklappt per Default); Klick auf den offenen Reiter
      oder ⌄ rechts klappt das Dock auf die Reiterleiste zusammen (gemerkt)
- [ ] Dock-Höhe: die Kante über den Reitern (`#dockEdge`, Cursor
      row-resize, Hover in Akzentfarbe) zieht das Dock höher/flacher, die
      Höhe merkt sich der Browser (`unidaw.dockH`, auch nach Neuladen);
      Timeline behält 10 rem, das Dock 7 rem (auch beim Verkleinern des
      Fensters); Doppelklick auf die Kante = Standard 42 %; eingeklappt ist
      die Kante weg, Aufklappen per Reiter bringt die gezogene Höhe zurück
      (geprüft in `fehler_check.js`, Checks „D“)
- [ ] Dock-Reiter Routing · Mixer · Notes · Recipes (`#dockTabs`, gemerkt
      im Browser); der offene
      Reiter ist unterstrichen; Clip-Bearbeitung (Noten) holt Notes, „Find a
      recipe“ holt Recipes, Klick auf ein Modul holt Routing (im Mixer
      bleibt die Karte im Mixer-Reiter); Bedienelemente im Dock kompakt
      (1,75 rem), nichts verdeckt
- [ ] Track-Header: Name immer lesbar (zweite Zeile für Arm/S/M und
      Geräte-Dropdown), Typ als Farbpunkt (Cyan Audio, Violett MIDI)
- [ ] Track-Header gleich ausgerichtet, ob mit oder ohne aufklappbare Lanes
      (Platzhalter `grpspace` in Pfeilbreite) — Punkt, Name, Arm/S/M stehen
      bei allen Hauptspuren an derselben x-Position
- [ ] Tempo-Tag in jeder Section ("120 · 4/4", hell = eigen, blass =
      geerbt); Klick öffnet das Popover (BPM, Takt, Takte; Enter = Apply,
      Esc schließt, "Inherit" nur bei eigenem Tempo); Taktlinien im Band,
      sobald ein Takt mindestens 8 px breit ist
- [ ] Tempo-Rampe: Häkchen "Ramp to next" im Tempo-Popover; der Tag zeigt
      dann "100 ↗ 140 · 4/4" (↘ beim Ritardando), der Toast "100 → 140 BPM";
      Rampe an, aber gleiches Folge-Tempo bzw. letzte Section: Tag ohne
      Pfeil, Tooltip sagt warum; Band ohne festes Taktraster (Takte
      ungleich lang); "Bars" danach setzt die Länge passend zur Rampe
- [ ] Section-Buttons sitzen direkt hinter dem Namen (nicht am Ende —
      lange Sections würden sie off-screen schieben)
- [ ] Jede Section zeigt ohne Hover ihren ganzen Namen, auch eine schmale
      (z. B. 10 s bei 13 px/s): verborgene Buttons belegen keinen Platz; bei
      Hover/Wahl sind alle vier Buttons im Bild, Name und Tempo kürzen mit
      „…“ (`fehler_check.js`, Abschnitt S)
- [ ] Icons haben Kontrast auf ihrem Grund (kein text-quiet auf hellem
      Button); Record einheitlich rot für beide Typen
- [ ] Clip zeigt seinen Inhalt (Waveform bzw. Noten über `clipPeaks`)
- [ ] Transport-Leiste hat genau einen Record-, einen Monitor- und einen
      Insert-Button (`#hdrRec`, beide Typen) — immer sichtbar (der Stop
      muss da sein); der Record-Tooltip sagt, wohin der Take geht: in einem
      Node anderer Sorte legt Auto-Branch einen Collect-Node an, aus = der
      Take landet in diesem Node
- [ ] Arm-Schalter je Spur (rund, rot = geärmt, Spur leicht rot getönt)
      immer sichtbar; während der
      Aufnahme gesperrt und weiter "an" (Gruppe bleibt geärmt, auch wenn
      die Zuweisung auf der neuen Lane liegt)
- [ ] Freeze-Schalter je Spur (Schneeflocke, violett = gefroren, Spur
      leicht violett getönt, Clips blass) immer sichtbar; Flatten (⤓) nur
      gefroren, Toast mit Undo; eine gesperrte
      Bearbeitung an der gefrorenen Spur zeigt die Meldung als Toast
- [ ] Spurkopf: Zeile 1 Name und Pegel (6 px hoch, Klickfläche 24 px),
      Zeile 2 ● S M A ❄ (⤓ gefroren) mit 24 px und die Ziele „→ …“ — in
      1366×768 nichts abgeschnitten, auch eingerückt in einer Gruppe und
      gefroren mit sechs Knöpfen (Checks „K“)
- [ ] Gruppe (Submix) als Ordner in der Timeline: Kopfzeile (Ordner-Icon,
      Name, dB, Mitgliederzahl, ×) an der Stelle ihres ersten Mitglieds,
      Mitglieder eingerückt mit Führungslinie darunter, Pfeil klappt ein (die
      Lane zeigt die Clips der Mitglieder weiter als Balken); S M A, Fader und
      Pegel in Zeile 2 passen in die Header-Breite (auch 1366×768), die Kette
      "→ … → Main" in Zeile 3, "+" für ein Plugin;
      eine leere Gruppe steht unten und bleibt Drop-Ziel
- [ ] Comping: jeder Take einer Gruppe mit mindestens zwei Takes hat unten
      die Comp-Leiste (Fadenkreuz); farbig, wo er im Comp
      spielt (blass, solange der Comp nicht spielt), voll deckend auch in
      der gedimmten (gemuteten) Take-Zeile; die Comp-Lane
      ("<Spur>.comp", Name kursiv) zeigt je Teil oben die Farbe seines Takes
- [ ] Input-Strip (Gain + Meter) unten im Panel sichtbar, sobald der Header
      da ist — das `pollInputLevel`-Gate hängt am Strip
- [ ] MIDI-Strip (Piano-Roll) im Reiter Notes — sichtbar, sobald der
      Header da ist; Tonumfang 0–127 (Rolle scrollt, füllt die Höhe des
      Reiters, Kopf bleibt eine Zeile — Hinweis gekürzt, ganz im Tooltip);
      aufgeklappt startet sie um C4, auf einer Drum-Spur (Name oder
      Instrument nach drum/kit/perc/beat/808/909/kick/snare) ab 36, beim
      Bearbeiten um die Noten des Clips; Raster und Clip-Ende reichen über
      alle 128 Zeilen
- [ ] Piano-Roll ohne feste Länge: so lang wie die Section bzw. der Clip
      (auch > 32 s), dazu ein Takt hinter der letzten Note
- [ ] Velocity-Spur „Vel“ unter der Rolle (neu und beim Bearbeiten): ein
      Balken je Note; Ziehen an der Spalte einer Note setzt sie (seitlich =
      mehrere, gezogene hell), das Feld links zeigt den Wert; neue Noten
      nehmen den Wert des Felds; Feld + Enter setzt genau die Noten des
      letzten Ziehens; Noten zeigen die Velocity als Deckkraft und im
      Tooltip (geprüft in `fehler_check.js`, Checks „P“)
- [ ] Neuer Clip aus der Rolle: Namensfeld (leer = „Notes“, Enter legt
      an) neben „Add … notes“; die Statuszeile nennt den Namen
- [ ] Clip-Länge in der Rolle (neu und beim Bearbeiten): Clip-Ende-Linie
      ziehen (rastet auf 1/16, Griff links der Linie, ein Klick in die
      Spalte dahinter zeichnet eine Note) oder Feld „Length s“ (Sekunden
      oder Ende als „bar 5.1“); neu leer = bis zur letzten Note, beim
      Bearbeiten `trimClip` (abgeschnittene Noten fallen aus der Rolle)
- [ ] Geräte-Dropdown je Spur nur bei geärmter Spur und mehr als einem Gerät
      des Typs (state.inputs, Zuweisung markiert)
- [ ] Favoriten-Sterne: Clip im Clip-Popover oben („Mark as favorite“ /
      „Favorite“, gelb = favorisiert; Klick schließt das Popover), Section
      in ihrer Knopfleiste; favorisierte Clips tragen ★ unten rechts,
      Sections ★ vor dem Namen (Zustand aus state)
- [ ] Monitor-Strip (Interface): Kopfhörer-Schalter oben rechts (hervorgehoben
      = offen, Zustand im localStorage); der Strip steht
      oben im Dock und ist nach dem Öffnen sichtbar; Fader-Bahnen sichtbar,
      geroutete Fader grün getönt, Wert unter dem Fader ("off" ganz unten)

- [ ] Plugin-Fenster-Knopf (⧉) rechts im Kopf der Parameter-Karte eines
      Fremd-Plugins (VST3/LV2), hervorgehoben = Fenster offen; bei
      eingebauten Plugins (format builtin) kein Knopf; Fehler ("no editor",
      "no display") als Toast; Schließen nach einer Änderung im Fenster
      (`changed`) zeigt einen Toast mit Undo und frischt den Baum auf (mit
      Auto-Node: dessen Hinweis)
- [ ] Plugin-Sandbox: stürzt der Host-Prozess eines Moduls ab oder hängt,
      erscheint ohne Klick (über `position.sandboxSeq`) ein Toast; im
      Ketten-Slot ein Zeichen hinter dem Namen — gelb ↻ = mit dem letzten
      Zustand neu geladen (Tooltip nennt den Grund), rot ⚠ = stumm
      (gesperrt nach wiederholtem Absturz oder Neuladen gescheitert), Klick
      lädt neu; die Parameter-Karte zeigt den Hinweis als Zeile, rot mit
      „Reload“ (der Weg zurück); ein offenes Fenster des toten Hosts ist
      danach zu (Knopf nicht hervorgehoben)
- [ ] Parameter-Karte eines VST3 (Schalter an): Zeile „Runs …“ mit Auswahl
      (eigener Prozess je Plugin / je Modul / in unidaw); Ändern sagt im
      Toast, dass es beim nächsten Laden gilt; laufender Prozess daneben
- [ ] Plugin-Liste: Plugin-Dateien, deren Scan abstürzte (`scanErrors`),
      nennt ein Toast (Datei und Grund) — die Liste kommt trotzdem
- [ ] About / Licenses (ⓘ rechts in der Kopfleiste, Op `about`): immer da,
      auch ohne Song; der Dialog nennt Programm und Version, die Lizenz
      (GNU AGPLv3, Link auf gnu.org), den Quellcode-Link (AGPLv3 § 13 —
      fehlt er im Build, steht dort, wie man ihn setzt: `-DDAWD_SOURCE_URL`)
      und die Drittlizenzen aus `THIRD_PARTY_LICENSES.md` scrollbar — als
      Markdown gerendert (Überschriften, Tabellen, `Code`), kein Rohtext mit
      `#` und `|`;
      Links öffnen einen neuen Tab, Close und Escape schließen

## Aktionen ansteuerbar

- [ ] Jede Op-Familie (media, select, processing, tree, section, session,
      view, interface) hat einen erreichbaren Weg in der UI — API-only gilt als unfertig
- [ ] Monitor-Strip: Ausgangspaar wählen (Segment), Fader ziehen =
      `setInterface` routes (beim Ziehen kommt der letzte Wert an, kein
      Stau), Doppelklick off ↔ 0 dB; Mix- und Level-Fader rechts; Gain-Regler
      und 48V/Hi-Z/PAD unter den Eingängen (48V rot); "Digital" blendet
      S/PDIF/ADAT und PCM 9–18 ein; "Reset…" braucht einen zweiten Klick
- [ ] Kopf: Knopf „Audio“ (Wellen-Symbol, auch ohne Song) öffnet ein Popover mit API,
      Gerät, Rate und Puffer (ms); Buffer-Auswahl wirkt sofort; mit Song zusätzlich
      „Song rate“ (Device/kHz) und „Recording“ (16/24/32 float); unter JACK mit eigener
      Rate der Hinweis, dass der ganze Rechner mit dieser Rate läuft; Klick daneben
      schließt es, Öffnen allein ändert keine Version (kein Neuzeichnen)
- [ ] Im Audio-Popover darunter „OSC remote control“ (Tooltip mit allen
      Adressen): OSC Off/On, Port, „Listen on“ (Tooltip warnt vor dem Netz),
      „Allow from“ (IPv4, mit Komma; leer = „this computer only“);
      jede Änderung wirkt sofort, ein Fehler (Port belegt) steht in der
      Statuszeile und setzt die Felder auf den alten Stand zurück; die
      Zeile darunter zeigt „Listening on …“ mit Zahl der Nachrichten und der
      letzten Adresse (ok, ignored oder Fehlertext), bei verworfenen
      Paketen „N dropped (last from …)“
- [ ] Monitor-Strip: neben jedem Quellen-Fader und dem Mix-Fader schmale
      Pegelbalken des Geräts (`interfaceMeters`, ~12/s, nur bei offenem Strip):
      Fläche = RMS, Strich = Spitze (fällt langsam ab, rot nach 0 dBFS);
      Tooltip zeigt beide Werte
- [ ] Monitor-Strip mit Projekt: Stand sagt "saved with this song" (grün),
      sonst "not saved …"/"device differs …" mit "Save with song" bzw.
      "Keep device" und "Load song mix"; ohne Interface steht der Hinweis
      zum Einstecken, nach dem Einstecken kommt der Song-Mix von selbst
- [ ] Monitor-Strip: Gain- und Level-Regler sagen im Tooltip, dass sie Geräte-
      Einstellungen sind (auch der Drehregler vorn) und nicht mit dem Song
      gespeichert werden — der Projektstand zählt nur Matrix und Fader
- [ ] Track-Header: Hauptspur-Klick wählt das Ziel für Inserts und neue
      MIDI-Clips des eigenen Typs (Markierung) und den Fokus, Lane-Header-
      Klick hört (`laneActive`) — beide Wege bleiben getrennt; Klicks in die
      Spurfläche stellen nichts um
- [ ] Comp-Leiste: Ziehen markiert den Bereich und legt ihn per `compLane`
      in den Comp (der danach spielt), Toast mit Undo; Klicks darin ändern
      nichts; Lane-Header-Klick auf einen Take hört ihn ganz, auf den Comp
      den Comp (`laneActive`); eine Ablehnung (gefrorene Spur) als Toast
- [ ] Arm-Schalter: Klick = `armTrack` an/aus (Default-Gerät des Typs);
      Dropdown wählt das Gerät (`armTrack` input); nach dem Record bleibt
      die Spur geärmt
- [ ] Record: ein Klick nimmt alle geärmten Spuren parallel auf (Audio +
      MIDI, je Spur ein Take), die Meldung nennt Spuren und Typen; ohne
      geärmte Spur armt Record/Monitor die fokussierte Spur; Monitor hört
      alle geärmten Spuren (`monitor armed`) und ist nach einem Take wieder
      im vorherigen Zustand
- [ ] Favoriten-Sterne: Klick togglet (favoriteClip/favoriteSection), der
      Zustand kommt zurück nach dem Refresh
- [ ] "+ Audio" und "+ MIDI" am Ende der Trackliste erreichbar — der neue
      Track wird Ziel und Fokus seines Typs; "+ MIDI" öffnet die
      Instrument-Auswahl (Built-ins zuerst, gescannte danach, "No instrument
      (silent)"); eine stumme MIDI-Spur zeigt im Header "silent — pick an
      instrument" (Klick = dieselbe Auswahl, `pickInstrument`)
- [ ] Jeder Node-Zustand hat einen nächsten Schritt: frisch (jede Aktion,
      die erste gibt die Sorte), acted (Branch/Reset/Terminate, zweiter Take = erneut Record oder
      Insert im Track-Panel), terminated (Activate)
- [ ] Song-Baum flach: eingerückt nur an echten Verzweigungen, das
      Neueste oben; die Hauptlinie folgt dem aktuellen Node (sonst dem
      längsten Zweig), Nebenzweige stehen eingerückt über ihrem Abzweig und
      klappen über dessen Pfeil ein; auch bei 80+ Nodes in einer Kette kein
      waagrechtes Scrollen in der Leiste, Namen nicht von unsichtbaren
      Knöpfen gekürzt (Knöpfe liegen beim Hover über der Zeile); nach
      einem Node-Wechsel ist der aktuelle Node im Blick
- [ ] Node-Knöpfe ohne Hover: am aktuellen Node stehen sie immer da (✎
      bei Nodes ohne Notiz); jeder andere Node zeigt rechts „⋯“ (24 px) —
      Klick öffnet seine Knöpfe links daneben (Notiz, Fork, Bounce, Stern,
      Terminate …), sie bleiben offen bis zum Klick daneben; Hover zeigt
      sie wie bisher, „⋯“ bleibt dabei klickbar; alle Knöpfe ≥ 24 px
      (geprüft in `fehler_check.js`, Checks „K“)
- [ ] Node-Notiz: steht als zweite Zeile unter dem Node (bis 3 Zeilen,
      Tooltip mit allem und Zeitpunkt); Klick bearbeitet (Ctrl/Cmd+Enter
      oder Wegklicken speichert, Esc verwirft, leer löscht); ✎ in der
      Node-Leiste nur bei Nodes ohne Notiz; Umlaute bleiben erhalten
- [ ] Der Weg zurück existiert: nach jedem destruktiven Schritt ist der
      Zustand wieder herstellbar; was keinen Weg zurück hat, bewacht der
      Guard ("Compact…" zeigt vor dem Lauf die Vorschau und fragt nach);
      Section löschen (Papierkorb am Section-Tag, zweiter Klick) nennt Undo
      und die an den Kanten abgeschnittenen Clips im Toast
- [ ] Statuszeile unten (`#statusbar`, eigene Zeile, 28 px): verdeckt nichts
      (auch nicht in 1366×768); die letzte Meldung bleibt stehen und wird
      nach 3,2 s blass (Fehler rot, nach 7 s blass); lange Texte enden mit …
      und stehen ganz im Tooltip; ein Knopf dahinter (Undo, Redo, Back to …)
      verschwindet nach 6,5 s; rechts laufende Hintergrund-Jobs (⟳ …)
- [ ] Abgelehnte Aktionen zeigen eine verständliche Meldung (`say`, Statuszeile) —
      nichts passiert stillschweigend; Datei-Inputs haben ihren
      `onchange`-Handler (Insert-Button führt bis zum Clip)
- [ ] Record/Insert/Piano-Roll ohne gewählte Section nehmen die Section
      unter dem Playhead (`targetSection`) — kein "select first"-Fehler
- [ ] Datei-Drop auf die Timeline: Spur unter dem Zeiger (Lane → Gruppe,
      falscher Typ oder freie Fläche → neue Spur), Section und Offset an der
      Zeit; Drop-Marker mit Label zeigt das Ziel vor dem Loslassen
- [ ] Lineal: Klick/Ziehen setzt den Playhead (`play from`), läuft der
      Transport, springt er; Tastatur Space/L/R/Home/+/-/Entf/Esc greift
      nicht in Eingabefeldern
- [ ] Zoom (− / + in der Ecke, Tasten +/-) hält den Playhead bzw. Cursor an
      seiner Stelle, wenn er zu sehen ist, sonst die Mitte der sichtbaren
      Spuren; am Song-Anfang bleibt 0:00 links stehen; Ctrl+Mausrad hält die
      Stelle unter der Maus
- [ ] Taste R = Record-Button (alle geärmten Spuren), in jedem Node
- [ ] Routing-Reiter, Complex: Routing-Matrix (`state.routing`) — Zeilen = Ausgänge
      (Spuren, dann Module), Spalten = Eingänge (Module, dann Main); Klick
      in eine Zelle = `connect`/`disconnect`, verbundene Zellen gefüllt in
      der Farbe des Signals (Violett MIDI, Cyan Audio); die Matrix scrollt in
      sich (füllt das Dock, beide Scrollbars im Bild), Kopfzeile „to“ oben
      und Spalte „from“ links bleiben beim Scrollen stehen; der Scroll-Stand
      übersteht das Neuzeichnen; der Hinweis darunter ist eine Zeile (ganz
      im Tooltip)
- [ ] Ungültige Zellen (Typ-Regel, Schleife) sind gedimmt, nennen den Grund
      aus `matrix[].cells[].reason` als Tooltip und beim Klick als Toast —
      die Regeln stehen nur im Backend, das UI dupliziert sie nicht
- [ ] "+ Add plugin…" (Routing-Reiter oben) scannt einmal (Built-ins sofort, nach
      Klasse gruppiert) und legt ein unverdrahtetes Modul an; "+" je
      Spur-Zeile hängt es ans Ende der Kette der Spur (`addPlugin` track/at)
- [ ] Modul-Spalte: Klasse als Badge (MIDI FX / Instr / FX), Name-Klick
      öffnet die Parameter (Regler rufen `setParameter {plugin}`), × entfernt
      das Modul mit Bestätigung am Button (`removePlugin`)
- [ ] Clip-Leiste (am gewählten Clip): ⧉ kopiert direkt dahinter (auch
      Ctrl/Cmd+D am gewählten Clip), ✂ erscheint nur, wenn Cursor bzw.
      Playhead im Clip steht, und teilt dort (auch S); nach dem Teilen ist die
      rechte Hälfte gewählt; Kopien laufen auch über Section-Grenzen
- [ ] Clip-Leiste bei schmalem Clip (oder kleinem Zoom): passt sie nicht in
      den Clip samt 30 px Körper zum Greifen (und Platz für „+N“), steht sie
      rechts neben ihm über dem Nachbarn (`.beside`) — sie deckt nie den
      Körper (ein 2-s-Clip bei 60 px/s bleibt verschiebbar) — kein Knopf
      abgeschnitten
- [ ] Audio-Clip: ♪ transkribiert den Clip nach MIDI (neue Spur, gleiche
      Stelle, Toast mit Notenzahl und Hinweis aufs Instrument); ♪♪ als
      gespielte Akkorde (Toast mit Anschlägen, Stimmen, Lage);
      Referenz, Menü Make: Auswahl melody/bass/all notes/played chords + "To
      MIDI" legt die Noten der Quelle im Referenz-Bereich in die gewählte
      Section (ohne Referenz gesperrt); "Notes only" zeigt sie nur im
      Analyse-Panel (Zahl, Lage, Notennamen, Tooltip mit Zeiten), dort
      „Add as clip“
- [ ] Audio-Clip ⧈ und Referenz Make → "Stems to tracks" (im Bereich): Statuszeile zählt den
      Fortschritt hoch ("separating … N%"), am Ende vier neue Spuren
      (drums, bass, other, vocals) an derselben Stelle bzw. in der gewählten
      Section; Fehler (Modell fehlt, Abbruch) als Fehler-Toast
- [ ] Clip-Leiste: ± öffnet das Clip-Popover — Audio: Gain dB, Transpose,
      Speed, Follow tempo (zeigt die aktuellen Werte; Toast nennt das
      gemessene Quelltempo), Fade in/out, Kurve, Reverse, Normalize
      (—/Peak/RMS; Target dB wird nach Modus vorbelegt −1/−20 und erst mit
      Modus frei, Gain dB ist dann gesperrt; Toast nennt gemessen → Ziel und
      den neuen Gain), Crossfade (Häkchen = Flag dieses Clips, neue und
      geladene Clips an), Kurve „auto (convex in crossfades)“ bis eine Kurve
      gewählt wird (zeigt die gesetzte; Apply schickt sie nur bei Änderung);
      Loop (Häkchen = loopClip über das, was der Clip zeigt; aus = ein
      Durchgang) mit Loop passes (zeigt die aktuellen Durchgänge), Warp
      (Auswahl zeigt die Marker-Zahl; „to the beats (detect)“ = warpClip
      detect, „clear“ nur bei gewarptem Clip; Toast nennt Marker und
      Quelltempo), Clip FX (Auswahl zeigt die Zahl der Schritte; „through
      <Modul>“ je Audio-Effekt-Modul des Projekts = processClip mit FX tail s
      als Ausklang, „back to the original“ nur bei gerechnetem Clip; Toast
      nennt Modul und Renderzeit bzw. „original back“);
      MIDI: Volume dB
      (nur leiser), Transpose; Apply/Enter
      schickt nur Geändertes, Esc/Klick daneben schließt; Fehler als Toast
- [ ] Clip-Popover oben (Audio und MIDI): Start s (= `moveClip`), Length s
      (= `trimClip` rechte Kante), Copy at s (leer = keine Kopie, sonst
      `duplicateClip`); Felder zeigen die Lage auf 1/1000 s, nur
      Geändertes geht raus; jedes nimmt Sekunden oder Takt.Schlag — „bar
      3.2“ (Takt des Songs, jede Section zählt ihre Takte in ihrem Takt) oder
      „Verse 3.2“ (in der Section, wie das Label beim Ziehen); Length nimmt
      einen Takt als Ende des Clips; Copy at mit Takt schickt `section` +
      `startBeat` (das Backend rechnet); Unlesbares: Meldung „seconds or bar
      3.2“, nichts geht raus, Popover bleibt; Tooltips nennen die Lage als
      „bar 7.1 = Verse 3.1“
- [ ] Clip-Popover bleibt im Bild: unter dem ±, sonst darüber, sonst so weit
      oben wie nötig; höher als das Fenster scrollt es in sich — jedes Feld
      (auch „Fade out s“, „Clip FX“) ist in 1366×768 erreichbar
- [ ] Geloopter Audio-Clip: ⟲ vor dem Namen, gestrichelte Linien, wo der
      Loop von vorn beginnt, Waveform wiederholt sich; Tooltip nennt den
      Bereich der Datei und den Offset. Gewarpter Clip: kurze Striche unten an
      den Markern (auf hellem und dunklem Clip-Grund sichtbar; unten, weil
      oben rechts die Clip-Leiste liegt), Tooltip nennt Marker-Zahl und
      Quelltempo. Gerechneter Clip (processClip): ƒx vor dem Namen,
      gestrichelter Rand, mit tail um den Ausklang länger (Waveform zeigt die
      Hall-Fahne); Tooltip nennt die Schritte (Module, tail) und die
      Originaldatei
- [ ] Clip mit fehlender Datei (state `missing`): ⚠ vor dem Namen, roter
      gestrichelter Rand, ohne Waveform; Tooltip nennt die Datei und den Weg
      zurück (Datei zurücklegen oder Clip löschen); Export/Render zeigt den
      Fehler als Toast
- [ ] Warp-Marker ziehen: Hover über einem Strich = Akzentfarbe, ew-resize;
      Ziehen zeigt eine Linie über die Clip-Höhe und „beat N“, rastet auf
      Sechzehntel des Song-Tempos (Shift = frei), bleibt zwischen den
      Nachbarn; erst das Loslassen schickt einen `warpClip` (ein Undo-Schritt),
      Toast nennt Beat, Marker-Zahl und Quelltempo; zu weit gezogen (Tempo
      zu einem Nachbarn außerhalb 0,25..4x) hält der Marker an der Grenze
      (`warpClip clamp`, Backend), Toast „warp marker held at beat … (asked
      …)“; anderer Regelverstoß = Fehler-Toast, der Strich springt zurück
- [ ] Ersten Warp-Marker (Beat > 0) nach rechts ziehen: das UI heftet den
      Clip-Anfang als Marker auf Beat 0 dazu — was am Clip-Start spielt,
      bleibt dort, der Bereich davor wird gedehnt (kein Fehler-Toast); ist
      er der einzige Marker, kommt einer am Clip-Ende dazu. Quelltempo und
      Clip-Länge bleiben, ab dem nächsten Marker bzw. Clip-Ende bleibt alles
      an seiner Stelle; Toast „(clip start pinned on beat 0[, clip end on
      beat N])“ mit Undo, ein Undo-Schritt
- [ ] Doppelklick in einen Audio-Clip (nicht auf Clip-Leiste, Loop-Leiste,
      Strich) setzt einen Warp-Marker auf die nächste Sechzehntel (Shift =
      frei) — er heftet, was dort spielt, der Klang bleibt; der erste Marker
      macht den Clip tempo-folgend (Toast sagt es, mit Undo). Doppelklick auf
      einen Strich löscht den Marker (Toast mit Undo; der letzte = clear).
      Ein Klick auf Strich oder Loop-Leiste wählt den Clip nicht
- [ ] Warp-Marker setzen, ziehen, löschen klingt sofort, auch beim
      Abspielen (Echtzeit-Warp seit 2026-10-31): keine Ladeleiste, kein
      gestreifter Clip; die Wiedergabe läuft ohne Aussetzer mit den neuen
      Markern weiter; es entsteht keine Datei in `media/`
- [ ] Geloopter Audio-Clip: Loop-Leiste unten (über der Comp-Leiste in
      Take-Lanes, über den Warp-Strichen); Griffe an den Neustarts = Loop-Länge
      (Start bleibt), Leiste ziehen = Offset (Durchgänge gleiten mit, Linien
      folgen), Ctrl/Cmd+Ziehen = Loop-Start (Länge bleibt, der Inhalt folgt der
      Maus); Label zeigt den Wert (Beats bei tempo-folgenden Clips, sonst
      Sekunden), rastet auf Sechzehntel (Shift = frei); ein `loopClip` beim
      Loslassen, Clip-Länge bleibt; die Clip-Kanten bleiben in der Leiste
      Trim-Griffe; Fehler (Loop außerhalb der Datei) als Toast
- [ ] Crossfade: überlappen sich zwei Audio-Clips einer Lane (der zweite
      beginnt im ersten und endet dahinter), liegt über der Überlappung eine
      Marke — gestrichelt gelb = beide spielen voll, dunkel mit X = sie
      blenden (Standard: eine neue Überlappung und ein geladenes älteres
      Projekt zeigen X); ⋈ unten in der Marke schaltet beide Clips um (Toast nennt
      beide), Tooltip nennt die Länge; die Marke folgt Verschieben/Trimmen
      und lässt die Startkante des hinteren Clips zum Trimmen frei (die
      ersten 8 px); der rechte Rand der Marke ist ein Griff für das Ende des
      vorderen Clips, das unter dem hinteren liegt (Hover: Linie in
      Akzentfarbe, ew-resize; Ziehen = `trimClip` am vorderen Clip, die
      Blenden folgen, Toast „trimmed“); bei sehr kleiner Überlappung
      (≤ 5 px) nur nach dem Hineinzoomen; der Name des hinteren Clips
      beginnt hinter der Überlappung (auch bei kleinem Zoom nicht unter ⋈)
- [ ] Section-Tag: ⧉ kopiert die Section samt Clip-Teilen, Automation und
      Markern dahinter, Toast nennt Namen, Clip-Zahl und die neuen Marker
      („Drop 2“), spätere Sections rücken sichtbar nach rechts
- [ ] Clips über Section-Grenzen: ein Clip zeichnet sich durchgehend über
      die Grenze (Section-Linie sichtbar dahinter); Section verschieben,
      kopieren, löschen oder an der Kante kürzen teilt bzw. schneidet ihn
      dort, verlängern teilt ihn an der Einfügestelle — nach dem Refresh
      stehen die Teile mit eigenen Namen ("long6 2")
- [ ] Marker: Doppelklick ins Lineal oder Taste M (am Cursor bzw.
      Playhead) setzt einen Marker („Marker N“, Toast mit Zeit und Takt);
      er steht als Fähnchen im Lineal, gestrichelte Linie über alle Spuren;
      Klick setzt den Cursor dorthin, Ziehen verschiebt (beim Loslassen,
      Fehler = Toast und Rücksprung), Doppelklick benennt um (Enter/Esc),
      × bei Hover löscht mit Undo-Toast; Tooltip nennt Zeit, Takt, Section;
      Section einfügen/verschieben/löschen nimmt die Marker sichtbar mit,
      ⧉ an der Section kopiert sie mit Suffix („Drop 2“)
- [ ] Auto-Section: reicht Material hinter die letzte Section (Insert,
      Drag, Trim, Kopie, Aufnahme beim Stopp), erscheint hinten eine neue
      Section ("Section N", Tempo geerbt) und der Toast nennt sie
      ("new section … the material runs past the last section"); zieht sich
      das Material im selben Node zurück (Trim/Löschen/Verschieben), schrumpft
      sie oder fällt weg und der Toast sagt es ("auto section … removed")
- [ ] Insert/Piano-Roll-Clip, der eine neue Lane anlegt und die bisher
      spielende stummschaltet: der Toast zeigt die Warnung (rot) mit den
      stummen Lanes — nichts wird still; der Insert ist ein Take (`newLane:
      true`), ein Piano-Roll-Clip über einem bestehenden gibt einen
      Fehler-Toast mit dem Clip im Weg
- [ ] Node mit Auto-Section: Badge „auto“ im Song-Baum neben der Sorte,
      Tooltip nennt die Section(s); von Hand angelegte Sections ohne Badge
- [ ] Record-Toast beim Start sagt, dass die Aufnahme bis zum Stopp läuft
      (kein Auto-Stop am Section-Ende); der Stop-Button bleibt erreichbar
- [ ] Piano-Roll: Raster = Sechzehntel im Tempo der Section (Beat-Linien
      kräftiger, Taktlinien nach der Taktart der Section — 3/4 alle 12
      Spalten); neue Noten sind eine Achtel lang, Ziehen rastet auf
      Sechzehntel
- [ ] Piano-Roll beim Bearbeiten eines Clips: Controller-Spur unter der
      Rolle, scrollt waagerecht mit; die Auswahl links zeigt Mod/Expr/Sus/
      Bend und jeden Controller des Clips (• = im Clip vorhanden); Ziehen
      zeichnet je Sechzehntel einen Wert (Pitch-Bend um die Mittellinie),
      Alt+Ziehen löscht, Fehler = Toast; "Export .mid" neben "Done"
      schreibt den Clip nach `exports/`
- [ ] Routing-Reiter: "+ Before Main…" bietet nur Audio-
      Effekte (Limiter, Compressor …) und hängt sie vor Main — Matrix/Ketten
      zeigen danach alle Wege durch das Modul
- [ ] Eine Plugin-Karte zur Zeit (`.onecard`), rechts neben Kette/Matrix/
      Pult; Klick auf ein anderes Modul ersetzt sie, derselbe Klick oder ×
      schließt sie; Karte und Ketten scrollen je für sich, Scroll-Stand
      bleibt beim Neuzeichnen
- [ ] Plugin-Karte: Parameter-Suche („find parameter“, `paramQuery`, alle
      Wörter in einem Namen, `a|b` = oder) mit Zahl „80 of 776 parameters“;
      „more …“ lädt weitere (Limit); Fokus bleibt beim Tippen im Feld; der
      Hinweis `note` (keine Übereinstimmung mit allen Wörtern) steht darunter
- [ ] Plugin-Karte: Zahlenfeld je Parameter (rechts, zeigt den Klartext):
      Zahl + Enter = Wert in der angezeigten Einheit (`real`), ohne Einheit
      0–1 (`value`), Wort = `text` („Saw“, „On“); Esc verwirft; Fehler in
      der Statuszeile, das Feld zeigt wieder den alten Wert
- [ ] Plugin-Karte: Regler zeigen den Klartext-Wert ("440Hz", Tooltip mit
      Realwert und Bereich); 4OSC zeigt darunter seine Einstellungen
      (Wave/Unison je Oszillator, LFO, Filter Type/Slope, Effekte an,
      Voice Mode, Voices) als Auswahl/Schalter/Zahl, Fehler als Toast
- [ ] Sampler/Drum Kit: Klick auf den Modulnamen öffnet die Karte mit der
      Belegung (Taste, Name, ▶ vorhören, × entfernen) und der Suche
      (Enter oder "Find"; Treffer mit Ordner, ▶, "+" auf die Taste im
      Zahlenfeld — Default die nächste freie); "no samples — search below"
      bei leerem Sampler; Hinweis-Toast, solange Bibliotheken indizieren
- [ ] Sampler-Karte, Suche "Sound": Zeile der Analyse zeigt "N of M samples
      analyzed · K without analysis"; "No analysis…" fragt nach dem Ordner
      und bestätigt das Löschen, danach steht ein Chip "no analysis: <Ordner>
      ×" (Tooltip: ganzer Pfad); Klick auf den Chip gibt nach Rückfrage frei;
      Fehler (relativer Pfad, schon gesperrt) als Toast; die Zeile bricht
      bei schmaler Karte um, statt den Text zu quetschen
- [ ] Sampler-Karte: ⚙ je Sample klappt Velocity-Layer (vel – ), Round-Robin-
      Gruppe (RR), Choke-Gruppe und Attack/Release in ms auf; Änderung per
      Enter/Verlassen, Fehler und Warnungen (Release an One-Shot) als Toast;
      gesetzte Werte stehen kurz in der Zeile (v64–127 rr1 ch1 A20 R300);
      Shift+"+" legt einen Treffer zusätzlich auf die Taste (Layer/Round
      Robin), "+" ersetzt wie bisher
- [ ] MIDI-Clip: Button "Q" quantisiert aufs zuletzt gewählte Raster
      (Default 1/16, volle Stärke; Tooltip zeigt Raster/Stärke/Swing);
      Shift+Klick fragt "Raster Stärke Swing" ("1/8T 0.7 0"); Toast mit Zahl
      verschobener Noten; Kopien desselben Patterns ziehen mit; Ctrl+Z
- [ ] Plugin-Karte: unter dem Titel Kategorie · Zweck aus dem Kernset
      (pluginInfo); mit Kurznotiz ein ⓘ, der Tooltip zeigt sie
- [ ] Plugin-Karte eines Plugins, das in Rezepten vorkommt: Zeile
      "Recipes: <Titel> · …" (pluginInfo `recipes`, verworfene nicht),
      Tooltip je Rezept mit Art, Status, Sternen und Id
- [ ] Plugin-Karte mit Steckbrief (z. B. 4OSC): Tooltip am Reglernamen zeigt
      die gemessene Wirkung und "needs: …"; Regler, die erst mit einem
      Schalter wirken, sind gepunktet unterstrichen (Pulse Width 2 → Wave 2)
- [ ] Plugin-Karte: Preset-Zeile oben (Suche nach Kategorie/Name, Auswahl
      "Kategorie / Name", Tooltip mit Anzahl); "Load" lädt (Toast mit Zahl
      geänderter Parameter, Regler und Einstellungen aktualisiert, Ctrl+Z
      nimmt es zurück); "Save…" fragt einen Namen ("Kategorie/Name" geht) und
      legt ein unidaw-Preset an, die Liste zeigt es danach; Surge XT zeigt die
      Werkspatches, Diva die .h2p-Presets, 4OSC/testgain nur eigene
- [ ] Preset-Zeile mit Katalog (MDA JX10): Suche nach Klang ("bright pad",
      "bass hard attack") findet passende Presets, die Liste zeigt "· Klasse",
      Tooltip am Eintrag die Klangworte; ohne Katalog (4OSC) wie bisher
- [ ] Preset-Zeile, Taste ≈ (Klangsuche): an = hervorgehoben, Platzhalter
      "describe the sound …"; eine Beschreibung ("warm e-piano") ordnet die
      Liste nach Ähnlichkeit (" · 0.31" am Eintrag), erkundete Klänge stehen
      mit ✦ und Tooltip "explored from <Preset>"; "Load" lädt auch sie
      (Toast mit geänderten Parametern); fehlen Vektoren, sagt ein Toast es;
      ≈ aus = wieder Namens-/Wortsuche
- [ ] Track-Header zeigt die Ziele der Spur ("→ 4OSC · Main", `routes`)
      oder "not routed — silent"
- [ ] Routing-Umschalter Simple/Complex (`.rmodes`, gemerkt in
      localStorage), der Mixer ist ein eigener Reiter — alle Ansichten
      zeigen denselben Graphen; „+ Add plugin…“ und „+ Before Main…“ oben
      neben dem Umschalter
- [ ] Simple: je Spur eine Kettenzeile (Slots → Main), Slots per Drag
      umsortieren (`setChain`), "+" hängt ein neues oder verlinktes Plugin
      an (nur passende Link-Kandidaten), × entfernt mit Bestätigung; Wet-
      Regler je Slot; "+ send" nur mit erlaubten Zielen (Matrix-Grund leer),
      Send-Chip mit Pegel und ×; Main gestrichen = Kette erreicht Main nicht;
      ein Send hinter dem letzten Modul (`postSends`) macht die Zeile nicht
      complex: Chip "post → Ziel" hinter Main mit Pegel und ×, Slots bleiben
      ziehbar, nach dem Umsortieren hängt der Send am neuen letzten Slot
- [ ] Simple: Zeilen mit Complex-Teilen gestrichelt, nur lesbar, nennen den
      Grund und bieten "Edit in Complex"; Busse (Module außerhalb der Ketten)
      als eigene Zeilen mit Main-Schalter
- [ ] Simple: eine Kette, die in die Master-Kette läuft, zeigt am Ende den
      Namen des ersten Master-Moduls (grün, nicht gestrichen), ebenso ein
      Bus, der in die Master-Kette läuft (kein Main-Schalter); die Master-
      Module stehen als letzte Bus-Zeilen in ihrer Reihenfolge mit Marke
      MASTER und "→ nächstes Modul" (letztes: Main-Schalter); "+ before"
      vor jedem Master-Modul bietet nur Audio-Effekte und setzt das neue
      Modul davor (`addPlugin {at: master, before}`), danach steht es in der
      Reihe davor, Fehler als Toast
- [ ] Mixer (Reiter Mixer, `renderMixer`): je Gruppenspur ein
      Kanalzug (Inserts → Sends → Pan → Fader mit Pegel → S/M → Ausgang),
      danach Busse (Module außerhalb der Ketten) und Main; bei vielen
      Spuren scrollt die Leiste waagrecht, nichts bricht um
- [ ] Mixer: Fader und Pan schicken `trackMix` beim Ziehen, beim
      Loslassen kommt der Stand (Refresh); Doppelklick = 0 dB bzw. Mitte;
      ganz unten = −∞; der Tooltip nennt den Ort des Faders (`state.fader`:
      nach der eigenen Kette, am Instrument, geteilt mit …); MIDI ohne
      Instrument: Fader und Pan gesperrt, "no fader"; automatisiert =
      gestrichelt wie die anderen Regler
- [ ] Mixer: der Pegel steht neben dem Fader, läuft beim Abspielen mit dem
      Header-Meter derselben Spur (am Ort des Faders gemessen, auch bei
      MIDI) und fällt beim Stopp zurück; Klick = Analyse der Spur, Main-Pegel
      im Main-Zug
- [ ] Transport: LUFS-Anzeige neben dem Main-Pegel ("M … S … I … LUFS"):
      beim Abspielen laufen M (400 ms) und S (3 s) mit, I seit Play-Start,
      erst "–" bis Messwerte da sind; nach dem Stopp gedimmt, nur I bleibt;
      Klick = Analyse des Songs; unter 1250 px nur I, unter 1100 px weg
      (Knöpfe rechts bleiben sichtbar)
- [ ] Mixer: Sends mit Pegel (`setEdge`) und pre/post/midi-Marke (pre =
      vor dem Fader, also vor der Kette; post = hinter dem letzten Modul,
      dort sitzt der Fader — kein "complex"), "+ send" nur mit erlaubten Zielen;
      Sidechain als "key → Modul" am Quell-Zug, "key ← Spur" am Bus;
      gefrorene Spur violett mit "frozen" über den Inserts, Fader/Pan/Sends
      bleiben bedienbar, die Schneeflocke taut auf; Lanes: Auswahl der
      spielenden Take (`laneActive`) nur bei mehr als einer Lane
- [ ] Complex: verbundene Zelle zeigt ihren Pegel als Füllung, vertikal
      ziehen = `setEdge`, Doppelklick = 100 %; Schleifen-Zellen (orange
      gestrichelt) legen per Klick eine Feedback-Kante an, Shift-Klick
      togglet Feedback auf jeder erlaubten Zelle; Feedback-Karte rechts mit
      Pegel und Verzögerung (Minimum ein Block)
- [ ] Sidechain: Strg-/Alt-Klick auf eine Zelle legt den Schlüssel in den
      zweiten Eingang (Zelle violett gestrichelt mit "SC") und nimmt ihn
      wieder weg; ohne Sidechain-Eingang nennt der Toast den Grund
      (`sidechainReason`); Sidechain-Karte rechts (auch im Simple-Mode)
      mit × je Schlüssel
- [ ] Automation: "A" im Track-Header klappt die Lane unter der
      Spur auf (orange, wenn die Spur Automation hat); Auswahl gruppiert
      "This track" / "Plugins" / "Other connections", automatisierte Ziele
      mit ●; Klick setzt einen Punkt, Ziehen verschiebt (zwischen den
      Nachbarn), Doppelklick löscht, Shift-Klick schaltet Stufe/Rampe
      (gefüllter Punkt = Stufe); × löscht die Kurve mit Bestätigung am
      Button; ohne Punkte zeigt die Lane den Basiswert gestrichelt plus Hinweis
- [ ] LFO in der Lane: ∿ legt einen LFO an das gewählte Ziel (blau = an,
      "A" im Track-Header orange auch bei nur einem LFO, Ziele mit LFO im
      Menü mit ∿); offen eine Zeile Welle / Rate (Notenwerte und Hz) / Tiefe,
      Tooltip der Tiefe nennt den Ausschlag; die Lane zeigt die Welle blau
      gestrichelt (auch ohne Punkte); ∿ ein zweites Mal entfernt ihn mit
      Bestätigung am Button
- [ ] Step-Modifier in der ∿-Zeile: Typ-Auswahl lfo/step (Wechsel auf
      step legt Stufen 1 0 −1 0 an); bei step statt der Welle ein schmales
      Textfeld mit den Stufen (−1..1, Leerzeichen/Komma), Enter übernimmt,
      Esc setzt zurück, ungültige Liste = Fehler-Toast und alte Stufen; Rate
      = Länge eines Schritts (Tooltip sagt es); die Lane zeigt die Stufen
      blau gestrichelt im Takt
- [ ] Gruppen-Automation: "A" in der Gruppen-Kopfzeile öffnet die
      Lane unter der Kopfzeile (orange, wenn die Gruppe Automation/Modifier
      hat); Auswahl "This group" mit Volume/Pan der Gruppe; der Regler in
      der Lane stellt den Gruppen-Fader (`groupMix`); ∿ auch hier
- [ ] Volume/Pan der Spur als Regler in der Lane (`trackMix`), Doppelklick
      = 0; automatisierte Regler (Lane, Wet, Send, Feedback, Matrix-Zelle mit
      "A") gestrichelt markiert, Tooltip nennt die Lane
- [ ] Kleine Regler (`input.rwet`) haben eine sichtbare Spur — die globale
      `input[type=range]`-Regel ist spezifischer als eine reine Klasse
- [ ] Pegel beim Abspielen: Main-Meter neben der Zeit, ein Meter je
      Spur-Header (grün → gelb → rot über die ganze Breite); nach dem Stop
      fallen alle auf 0; Klick auf ein Meter analysiert Song bzw. Spur
      (Ergebnis im Analyse-Panel: LUFS · LRA · dBTP · Peak · PLR), "Analyze"
      im Export-Popover misst den gewählten Bereich
- [ ] Analyse-Panel (rechte Spalte, unter der Referenz bis unten): jedes
      Ergebnis (Analyze, Compare, Tempo, Key, Structure, Voice, Drums,
      Match, Tune, Repeats, Layers, Cross-check, Classes, Split, Describe …)
      ist ein Eintrag, das neueste oben und markiert, mit Nummer, Ziel und
      Bereich; bleibt über Reiter und Node-Wechsel stehen; × nimmt
      einen weg, der Papierkorb im Kopf alle; ein Projektwechsel leert die
      Liste; die Statuszeile sagt nur „… result in the analysis panel“
- [ ] Analyse auf Material aus einem gesperrten Ordner (sampleLibraries
      noAnalysis; Decision 2026-11-18-analyse-ops-auf-gesperrten-samples):
      ML-Ops (Voice, Audio→MIDI, Stems, Classes, Match/Tune) zeigen die
      Ablehnung mit Spur und Datei in der Statuszeile; Messungen (Tempo,
      Key, Structure, Drums, Compare) zeigen ihr Ergebnis und danach
      „licence: …“ rot in der Statuszeile
- [ ] Analyze im Panel-Kopf: Ziel (Selected track · Song · Reference source ·
      File…) × Bereich (Selected section · Whole · Ruler selection ·
      Reference range); Datei-Ziele nur Whole/Reference range, Song-Ziele
      nicht Reference range (gesperrte Optionen); ohne gewählte Spur bzw.
      Referenz eine Meldung statt nichts; „Selected section“ ohne Wahl =
      Section unter dem Playhead; File… fragt nach dem Pfad (Vorschlag:
      der letzte)
- [ ] Ohne Projekt: rechts neben „Start a song idea“ Referenz + Analyse;
      Load… legt die Datei unter `<config>/references/` ab; Tempo, Key,
      Structure, Describe (Reference), Repeats, Layers, Split und Analyze
      (Ziel Reference source/File…) laufen; was den eigenen Song braucht
      (Compare, Spur-Knöpfe, Classes, Cross-check, To MIDI, Stems to tracks,
      Compare stems, From section, Ziele Selected track/Song) ist gesperrt;
      ein danach geöffnetes Projekt ohne eigene Referenz übernimmt sie samt
      Bereich
- [ ] Dieselbe Datei noch einmal einfügen bzw. als Referenz laden (gleicher
      Name, gleicher Inhalt) nimmt die vorhandene in `media/` — kein „-2“;
      anderer Inhalt unter gleichem Namen bekommt „-2“
- [ ] Mitlaufen (Knopf neben Play/Loop, Taste F, im Browser gemerkt,
      Standard an): beim Abspielen steht der Playhead in der Bildmitte, die
      Timeline zieht gleichmäßig darunter durch; am Song-Anfang wandert er
      erst bis zur Mitte; ein Loop-Sprung springt mit; aus = die Ansicht
      bleibt stehen (frei scrollen); Toast nennt den Zustand
- [ ] Lineal: Shift+Ziehen wählt einen Zeitbereich (grünes Band), schaltet
      Analyze auf „Ruler selection“; Klick ins Lineal oder Esc nimmt die
      Auswahl weg; Zoom hält das Band an seiner Zeit
- [ ] Layout Variante B: rechte Spalte „Reference + Analysis“ (`#side`,
      368 px, unter 1500 px Breite 320 px und der Baum 240 px), oben die
      Referenz, darunter das Analyse-Panel bis unten; Knopf rechts oben
      (`#rightBtn`, hervorgehoben = offen) klappt sie ein wie `#sideBtn`
      den Baum (gemerkt); in 1366×768 passen alle Referenz-Knöpfe ohne
      Scrollen (Teil-Chips höchstens drei Zeilen, dann scrollen nur sie),
      Menüs öffnen im Bild; unter 900 px ist sie zu und der Knopf zeigt sie
      als Leiste
- [ ] Referenz oben in der rechten Spalte (`#refStrip`, in jedem Reiter
      sichtbar): Auswahl der Referenzen dieses Projekts (frühere
      bleiben in der Liste, dazu frühere Mitschnitte), "Load…" lädt eine
      Datei hoch (je Projekt gemerkt); ohne Referenz sind die Knöpfe
      gesperrt und der Chip sagt "no reference"
- [ ] Referenz-Bereich einmal setzen: Chip oben rechts ("Ref Bass Chorus 1
      98.16–142.43 s · 129.9 BPM", Tooltip mit Datei, Bereich, Raster) zeigt,
      womit jeder Referenz-Knopf rechnet; Teil-Chips aus "Structure" (ein
      Klick setzt Start/Ende, mit Raster die erste Eins auf den Teilanfang),
      "Whole" = ganze Quelle, "From section" = so lang wie die gewählte
      Section; Felder Range (Start–Ende s) und Grid (BPM, bar one at s),
      falsche Eingabe = Meldung und alter Wert; je Projekt im Browser
      gemerkt (gilt nur für dieselbe Referenz-Datei); kein Referenz-Knopf
      fragt mehr per Prompt nach dem Bereich
- [ ] Quelle Mix | Vocals | Bass | Drums | Other: Stems kommen aus dem
      Stem-Cache von unidaw (kein Upload); kursiv = in diesem Browser noch
      nicht benutzt — der Klick holt sie aus dem Cache (sofort, "stems of
      the reference from the cache") oder trennt die ganze Referenz einmal
      (Statuszeile zählt %, ⟳ rechts, Stems-Knöpfe orange); danach rechnen
      alle Knöpfe mit dem Stem; Compare mit einem Stem misst die gewählte
      Spur gegen ihn (ohne Spur eine Meldung)
- [ ] Spur-Knöpfe "Track <Name>: Voice · Drums · Match · Tune": der Name
      folgt dem Klick auf einen Spurkopf (ohne Neuzeichnen), "–" ohne Wahl
- [ ] Menüs Describe ▾ · Part ▾ · Make ▾ unter den Knöpfen (passen in
      1366×768 ohne Scrollen); Klick daneben oder Esc schließt; Describe mit
      Umschalter Reference | Track <Name>: Describe voice/timbre/drums der
      Quelle im Bereich bzw. der gewählten Spur in der gewählten Section
      (sonst der unter dem Playhead) → Eintrag im Analyse-Panel (Kennwerte,
      Takte in Monospace, Hinweise)
- [ ] Make → "Compare stems…": Dialog mit je eigener Spur einer Auswahl
      (— / Vocals / Bass / Drums / Other stem), aus den Namen geraten (Vox →
      Vocals, Drums/Kick → Drums, Bass → Bass, MIDI-Spuren sonst Other);
      Compare holt fehlende Stems (wie oben) und schreibt je Paar Rollen,
      Pegel-Differenz, Abweichungen und Hinweise ins Panel; Esc/Cancel schließt
- [ ] Compare (Referenz): misst die gewählte Section, sonst den Song, gegen
      den Referenz-Bereich (`referenceStart/End`); Tabelle
      Mix/Reference/Diff und die Bandbalken (orange = mehr im Mix,
      blau = weniger, ±12 dB) im Analyse-Panel, Hinweise darunter
- [ ] Vergleichstabelle zeigt auch PLR, Crest, Floor und Clipped peaks
      (Tooltip je Zeile erklärt den Wert; fehlende Werte als "–"); die
      Analyse per Klick aufs Meter nennt PLR und, falls vorhanden, die Zahl
      der geclippten Spitzen
- [ ] Referenz: "Tempo" (ohne Referenz gesperrt) misst den Bereich (ganz
      ohne Bereich) und zeigt BPM · Takt, Takt 1, Takte und Konfidenz
      (Tooltip: Kandidaten, Hinweise); ist im Grid noch kein BPM, setzt es
      BPM und erste Eins dort ein; "Use as reference grid" setzt sie immer;
      "Use for <Section>" setzt Tempo und Takt der gewählten Section (sonst
      der ersten) und der Tempo-Tag im Band zeigt es; ohne Puls Fehler-Meldung
      statt Eintrag
- [ ] Referenz: "Key" (ohne Referenz gesperrt) misst Quelle und Bereich mit
      dem Grid und zeigt Tonart, Konfidenz, die häufigsten Akkorde und die
      Akkordfolge als Chips mit Taktnummer (Tooltip: Zeit, Konfidenz; an
      der Tonart: Tonleiter und Alternativen)
- [ ] Referenz: "Structure" (ohne Referenz gesperrt) analysiert die ganze
      Quelle (mit dem Grid, falls gesetzt), macht die Teile zu Bereichs-Chips
      ("Chorus 2" …, je Referenz gemerkt), setzt ein fehlendes BPM und zeigt Takte ·
      Tempo · Tonart und die Teile als Band (Breite = Dauer, Farbe =
      Buchstabe, heller = mehr Energie; Tooltip: Zeit, Takt, Energie,
      Akkorde); "Build sections" hängt die Teile als Sections an den Song
      (Tempo-Tags und Takte sichtbar), ein Fehler im Batch erscheint als Toast
- [ ] Ladeanzeige: dauert eine Anfrage länger als 0,3 s, zeigt eine Leiste
      oben mittig, worauf gewartet wird (z. B. "switching node (loading
      plugins) — loading plugins 2/3 · Surge XT … 0.4 s"), mit Sekunden und
      Zahl weiterer wartender Anfragen, darunter ein Balken (Plugin i von n,
      sonst laufender Streifen); sie verschwindet, sobald alles beantwortet
      ist. Die Phasen kommen aus `GET /progress` (antwortet auch, während der
      Daemon lädt): saving / reading the node / loading plugins / connecting
      the routing / loading samples, beim ersten Play "preparing audio"
- [ ] Hält ein Agent den Daemon (langer Befehl, nicht von dieser Seite),
      zeigt die Leiste ihn mit "(not from this page)" — kein stilles Hängen
- [ ] Hintergrund: laufen Stem-Trennung oder Klang-Analyse, steht rechts
      in der Statuszeile "⟳ separating stems 42 % · analysing samples 120/800"; ohne
      Hintergrund-Arbeit ist die Anzeige weg
- [ ] Während der Daemon lädt, stapeln sich keine Anzeige-Polls (position,
      levels, inputLevel, interfaceMeters pausieren): Play direkt nach einem
      Node-Wechsel wartet nur auf den Wechsel, nicht auf Dutzende Polls
- [ ] Node-Knopf "Bounce" (Welle mit Pfeil, an jedem nicht beendeten Node):
      rendert den Node und legt darunter einen neuen Zweig an (Ladeanzeige
      "bouncing the node to audio"); der neue Node ist offen, zeigt eine
      Spur "Bounce", die Sections und die Marker, Badge "collect"; Shift+Klick nur die
      gewählte Section (mit ihren Markern, nach vorn gerückt; hat sie eine
      Tempo-Rampe, folgt die nächste Section als Ziel, der Toast nennt sie); Toast nennt Label und Nummer; Undo nimmt den Zweig
      wieder weg; Fehler (beendeter Node) als Toast
- [ ] Song-Baum "A/B": an = Leiste unten (▶/❚❚, Position, Zeit, loop,
      Knöpfe "1: #n" je vorbereitetem Node, ✕); Klick auf einen Node (auch
      den aktuellen) bereitet ihn vor (Ladeanzeige "rendering") und schaltet
      an derselben Stelle um, der hörbare Node ist im Baum umrandet; Tasten
      1–9 schalten, Leertaste spielt/pausiert; aus = Projekt unverändert
- [ ] Recipes-Reiter: Suchfeld (Text über Titel, Ziel, Tags,
      Plugins, Schritte; sucht beim Tippen und mit Enter), Art (any kind/
      sound/chain/workflow/mix target), Status (not rejected = Default,
      drafts, checked, rejected, all), Reload, "Save…"; die Zeile nennt
      "N of M found" (und die Zahl im Katalog); leerer Katalog zeigt den
      Hinweis, keine Treffer den Weg zurück ("clear the search or the
      filters")
- [ ] Je Rezept: ▸/▾ Titel (Tooltip Ziel/Id/Ablage), Status-Chip, fünf
      Sterne (Klick setzt 1–5, rateRecipe stars), Art und Schrittzahl,
      Plugin- und Tag-Chips (Klick filtert, aktiver Filter als Chip
      "tag: … ✕"/"plugin: … ✕" in der Leiste, Klick nimmt ihn weg),
      "applied N× · worked ok of used" (nur wenn benutzt), Player für die
      Hörprobe (wenn vorhanden), Approve/Reject/Apply; Approve verschiebt
      in den Katalog (Liste lädt neu), Reject nimmt es aus "not rejected"
- [ ] Klick auf den Titel klappt die Details auf (recipeInfo): Ziel,
      Notizen, Spuren, Zutaten (Plugin mit den Reglern, die vom Standard
      abweichen), Schritte (Op-Aufrufe in Mono, Tooltip mit allen
      Parametern; Text-Schritte mit "by hand"), Herkunft, Varianten,
      Warnungen, Review-Kommentar, letzte Rückmeldungen (✓/✗); darunter
      Notizfeld mit "Worked"/"Did not work" (rateRecipe used/ok/note) und
      "Comment" (leer = Hinweis-Toast); aufgeklappte bleiben nach dem
      Neuladen offen
- [ ] Apply: auf die gewählte Spur der Art, die das Rezept für seine erste
      Spur braucht (Audio- oder MIDI-Spurkopf geklickt; `recipeInfo` vorher),
      sonst neue Spuren; Toast nennt die Spur; legt
      einen Zweig an (bei Auto-Branch aus oder frischem Node im Node), der
      Toast nennt den Node und bietet "Back to #<Eltern-Node>" zum
      Vergleich; Shift+Klick = Automation ab der gewählten Section, sonst
      die unter dem Playhead; fehlender Platzhalter ($tempo …) = Abfrage
      des Werts, Abbruch = Fehler-Toast ohne Änderung; nur Text-Schritte =
      Fehler-Toast und die Schritte klappen auf; behaltener Fader und
      Schritte von Hand stehen im Toast
- [ ] Rezept-Knopf (Blatt-Icon) nur am aktuellen Node: Popover mit "Find
      a recipe to apply here…" (holt den Reiter Recipes,
      Suche fokussiert) und dem Speichern-Formular (Titel = Node-Name,
      Ziel, Art, Tags, From = was der Node brachte oder die gewählte Spur,
      nur die gewählte Section, bei workflow ein Schritte-Feld mit einer
      Zeile je Schritt); "Save…" im Strip öffnet dasselbe Formular, das
      Popover bleibt im Bild (öffnet nach oben, wenn unten kein Platz
      ist); Fehler (z. B. ohne Ziel) als Toast, das Popover bleibt offen;
      Erfolg nennt Plugins, Schritte und Warnungen, das neue Rezept steht
      aufgeklappt in der Liste; Enter speichert (nicht im Schritte-Feld),
      Esc und Klick daneben schließen
- [ ] Aktueller Node mit allen Knöpfen: der Name bleibt lesbar (mind.
      4,5 rem), die Knöpfe rutschen in eine zweite Zeile statt aus dem
      Baum zu laufen
- [ ] Referenz, Part ▾ "Repeats" (ohne Referenz gesperrt) sucht den
      Referenz-Bereich (ohne Bereich eine Meldung) im ganzen Song: die
      Fundstellen mit Ähnlichkeit und Pegel und die Hinweise im Panel;
      Ladeanzeige "looking for repeats …" während der Suche
- [ ] Referenz, Part ▾ "Layers" (ohne Referenz gesperrt) zerlegt den
      Referenz-Bereich (ohne Bereich eine Meldung); Shift+Klick fragt dazu
      nach einer freien Stelle (falsches Format = Meldung); je Schicht Stem, Rolle, Lage,
      Takte, Pegel ("loudest" bzw. dB), Attack und Klang, die Datei im
      Tooltip, die Schichten der freien Stelle abgesetzt ("bare place …"),
      die getrennten Schichten (split) wie die übrigen, darunter die
      Hinweise; Ladeanzeige "splitting the part …"
- [ ] Referenz, Part ▾ "Cross-check" (ohne Referenz gesperrt) nimmt Bereich
      und Grid (ohne Bereich bzw. BPM eine Meldung) und fragt nur nach dem
      eigenen Render (Vorschlag: der letzte); Shift+Klick fragt dazu nach
      einer freien Stelle; je Schicht Urteil, Pegel beider Seiten
      und Einsatz-Übereinstimmung, Dateien im Tooltip, darunter die Hinweise;
      Ladeanzeige "separating both …"
- [ ] Referenz, Part ▾ "Classes" (ohne Referenz gesperrt) ohne gewählte
      Section = Meldung "click a section first"; der Start in der Referenz
      ist der Bereichsanfang (ohne Bereich 0); Shift+Klick fragt nach den
      Klassen zum Weglassen; die Statuszeile zählt den Fortschritt hoch, danach im
      Panel Hinweise, je Spur ihre Klassen, je Klasse Pegel beider
      Seiten (gleiches Signal als Gruppe im Tooltip); Modell fehlt = Fehler-Toast
- [ ] Referenz, Part ▾ "Split" (immer aktiv) fragt nach der Datei (Vorschlag:
      die Quelle) und den Beispielen ("name=pfad; name=pfad"), falsches Format
      = Meldung, sonst je Teil Name, Pegel und Datei im Panel; Ladeanzeige "splitting
      with the example sounds …"
- [ ] Referenz: "Drums" wie "Voice" (gesperrt ohne Referenz, Meldung
      ohne gewählte Spur); der Eintrag zeigt Spur, Muster-Länge und die ersten
      Takte Kick/Snare der Referenz in Monospace und die Abweichungen
- [ ] Spurkopf: Doppelklick auf den Spurnamen macht ihn editierbar (auf
      einer Lane der Name der Gruppe); Enter speichert, Esc bricht ab; die
      Lanes heissen danach "<Name>.<n>"; ein doppelter Name zeigt einen
      Fehler-Toast. Gilt auch auf einer noch nicht gewählten Spur: der
      Kopf-Klick stellt nur die Markierung um (zeichnet nicht neu) — 10 von
      10 Doppelklicks öffnen das Feld (`tools/ui_replay/fehler_check.sh`)
- [ ] Routing (Simple und Complex): Doppelklick auf einen Modulnamen macht
      ihn editierbar (Rahmen, leer zeigt den Plugin-Namen als Platzhalter);
      Enter/Verlassen speichert, Esc bricht ab; der Name steht danach in
      Kette, Bus-Zeile, Matrix-Kopf und den Routen der Spur, der Tooltip
      nennt Id und Plugin; ein doppelter Name zeigt einen Fehler-Toast.
      Der einfache Klick (Karte auf/zu) wartet 260 ms, der Doppelklick
      bricht ihn ab — das Feld öffnet jedes Mal, die Karte bleibt, wie sie war
- [ ] Referenz: "Voice" (ohne Referenz gesperrt) ohne gewählte Spur
      sagt per Meldung, dass erst ein Spurkopf geklickt werden muss; Audio-
      und MIDI-Spuren gelten (sind beide gewählt, die zuletzt geklickte);
      die Referenz-Seite ist Quelle + Bereich + Grid (`reference: {file,
      start, end, bpm, firstDownbeat}`); der Eintrag zeigt "Spur: Rolle",
      die Rolle der Referenz und die Abweichungen (fett das Was, dahinter
      ref · yours → Vorschlag, Tooltip: passende Parameter); bei Stille der
      Spur eine Fehler-Meldung
- [ ] Referenz: "Match" und "Tune" (ohne Referenz gesperrt)
      nehmen die gewählte MIDI-Spur (auch wenn danach eine Audio-Spur
      geklickt wurde); ohne gewählte MIDI-Spur bzw. ohne Instrument auf ihr:
      Fehler-Meldung; der Teil der Referenz ist der Bereich (ohne Bereich
      oder länger als 30 s eine Meldung mit der Länge), die Noten der
      gewählten Section spielen; die Statuszeile zählt die
      Prozente hoch, das UI bleibt bedienbar (Play, Klicks). Match: Zeile
      "sound match for …", je Treffer fett Name · Abstand, dahinter, was am
      weitesten weg ist, Tooltip mit den Teilen, Knopf "Load" lädt ihn ins
      Instrument (Undo nimmt es zurück). Tune: Zeile "tuned …: distance
      vorher → nachher", je Regler von → nach (oder "stays"), Tooltip warum
      gewählt; Undo nimmt die Regler in einem Schritt zurück; die Zeile
      darunter nennt "level vs reference vorher → nachher dB (Bereich)";
      Hinweise (anderer Rhythmus, gemischte Referenz, leiser) stehen in der
      Liste; ist das Ergebnis mehr als 6 dB leiser, ist nichts gesetzt, der
      Toast sagt "not applied …" und "Apply anyway" setzt die Werte (danach
      gesperrt, Undo nimmt es zurück)
- [ ] Referenz: "●" (rot, neben Load…) startet den Mitschnitt des
      Systemaudios, pulsiert dann als "■ Stop" mit "capturing" Laufzeit und
      Pegel daneben ("silent", solange nichts ankommt); startet er nicht
      (kein PipeWire), kommt ein Fehler-Toast und der Knopf bleibt
      "●"; reißt er ab (PipeWire-Neustart), steht "reconnecting
      to the audio system …" daneben, bis er weiterläuft, der Stop-Toast
      nennt die Neustarts; nach dem Stop steht die Songliste da (Nummer,
      Start–Ende, Dauer, Pegel; kurze Geräusche gedimmt mit "short" und
      Tooltip, im Kopf mitgezählt), "▶" hört vor, "Use" macht den
      Song zur Referenz (Name in der Auswahl, Zeile orange, Knopf "In use"),
      "Save all" legt alle Songs ab (ohne die kurzen), "✕" blendet die Liste aus; frühere
      Mitschnitte unten in der Referenz-Auswahl ("Captures — find the songs";
      beim Mitschneiden nicht);
      nach einem Neuladen der Seite zeigt der Knopf einen laufenden
      Mitschnitt weiter an
- [ ] Export-Button öffnet das Export-Popover (Format, Samplerate,
      Bittiefe — bei OGG gesperrt —, Bereich ganze Länge/Section, gewählte
      Section vorbelegt, Normalisieren, Ausklang „Tail s“ als Zahl 0–30,
      Stems); die Wahl bleibt im localStorage; Feld „File“: leer =
      `exports/<Node>[-Section]` (Platzhalter folgt dem Bereich), sonst
      relativ zum Projekt (Unterordner gehen) oder absolut, ohne Endung;
      Stems nehmen es als Ordner; der Wert bleibt bis zum Neuladen stehen;
      Export verlinkt die Datei(en) im Toast; Enter = Export, Esc schließt
- [ ] Gruppen: "+ Group" legt eine leere Gruppe an; Spur-Header auf eine
      Gruppen-Kopfzeile ziehen = hinein/umziehen, auf "+ Group" = neue
      Gruppe mit der Spur, auf die "+"-Zeile = heraus (Ziel hebt sich beim
      Ziehen ab, Toast nennt das Ergebnis, Fehler wie "is shared by …" als
      Toast); Doppelklick auf den Namen benennt um; × löst auf, Toast mit
      Undo; S/M der Gruppe sind Abhör-Hilfen (Tooltip sagt es); Fader
      Doppelklick = 0 dB; Export "groups as one" nur, wenn es Gruppen gibt
- [ ] Clip-Knopf "⤓" rendert den Clip durch seine Spur nach `exports/`
      (`<Spur>-<Clip>.<Format>`, Format/Rate/Bittiefe/Normalisieren/Ausklang
      aus dem Export-Popover, MIDI-Format dort = WAV); Shift+Klick mit
      Master-Kette in eigene Datei `<Spur>-<Clip>-master.<Format>`, Toast
      sagt „with master chain“; Toast verlinkt die Datei, nennt geclippte Samples;
      gefrorene Spur = Fehlermeldung im Toast
- [ ] Export-Popover "Batch": Zeilen Batch nodes (This node / Favorite
      nodes / This + favorites / Pick nodes… — Liste aller Nodes zum
      Ankreuzen, auch beendete („· terminated“), der aktuelle vorbelegt,
      keiner = Fehlermeldung), Batch
      ranges (Range above / Each section / Song + each section), Batch
      formats (Häkchen WAV/FLAC/AIFF/OGG); Knopf "Batch" neben Export
      rendert in den Ordner aus „File“, leer nach `exports/batch-<Zeit>/`
      (der Toast nennt den Ordner relativ zum Projekt); das Popover bleibt
      mit Liste im Bild; Ladeanzeige
      "rendering the batch — node i/n, render j/m"; Toast mit Anzahl, Zeit
      und Links; keine Favoriten bzw. kein Format = Fehlermeldung; Teilfehler
      nennt "N of M renders failed"; die Batch-Wahl bleibt im localStorage
- [ ] Format "MIDI (.mid)" im Export-Popover sperrt Samplerate, Bittiefe,
      Normalisieren, Ausklang und Stems; Bereich Song/Section gilt; der
      Toast nennt Datei und Spuren, ohne MIDI im Bereich die Fehlermeldung

## Interaktion

- [ ] Doppelklick vs. Klick auf einem Element: der Klick verzögert sich
      (Timer), der Doppelklick cancelt ihn — sonst feuert beides
- [ ] Buttons in klickbaren Elementen stoppen die Element-Aktionen
      (stopPropagation auf pointerdown/click/dblclick)
- [ ] Inline-Inputs (Rename) schließen mit Enter/blur ab, Escape bricht ab;
      Fokus läuft
- [ ] Tastenkürzel (Leertaste, R, M, L, S, Entf, Ctrl+Z …) feuern nicht in
      Feldern — auch nicht in Namen, die an der Stelle editierbar werden
      (`contentEditable`: Spur, Modul, Gruppe, Node); "Bass Synth R M"
      bleibt ganz, keine Aufnahme, kein Marker, kein Play
- [ ] Klick auf ein Element zeichnet nicht neu, wenn auf demselben Element
      ein Doppelklick wartet (sonst trifft der Doppelklick ein abgehängtes
      Element) — Markierung an Ort und Stelle umstellen oder den Klick
      verzögern
- [ ] Drags: Ghost/Gap-Feedback beim Ziehen, Drop erst beim Loslassen
      bestätigt; Drag startet nicht an Buttons (pointerdown stopPropagation)
- [ ] Clip-Drags: Körper ziehen = Move, Kanten ziehen = Trim — Cursor-Hinweis
      (grab / ew-resize), 2-px-Schwelle; ein Drag gilt nicht als Klick-Auswahl
- [ ] Clip-Drags rasten ein: Start (Move, linke Kante) bzw. Ende (rechte
      Kante) auf das Raster nach Zoom — das feinste aus 1/16 · 1/8 · 1/4 ·
      Takt, dessen Schritt an der Stelle ≥ 10 px ist (120 BPM: unter 20 px/s
      Takt, ab 80 px/s 1/16), gezählt ab dem Section-Start im Takt der
      Section; Shift = frei (1/100 s); das Label im Clip zeigt beim Ziehen
      Stelle und Raster („Verse 3.2 · 13.333 s · snap 1/8“, „· free“ mit
      Shift), der Toast nennt Zeit und Takt (Checks „C“ Raster je Zoom)
- [ ] Ctrl/Alt + Körper ziehen = Kopie an der Stelle (`duplicateClip`
      start, gerastet): der Clip bleibt stehen, eine blasse Kopie
      (gestrichelt) zeigt den Ort, Label „copy to …“; danach ist die Kopie
      gewählt
- [ ] Trim-Zone (8 px innen an jeder Kante) über die ganze Clip-Höhe: ein
      Warp-Griff am Rand (Marker bei 0 s oder am Clip-Ende) und die
      Clip-Leiste (sitzt 9 px vor dem rechten Rand bzw. vor dem rechten
      Rand der Timeline) sperren sie nicht
- [ ] Ganz verdeckter Clip (Kopie an derselben Stelle, längerer Clip
      darüber, oder mehrere Clips darüber zusammen — z. B. unter zwei
      Nachbarn): der oberste Clip über seinem Anfang zeigt vor dem Namen "+N"
      (Tooltip nennt die verdeckten); Klick wählt einen (nächster Klick den
      nächsten), der gewählte liegt oben — ziehen, trimmen, Knöpfe gehen
      (auch über der Crossfade-Marke ⋈ der beiden); Esc bzw. Klick daneben
      legt ihn zurück. Die Marke bleibt bei ⟲/ƒx/⚠ vor dem Namen, die
      Knopfleiste deckt sie nicht (zu schmal → Leiste daneben);
      eine Kopie (⧉) des gewählten verdeckten Clips lässt sich ziehen
- [ ] Clip-Buttons (Kopie, Teilen, ±, Entfernen …) in einer Leiste — nie
      übereinander; sichtbar nur am gewählten Clip, nicht beim Überfahren
      (Decision 2026-10-31-clip-leiste-nur-gewaehlt; dunkler Grund, auf jeder
      Clip-Farbe lesbar), startet keinen Drag; Knöpfe 24 × 24 px; die Leiste
      sitzt am sichtbaren Teil des Clips (klebt am rechten Rand der
      Timeline, wenn das Clip-Ende außerhalb liegt, nie über den ersten
      30 px des Clips — der Anfang bleibt greifbar); Klick wählt den Clip
      (≈ 0,26 s verzögert wie bei Sections), die Leiste bleibt bei
      weggezogener Maus (Checks „K“); Doppelklick auf einen nicht gewählten
      Clip trifft immer den Clip — Warp-Marker bzw. Piano-Roll, nie einen
      Knopf der Leiste, auch bei einem kurzen Clip in großem Zoom
- [ ] Clip löschen (Button oder Entf auf dem gewählten Clip) passiert
      sofort; der Toast bietet "Undo" (`undoDelete` mit dem Token) — danach
      steht der Clip an derselben Stelle und die deleteClip-Aktion ist weg
- [ ] Doppelklick auf einen MIDI-Clip öffnet ihn in der Piano-Roll (Clip
      violett umrandet, "Editing … saves automatically", Done/Esc beendet);
      Noten ziehen = verschieben, Klick = entfernen, leere Fläche = neue
      Note; Speichern debounced über `setMidiNotes`, Clip-Ende-Linie in der
      Rolle; Node-Wechsel oder gelöschter Clip beendet die Bearbeitung —
      nicht der Auto-Node, den das Speichern selbst anlegt
- [ ] Destruktive Tree-Buttons (Reset, Terminate, Delete) bestätigen sich am
      Button (erster Klick schärft, zweiter führt aus) — kein Dialog
- [ ] Section-Farbe über Swatches (Popover schließt bei Klick daneben),
      eigener Farbton über den Regenbogen-Swatch
- [ ] "Compact…" im Tree: Trockenlauf zuerst, Bestätigung listet je Node,
      was fliegt (Nodes, Clips, Sections, Tracks); ohne Favoriten oder ohne
      Arbeit nur eine Meldung, kein Dialog; nach dem Lauf Refresh (aktuelle
      Node kann auf einen Vorfahren springen)
- [ ] Guards aus der Engine führen zurück: bei `!ok` Zustand neu rendern
      (der Spring-Back), Meldung zeigen

## Zustand

- [ ] Änderung von außen (Agent, OSC, Skript über die API) erscheint
      ohne Klick (Positions-Poll sieht die neue Version, ~0,5 s): Fader,
      M/S, Clips, Baum; kein Refresh-Knopf mehr. Nicht mitten in einer
      Bedienung: bei gehaltener Maustaste, Fokus in einem Feld, offenem
      Popover oder laufendem eigenem Befehl erst danach (Decision
      2026-10-31-aenderungen-von-aussen-live)
- [ ] Auch ohne offenen Song: legt ein Agent einen Song an (`treeSeed`)
      oder öffnet einen (`treeOpen`), zeigt die leere UI ihn ohne Neuladen
      (Poll läuft im Start-Panel weiter, ~2 Hz; `fehler_check.sh`, L)
- [ ] Neuzeichnen der Timeline (Klick auf A, M, S, Arm, Gruppe auf/zu,
      Änderung von außen) lässt die Ansicht stehen: gleicher Scroll-Stand
      senkrecht und waagrecht (`renderLinear` hält ihn; vorher sprang sie
      beim „A“ an „Bass“ auf den Anfang)
- [ ] Sichtbarkeiten nur in `applyNodeUi()` setzen (eine Stelle) — kein
      doppeltes Setzen in render*-Funktionen
- [ ] Auswahl überlebt den Refresh (Section,
      Record-Ziele beider Typen und Clip als Seiten-Zustand); Zoom,
      Sidebar-Breite/-Sichtbarkeit und zuletzt geöffnete Projekte im
      localStorage (jeder Zugriff in try/catch)
- [ ] Leere Zustände zeigen den nächsten Schritt: kein Song (Hero mit
      "Open project folder…" und zuletzt geöffneten Projekten), keine Clips
      (Drop-Hinweis in der Timeline), keine Tracks im Rack (Hinweis)
- [ ] Ordner-Dialog: Songs (`listDir` projects) mit Noten-Icon und
      "song"-Tag öffnen per Klick; andere Ordner: Klick blättert, Doppelklick
      legt dort einen Song an; der Hauptbutton sagt "Open this song" bzw.
      "Start a song here"; zuletzt geöffnete Projekte oben im Startordner
- [ ] Der Refresh bei fehlendem Baum räumt auf (Body-Klassen, Panes
      zurücksetzen)

## Tests

- [ ] `tests/run_tests.py` `ui_html`-Marker aktuell (entfernte Elemente
      raus, neue rein)
- [ ] Neues Feature mit Tests; geändertes Verhalten: Test mit geändert
- [ ] `bash tests/run.sh` grün vor dem Commit
- [ ] Bedienung per Browser (headless): `tools/ui_replay/fehler_check.sh
      <port>` — Umbenennen, Kürzel in Feldern, gewählte Spur für
      Referenz/Rezept, Trim an Warp-Kanten, verdeckte Clips (eigene
      Instanz, Song im tmpfs)

## Sample-Suche nach Klang (Sampler-Karte)

- [ ] Umschalter Name/Sound über dem Suchfeld (Sampler-Modul aufgeklappt); die Wahl bleibt über Reloads (localStorage), der
      Platzhalter sagt, was einzugeben ist
- [ ] Sound-Modus zeigt den Stand der Analyse ("N of M samples analyzed"
      mit Analyze, beim Laufen Fortschritt + Restzeit mit Stop) — ohne
      Analyse führt der Hinweis in der Trefferzeile zum Analyze-Knopf
- [ ] Jeder Treffer und jedes belegte Sample hat ≈ (ähnliche suchen); die
      Trefferzeile nennt "≈ <Name>", Kategorie und Ähnlichkeit, der Tooltip
      die Merkmale (Punch, Attack, Ton, Vibrato, bpm, Loop)
- [ ] Unter den Treffern steht, was gefiltert/sortiert wurde ("only snare
      (from the text) · sorted by punch desc")

