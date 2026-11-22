#pragma once

#include "Dawd.h"
#include "Routing.h"

// Track-, Lane-, Section- und Insert-Helfer: alles, was direkt auf dem Edit
// arbeitet, ohne Op-Handler zu sein.
namespace dawd
{
    // Nutzer-Spur am Index holen, fehlende hinten anlegen (Audio, an Main).
    // Ein Instrument bekommt keine Spur automatisch — das Routing
    // (Routing.h) fuehrt MIDI-Spuren durch Module bis Main.
    te::AudioTrack* getOrInsertAudioTrackAt (te::Edit& edit, int index);

    // Lane-Spur: eigener Track, dessen Output in den Zieltrack laeuft — der
    // Clip klingt durch dessen Routing (Module, Volume). Die Lane erbt den
    // Spurtyp. Lane benannt nach dem Zieltrack: Track 2 -> Track 2.1, 2.2 ...
    te::AudioTrack* makeLaneTrack (te::Edit& edit, te::AudioTrack& target);

    // Nur eine Lane spielt: die neue aktiviert ihre Gruppe — die Geschwister-
    // Lanes werden gemutet, die Gruppenspur bleibt als Bus an (ihr Track-Mute
    // wuerde die Lanes mit toeten, die durch sie hindurchlaufen). Ihre Clips
    // bleiben unangetastet — Seed-Audio als Song-Grundlage spielt weiter.
    void activateLane (te::Edit& edit, te::AudioTrack& lane);

    // Die spielende Lane einer Gruppe (nicht stumm), sonst nullptr
    te::AudioTrack* activeLaneOf (te::Edit& edit, te::AudioTrack& group);

    // Raster fuer Quantisieren/Groove: "1/16", "1/8T" (Triole), "1/4." (punktiert)
    // in Notenwerten (1/4 = ein Beat) oder eine Zahl in Beats. Fehlertext oder "".
    juce::String parseGrid (const juce::var& grid, double& beats);

    // Kein Clip der Spur liegt in [start, end) (Sekunden)
    bool rangeIsFree (te::AudioTrack& track, double start, double end);

    // Die Clips der Spur in [start, end) als "'Name' (12.0-20.0 s)" — fuer
    // Meldungen, warum ein Clip eine neue Lane bekam (Praxistest 2/3)
    juce::StringArray clipsInRange (te::AudioTrack& track, double start, double end);

    // Die Lanes der Gruppe, die gerade spielen und Clips haben — die eine
    // neue Lane stummschaltet (Namen fuer mutedLanes)
    juce::StringArray playingLanesWithClips (te::Edit& edit, te::AudioTrack& group);

    // Inputs aktivieren, Kontext allokieren, Instanz finden (record + monitor).
    // Der Kontext muss vor dem ersten Record/Monitor stehen — ohne Graph kein
    // Live-Monitoring.
    te::InputDeviceInstance* armInputInstance (DawdContext& ctx, bool midi);

    // Instanz fuer ein bestimmtes Eingangsgeraet im Playback-Kontext — nach
    // einem Rebuild holen (Pointer in den alten Kontext haengen sonst).
    te::InputDeviceInstance* inputInstanceFor (DawdContext& ctx, te::InputDevice* device);

    // Verfuegbare Eingaenge (wave + midi) mit ihren Spur-Zuweisungen —
    // listInputs und der state op lesen sie (die UI zeigt die Zuweisung im
    // Spur-Dropdown).
    juce::Array<juce::var> enumerateInputs (DawdContext& ctx);

    // Eindeutiger Anzeigename eines Eingangs: gleich benannte Geraete (ein
    // Interface mit zwei MIDI-Ports gleichen Namens) bekommen " #2", " #3" …
    // — Ausgaben und Suche (assignInput, armTrack, record) laufen darueber
    juce::String inputDisplayName (DawdContext& ctx, te::InputDevice& device);
    te::InputDevice* findInputByName (DawdContext& ctx, const juce::String& name);

    // Komprimierte Formate (ogg, flac, mp3) einmal nach media/<name>.<ext>.wav
    // dekodieren (Render/Sampler mit dem Original kaeme nicht zurueck);
    // unkomprimierte Dateien unveraendert. Fehlertext in err.
    juce::File playableAudioFile (DawdContext& ctx, const juce::File& file, juce::String& err);

    // Der Clip bekommt die ganze Datei — auch ueber das Section-Ende hinaus.
    juce::String insertAudioClip (DawdContext& ctx, int trackIndex, const juce::File& file, double start,
                                  double* lengthOut = nullptr);

    // MidiClip auf der Spur anlegen und mit einer Sequenz fuellen — eine
    // Audio-Spur wird dabei MIDI-Spur. Die Zeitstempel der Sequenz sind Sekunden relativ zum Clip-Start
    // (importMidiSequence rechnet sie ueber das Edit-Tempo in Beats um);
    // length <= 0 nimmt das Sequenz-Ende, notesOut meldet die Notenzahl im Clip.
    juce::String insertMidiSequence (DawdContext& ctx, int trackIndex, const juce::String& name,
                                     juce::MidiMessageSequence& seq, double start, double length,
                                     double* lengthOut, int* notesOut = nullptr);

    // Noten mit Zeiten relativ zum Clip-Start (Sekunden) in den Clip mergen.
    // Die Engine liest die Zeitstempel als absolute Edit-Zeit — ohne Versatz
    // landeten Noten in Sections, die nicht bei 0 beginnen, vor dem Clip.
    void mergeNotesRelative (te::MidiClip& clip, juce::MidiMessageSequence& seq);

    // Beats ab einer Songzeit in Sekunden umrechnen (ueber die Tempospur
    // der Engine, also auch ueber Tempowechsel zwischen Sections hinweg):
    // Sekunden von from bis from + beats. Ein Beat = ein Schlag des
    // Section-Tempos.
    double beatsToSeconds (te::Edit& edit, double fromSeconds, double beats);

    // Warten, bis kein Audio-Clip mehr seine Abspieldatei erzeugt (rueck-
    // waerts gerenderte Kopie, Proxy) — die Engine rendert sie im
    // Hintergrund und meldet sich ueber den Message-Thread; ein Render davor
    // naehme noch das Original. false nach timeoutMs.
    bool waitForClipRenders (te::Edit& edit, int timeoutMs = 30000);

    // Fehlende Mediendateien (Todo 2026-10-26-fehlende-mediendatei-haengt):
    // Die Engine kennt die Laenge einer Datei aus ihrem Cache; fehlt die
    // Datei, bevor ein Reader sie geoeffnet hat, wartet der Offline-Render je
    // Block 5 s auf sie (WaveNode, AudioFileCache) — der Render haengt
    // praktisch, der Message-Thread mit. kind: clip (Audio-Clip einer
    // Nutzer-Spur, track = Index in userTracks), freeze (Audio einer
    // gefrorenen Spur, track = ihre Gruppe), sample (Sampler-Modul, module =
    // Id, name = Sample). scope: nur, was dieser Render liest (Spuren,
    // allowedClips, Zeitbereich; Sampler nicht — sie laden in den Speicher
    // und haengen nicht); ohne scope alles.
    struct MissingFile
    {
        juce::String kind, trackName, name, module;
        int track = -1;
        juce::File file;
    };

    bool isClipFileMissing (const te::AudioClipBase&);
    juce::Array<MissingFile> missingMedia (te::Edit&, const te::Renderer::Parameters* scope = nullptr);
    juce::var missingMediaList (const juce::Array<MissingFile>&);   // state.missingFiles
    juce::String missingMediaError (const juce::Array<MissingFile>&);   // Fehlertext fuer Render/Analyse
    double secondsToBeats (te::Edit& edit, double fromSeconds, double seconds);

    // Noten aus dem Befehl in eine Sequenz (Zeiten in Sekunden relativ zum
    // Clip-Start): je Note {pitch, start, length} in Sekunden oder {pitch,
    // beat, beats} (auch startBeat/lengthBeats) in Beats ab dem Clip-Start,
    // velocity 1..127 (Default 100); gemischt in einer Liste abgelehnt,
    // unbekannte Felder auch. notesEnd = Ende der letzten Note, inBeats =
    // die Noten kamen in Beats. Fehlertext ("invalid note 3: …") oder "".
    juce::String parseNotes (te::Edit& edit, const juce::var& notes, double clipStart,
                             juce::MidiMessageSequence& seq, double& notesEnd, bool* inBeats = nullptr);

    // Controller-Events eines MIDI-Clips fuer die API (clipNotes): je Event
    // {controller, beat (ab Clip-Anfang), start (s), value}, nur was im Clip
    // liegt, nach Beat sortiert. controller = CC-Nummer 0..127 oder
    // "pitchBend" (value -8192..8191, 0 = Mitte); CC-Werte 0..127. Aus
    // MIDI-Dateien kommen auch "program", "channelPressure", "aftertouch".
    juce::Array<juce::var> clipControllers (te::Edit& edit, te::MidiClip& clip);

    // MIDI-Datei in einen MidiClip auf der Spur — die Noten aller Tracks der
    // Datei landen in einem Clip; die Timestamps (nach dem Umrechnen in
    // Sekunden) sind relativ zum Clip-Start.
    juce::String insertMidiClip (DawdContext& ctx, int trackIndex, const juce::File& file, double start,
                                 double* lengthOut);

    // Clip auf einer Spur ueber den Namen finden — die Adressierung aus
    // state.trackList (die lineare Ansicht zeigt ihn). Jeder Insert/
    // Take lebt in einer eigenen Lane, der Name ist je Spur eindeutig.
    te::Clip* findClipByName (te::AudioTrack& track, const juce::String& name);

    // Freier Clip-Name auf der Spur: base, sonst "base 2", "base 3" … (ein
    // angehaengtes " <n>" an base wird erst abgeschnitten — Kopien von
    // "Notes 2" heissen "Notes 3", nicht "Notes 2 2")
    juce::String uniqueClipName (te::AudioTrack& track, const juce::String& base);

    // Sektion = MarkerClip auf dem MarkerTrack des Edits (Name + Zeitbereich).
    te::MarkerClip* findSection (te::Edit& edit, const juce::String& name);

    // Sections nach Start sortiert — die Reihenfolge im Song.
    std::vector<te::MarkerClip*> sortedSections (te::Edit& edit);

    // Section als MarkerClip anlegen — Farbe landet im Edit (andere Branches
    // erben sie). Liefert den Clip, nullptr wenn der Insert fehlschlaegt.
    te::MarkerClip* insertSection (te::Edit& edit, const juce::String& name, double start, double end,
                                   juce::Colour colour);

    // Default-Laenge einer neuen Section in Sekunden — spaeter pro User
    // konfigurierbar, jetzt fest.
    constexpr double defaultSectionLength = 30.0;

    // "Section N" — naechste freie Nummer ueber den bestehenden Namen.
    juce::String nextSectionName (te::Edit& edit);

    // Erste Paletten-Farbe, die keine bestehende Section traegt — jede
    // Section hat ihre eigene.
    juce::Colour nextSectionColour (te::Edit& edit);

    // Tempo und Takt je Section: am Marker gespeichert (dawdBpm, dawdTimeSig
    // "3/4"). Ohne eigene Angabe gilt die Vorgaengerin, die erste Section ohne
    // Angabe 120 BPM, 4/4. applySectionTempos baut daraus die Tempospur der
    // Engine neu (Wechsel als Sprung am Section-Start, Takt auf den naechsten
    // Beat gerundet). Sections behalten ihre Laenge in Sekunden, Audio bleibt
    // liegen; MIDI-Noten liegen in Beats und folgen dem Tempo.
    struct SectionTempo
    {
        double bpm = 120.0;
        int num = 4, den = 4;
        bool bpmSet = false, sigSet = false;
        bool ramp = false;        // eigene Angabe (dawdRamp), wird nicht geerbt
        double bpmEnd = 120.0;    // Tempo am Ende: mit Rampe das der naechsten Section, sonst bpm

        bool ramps() const { return ramp && bpmEnd != bpm; }
        double barSeconds() const { return num * (4.0 / den) * 60.0 / bpm; }
        double beatsPerBar() const { return num * (4.0 / den); }
        juce::String timeSig() const { return juce::String (num) + "/" + juce::String (den); }
    };

    // Effektive Werte, parallel zu sortedSections
    std::vector<SectionTempo> sectionTempos (te::Edit& edit);

    // Tempo der Section, die bei t endet (fuer eine neue Section dort)
    SectionTempo tempoBefore (te::Edit& edit, double t);

    // bpm (0 = erben), timeSig ("" = erben); void = unveraendert. Fehlertext
    // oder "" (API-Vertrag), bei Fehler bleibt alles unveraendert.
    juce::String setSectionTempo (te::MarkerClip& section, const juce::var& bpm, const juce::var& timeSig);
    juce::String validateSectionTempo (const juce::var& bpm, const juce::var& timeSig);

    void applySectionTempos (te::Edit& edit);

    // Offset in Beats (Todo 2026-10-26-tempowechsel-haelt-clip-offset):
    // Clips, die dem Tempo folgen (followTempo, gewarpt), spielen ihr
    // Material in Beats, die Engine haelt den Offset aber in Sekunden
    // (getOffsetInBeats = Offset * Beats/s am Clip-Start). Aendert sich das
    // Tempo am Clip, rueckte das Material um Offset * (1 - alt/neu) Beats.
    // Section-Ops merken sich darum vor der Aenderung den Offset in Beats
    // (followTempoOffsets) und stellen ihn nach applySectionTempos wieder her
    // (keepOffsetBeats) — nur an Clips, deren Offset in Sekunden die Op nicht
    // selbst geaendert hat (an einer Grenze abgeschnitten); geteilte Teile
    // mit neuer ID bleiben, wie die Op sie gelegt hat.
    struct OffsetBeats { te::EditItemID id; double seconds = 0.0, beats = 0.0; };
    std::vector<OffsetBeats> followTempoOffsets (te::Edit& edit);
    void keepOffsetBeats (te::Edit& edit, const std::vector<OffsetBeats>& before);

    // Tempo-Rampe (2026-10-25, Voreinstellung — Frage an Matthias im Todo
    // clips-arrangement): ramp an einer Section = ihr Tempo gleitet linear
    // (in Beats) vom eigenen bpm zum bpm der naechsten Section, die es an
    // ihrem Start erreicht (Accelerando/Ritardando). Die Section behaelt ihre
    // Sekunden wie bei jedem Tempowechsel; die Tempospur der Engine bekommt
    // am Section-Start einen Punkt mit Kurve 0 (linear) statt 1 (Sprung).
    // Ohne folgende Section mit anderem Tempo wirkt sie nicht.
    juce::String setSectionRamp (te::MarkerClip& section, const juce::var& ramp);   // void = unveraendert; Fehlertext oder ""

    // Laenge einer Section in Takten: mit Rampe ueber die Tempospur, sonst
    // Sekunden / Taktlaenge. Und umgekehrt: Sekunden fuer bars Takte.
    double sectionBars (te::Edit& edit, te::MarkerClip& section, const SectionTempo& tempo);
    double secondsForBars (const SectionTempo& tempo, double bars);

    // Marker (2026-10-25): benannte Zeitpunkte neben den Sections, im Edit
    // unter DAWDMARKERS/MARKER {name, time} — also je Node, Undo ueber die
    // Schnappschuesse. Die Zeit-Helfer unten nehmen sie mit (Voreinstellung,
    // Frage an Matthias im Todo clips-arrangement; Schalter
    // markersFollowTime in EngineHelpers.cpp): Einfuegen schiebt sie,
    // Entfernen loescht die im Bereich und schiebt den Rest, Umordnen zieht
    // sie mit ihrem Stueck; Kopieren (duplicateTime) kopiert sie mit
    // eindeutigem Suffix (Matthias 2026-10-25).
    struct Marker { juce::String name; double time; };
    std::vector<Marker> sortedMarkers (te::Edit&);
    juce::ValueTree findMarker (te::Edit&, const juce::String& name);   // ungueltig = keiner
    void addMarker (te::Edit&, const juce::String& name, double time);
    void setMarker (juce::ValueTree& marker, const juce::String& name, double time);
    void removeMarker (te::Edit&, const juce::String& name);
    juce::String nextMarkerName (te::Edit&);   // "Marker N"

    // Fuer die API: {name, time, section, bar, beat} — Takt und Schlag
    // 1-basiert ab Songanfang ueber die Tempospur (Sections)
    juce::var describeMarker (te::Edit&, const Marker&);

    // Zeitbereiche (Sections sind Zeitbereiche, das Material ist frei —
    // 2026-10-24). Alle Section-Ops schneiden, verschieben und kopieren Zeit
    // ueber diese Helfer: Clips der Nutzer-Spuren werden an den Grenzen
    // geteilt bzw. abgeschnitten (MIDI-Noten und Audio bleiben an ihrer
    // Songzeit, der Audio-Offset waechst nur), Automation wird wie Clips
    // behandelt (an Grenzen haelt ein Punkt den Wert).

    // Clip an der Songzeit at teilen: links behaelt den Namen, rechts
    // bekommt einen freien ("Notes 2") und ist keine Pattern-Kopie mehr;
    // MIDI-Noten, die eine Haelfte nur verdeckt, fallen weg. nullptr, wenn at
    // nicht im Clip liegt.
    te::Clip* splitClipAt (te::Clip& clip, double at);

    // Alle Clips, die ueber t reichen, an t teilen
    void splitClipsAt (te::Edit& edit, double t);

    // Zeit bei t einfuegen: Clips ueber t werden geteilt, ab t ruecken
    // Sections, Clips und Automation um length nach rechts; in der Luecke
    // haelt die Automation den Wert an t.
    void insertTime (te::Edit& edit, double t, double length);

    // Zeit [start, end) entfernen: Clips darin fallen weg, Clips ueber den
    // Grenzen werden dort abgeschnitten, Automation darin faellt weg (an der
    // Naht springt sie auf den Wert bei end); ab end rueckt alles nach links.
    // Sections im Bereich entfernt bzw. kuerzt der Aufrufer selbst. Liefert
    // die Zahl der entfernten Clips, cutOut die der abgeschnittenen.
    int removeTime (te::Edit& edit, double start, double end, int* cutOut = nullptr);

    // Zeit [start, end) als Kopie bei at einfuegen (at liegt nicht im
    // Bereich): Zeit einfuegen wie insertTime, dann die Clip-Teile des
    // Bereichs auf ihren Spuren und seine Automation hineinkopieren; die
    // Marker darin kommen mit, mit eindeutigem Suffix ("Drop 2", Matthias
    // 2026-10-25; ihre Namen in markersOut). Liefert die Zahl der kopierten
    // Clips.
    int duplicateTime (te::Edit& edit, double start, double end, double at,
                       juce::StringArray* markersOut = nullptr);

    // Bereiche umordnen (moveSection): jedes Stueck [from, to) wandert um
    // delta; Clips ueber Grenzen mit unterschiedlichem delta werden geteilt,
    // die Automation zieht stueckweise mit. Die Stuecke muessen die Zeit
    // luecken- und ueberlappungsfrei abdecken (letztes bis unendlich).
    struct TimeShift { double from, to, delta; };
    void reorderTime (te::Edit& edit, const std::vector<TimeShift>& shifts);

    // Comping (2026-10-24): der Comp einer Gruppe ist eine eigene Lane
    // ("<Gruppe>.comp", Marke dawdComp) aus Kopien der Take-Clips — je Teil
    // die Quell-Lane (dawdCompFrom = ItemID). Sie ist eine Lane wie jede
    // andere: laneActive waehlt sie oder einen ganzen Take, Section-Ops,
    // Undo, Freeze und Render behandeln ihre Clips wie alle Clips.
    bool isCompLane (const te::AudioTrack&);
    te::AudioTrack* compLaneOf (te::Edit&, const te::AudioTrack& group);

    // Quell-Lane eines Comp-Teils, sonst nullptr
    te::AudioTrack* compSourceOf (te::Edit&, const te::Clip&);

    // Im Comp der Gruppe spielt in [start, end) die Lane take. Fehlt der
    // Comp, entsteht er als Kopie der Lane, die jetzt spielt. Die Uebergaenge
    // blenden ueber fade Sekunden (mittig auf der Grenze; Audio — MIDI
    // schneidet hart, fade 0). Liefert die Comp-Lane (nicht aktiviert).
    te::AudioTrack* compRange (te::Edit&, te::AudioTrack& take, double start, double end, double fade);

    // Bereiche des Comps nach Songzeit: [{lane (Index, -1 = weg), name,
    // start, end}] — Blenden gelten ab ihrer Mitte, gleiche Lane am Stueck
    // ist ein Bereich
    juce::Array<juce::var> compRanges (te::Edit&, te::AudioTrack& comp);

    // Material (Clips, Aufnahmen, Automation) darf ueber Section-Grenzen
    // laufen (2026-10-24). Reicht ein Clip hinter die letzte Section, haengt
    // autoSection eine neue an — bis zum Materialende in ganzen Takten
    // (aufgerundet), Name "Section N", Tempo/Takt geerbt — damit der Song
    // ohne Luecke weiter in Sections liegt. Ergebnis {name, start, end, bars}
    // fuer die Antwort, void = keine noetig.
    juce::var autoSection (te::Edit& edit);

    // Gegenweg der Auto-Section (Praxistest 2: ein ganzer Stem fuer Chops
    // legte jedes Mal "Section 1" bis 220 s an, der Agent loeschte sie von
    // Hand): reicht kein Material mehr in die letzte Section und ist sie eine
    // unveraenderte Auto-Section aus `names`, faellt sie weg; reicht es noch
    // hinein, schrumpft sie auf das Materialende in ganzen Takten. Ergebnis
    // {name, removed: true} | {name, end, bars} | void.
    juce::var retractAutoSection (te::Edit& edit, const juce::StringArray& names);

    // Marke der Auto-Section (am Marker-Clip); updateSection nimmt sie weg
    extern const juce::Identifier autoSectionProp;

    // Spielt das Instrument einstimmig? Parameter "… Play Mode"/"Voice Mode"
    // mit Mono/Legato (Surge XT, Diva, Helm …): "A Play Mode: Mono" oder "".
    // Praxistest 1c: Akkorde liefen in einen Mono-Lead, von jedem Akkord
    // klang ein Ton, welcher, hing von den Noten davor ab.
    juce::String monoModeOf (te::Plugin&);

    // Klingen zwei Noten der Folge gleichzeitig (> 30 ms Ueberlappung)?
    bool hasChords (const juce::MidiMessageSequence&);

    // Ausstehende asynchrone Plugin-Updates jetzt ausfuehren (Sampler baut
    // seine Sounds sonst erst in der naechsten Message-Loop-Runde — ein
    // Render direkt nach treeCheckout im selben Batch war ohne Samples)
    void settlePlugins (te::Edit&);

    // VST3 (im Prozess und in der Sandbox): Controller mit dem aktuellen
    // Zustand der Komponente abgleichen, dann Namen und Werte neu lesen —
    // auch die Parameter der Engine, sofort (nicht erst in der naechsten
    // Message-Loop-Runde). Surge XT uebernimmt einen Zustand (loadPreset,
    // neue Instanz) erst im naechsten Block und meldet die neuen Werte nie:
    // listParameters zeigte die des vorigen Presets, ein setParameter auf
    // den alten Wert kam nie an (Render zufaellig statt mit Retrigger). Die
    // neuen Namen meldet es aus einem eigenen Thread, sie kamen erst nach
    // der laufenden Op an (unter Last auch spaeter). Nur aufrufen, nachdem
    // das Plugin einen Block bekam und keine Werte von dawd offen sind.
    // false = kein VST3 oder nicht moeglich.
    bool syncPluginParameters (te::Plugin&);

    // 32 stille Bloecke ohne Graph (wie settleParameterNames), danach
    // syncPluginParameters
    void settlePluginNow (te::Plugin&);

    // Parameter-Namen dynamischer Plugins: Surge XT meldet nach dem Laden
    // eines Zustands und als neue Instanz Rohnamen ("FX A1 Param 3", "M5: -",
    // Surge XT Effects "FX Parameter 0"), die echten erst nach einigen
    // Audio-Bloecken. dawd baut den Graphen erst beim Abspielen — nach
    // treeOpen, Checkout, Undo, Auto-Node blieben die Rohnamen bis zum
    // ersten Render (Praxistest 1n P3, 1m P4). Neue Instanzen (und das Modul
    // touched nach loadPreset/setPluginFile) ohne Graph bekommen darum 32
    // stille Bloecke, danach holt syncPluginParameters Namen und Werte in
    // derselben Op. Nicht abgedeckt: Surge XT Effects nach FX-Typ-Wechsel
    // per setParameter — da helfen stille Bloecke nicht, erst ein Render.
    void settleParameterNames (te::Edit&, te::Plugin* touched = nullptr);

    // Zu Beginn der naechsten Op: die eben beruhigten Plugins noch einmal
    // abgleichen (lief ein Graph, kamen die Bloecke erst danach) und ihre
    // echten Namen merken (rememberParameterNames) — ein Auto-Node in
    // dieser Op laedt sie neu, mit Rohnamen
    void rememberSettledNames();

    // Edit-Stand ohne Abhoer-Hilfen (Solo/Mute, Decision 2026-10-20): kein
    // solo, Mute je Spur (ItemID) wie gespeichert. Fuer Kopien eines Stands
    // (Undo) und die Node-Datei (saveCurrentEdit).
    void applySavedListening (juce::ValueTree& editState, const std::map<juce::String, bool>& savedMute);
    bool anyTrackSolo (te::Edit&);

    // Crossfades (setClip crossfade = autoCrossfade der Engine): die
    // Blendlaengen der Ueberlappungen haelt die Engine nur im Speicher und
    // rechnet sie ueber die Message-Loop nach — nach dem Laden fehlten sie
    // (Blende 0), nach dem Verschieben kamen sie erst eine Runde spaeter.
    // Markiert jeden Clip mit Crossfade zum Nachrechnen; die naechste
    // Abfrage (Render, state) rechnet synchron. executeOp ruft es nach jeder
    // Mutation, loadNodeEdit nach dem Laden.
    // Standard (Decision 2026-10-25-crossfade-standard): Crossfade an, auch
    // fuer Clips aus bestehenden Projekten — "nie gesetzt" = Clip ohne die
    // Eigenschaft autoCrossfade im Edit wird an; ein ausdruecklich
    // ausgeschalteter (setClip crossfade false, autoCrossfade="0") bleibt
    // aus. Die Blendkurve fuehrt dawd, solange fadeCurve nie gesetzt wurde
    // (Merker dawdAutoCurve): convex auf der Seite, die ueberblendet, sonst
    // linear.
    void refreshAutoCrossfades (te::Edit&);

    // Master-Lautstaerke 0 dB (Decision docs/decisions/2026-10-26-master-0db.md):
    // die Engine legt Edits mit -3 dB an (Edit::Options::defaultMasterVolumedB),
    // der Render laesst den Master weg — die Wiedergabe war 3 dB leiser als
    // die Datei. Ersetzt nur die Engine-Vorgabe (-3 dB), einmal je Edit
    // (Merker dawdMasterUnity am Master-Volume-State); ein anderer Wert
    // bleibt. Laeuft in ensureRouting, also fuer jeden neuen und geladenen Edit.
    void unityMasterVolume (te::Edit&);

    // Blendkurve des Clips fuehrt dawd (fadeCurve nie gesetzt bzw. "auto")
    bool isFadeCurveAuto (const te::AudioClipBase&);
    void setFadeCurveAuto (te::AudioClipBase&, bool);

    // Name der Blendkurve wie in setClip fadeCurve (linear, convex, concave, sCurve)
    juce::String fadeCurveName (te::AudioFadeCurve::Type);

    // Warp-Marker und Loop an Audio-Clips (warpClip, loopClip). Ein
    // gewarpter Clip spielt die Originaldatei und folgt dem Tempo
    // (autoTempo): Beats des Materials zum Tempo der LoopInfo ->
    // Warp-Sekunden -> ueber die Marker Sekunden der Originaldatei. Das
    // rechnet die Engine beim Abspielen in Echtzeit (getWarpPointsForClip,
    // Patch tracktion-warp-realtime). Kind DAWDWARP am Clip: source
    // (Originaldatei), length (ihre Laenge in s), realtime (true seit
    // 2026-10-31), je Nutzer-Marker MARKER {source, warp}; dazu gelten Anker
    // an Dateianfang (0 -> 0) und -ende (Laenge -> Laenge).
    extern const juce::Identifier warpTreeId;
    bool isWarped (const te::AudioClipBase&);
    juce::File unwarpedFile (const te::AudioClipBase&);   // Originaldatei (ohne Warp: die des Clips)

    // Punkte {Quelle, Warp} fuer die Echtzeit-Warp der Engine; leer ohne
    // Warp. Laeuft beim Bau des Wiedergabe-Graphen
    std::vector<std::pair<te::TimePosition, te::TimePosition>> realtimeWarpPoints (const te::AudioClipBase&);

    // Projekte vor 2026-10-31: ein gewarpter Clip spielte eine gerenderte
    // media/<name>-warp<N>.wav — beim Laden zurueck auf die Originaldatei
    // (Echtzeit-Warp). Die alte Datei raeumt treeCompact weg
    void playWarpSources (te::Edit&);

    // Sekunde der Originaldatei zu einer Warp-Sekunde und zurueck (linear
    // zwischen den Markern, aussen Tempo 1); ohne Warp gleich
    double warpToSource (const te::AudioClipBase&, double warpSeconds);
    double sourceToWarp (const te::AudioClipBase&, double sourceSeconds);

    // Sekunde der Originaldatei an einer Stelle im Clip (Sekunden ab
    // Clip-Start): Offset, Tempo (speed oder followTempo), Loop, Warp-Marker
    double clipSourceTime (te::AudioClipBase&, double clipSeconds);

    // Nutzer-Marker eines gewarpten Clips: [{source, beat, time}] — beat
    // und time ab dem Clip-Start; leer ohne Warp
    juce::Array<juce::var> warpMarkersOf (te::AudioClipBase&);

    // Loop eines Audio-Clips fuer state/loopClip: {start, length, offset}
    // in Sekunden der Datei, bei Clips, die dem Tempo folgen, dazu
    // startBeat/lengthBeats/offsetBeats (Beats des Materials) und restarts
    // (Sekunden ab Clip-Start, an denen der Loop von vorn beginnt; hoechstens
    // 64); void ohne Loop
    juce::var clipLoopOf (te::AudioClipBase&);

    // Spur-Namen (renameTrack): eine Gruppenspur traegt den Namen, ihre Lanes
    // heissen "<Name>.<n>". Ein Name gilt als Adresse statt des Index.
    int trackIndexFor (te::Edit&, const juce::String& nameOrIndex);   // -1 = keine
    juce::String checkTrackName (te::Edit&, int index, const juce::String& name);   // Fehlertext oder ""
    void renameTrack (te::Edit&, int index, const juce::String& name);
}
