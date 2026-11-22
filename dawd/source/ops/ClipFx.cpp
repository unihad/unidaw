#include "Ops.h"
#include "../EngineHelpers.h"
#include "../Origins.h"

// Clip-FX (2026-10-26, Familie processing): einen Audio-Clip offline durch
// eine Kette von Modulen rechnen. processClip {track, name, plugins, tail?}
// rendert den Clip so, wie er spielt (Offset, Laenge, Gain, Tempo, Loop,
// rueckwaerts — ohne seine Blenden), durch die Module der Reihe nach, mit
// tail Sekunden Ausklang, nach media/<Clip>-fx.wav. Der Clip spielt danach
// diese Datei 1:1 (Tempo 1, kein Loop, Gain 0 dB), ist um tail laenger und
// behaelt seine Blenden — sie wirken nach den Effekten.
//
// Render: der vorhandene Weg (renderWithParams, nur dieser Clip ueber
// allowedClips wie Clip-Render und Freeze). Die Module bleiben, wo sie sind;
// fuer die Dauer des Renders laeuft der Clip ueber Abgriffe wie beim Freeze:
// ein Send am Anfang seiner Spur in das erste Modul, hinter dem Rack jedes
// Moduls ein Send ins naechste, hinter dem letzten auf Main; die eigenen
// Ausgaenge der Spur und der Module (Sends, Fader) sind so lange aus. Die
// Module klingen mit ihren aktuellen Einstellungen (Dry/Wet), VST3/LV2 in
// der Sandbox wie im Mix.
//
// Rueckweg: Kind DAWDFX am Clip — ORIGINAL (Kopie des Clips vor dem ersten
// processClip), source (dessen Datei), file (die gerechnete Datei), je
// Schritt STEP {plugins, names, tail}. clear: true holt den Clip wie vorher
// zurueck; die Sekunde x der gerechneten Datei ist die Sekunde x des
// Originals ab seinem Start — ein verschobener oder gekuerzter Clip kommt
// an derselben Stelle mit demselben Ausschnitt zurueck.
namespace dawd
{
    const juce::Identifier fxTreeId ("DAWDFX");

    namespace
    {
        const juce::Identifier originalId ("ORIGINAL"), stepId ("STEP");

        bool isNumber (const juce::var& v)  { return v.isDouble() || v.isInt() || v.isInt64(); }

        // Bus, auf dem ein Modul hoert (sein AuxReturn)
        int busOfModule (te::AudioTrack& m)
        {
            for (auto* p : m.pluginList.getPlugins())
                if (auto* r = dynamic_cast<te::AuxReturnPlugin*> (p))
                    return r->busNumber.get();

            return -1;
        }

        juce::ValueTree sendState (int bus)
        {
            juce::ValueTree send (te::IDs::PLUGIN);
            send.setProperty (te::IDs::type, te::AuxSendPlugin::xmlTypeName, nullptr);
            send.setProperty (te::IDs::busNum, bus, nullptr);
            return send;
        }

        // Abgriffe fuer die Dauer des Renders; der Destruktor raeumt auf
        struct ScopedTaps
        {
            ~ScopedTaps()
            {
                for (auto& t : taps)
                    if (t != nullptr)
                        t->deleteFromParent();

                for (auto* p : off)
                    p->setEnabled (true);

                for (auto& t : unmuted)
                    t->setMute (true);

                for (auto* c : unmutedClips)
                    c->setMuted (true);
            }

            // Send an Stelle index der Liste, alles andere ab from aus
            bool tap (te::AudioTrack& track, int index, int bus, int disableFrom)
            {
                auto plugins = track.pluginList.getPlugins();

                for (int i = disableFrom; i < plugins.size(); ++i)
                    if (plugins[i]->isEnabled())
                    {
                        plugins[i]->setEnabled (false);
                        off.add (plugins[i]);
                    }

                auto p = track.pluginList.insertPlugin (sendState (bus), index);

                if (auto* s = dynamic_cast<te::AuxSendPlugin*> (p.get()))
                    s->setGainDb (0.0f);

                taps.add (p);
                return p != nullptr;
            }

            juce::Array<te::Plugin::Ptr> taps;
            juce::Array<te::Plugin*> off;
            juce::Array<te::Track::Ptr> unmuted;
            juce::Array<te::Clip*> unmutedClips;
        };

        // Den Clip durch die Module nach dest rendern (32-Bit-Float, Rate des
        // Geraets), Bereich = Clip + tail. Fehlertext oder ""
        juce::String renderThrough (DawdContext& ctx, te::AudioTrack& track, te::AudioClipBase& ac,
                                    const juce::Array<te::AudioTrack*>& chain, double tail, const juce::File& dest)
        {
            auto& edit = *ctx.edit;
            te::Renderer::Parameters r (edit);
            r.destFile = dest;
            r.audioFormat = ctx.engine->getAudioFileFormatManager().getWavFormat();
            r.bitDepth = 32;
            r.sampleRateForAudio = freezeSampleRate (ctx);
            r.blockSizeForAudio = 512;
            r.canRenderInMono = false;
            r.checkNodesForAudio = false;
            r.ditheringEnabled = false;
            r.tracksToDo.setRange (0, (int) te::getAllTracks (edit).size(), true);
            r.allowedClips.add (&ac);
            const auto pos = ac.getPosition();
            r.time = { pos.getStart(), pos.getEnd() + te::TimeDuration::fromSeconds (tail) };

            ScopedTaps taps;

            // Die Blenden wirken nach den Effekten (sie bleiben am Clip):
            // fuer den Render aus, ebenso der automatische Crossfade
            const auto fadeIn = ac.getFadeIn(), fadeOut = ac.getFadeOut();
            const bool autoFade = ac.getAutoCrossfade();
            ac.setAutoCrossfade (false);
            ac.setFadeIn ({});
            ac.setFadeOut ({});

            const juce::ScopeGuard fades { [&]
            {
                ac.setFadeIn (fadeIn);
                ac.setFadeOut (fadeOut);
                ac.setAutoCrossfade (autoFade);
            } };

            // Stummer Clip, stumme Lane (anderer Take): klingen fuer den Render
            auto* group = laneGroupOf (track) != nullptr ? laneGroupOf (track) : &track;

            for (auto* t : { &track, group })
                if (t->isMuted (false) && ! taps.unmuted.contains (t))
                {
                    t->setMute (false);
                    taps.unmuted.add (t);
                }

            if (ac.isMuted())
            {
                ac.setMuted (false);
                taps.unmutedClips.add (&ac);
            }

            // Eingang: Anfang der Gruppenspur (eine Lane laeuft dort hinein)
            // in das erste Modul, die Ausgaenge der Spur aus
            if (! taps.tap (*group, 0, busOfModule (*chain[0]), 0))
                return "cannot tap the track";

            for (int i = 0; i < chain.size(); ++i)
            {
                auto& m = *chain[i];
                const int at = m.pluginList.indexOf (moduleRackOf (m)) + 1;
                const int bus = i + 1 < chain.size() ? busOfModule (*chain[i + 1]) : 0;   // 0 = Main

                if (at <= 0 || ! taps.tap (m, at, bus, at))
                    return "cannot tap module " + m.getName();
            }

            return renderWithParams (ctx, r);
        }

        juce::File fxFileFor (DawdContext& ctx, const juce::String& clipName)
        {
            auto editFile = ctx.edit->editFileRetriever ? ctx.edit->editFileRetriever() : juce::File();
            auto dir = editFile != juce::File() ? editFile.getSiblingFile ("media")
                                                : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("dawd-media");
            dir.createDirectory();
            return dir.getNonexistentChildFile (juce::File::createLegalFileName (clipName) + "-fx", ".wav", false);
        }

        void setClipFile (DawdContext& ctx, te::AudioClipBase& ac, const juce::File& f)
        {
            ac.getSourceFileReference().setToFile (f, te::SourceFileReference::PathStyle::chooseBest, true);
            ctx.engine->getAudioFileManager().validateFile (te::AudioFile (*ctx.engine, f), true);
        }

        // Was am Clip bleibt, wenn clear das Original zurueckholt: Name,
        // Mute und die Blenden (sie galten auch am gerechneten Clip)
        bool keptOnClear (const juce::Identifier& prop)
        {
            static const juce::Array<juce::Identifier> kept { te::IDs::name, te::IDs::mute, te::IDs::fadeIn, te::IDs::fadeOut,
                                                              te::IDs::fadeInType, te::IDs::fadeOutType, te::IDs::autoCrossfade,
                                                              te::IDs::colour, juce::Identifier ("dawdAutoCurve") };
            return kept.contains (prop);
        }

        // Gerechneter Clip zurueck zum Original: Eigenschaften und Kinder aus
        // ORIGINAL (Kinder wie LOOPINFO auf dem vorhandenen Kind, die
        // Engine haelt Verweise darauf), dann Lage: Sekunde x der Datei =
        // Sekunde x des Originals ab seinem Start
        void restoreOriginal (DawdContext& ctx, te::AudioClipBase& ac)
        {
            auto fx = ac.state.getChildWithName (fxTreeId);
            auto orig = fx.getChildWithName (originalId);
            const auto pos = ac.getPosition();
            const bool plain = std::abs (ac.getSpeedRatio() - 1.0) < 1.0e-9 && ! ac.getAutoTempo() && ! ac.isLooping() && ! ac.getIsReversed();
            const double s0 = juce::jmax (0.0, pos.getStart().inSeconds() - (plain ? pos.getOffset().inSeconds() : 0.0));
            const double origLen = (double) orig.getProperty (te::IDs::length, 0.0);

            ac.state.removeChild (fx, nullptr);

            // Die Engine rechnet beim Setzen mancher Eigenschaften andere um
            // (rueckwaerts spiegelt die Loop-Punkte, followTempo wandelt den
            // Loop): zuerst die Datei, am Ende Lage und Loop noch einmal
            ac.state.setProperty (te::IDs::source, orig.getProperty (te::IDs::source), nullptr);

            for (int i = ac.state.getNumProperties(); --i >= 0;)
                if (auto p = ac.state.getPropertyName (i); ! keptOnClear (p) && p != te::IDs::id && ! orig.hasProperty (p))
                    ac.state.removeProperty (p, nullptr);

            for (int i = 0; i < orig.getNumProperties(); ++i)
                if (auto p = orig.getPropertyName (i); ! keptOnClear (p) && p != te::IDs::id)
                    ac.state.setProperty (p, orig.getProperty (p), nullptr);

            for (auto child : orig)
            {
                if (auto now = ac.state.getChildWithName (child.getType()); now.isValid())
                    now.copyPropertiesAndChildrenFrom (child, nullptr);
                else
                    ac.state.appendChild (child.createCopy(), nullptr);
            }

            for (auto p : { te::IDs::start, te::IDs::length, te::IDs::offset, te::IDs::loopStart, te::IDs::loopLength,
                            te::IDs::loopStartBeats, te::IDs::loopLengthBeats })
                if (orig.hasProperty (p))
                    ac.state.setProperty (p, orig.getProperty (p), nullptr);

            // Ohne Kind im Original (DAWDWARP, wenn es nicht gewarpt war) weg
            if (! orig.getChildWithName (warpTreeId).isValid())
                ac.state.removeChild (ac.state.getChildWithName (warpTreeId), nullptr);

            // Wie warpClip clear: die Datei der Engine bekannt machen, sonst
            // spielt der Clip aus einem alten Lese-Stand
            setClipFile (ctx, ac, ac.getOriginalFile());

            // An die Stelle des Materials, dann auf den Ausschnitt, den der
            // gerechnete Clip zeigte (nicht ueber das Original hinaus)
            ac.setStart (te::TimePosition::fromSeconds (s0), false, true);

            if (! plain)
                return;

            const double from = pos.getStart().inSeconds(), to = juce::jmin (pos.getEnd().inSeconds(), s0 + origLen);

            if (from > s0 + 1.0e-6 && from < to)
                ac.setStart (pos.getStart(), true, false);

            if (to > ac.getPosition().getStart().inSeconds() + 1.0e-6)
                ac.setLength (te::TimeDuration::fromSeconds (to - ac.getPosition().getStart().inSeconds()), true);
        }
    }

    bool isClipProcessed (const te::AudioClipBase& ac)
    {
        return ac.state.getChildWithName (fxTreeId).isValid();
    }

    juce::var clipFxOf (const te::AudioClipBase& ac)
    {
        auto fx = ac.state.getChildWithName (fxTreeId);

        if (! fx.isValid())
            return {};

        auto list = [] (const juce::String& text, const char* sep)
        {
            juce::Array<juce::var> out;

            for (auto& x : juce::StringArray::fromTokens (text, sep, ""))
                out.add (x);

            return out;
        };

        juce::Array<juce::var> steps;

        for (auto s : fx)
            if (s.hasType (stepId))
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("plugins", list (s.getProperty ("plugins").toString(), ","));
                o->setProperty ("names", list (s.getProperty ("names").toString(), "\n"));
                o->setProperty ("tail", (double) s.getProperty ("tail", 0.0));
                steps.add (juce::var (o));
            }

        auto* o = new juce::DynamicObject();
        o->setProperty ("steps", steps);
        o->setProperty ("original", fx.getProperty ("source").toString());
        o->setProperty ("file", fx.getProperty ("file").toString());
        return juce::var (o);
    }

    // processClip {track, name, plugins: [Modul-Id oder Name …], tail?} |
    // {track, name, clear: true}
    juce::var opProcessClip (DawdContext& ctx, const juce::var& cmd)
    {
        auto& edit = *ctx.edit;
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();
        auto tracks = userTracks (edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        auto* track = tracks[trackIndex];
        auto* clip = findClipByName (*track, name);
        auto* ac = dynamic_cast<te::AudioClipBase*> (clip);

        if (clip != nullptr && ac == nullptr)
            return resultObj (false, { { "error", "not an audio clip: " + name + " - processClip works on audio (render a MIDI clip with render {track, name} first)" } });

        if (ac == nullptr)
            return resultObj (false, { { "error", "no such audio clip: " + name } });

        const bool clear = (bool) cmd.getProperty ("clear", false);

        if ((int) clear + (int) cmd.hasProperty ("plugins") != 1)
            return resultObj (false, { { "error", "need plugins: [module ids or names] or clear: true" } });

        auto* group = laneGroupOf (*track) != nullptr ? laneGroupOf (*track) : track;

        if (auto err = frozenTrackError (ctx, tracks.indexOf (group)); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        auto params = new juce::DynamicObject();
        params->setProperty ("track", trackIndex);
        params->setProperty ("name", name);

        if (clear)
        {
            if (! isClipProcessed (*ac))
                return resultObj (false, { { "error", "clip has no processing to clear" } });

            restoreOriginal (ctx, *ac);
            auto newSection = autoSection (edit);
            saveCurrentEdit (ctx);
            params->setProperty ("clear", true);
            recordNodeAction (ctx, "process", "processClip", juce::var (params));
            recordAutoSection (ctx, newSection);

            auto res = resultObj (true, { { "track", trackIndex }, { "name", name }, { "processed", false },
                                          { "file", ac->getOriginalFile().getFullPathName() },
                                          { "start", ac->getPosition().getStart().inSeconds() },
                                          { "length", ac->getPosition().getLength().inSeconds() } });

            if (! newSection.isVoid())
                res.getDynamicObject()->setProperty ("autoSection", newSection);

            return res;
        }

        // Kette: Module (Id oder Name), Audio-Effekte, jedes einmal
        auto* list = cmd.getProperty ("plugins", {}).getArray();

        if (list == nullptr || list->isEmpty())
            return resultObj (false, { { "error", "plugins must be a list of module ids or names (addPlugin {name} makes a module that is not wired)" } });

        juce::StringArray ids, names;
        juce::Array<te::AudioTrack*> chain;

        for (auto& v : *list)
        {
            auto ref = v.toString();
            auto id = v.isString() ? moduleIdFor (edit, ref) : juce::String();
            auto* m = id.isNotEmpty() ? findModuleTrack (edit, id) : nullptr;

            if (m == nullptr)
                return resultObj (false, { { "error", "no such plugin: " + ref } });

            auto* p = modulePlugin (*m);
            auto pname = moduleLabel (*m).isNotEmpty() ? moduleLabel (*m) : p != nullptr ? p->getName() : m->getName();
            auto cls = findRouteNode (edit, id).cls;

            if (cls != "audioFx")
                return resultObj (false, { { "error", id + " (" + pname + ") is " + (cls == "instrument" ? "an instrument" : "a MIDI effect")
                                                      + " - processClip needs audio effects" } });

            if (ids.contains (id))
                return resultObj (false, { { "error", id + " (" + pname + ") is twice in plugins - add a second module (addPlugin or linkPlugin)" } });

            if (moduleRackOf (*m) == nullptr)
                return resultObj (false, { { "error", "module " + id + " has no rack" } });

            ids.add (id);
            names.add (pname);
            chain.add (m);
        }

        const auto tailVar = cmd.getProperty ("tail", 0.0);
        const double tail = isNumber (tailVar) ? (double) tailVar : -1.0;

        if (tail < 0.0 || tail > 30.0)
            return resultObj (false, { { "error", "tail must be 0..30 (seconds of decay after the clip end)" } });

        auto dest = fxFileFor (ctx, name);
        const auto input = ac->getSourceFileReference().getFile();
        const auto started = juce::Time::getMillisecondCounterHiRes();

        if (auto err = renderThrough (ctx, *track, *ac, chain, tail, dest); err.isNotEmpty())
        {
            dest.deleteFile();
            return resultObj (false, { { "error", err } });
        }

        const double renderSec = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
        recordOrigins (dest, { input.getFullPathName(), unwarpedFile (*ac).getFullPathName() });   // Lizenz-Sperre

        // Rueckweg: Original nur beim ersten Schritt sichern
        const auto pos = ac->getPosition();
        auto fx = ac->state.getChildWithName (fxTreeId);

        if (! fx.isValid())
        {
            juce::ValueTree orig (originalId);
            orig.copyPropertiesAndChildrenFrom (ac->state, nullptr);
            fx = juce::ValueTree (fxTreeId);
            fx.setProperty ("source", unwarpedFile (*ac).getFullPathName(), nullptr);
            fx.appendChild (orig, nullptr);
        }
        else
        {
            ac->state.removeChild (fx, nullptr);
        }

        fx.setProperty ("file", dest.getFullPathName(), nullptr);
        juce::ValueTree step (stepId);
        step.setProperty ("plugins", ids.joinIntoString (","), nullptr);
        step.setProperty ("names", names.joinIntoString ("\n"), nullptr);
        step.setProperty ("tail", tail, nullptr);
        fx.appendChild (step, nullptr);

        // Der Clip spielt die Datei 1:1 — alles, was der Render schon
        // enthaelt, zurueck auf neutral (Blenden bleiben)
        ac->state.removeChild (ac->state.getChildWithName (warpTreeId), nullptr);

        if (ac->isLooping())
            ac->disableLooping();

        if (ac->getIsReversed())
            ac->setIsReversed (false);

        ac->setAutoTempo (false);
        ac->setAutoPitch (false);
        ac->setSpeedRatio (1.0);
        ac->setPitchChange (0.0f);
        ac->setGainDB (0.0f);
        ac->setPan (0.0f);
        setClipFile (ctx, *ac, dest);
        ac->setPosition ({ { pos.getStart(), pos.getEnd() + te::TimeDuration::fromSeconds (tail) }, te::TimeDuration() });
        ac->state.appendChild (fx, nullptr);

        auto newSection = autoSection (edit);
        saveCurrentEdit (ctx);

        juce::Array<juce::var> idList;

        for (auto& id : ids)
            idList.add (id);

        params->setProperty ("plugins", idList);
        params->setProperty ("tail", tail);
        recordNodeAction (ctx, "process", "processClip", juce::var (params));
        recordAutoSection (ctx, newSection);

        auto newPos = ac->getPosition();
        auto res = resultObj (true, { { "track", trackIndex }, { "name", name }, { "processed", true },
                                      { "plugins", idList }, { "tail", tail }, { "file", dest.getFullPathName() },
                                      { "original", fx.getProperty ("source").toString() },
                                      { "steps", fx.getNumChildren() - 1 },
                                      { "start", newPos.getStart().inSeconds() }, { "length", newPos.getLength().inSeconds() },
                                      { "renderSec", std::round (renderSec * 1000.0) / 1000.0 } });

        if (! newSection.isVoid())
            res.getDynamicObject()->setProperty ("autoSection", newSection);

        return res;
    }
}
