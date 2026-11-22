#include "Ops.h"
#include "../EngineHelpers.h"

// Songstruktur: Sections anlegen, anpassen, verschieben, kopieren und
// loeschen. Node-Aktionen (collect, deleteSection process) — der Node merkt
// sich, dass hier etwas an der Struktur passiert ist. Das Section-Management
// steuert das Flow-UI komplett aus der linearen Track-Ansicht: Namen, Laenge
// und Farbe sind deshalb optional (Defaults hier), der Name lautet
// "Section N". Sections sind Zeitbereiche, das Material ist frei (seit
// 2026-10-24): jede Op arbeitet ueber die Zeitbereich-Helfer in
// EngineHelpers (insertTime, removeTime, duplicateTime, reorderTime).
namespace dawd
{
    namespace
    {
        // "#rrggbb" oder "rrggbb" — ungültige Laengen liefern transparent.
        juce::Colour parseColour (juce::String s)
        {
            s = s.trim().removeCharacters ("#");

            if (s.length() == 6)
                s = "ff" + s;

            if (s.length() != 8)
                return {};

            return juce::Colour::fromString (s);
        }

        juce::String colourParam (juce::Colour c)
        {
            return "#" + c.toDisplayString (false);
        }

        // Effektives Tempo einer Section (geerbt oder eigen)
        SectionTempo tempoOf (te::Edit& edit, te::MarkerClip& section)
        {
            auto secs = sortedSections (edit);
            auto tempos = sectionTempos (edit);

            for (size_t i = 0; i < secs.size(); ++i)
                if (secs[i] == &section)
                    return tempos[i];

            return {};
        }

        double barsOf (te::Edit& edit, te::MarkerClip& section, const SectionTempo& t)
        {
            return std::round (sectionBars (edit, section, t) * 1000.0) / 1000.0;
        }

        // Tempo-Parameter einer Section-Op (void = nicht gesetzt)
        juce::var optional (const juce::var& cmd, const char* key)
        {
            return cmd.hasProperty (key) ? cmd.getProperty (key, {}) : juce::var();
        }
    }

    // Section anlegen — Name/Laenge/Farbe optional (Defaults: "Section N",
    // defaultSectionLength, naechste Paletten-Farbe). Position erste/letzte/
    // hinter einer genannten; ohne bestehende Sections entfaellt die Wahl:
    // die erste Section startet bei 0.
    juce::var opAddSection (DawdContext& ctx, const juce::var& cmd)
    {
        auto sections = sortedSections (*ctx.edit);
        auto name = cmd.getProperty ("name", {}).toString().trim();

        if (name.isEmpty())
            name = nextSectionName (*ctx.edit);

        auto bpm = optional (cmd, "bpm");
        auto timeSig = optional (cmd, "timeSig");
        bool hasBars = cmd.hasProperty ("bars");
        auto bars = (double) cmd.getProperty ("bars", 0.0);

        if (auto err = validateSectionTempo (bpm, timeSig); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (hasBars && ! (bars > 0.0))
            return resultObj (false, { { "error", "bars must be positive" } });

        double length = cmd.hasProperty ("length") ? (double) cmd.getProperty ("length", 0.0)
                                                   : defaultSectionLength;

        if (! (length > 0.0))
            return resultObj (false, { { "error", "length must be positive" } });

        if (findSection (*ctx.edit, name) != nullptr)
            return resultObj (false, { { "error", "section exists: " + name } });

        // after allein reicht (Praxistest 3: {after: "Refrain"} ohne
        // position landete still am Ende)
        auto position = cmd.getProperty ("position", cmd.hasProperty ("after") ? "after" : "last").toString();

        if (cmd.hasProperty ("after") && position != "after")
            return resultObj (false, { { "error", "after needs position 'after' (or leave position out) - got position '" + position + "'" } });

        double start = 0.0;
        bool shift = false;

        if (! sections.empty())
        {
            if (position == "after")
            {
                auto after = cmd.getProperty ("after", {}).toString();
                auto* anchor = findSection (*ctx.edit, after);

                if (anchor == nullptr)
                    return resultObj (false, { { "error", "no such section: " + after } });

                start = anchor->getPosition().getEnd().inSeconds();
            }
            else if (position == "first")
            {
                start = 0.0;
            }
            else if (position == "last")
            {
                start = sections.back()->getPosition().getEnd().inSeconds();
            }
            else
            {
                return resultObj (false, { { "error", "position must be first, last or after (with after: <section name>)" } });
            }

            shift = true;
        }

        // Laenge in Takten: im Tempo der neuen Section (eigen oder von der
        // Section davor geerbt)
        auto tempo = tempoBefore (*ctx.edit, start);

        if (! bpm.isVoid() && (double) bpm > 0.0)
            tempo.bpm = (double) bpm;

        if (! timeSig.isVoid() && timeSig.toString().isNotEmpty())
        {
            tempo.num = timeSig.toString().upToFirstOccurrenceOf ("/", false, false).getIntValue();
            tempo.den = timeSig.toString().fromFirstOccurrenceOf ("/", false, false).getIntValue();
        }

        if (hasBars)
            length = bars * tempo.barSeconds();

        const auto offsets = followTempoOffsets (*ctx.edit);

        // Zeit einfuegen: spaetere Sections, Clips und Automation ruecken
        // nach rechts, ein Clip ueber der Einfuegestelle wird dort geteilt
        if (shift)
            insertTime (*ctx.edit, start, length);

        auto colour = cmd.hasProperty ("color")
                          ? parseColour (cmd.getProperty ("color", {}).toString())
                          : nextSectionColour (*ctx.edit);

        if (colour.isTransparent())
            colour = nextSectionColour (*ctx.edit);

        auto end = start + length;
        auto* section = insertSection (*ctx.edit, name, start, end, colour);

        if (section == nullptr)
            return resultObj (false, { { "error", "cannot insert section" } });

        setSectionTempo (*section, bpm, timeSig);
        applySectionTempos (*ctx.edit);
        keepOffsetBeats (*ctx.edit, offsets);
        saveCurrentEdit (ctx);
        recordNodeAction (ctx, "collect", "addSection");

        auto eff = tempoOf (*ctx.edit, *section);
        return resultObj (true, { { "name", name }, { "start", start },
                                  { "end", end }, { "length", length },
                                  { "color", colourParam (colour) },
                                  { "bpm", eff.bpm }, { "timeSig", eff.timeSig() },
                                  { "bars", barsOf (*ctx.edit, *section, eff) } });
    }

    // Bestehende Section anpassen: umbenennen, Farbe, Grenze und/oder Laenge
    // aendern. Start = Grenze zwischen den Sections (der Vorgaenger endet
    // dort, die Laenge der Section aendert sich — die erste Section startet
    // fix bei 0; Clips bleiben, wo sie sind); Laenge = Ende ziehen:
    // verlaengern fuegt am Ende Zeit ein (ein Clip ueber dem Ende wird
    // geteilt, der hintere Teil rueckt mit), kuerzen entfernt Zeit am Ende
    // (Clips dort abgeschnitten); alles dahinter rueckt mit. Keine Luecken
    // im Arrangement.
    juce::var opUpdateSection (DawdContext& ctx, const juce::var& cmd)
    {
        auto name = cmd.getProperty ("name", {}).toString();
        auto* section = findSection (*ctx.edit, name);

        if (section == nullptr)
            return resultObj (false, { { "error", "no such section: " + name } });

        auto newLabel = cmd.getProperty ("label", {}).toString();
        bool hasStart = cmd.hasProperty ("start");
        bool hasLength = cmd.hasProperty ("length");
        double newStart = hasStart ? (double) cmd.getProperty ("start", 0.0) : -1.0;
        double newLength = hasLength ? (double) cmd.getProperty ("length", 0.0) : -1.0;
        auto colourParamStr = cmd.getProperty ("color", {}).toString();
        auto bpm = optional (cmd, "bpm");
        auto timeSig = optional (cmd, "timeSig");
        bool hasBars = cmd.hasProperty ("bars");
        auto bars = (double) cmd.getProperty ("bars", 0.0);

        auto ramp = optional (cmd, "ramp");

        if (newLabel.isEmpty() && ! (newLength > 0.0) && ! hasStart && colourParamStr.isEmpty()
            && bpm.isVoid() && timeSig.isVoid() && ! hasBars && ramp.isVoid())
            return resultObj (false, { { "error", "nothing to update (label, start, length, bars, color, bpm, timeSig or ramp)" } });

        if (auto err = validateSectionTempo (bpm, timeSig); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        if (! ramp.isVoid() && ! ramp.isBool())
            return resultObj (false, { { "error", "ramp must be true or false" } });

        if (hasBars && ! (bars > 0.0))
            return resultObj (false, { { "error", "bars must be positive" } });

        if (hasBars && hasLength)
            return resultObj (false, { { "error", "give length or bars, not both" } });

        if (newLabel.isNotEmpty() && newLabel != name && findSection (*ctx.edit, newLabel) != nullptr)
            return resultObj (false, { { "error", "section exists: " + newLabel } });

        // Offset der Clips, die dem Tempo folgen, in Beats halten
        const auto offsets = followTempoOffsets (*ctx.edit);

        // Tempo zuerst: bars rechnet im neuen Tempo der Section (mit Rampe
        // ueber deren Verlauf)
        setSectionTempo (*section, bpm, timeSig);
        setSectionRamp (*section, ramp);

        if (hasBars)
        {
            hasLength = true;
            newLength = secondsForBars (tempoOf (*ctx.edit, *section), bars);
        }

        if (newLabel.isNotEmpty() && newLabel != name)
            section->setName (newLabel);

        juce::Colour colour;

        if (colourParamStr.isNotEmpty())
        {
            colour = parseColour (colourParamStr);

            if (colour.isTransparent())
                return resultObj (false, { { "error", "invalid color" } });

            section->setColour (colour);
        }

        if (hasStart || hasLength)
        {
            auto pos = section->getPosition();
            double oldStart = pos.getStart().inSeconds();
            double oldEnd = pos.getEnd().inSeconds();

            // Vorgaenger in der Reihenfolge (nach Start sortiert)
            te::MarkerClip* prev = nullptr;

            for (auto* sec : sortedSections (*ctx.edit))
            {
                if (sec == section)
                    break;

                prev = sec;
            }

            double ns = hasStart ? newStart : oldStart;

            if (hasStart)
            {
                // Anfang ziehen = Grenze zwischen den Sections — keine
                // Luecke: der Vorgaenger endet am neuen Start. Die erste
                // Section startet fix bei 0.
                if (prev == nullptr)
                    return resultObj (false, { { "error", "the first section starts at 0" } });

                double prevStart = prev->getPosition().getStart().inSeconds();
                ns = juce::jlimit (prevStart + 0.01, oldEnd - 0.01, ns);
                prev->setEnd (te::TimePosition::fromSeconds (ns), false);
                // Nur der Start wandert — das Ende bleibt, die Laenge aendert sich
                section->setStart (te::TimePosition::fromSeconds (ns), false, false);
            }

            if (hasLength)
            {
                // Ende ziehen — Zeit am Ende einfuegen oder entfernen, alles
                // dahinter rueckt mit (keine Luecken im Arrangement)
                double base = hasStart ? ns : oldStart;
                double ne = base + newLength;

                if (! (newLength >= 0.01))
                    return resultObj (false, { { "error", "section must be at least 0.01 s" } });

                if (ne > oldEnd)
                    insertTime (*ctx.edit, oldEnd, ne - oldEnd);
                else if (ne < oldEnd)
                    removeTime (*ctx.edit, ne, oldEnd);

                section->setEnd (te::TimePosition::fromSeconds (ne), false);
            }
        }

        // Angefasst = gewollt: eine Auto-Section faellt danach nicht mehr
        // von selbst weg (retractAutoSection)
        section->state.removeProperty (autoSectionProp, nullptr);

        applySectionTempos (*ctx.edit);
        keepOffsetBeats (*ctx.edit, offsets);
        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("name", name);

        if (newLabel.isNotEmpty())
            params->setProperty ("label", newLabel);

        if (! bpm.isVoid())
            params->setProperty ("bpm", bpm);

        if (! timeSig.isVoid())
            params->setProperty ("timeSig", timeSig);

        if (! ramp.isVoid())
            params->setProperty ("ramp", ramp);

        if (hasBars)
            params->setProperty ("bars", bars);

        if (hasStart)
            params->setProperty ("start", newStart);

        if (hasLength && ! hasBars)
            params->setProperty ("length", newLength);

        if (colourParamStr.isNotEmpty())
            params->setProperty ("color", colourParamStr);

        recordNodeAction (ctx, "collect", "updateSection", juce::var (params));

        auto eff = tempoOf (*ctx.edit, *section);
        return resultObj (true, { { "name", newLabel.isNotEmpty() ? newLabel : name },
                                  { "start", section->getPosition().getStart().inSeconds() },
                                  { "end", section->getPosition().getEnd().inSeconds() },
                                  { "color", colourParam (section->getColour()) },
                                  { "bpm", eff.bpm }, { "timeSig", eff.timeSig() },
                                  { "bars", barsOf (*ctx.edit, *section, eff) },
                                  { "ramp", eff.ramp }, { "bpmEnd", eff.bpmEnd } });
    }

    // Section verschieben: sie wandert zwischen die Nachbarn (first, last
    // oder hinter eine genannte), alle Sections liegen danach lueckenlos ab
    // 0 — ihre Laengen bleiben. Ihr Zeitbereich wandert mit (reorderTime):
    // Clips ueber einer Grenze, an der sich die Verschiebung aendert, werden
    // geteilt, jeder Teil wandert mit seiner Section, Automation auch.
    juce::var opMoveSection (DawdContext& ctx, const juce::var& cmd)
    {
        auto name = cmd.getProperty ("name", {}).toString();
        auto* moved = findSection (*ctx.edit, name);

        if (moved == nullptr)
            return resultObj (false, { { "error", "no such section: " + name } });

        auto rest = sortedSections (*ctx.edit);
        rest.erase (std::remove (rest.begin(), rest.end(), moved), rest.end());

        std::vector<te::MarkerClip*> newOrder = rest;
        auto position = cmd.getProperty ("position", cmd.hasProperty ("after") ? "after" : "last").toString();

        if (cmd.hasProperty ("after") && position != "after")
            return resultObj (false, { { "error", "after needs position 'after' (or leave position out) - got position '" + position + "'" } });

        if (position == "after")
        {
            auto after = cmd.getProperty ("after", {}).toString();
            auto* anchor = findSection (*ctx.edit, after);

            if (anchor == nullptr && after.isNotEmpty())
                return resultObj (false, { { "error", "no such section: " + after } });

            newOrder.clear();

            for (auto* sec : rest)
            {
                newOrder.push_back (sec);

                if (sec == anchor)
                    newOrder.push_back (moved);
            }

            // anchor fehlt in rest (== moved oder leer): die Reihenfolge
            // bleibt, moved haelt die Position (frueher lagen die anderen
            // dann ab 0 und ueberlappten moved — reorderTime braucht eine
            // lueckenlose Abdeckung)
            if (std::find (newOrder.begin(), newOrder.end(), moved) == newOrder.end())
                newOrder = sortedSections (*ctx.edit);
        }
        else if (position == "first")
        {
            newOrder.insert (newOrder.begin(), moved);
        }
        else if (position == "last")
        {
            // Fehlte bis 2026-10-29: die Section blieb stehen, die anderen
            // rueckten ab 0 nach und lagen ueber ihr (Praxistest 3)
            newOrder.push_back (moved);
        }
        else
        {
            return resultObj (false, { { "error", "position must be first, last or after (with after: <section name>)" } });
        }

        // Kompakt ab 0: Laengen bleiben, Start neu. Gemessen auf den
        // Original-Stand, dahinter (Material jenseits der letzten Section)
        // bleibt stehen.
        std::vector<TimeShift> shifts;
        double cum = 0.0, lastEnd = 0.0;

        for (auto* sec : newOrder)
        {
            auto pos = sec->getPosition();
            shifts.push_back ({ pos.getStart().inSeconds(), pos.getEnd().inSeconds(), cum - pos.getStart().inSeconds() });
            cum += pos.getLength().inSeconds();
            lastEnd = juce::jmax (lastEnd, pos.getEnd().inSeconds());
        }

        shifts.push_back ({ lastEnd, HUGE_VAL, 0.0 });
        const auto offsets = followTempoOffsets (*ctx.edit);
        reorderTime (*ctx.edit, shifts);

        for (size_t i = 0; i < newOrder.size(); ++i)
            if (std::abs (shifts[i].delta) >= 0.001)
                newOrder[i]->setStart (te::TimePosition::fromSeconds (shifts[i].from + shifts[i].delta), false, true);

        // Tempo reist mit der Section; geerbtes Tempo folgt der neuen
        // Reihenfolge (der Offset folgender Clips bleibt in Beats)
        applySectionTempos (*ctx.edit);
        keepOffsetBeats (*ctx.edit, offsets);
        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("name", name);
        params->setProperty ("position", position);

        if (position == "after")
            params->setProperty ("after", cmd.getProperty ("after", {}).toString());

        recordNodeAction (ctx, "collect", "moveSection", juce::var (params));

        return resultObj (true, { { "name", name },
                                  { "start", moved->getPosition().getStart().inSeconds() },
                                  { "end", moved->getPosition().getEnd().inSeconds() } });
    }

    // Section samt Inhalt kopieren (Strophe 2 aus Strophe 1): die Kopie
    // landet direkt hinter dem Original (oder hinter after), spaetere
    // Sections, Clips und Automation ruecken nach rechts (ein Clip ueber der
    // Einfuegestelle wird geteilt). Kopiert wird der Zeitbereich
    // (duplicateTime): die Teile aller Clips darin auf ihren Spuren/Lanes,
    // die Automation, die Marker (Name mit Suffix), Tempo/Takt der Section.
    juce::var opDuplicateSection (DawdContext& ctx, const juce::var& cmd)
    {
        auto srcName = cmd.getProperty ("name", {}).toString();
        auto* src = findSection (*ctx.edit, srcName);

        if (src == nullptr)
            return resultObj (false, { { "error", "no such section: " + srcName } });

        auto newName = cmd.getProperty ("newName", {}).toString().trim();

        if (newName.isEmpty())
            for (int n = 2; newName.isEmpty() || findSection (*ctx.edit, newName) != nullptr; ++n)
                newName = srcName + " " + juce::String (n);

        if (findSection (*ctx.edit, newName) != nullptr)
            return resultObj (false, { { "error", "section exists: " + newName } });

        auto* anchor = src;

        if (cmd.hasProperty ("after"))
        {
            auto after = cmd.getProperty ("after", {}).toString();
            anchor = findSection (*ctx.edit, after);

            if (anchor == nullptr)
                return resultObj (false, { { "error", "no such section: " + after } });
        }

        auto srcStart = src->getPosition().getStart().inSeconds();
        auto len = src->getPosition().getLength().inSeconds();
        auto insertAt = anchor->getPosition().getEnd().inSeconds();

        auto colour = src->getColour();
        auto bpm = src->state.getProperty ("dawdBpm"), timeSig = src->state.getProperty ("dawdTimeSig");
        auto ramp = src->state.getProperty ("dawdRamp");

        juce::StringArray markers;
        const auto offsets = followTempoOffsets (*ctx.edit);
        auto copied = duplicateTime (*ctx.edit, srcStart, srcStart + len, insertAt, &markers);
        auto* section = insertSection (*ctx.edit, newName, insertAt, insertAt + len, colour);

        if (section == nullptr)
            return resultObj (false, { { "error", "cannot insert section" } });

        if (! bpm.isVoid())
            section->state.setProperty ("dawdBpm", bpm, nullptr);

        if (! timeSig.isVoid())
            section->state.setProperty ("dawdTimeSig", timeSig, nullptr);

        if (! ramp.isVoid())
            section->state.setProperty ("dawdRamp", ramp, nullptr);

        applySectionTempos (*ctx.edit);
        keepOffsetBeats (*ctx.edit, offsets);
        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("name", srcName);
        params->setProperty ("newName", newName);
        recordNodeAction (ctx, "collect", "duplicateSection", juce::var (params));

        auto eff = tempoOf (*ctx.edit, *section);
        return resultObj (true, { { "name", newName }, { "start", insertAt }, { "end", insertAt + len },
                                  { "length", len }, { "bpm", eff.bpm }, { "timeSig", eff.timeSig() },
                                  { "bars", barsOf (*ctx.edit, *section, eff) }, { "clips", copied },
                                  { "markers", markers } });
    }

    // Section loeschen (Decision 2026-10-15, seit Undo rueckholbar): der
    // Zeitbereich der Section faellt weg (removeTime) — Clips darin ganz,
    // Clips ueber ihren Grenzen werden dort abgeschnitten, Automation darin
    // faellt weg; spaetere Sections, Clips und Automation ruecken nach links
    // (keine Luecke). Die letzte Section bleibt — ein Song ohne Section kann
    // nichts aufnehmen.
    juce::var opDeleteSection (DawdContext& ctx, const juce::var& cmd)
    {
        auto name = cmd.getProperty ("name", {}).toString();
        auto* section = findSection (*ctx.edit, name);

        if (section == nullptr)
            return resultObj (false, { { "error", "no such section: " + name } });

        if (sortedSections (*ctx.edit).size() < 2)
            return resultObj (false, { { "error", "cannot delete the only section" } });

        auto start = section->getPosition().getStart().inSeconds();
        auto end = section->getPosition().getEnd().inSeconds();
        int cut = 0;

        const auto offsets = followTempoOffsets (*ctx.edit);
        section->removeFromParent();
        auto removed = removeTime (*ctx.edit, start, end, &cut);
        applySectionTempos (*ctx.edit);
        keepOffsetBeats (*ctx.edit, offsets);
        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("name", name);
        params->setProperty ("clips", removed);
        params->setProperty ("cut", cut);
        recordNodeAction (ctx, "process", "deleteSection", juce::var (params));

        return resultObj (true, { { "name", name }, { "clips", removed }, { "cut", cut }, { "length", end - start } });
    }

    // ---- Marker (2026-10-25): benannte Zeitpunkte neben den Sections ----

    namespace
    {
        // Zeit aus time (Sekunden) oder bar (+ beat, 1-basiert ab Songanfang,
        // ueber die Tempospur). Muss im Song liegen (0 .. Ende der letzten
        // Section). Fehlertext oder "".
        juce::String markerTime (te::Edit& edit, const juce::var& cmd, double& t)
        {
            if (cmd.hasProperty ("time") && cmd.hasProperty ("bar"))
                return "give time or bar, not both";

            if (cmd.hasProperty ("beat") && ! cmd.hasProperty ("bar"))
                return "beat needs bar";

            if (cmd.hasProperty ("bar"))
            {
                const auto bar = (int) cmd.getProperty ("bar", 0);
                const auto beat = (double) cmd.getProperty ("beat", 1.0);

                if (bar < 1 || beat < 1.0)
                    return "bar and beat count from 1";

                t = edit.tempoSequence.toTime (te::tempo::BarsAndBeats { bar - 1, te::BeatDuration::fromBeats (beat - 1.0) }).inSeconds();
            }
            else
            {
                t = (double) cmd.getProperty ("time", -1.0);
            }

            auto secs = sortedSections (edit);
            const double end = secs.empty() ? 0.0 : secs.back()->getPosition().getEnd().inSeconds();

            if (! (t >= 0.0) || t > end + 1.0e-6)
                return "time must lie in the song (0.." + juce::String (end, 3) + " s)";

            return {};
        }

        juce::var markerResult (te::Edit& edit, const juce::String& name)
        {
            for (auto& m : sortedMarkers (edit))
                if (m.name == name)
                {
                    auto r = describeMarker (edit, m);
                    r.getDynamicObject()->setProperty ("ok", true);
                    return r;
                }

            return resultObj (false, { { "error", "no such marker: " + name } });
        }
    }

    // Marker anlegen: name (frei, Default "Marker N", eindeutig), time
    // (Sekunden) oder bar/beat. Aendert keine Zeit — collect wie die Sections.
    juce::var opAddMarker (DawdContext& ctx, const juce::var& cmd)
    {
        auto name = cmd.getProperty ("name", {}).toString().trim();

        if (name.isEmpty())
            name = nextMarkerName (*ctx.edit);

        if (findMarker (*ctx.edit, name).isValid())
            return resultObj (false, { { "error", "marker exists: " + name } });

        if (! cmd.hasProperty ("time") && ! cmd.hasProperty ("bar"))
            return resultObj (false, { { "error", "time or bar needed" } });

        double t = 0.0;

        if (auto err = markerTime (*ctx.edit, cmd, t); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        addMarker (*ctx.edit, name, t);
        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("name", name);
        params->setProperty ("time", t);
        recordNodeAction (ctx, "collect", "addMarker", juce::var (params));

        return markerResult (*ctx.edit, name);
    }

    // Marker verschieben (time oder bar/beat) und/oder umbenennen (newName).
    // Eine Aktion je Marker (coalesce ueber name) — Ziehen bucht nicht jeden Schritt.
    juce::var opUpdateMarker (DawdContext& ctx, const juce::var& cmd)
    {
        auto name = cmd.getProperty ("name", {}).toString();
        auto marker = findMarker (*ctx.edit, name);

        if (! marker.isValid())
            return resultObj (false, { { "error", "no such marker: " + name } });

        auto newName = cmd.getProperty ("newName", {}).toString().trim();
        const bool move = cmd.hasProperty ("time") || cmd.hasProperty ("bar");

        if (newName.isEmpty() && ! move)
            return resultObj (false, { { "error", "nothing to update (time, bar/beat or newName)" } });

        if (newName.isNotEmpty() && newName != name && findMarker (*ctx.edit, newName).isValid())
            return resultObj (false, { { "error", "marker exists: " + newName } });

        double t = (double) marker.getProperty ("time", 0.0);

        if (move)
            if (auto err = markerTime (*ctx.edit, cmd, t); err.isNotEmpty())
                return resultObj (false, { { "error", err } });

        const auto finalName = newName.isNotEmpty() ? newName : name;
        setMarker (marker, finalName, t);
        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("name", name);

        if (finalName != name)
            params->setProperty ("newName", finalName);

        params->setProperty ("time", t);
        recordNodeAction (ctx, "collect", "updateMarker", juce::var (params), {}, true);

        return markerResult (*ctx.edit, finalName);
    }

    // Marker loeschen — process wie deleteSection (zaehlt im collect-Node als
    // Arrangieren, kein Kind-Node)
    juce::var opDeleteMarker (DawdContext& ctx, const juce::var& cmd)
    {
        auto name = cmd.getProperty ("name", {}).toString();

        if (! findMarker (*ctx.edit, name).isValid())
            return resultObj (false, { { "error", "no such marker: " + name } });

        removeMarker (*ctx.edit, name);
        saveCurrentEdit (ctx);

        auto params = new juce::DynamicObject();
        params->setProperty ("name", name);
        recordNodeAction (ctx, "process", "deleteMarker", juce::var (params));

        return resultObj (true, { { "name", name } });
    }
}
