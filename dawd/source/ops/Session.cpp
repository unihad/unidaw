#include "Ops.h"
#include "../EngineHelpers.h"
#include "../OpRegistry.h"

#include "DawdAbout.h"   // generiert (CMakeLists.txt): Version, Quellcode-Link, Drittlizenzen

namespace dawd
{
    // Lebenszeichen und Uhr: Agenten haben keine — fuer Timeboxen die
    // Ortszeit (ISO) und Millisekunden seit 1970
    juce::var opPing (DawdContext&, const juce::var&)
    {
        auto now = juce::Time::getCurrentTime();
        return resultObj (true, { { "time", now.toISO8601 (true) }, { "epochMs", now.toMilliseconds() } });
    }

    // Selbstbeschreibung fuer Agenten: Name, Familie, Mutation — generiert aus
    // der Registry, damit neue Ops hier nichts nachzupflegen brauchen.
    juce::var opListOps (DawdContext&, const juce::var&)
    {
        juce::Array<juce::var> ops;

        for (auto& def : opTable())
            ops.add (resultObj (true, { { "name", def.name },
                                        { "kind", def.kind },
                                        { "mutating", def.mutating } }));

        return resultObj (true, { { "ops", ops } });
    }

    // Lizenz und Quellcode (AGPLv3 § 13: dawd bedient die Flow-UI uebers
    // Netz, Nutzer bekommen den Quellcode angeboten). sourceUrl setzt der
    // Build (-DDAWD_SOURCE_URL); leer heisst: dieser Build nennt keinen Link.
    // thirdParty ist THIRD_PARTY_LICENSES.md zur Bauzeit, leer ohne Datei.
    juce::var opAbout (DawdContext&, const juce::var&)
    {
        return resultObj (true, { { "name", "unidaw" },
                                  { "appVersion", UNIDAW_VERSION },
                                  { "license", "AGPL-3.0" },
                                  { "licenseName", "GNU Affero General Public License, version 3" },
                                  { "licenseUrl", "https://www.gnu.org/licenses/agpl-3.0.html" },
                                  { "sourceUrl", UNIDAW_SOURCE_URL },
                                  { "thirdParty", juce::String::fromUTF8 ((const char*) dawdThirdPartyLicenses) } });
    }

    juce::var opNewEdit (DawdContext& ctx, const juce::var& cmd)
    {
        // Takes gehoeren zum Edit, in dem sie aufgenommen wurden
        ctx.recordedTakes.clear();

        auto path = cmd.getProperty ("path", {}).toString();

        ctx.deletedClips.clear();

        if (path.isEmpty())
        {
            ctx.edit = te::Edit::createSingleTrackEdit (*ctx.engine);
        }
        else
        {
            auto editFile = juce::File (path);

            if (editFile.existsAsFile())
                ctx.edit = te::loadEditFromFile (*ctx.engine, editFile);
            else
            {
                // Neues Projekt ohne Spur — Spuren entstehen durch die
                // Aufnahme (numAudioTracks-Default ist 1)
                auto id = te::ProjectItemID::createNewID (te::ProjectID{});

                ctx.edit = te::Edit::createEdit (te::Edit::Options
                                                 {
                                                     *ctx.engine,
                                                     te::loadEditFromFile (*ctx.engine, {}, id),
                                                     id,
                                                     te::Edit::forEditing,
                                                     nullptr,
                                                     te::Edit::getDefaultNumUndoLevels(),
                                                     [editFile] { return editFile; },
                                                     {},
                                                     0
                                                 });
            }
        }

        if (ctx.edit != nullptr && path.isNotEmpty())
            bindEditResolver (ctx, juce::File (path));

        if (ctx.edit != nullptr)
        {
            ensureRouting (*ctx.edit);
            applySectionTempos (*ctx.edit);
            playWarpSources (*ctx.edit);
        }

        ctx.monitorOn = false;
        return resultObj (ctx.edit != nullptr, { { "path", path } });
    }

    juce::var opCreateTrack (DawdContext& ctx, const juce::var& cmd)
    {
        auto index = (int) (double) cmd.getProperty ("index", (double) userTracks (*ctx.edit).size());

        // Mit instrument ist die Spur eine MIDI-Spur, auch ohne type
        // (Praxistest 1k); nur type audio dazu ist ein Widerspruch
        const auto type = cmd.getProperty ("type", cmd.hasProperty ("instrument") ? "midi" : "audio").toString();

        // Vor dem Anlegen pruefen — ein Fehlschlag laesst keine Spur zurueck
        if (cmd.hasProperty ("instrument") && type != "midi")
            return resultObj (false, { { "index", index }, { "error", "instrument needs type midi" } });

        auto track = getOrInsertAudioTrackAt (*ctx.edit, index);

        // Typ-Auswahl der UI ("+ MIDI"): eine MIDI-Spur ist unverdrahtet und
        // stumm, bis sie durch ein Instrument geroutet wird (Routing.h);
        // eine Audio-Spur haengt an Main
        if (track != nullptr && type == "midi")
            setTrackType (*ctx.edit, *track, true);

        if (track == nullptr)
            return resultObj (false, { { "index", index }, { "error", "cannot create track" } });

        if (cmd.hasProperty ("name"))
        {
            if (auto err = checkTrackName (*ctx.edit, index, cmd.getProperty ("name", {}).toString()); err.isNotEmpty())
            {
                ctx.edit->deleteTrack (track);
                return resultObj (false, { { "index", index }, { "error", err } });
            }

            renameTrack (*ctx.edit, index, cmd.getProperty ("name", {}).toString());
        }

        // MIDI-Spur mit Instrument (Collect, "+ MIDI"): gleich hoerbar —
        // Spur -> Instrument -> Main, als collect-Aktion
        if (cmd.hasProperty ("instrument"))
        {
            auto r = addInstrumentToTrack (ctx, index, cmd);

            if (! (bool) r.getProperty ("ok", false))
            {
                // Spur wieder weg — der Befehl schlaegt ganz fehl
                ctx.edit->deleteTrack (track);
                r.getDynamicObject()->setProperty ("index", index);
                return r;
            }

            auto params = new juce::DynamicObject();
            params->setProperty ("track", index);
            params->setProperty ("type", "midi");
            params->setProperty ("instrument", r.getProperty ("instrument", {}));
            params->setProperty ("id", r.getProperty ("id", {}));
            recordNodeAction (ctx, "collect", "createTrack", juce::var (params));
            r.getDynamicObject()->setProperty ("index", index);
            return r;
        }

        return resultObj (true, { { "index", index } });
    }

    // Spur benennen ("Bass", "Akkorde"): die Gruppenspur, ihre Lanes heissen
    // mit ("Bass.1"); der Name gilt ueberall als Adresse statt des Index
    // (Praxistest 1d: alle Spuren hiessen "Track n"). Undo, keine Node-Aktion.
    juce::var opRenameTrack (DawdContext& ctx, const juce::var& cmd)
    {
        auto tracks = userTracks (*ctx.edit);
        auto index = trackIndexFor (*ctx.edit, cmd.getProperty ("track", {}).toString());

        if (index < 0 || index >= tracks.size())
            return resultObj (false, { { "error", "no such track: " + cmd.getProperty ("track", {}).toString() } });

        auto name = cmd.getProperty ("name", {}).toString().trim();
        auto* group = laneGroupOf (*tracks[index]) != nullptr ? laneGroupOf (*tracks[index]) : tracks[index];
        const int gi = tracks.indexOf (group);

        if (auto err = checkTrackName (*ctx.edit, gi, name); err.isNotEmpty())
            return resultObj (false, { { "error", err } });

        renameTrack (*ctx.edit, gi, name);
        saveCurrentEdit (ctx);
        juce::StringArray lanes;

        for (auto* t : userTracks (*ctx.edit))
            if (laneGroupOf (*t) == group)
                lanes.add (t->getName());

        return resultObj (true, { { "track", gi }, { "name", group->getName() }, { "lanes", juce::var (lanes) } });
    }

    juce::var opListDir (DawdContext&, const juce::var& cmd)
    {
        auto p = cmd.getProperty ("path", {}).toString();
        auto dir = juce::File (p.isEmpty()
                                   ? juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName()
                                   : p);

        if (! dir.isDirectory())
            return resultObj (false, { { "error", "no such dir: " + dir.getFullPathName() } });

        juce::Array<juce::var> dirs, projects;

        for (auto& c : dir.findChildFiles (juce::File::findDirectories, false, "*"))
        {
            if (c.getFileName().startsWithChar ('.'))
                continue;

            dirs.add (c.getFullPathName());

            // Projektordner (Song-Baum vorhanden) — das UI markiert sie und
            // oeffnet sie per Klick direkt; dirs bleibt die volle Liste
            if (c.getChildFile ("tree.json").existsAsFile())
                projects.add (c.getFullPathName());
        }

        // Dateien dazu, sortiert und begrenzt (Praxistest 1g/1h: Agenten
        // fanden ihre Renders und Stems nicht)
        constexpr int maxFiles = 200;
        auto found = dir.findChildFiles (juce::File::findFiles, false, "*");
        found.sort();
        juce::Array<juce::var> files;
        int total = 0;

        for (auto& f : found)
            if (! f.getFileName().startsWithChar ('.') && total++ < maxFiles)
                files.add (f.getFullPathName());

        auto res = resultObj (true, { { "path", dir.getFullPathName() },
                                      { "dirs", dirs },
                                      { "files", files },
                                      { "projects", projects },
                                      { "project", dir.getChildFile ("tree.json").existsAsFile() },
                                      { "parent", dir.getParentDirectory().getFullPathName() } });

        if (total > maxFiles)
            res.getDynamicObject()->setProperty ("filesTotal", total);

        return res;
    }

    juce::var opCreateDir (DawdContext&, const juce::var& cmd)
    {
        auto parent = juce::File (cmd.getProperty ("path", {}).toString());
        auto name = cmd.getProperty ("name", {}).toString().trim();

        if (name.isEmpty() || name.containsChar (juce::File::getSeparatorChar())
            || name.containsChar ('/') || ! parent.isDirectory())
            return resultObj (false, { { "error", "need path and plain name" } });

        auto dir = parent.getChildFile (name);

        if (dir.exists())
            return resultObj (false, { { "error", "already exists: " + dir.getFullPathName() } });

        if (! dir.createDirectory().wasOk())
            return resultObj (false, { { "error", "cannot create: " + dir.getFullPathName() } });

        return resultObj (true, { { "dir", dir.getFullPathName() } });
    }

    // Mehrere Befehle in einem Aufruf (Agenten sparen Roundtrips): jeder
    // laeuft ueber executeOp — Guards, Auto-Branch, Version und Event-Log
    // gelten je Befehl. Nicht transaktional: was vor einem Fehler lief,
    // bleibt. Der Graph baut erst nach dem Batch neu (der Neuaufbau-Timer
    // feuert zwischen den Befehlen nicht).
    juce::var opBatch (DawdContext& ctx, const juce::var& cmd)
    {
        constexpr int maxCommands = 200;
        auto commands = cmd.getProperty ("commands", {});

        if (! commands.isArray() || commands.size() == 0)
            return resultObj (false, { { "error", "commands must be a non-empty array" } });

        if (commands.size() > maxCommands)
            return resultObj (false, { { "error", "too many commands (max " + juce::String (maxCommands) + ")" } });

        auto stopOnError = (bool) cmd.getProperty ("stopOnError", true);
        juce::Array<juce::var> results;
        int failed = -1;

        for (int i = 0; i < commands.size(); ++i)
        {
            auto c = commands[i];
            auto op = c.getProperty ("op", {}).toString();
            juce::var r;

            if (failed >= 0 && stopOnError)
                r = resultObj (false, { { "skipped", true } });
            else if (op.isEmpty())
                r = resultObj (false, { { "error", "missing op" } });
            else if (op == "batch" || op == "quit")
                r = resultObj (false, { { "error", op + " is not allowed in a batch" } });
            else
                r = executeOp (ctx, op, c);

            if (auto* o = r.getDynamicObject())
                o->setProperty ("op", op);

            if (! (bool) r.getProperty ("ok", false) && failed < 0)
                failed = i;

            results.add (r);
        }

        return resultObj (failed < 0, { { "results", results }, { "failed", failed }, { "stopOnError", stopOnError } });
    }

    juce::var opQuit (DawdContext& ctx, const juce::var&)
    {
        flushAutosave (ctx);
        juce::MessageManager::getInstance()->stopDispatchLoop();
        return resultObj (true);
    }
}
