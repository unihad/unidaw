#include "Presets.h"
#include "EngineHelpers.h"

namespace dawd
{
    namespace
    {
        juce::String pluginNameOf (te::Plugin& p)
        {
            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (&p))
                return ep->desc.name;

            return p.getName();
        }

        juce::String safeName (const juce::String& s)
        {
            return juce::File::createLegalFileName (s).trim();
        }

        juce::File home()
        {
            return juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        }

        // Preset-Dateien unter root sammeln; Kategorie = erster Unterordner
        void collectFiles (const juce::File& root, const juce::String& pattern, std::vector<PresetEntry>& out)
        {
            if (! root.isDirectory())
                return;

            for (const auto& entry : juce::RangedDirectoryIterator (root, true, pattern, juce::File::findFiles))
            {
                auto f = entry.getFile();
                auto rel = f.getRelativePathFrom (root).replaceCharacter ('\\', '/');
                PresetEntry e;
                e.name = f.getFileNameWithoutExtension();
                e.category = rel.containsChar ('/') ? rel.upToFirstOccurrenceOf ("/", false, false) : juce::String();
                e.source = f.hasFileExtension ("dawdpreset") ? "dawd" : "file";
                e.file = f;
                out.push_back (e);
            }
        }

        bool isSurgeSynth (te::Plugin& p)
        {
            auto n = pluginNameOf (p);
            return n.startsWithIgnoreCase ("Surge XT") && ! n.containsIgnoreCase ("Effects");
        }

        juce::String applyState (te::Plugin& p, const juce::MemoryBlock& state)
        {
            auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);

            if (ep == nullptr || ep->getAudioPluginInstance() == nullptr)
                return "plugin is not loaded";

            ep->getAudioPluginInstance()->setStateInformation (state.getData(), (int) state.getSize());
            ep->flushPluginStateToValueTree();
            return {};
        }

        // Roher Zustand der Komponente (Surge-Patch, u-he-h2p): JUCEs
        // VST3-Host erwartet seine XML-Huelle (IComponent), andere den Block
        juce::String applyComponentState (te::Plugin& p, const juce::MemoryBlock& raw)
        {
            auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);

            if (ep == nullptr || ep->desc.pluginFormatName != "VST3")
                return applyState (p, raw);

            juce::XmlElement state ("VST3PluginState");
            state.createNewChildElement ("IComponent")->addTextElement (raw.toBase64Encoding());
            juce::MemoryBlock blob;
            juce::AudioProcessor::copyXmlToBinary (state, blob);
            return applyState (p, blob);
        }

        bool isUhe (te::Plugin& p)
        {
            auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);
            return ep != nullptr && ep->desc.manufacturerName.containsIgnoreCase ("u-he");
        }

        std::vector<float> paramValues (te::Plugin& p)
        {
            std::vector<float> v;

            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (&p); ep != nullptr && ep->getAudioPluginInstance() != nullptr)
            {
                for (auto* prm : ep->getAudioPluginInstance()->getParameters())
                    v.push_back (prm->getValue());
            }
            else
            {
                for (auto* prm : p.getAutomatableParameters())
                    v.push_back (prm->getCurrentNormalisedValue());
            }

            return v;
        }

        // Ein stiller Block durchs Plugin (nur wenn es vorbereitet ist):
        // Surge XT laedt einen Patch erst im naechsten Audio-Block
        void processSilentBlock (te::Plugin& p)
        {
            auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);
            auto* pi = ep != nullptr ? ep->getAudioPluginInstance() : nullptr;

            if (pi == nullptr || pi->getSampleRate() <= 0.0 || pi->getBlockSize() <= 0)
                return;

            const int chans = juce::jmax (1, pi->getTotalNumInputChannels(), pi->getTotalNumOutputChannels());
            const int n = juce::jmin (pi->getBlockSize(), 256);
            juce::AudioBuffer<float> buf (chans, n);
            buf.clear();
            te::MidiMessageArray midi;
            ep->applyToBuffer (te::PluginRenderContext (&buf, 0, n, &midi, 0.0, te::TimeRange(), false, false, false, false));
        }

        bool isDexed (te::Plugin& p)
        {
            return dynamic_cast<te::ExternalPlugin*> (&p) != nullptr && pluginNameOf (p).equalsIgnoreCase ("Dexed");
        }

        // DX7-Bank (Cartridge): F0 43 0n 09 20 00, 32 gepackte Stimmen zu je
        // 128 Byte, Pruefsumme, F7 — der Name steht je Stimme in Byte 118..127
        bool readDx7Bank (const juce::File& f, juce::MemoryBlock& mb)
        {
            if (! f.loadFileAsData (mb) || mb.getSize() < 4104)
                return false;

            auto* d = static_cast<const juce::uint8*> (mb.getData());

            if (d[0] != 0xF0 || d[1] != 0x43 || d[3] != 0x09 || d[4] != 0x20 || d[5] != 0x00)
                return false;

            mb.setSize (4104);
            return true;
        }

        juce::StringArray dx7VoiceNames (const juce::MemoryBlock& bank)
        {
            juce::StringArray names;
            auto* d = static_cast<const juce::uint8*> (bank.getData());

            for (int v = 0; v < 32; ++v)
            {
                juce::String n;

                for (int i = 0; i < 10; ++i)
                {
                    // DX7-Zeichensatz wie Dexed ihn zeigt: 92 (Yen) -> Y,
                    // 126 -> >, 127 -> <
                    auto c = (int) (d[6 + v * 128 + 118 + i] & 0x7f);
                    c = c == 92 ? 'Y' : c == 126 ? '>' : c == 127 ? '<' : c;
                    n << juce::String::charToString ((juce::juce_wchar) (c >= 32 && c < 127 ? c : ' '));
                }

                names.add (n.trim());
            }

            return names;
        }

        // Stimme v der Bank (VMEM, 128 Byte gepackt) in Dexeds Edit-Puffer
        // (VCED, 155 Byte) entpacken — wie Dexeds Cartridge::unpackProgram
        void unpackDx7Voice (const juce::MemoryBlock& bank, int v, juce::uint8* out)
        {
            auto* b = static_cast<const juce::uint8*> (bank.getData()) + 6 + v * 128;

            for (int op = 0; op < 6; ++op)
            {
                auto* in = b + op * 17;
                auto* o = out + op * 21;

                for (int i = 0; i < 11; ++i)
                    o[i] = in[i] & 0x7f;                // EG-Raten/-Pegel, Breakpoint, Tiefen

                o[11] = in[11] & 3;                      // Kurve links
                o[12] = (in[11] >> 2) & 3;               // Kurve rechts
                o[13] = in[12] & 7;                      // Rate Scaling
                o[14] = in[13] & 3;                      // Amp Mod Sens
                o[15] = (in[13] >> 2) & 7;               // Key Velocity Sens
                o[16] = in[14] & 0x7f;                   // Output Level
                o[17] = in[15] & 1;                      // Oszillator-Modus
                o[18] = (in[15] >> 1) & 0x1f;            // Coarse
                o[19] = in[16] & 0x7f;                   // Fine
                o[20] = (in[12] >> 3) & 0x0f;            // Detune
            }

            for (int i = 0; i < 8; ++i)
                out[126 + i] = b[102 + i] & 0x7f;        // Pitch-EG

            out[134] = b[110] & 0x1f;                    // Algorithmus
            out[135] = b[111] & 7;                       // Feedback
            out[136] = (b[111] >> 3) & 1;                // Osc Key Sync
            out[137] = b[112] & 0x7f;                    // LFO Speed
            out[138] = b[113] & 0x7f;                    // LFO Delay
            out[139] = b[114] & 0x7f;                    // LFO PM Depth
            out[140] = b[115] & 0x7f;                    // LFO AM Depth
            out[141] = b[116] & 1;                       // LFO Key Sync
            out[142] = (b[116] >> 1) & 7;                // LFO Wave
            out[143] = (b[116] >> 4) & 7;                // LFO PM Sens
            out[144] = b[117] & 0x7f;                    // Transpose

            for (int i = 0; i < 10; ++i)
                out[145 + i] = b[118 + i] & 0x7f;        // Name
        }

        // Cartridges als Presets: Kategorie = Cartridge, Name = Stimme,
        // program = Stimme 0..31
        void collectDx7Banks (const juce::File& root, std::vector<PresetEntry>& out)
        {
            if (! root.isDirectory())
                return;

            for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*.syx", juce::File::findFiles))
            {
                juce::MemoryBlock bank;

                if (! readDx7Bank (entry.getFile(), bank))
                    continue;

                auto names = dx7VoiceNames (bank);

                for (int v = 0; v < 32; ++v)
                {
                    PresetEntry e;
                    e.name = names[v].isNotEmpty() ? names[v] : "Voice " + juce::String (v + 1);
                    e.category = entry.getFile().getFileNameWithoutExtension();
                    e.source = "file";
                    e.file = entry.getFile();
                    e.program = v;
                    out.push_back (e);
                }
            }
        }

        // Dexed: Cartridge im Zustand ersetzen (dexedState/dexedBlob, Attribut
        // sysex — als base64:sysex gespeichert), dann die Stimme waehlen
        juce::String loadDx7Voice (te::ExternalPlugin& ep, const juce::File& f, int voice)
        {
            juce::MemoryBlock bank;

            if (! readDx7Bank (f, bank))
                return "not a DX7 cartridge (32 voices): " + f.getFileName();

            if (voice < 0 || voice > 31)
                return "program must be 0..31 (voice of the cartridge)";

            if (ep.desc.pluginFormatName != "VST3" || ep.getAudioPluginInstance() == nullptr)
                return "DX7 cartridges load into Dexed VST3 only";

            juce::MemoryBlock blob;
            ep.getAudioPluginInstance()->getStateInformation (blob);
            auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize());
            juce::MemoryBlock comp;

            if (xml == nullptr || xml->getChildByName ("IComponent") == nullptr
                || ! comp.fromBase64Encoding (xml->getChildByName ("IComponent")->getAllSubText()))
                return "cannot read the Dexed state";

            auto dexed = juce::AudioProcessor::getXmlFromBinary (comp.getData(), (int) comp.getSize());
            auto* cart = dexed != nullptr ? dexed->getChildByName ("dexedBlob") : nullptr;
            juce::String sysexAttr;

            if (cart != nullptr)
                for (auto name : { "base64:sysex", "sysex" })
                    if (cart->hasAttribute (name))
                        sysexAttr = name;

            // Wert wie gelesen zurueckschreiben (mit oder ohne "base64:" davor)
            auto text = sysexAttr.isNotEmpty() ? cart->getStringAttribute (sysexAttr) : juce::String();
            auto prefix = text.startsWith ("base64:") ? juce::String ("base64:") : juce::String();
            juce::MemoryBlock old;

            if (sysexAttr.isEmpty() || ! old.fromBase64Encoding (text.substring (prefix.length())) || old.getSize() != 4104)
                return "unexpected Dexed state";

            cart->setAttribute (sysexAttr, prefix + bank.toBase64Encoding());

            // Edit-Puffer (program, 161 Byte: Stimme + Operator-Schalter) mit
            // der Stimme fuellen — ohne ihn verwirft Dexed den ganzen
            // Zustand; ein Programmwechsel ueber den Host kaeme ohne
            // laufendes Audio nicht an
            juce::String programAttr;

            for (auto name : { "base64:program", "program" })
                if (cart->hasAttribute (name))
                    programAttr = name;

            auto ptext = programAttr.isNotEmpty() ? cart->getStringAttribute (programAttr) : juce::String();
            auto pprefix = ptext.startsWith ("base64:") ? juce::String ("base64:") : juce::String();
            juce::MemoryBlock pgm;

            if (programAttr.isEmpty() || ! pgm.fromBase64Encoding (ptext.substring (pprefix.length())) || pgm.getSize() < 155)
                return "unexpected Dexed state";

            unpackDx7Voice (bank, voice, static_cast<juce::uint8*> (pgm.getData()));
            cart->setAttribute (programAttr, pprefix + pgm.toBase64Encoding());
            dexed->setAttribute ("currentProgram", voice);

            juce::MemoryBlock raw;
            juce::AudioProcessor::copyXmlToBinary (*dexed, raw);
            juce::XmlElement state ("VST3PluginState");
            state.createNewChildElement ("IComponent")->addTextElement (raw.toBase64Encoding());

            if (auto* ctl = xml->getChildByName ("IEditController"))
                state.addChildElement (new juce::XmlElement (*ctl));

            juce::MemoryBlock wrapped;
            juce::AudioProcessor::copyXmlToBinary (state, wrapped);

            return applyState (ep, wrapped);
        }
    }

    juce::String setPluginFile (te::Plugin& p, const juce::File& file, juce::String& kind)
    {
        auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);

        if (ep == nullptr || ep->getAudioPluginInstance() == nullptr)
            return "plugin has no file setting";

        if (! ep->desc.name.equalsIgnoreCase ("sfizz") || ep->desc.pluginFormatName != "VST3")
            return juce::String::fromUTF8 ("plugin has no file setting (supported: sfizz VST3 — .sfz)");

        if (! file.hasFileExtension ("sfz"))
            return "sfizz needs an .sfz file";

        // Zustand der Komponente aus der XML-Huelle des Hosts holen
        juce::MemoryBlock blob;
        ep->getAudioPluginInstance()->getStateInformation (blob);
        auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize());
        juce::MemoryBlock comp;

        if (xml == nullptr || xml->getChildByName ("IComponent") == nullptr
            || ! comp.fromBase64Encoding (xml->getChildByName ("IComponent")->getAllSubText()))
            return "cannot read the sfizz state";

        // SfizzVstState: uint64 Version, dann der Pfad als Str8 (int32 Laenge
        // inkl. Null, Zeichen), Rest unveraendert (Lautstaerke, Stimmen …)
        auto* d = static_cast<const char*> (comp.getData());

        if (comp.getSize() < 12)
            return "unexpected sfizz state";

        auto n = (size_t) juce::ByteOrder::littleEndianInt (d + 8);

        if (12 + n > comp.getSize())
            return "unexpected sfizz state";

        auto path = file.getFullPathName();
        juce::MemoryBlock out (d, 8);
        auto len = (juce::uint32) path.getNumBytesAsUTF8() + 1;
        auto le = juce::ByteOrder::swapIfBigEndian (len);
        out.append (&le, 4);
        out.append (path.toRawUTF8(), len);
        out.append (d + 12 + n, comp.getSize() - 12 - n);

        juce::XmlElement state ("VST3PluginState");
        state.createNewChildElement ("IComponent")->addTextElement (out.toBase64Encoding());

        if (auto* ctl = xml->getChildByName ("IEditController"))
            state.addChildElement (new juce::XmlElement (*ctl));

        juce::MemoryBlock wrapped;
        juce::AudioProcessor::copyXmlToBinary (state, wrapped);
        kind = "sfz";

        if (auto err = applyState (p, wrapped); err.isNotEmpty())
            return err;

        for (int i = 0; i < 5; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

        ep->flushPluginStateToValueTree();
        return {};
    }

    juce::File dawdPresetDir (DawdContext& ctx, te::Plugin& p)
    {
        return ctx.configDir.getChildFile ("presets").getChildFile (safeName (pluginNameOf (p)));
    }

    std::vector<PresetEntry> presetsFor (DawdContext& ctx, te::Plugin& p)
    {
        std::vector<PresetEntry> out;
        collectFiles (dawdPresetDir (ctx, p), "*.dawdpreset", out);

        if (auto* ep = dynamic_cast<te::ExternalPlugin*> (&p))
        {
            // Programme nur, wenn es wirklich eine Liste mit Namen gibt (viele
            // Plugins melden ein "Default", u-he 128 leere "Program N")
            std::vector<PresetEntry> programs;
            bool named = false;

            if (ep->getNumPrograms() > 1)
                for (int i = 0; i < ep->getNumPrograms(); ++i)
                {
                    PresetEntry e;
                    e.name = ep->getProgramName (i).trim();
                    e.source = "program";
                    e.program = i;
                    named = named || ! (e.name.isEmpty() || e.name.matchesWildcard ("Program *", true)
                                        || e.name.equalsIgnoreCase ("Default") || e.name.containsOnly ("0123456789 "));

                    if (e.name.isNotEmpty())
                        programs.push_back (e);
                }

            if (named)
                out.insert (out.end(), programs.begin(), programs.end());

            // u-he: Presets als .h2p unter ~/.u-he/<Plugin>/Presets|UserPresets/<Plugin>
            if (isUhe (p))
                for (auto sub : { "Presets", "UserPresets" })
                    collectFiles (home().getChildFile (".u-he").getChildFile (ep->desc.name).getChildFile (sub)
                                      .getChildFile (ep->desc.name), "*.h2p", out);

            if (ep->desc.pluginFormatName == "VST3")
                for (auto root : { home().getChildFile (".vst3/presets"), juce::File ("/usr/share/vst3/presets"),
                                   juce::File ("/usr/local/share/vst3/presets") })
                    collectFiles (root.getChildFile (safeName (ep->desc.manufacturerName)).getChildFile (safeName (ep->desc.name)),
                                  "*.vstpreset", out);
        }

        if (isSurgeSynth (p))
            for (auto base : { home().getChildFile (".local/share/surge-xt"), juce::File ("/usr/share/surge-xt"),
                               juce::File ("/usr/local/share/surge-xt") })
                for (auto sub : { "patches_factory", "patches_3rdparty" })
                    collectFiles (base.getChildFile (sub), "*.fxp", out);

        if (isSurgeSynth (p))
            collectFiles (home().getChildFile ("Documents/Surge XT/Patches"), "*.fxp", out);

        // Dexed: DX7-Cartridges als Presets (Kategorie = Cartridge)
        if (isDexed (p))
            for (auto root : { home().getChildFile (".local/share/DigitalSuburban/Dexed/Cartridges"),
                               home().getChildFile ("Documents/DigitalSuburban/Dexed/Cartridges") })
                collectDx7Banks (root, out);

        // sfizz: SFZ-Instrumente als "Presets" (Kategorie = Ordner)
        if (pluginNameOf (p).equalsIgnoreCase ("sfizz"))
            for (auto root : { home().getChildFile ("sfz"), home().getChildFile (".local/share/sfz"),
                               home().getChildFile ("Documents/SFZ") })
                collectFiles (root, "*.sfz", out);

        // Eigene zuerst, dann Programme (in ihrer Reihenfolge), dann Dateien
        // nach Kategorie und Name — stabil fuer offset-Blaettern
        auto rank = [] (const PresetEntry& e) { return e.source == "dawd" ? 0 : e.source == "program" ? 1 : 2; };
        std::stable_sort (out.begin(), out.end(), [&] (const PresetEntry& a, const PresetEntry& b)
        {
            if (rank (a) != rank (b))
                return rank (a) < rank (b);

            if (a.source == "program")
                return a.program < b.program;

            if (int c = a.category.compareIgnoreCase (b.category); c != 0)
                return c < 0;

            return a.name.compareIgnoreCase (b.name) < 0;
        });

        return out;
    }

    bool matchesCatalogPath (const juce::File& f, const juce::String& path)
    {
        if (path.isEmpty() || f == juce::File())
            return false;

        if (juce::File::isAbsolutePath (path))
            return f == juce::File (path);

        return f.getFullPathName().replaceCharacter ('\\', '/').endsWith ("/" + path);
    }

    juce::String loadPresetEntry (DawdContext& ctx, te::Plugin& p, const PresetEntry& entry, int& changed, bool* stateChanged)
    {
        // Eintrag aus einem Katalog mit relativem Pfad: installierte Datei suchen
        auto e = entry;

        if (e.source != "program" && e.file == juce::File() && e.catalogPath.isNotEmpty())
        {
            for (auto& x : presetsFor (ctx, p))
                if (matchesCatalogPath (x.file, e.catalogPath))
                {
                    e.file = x.file;
                    break;
                }

            if (e.file == juce::File())
                return "preset file not installed: " + e.catalogPath;
        }

        auto before = paramValues (p);
        auto stateOf = [&p]
        {
            juce::MemoryBlock mb;

            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (&p); ep != nullptr && ep->getAudioPluginInstance() != nullptr)
                ep->getAudioPluginInstance()->getStateInformation (mb);

            return mb;
        };
        auto stateBefore = stateOf();
        juce::String err;

        if (e.source == "program")
        {
            auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);

            if (ep == nullptr || e.program < 0 || e.program >= ep->getNumPrograms())
                return "no such program";

            ep->setCurrentProgram (e.program, true);
            ep->flushPluginStateToValueTree();
        }
        else if (e.file.hasFileExtension ("dawdpreset"))
        {
            auto xml = juce::XmlDocument::parse (e.file);

            if (xml == nullptr || ! xml->hasTagName ("DAWDPRESET"))
                return "not a dawd preset: " + e.file.getFileName();

            if (xml->getStringAttribute ("plugin") != pluginNameOf (p))
                return "preset is for " + xml->getStringAttribute ("plugin") + ", not " + pluginNameOf (p);

            if (auto* ep = dynamic_cast<te::ExternalPlugin*> (&p))
            {
                juce::MemoryBlock mb;

                if (! mb.fromBase64Encoding (xml->getAllSubText().trim()))
                    return "broken preset: " + e.file.getFileName();

                err = applyState (*ep, mb);
            }
            else if (auto* child = xml->getFirstChildElement())
            {
                auto tree = juce::ValueTree::fromXml (*child);

                if (! tree.isValid())
                    return "broken preset: " + e.file.getFileName();

                // Die Id bleibt die des Plugins: Sampler & Co. kopieren beim
                // Restore alle Eigenschaften, auch die Id aus dem Preset —
                // die Rack-Verbindungen zeigten danach ins Leere, und nach
                // dem naechsten Laden der Edit war das Modul stumm
                // (Praxistest 1i: Drums nach jedem Node-Wechsel still)
                tree.setProperty (te::IDs::id, p.state.getProperty (te::IDs::id), nullptr);
                p.restorePluginStateFromValueTree (tree);
            }
            else
                return "broken preset: " + e.file.getFileName();
        }
        else if (e.file.hasFileExtension ("vstpreset"))
        {
            auto* ep = dynamic_cast<te::ExternalPlugin*> (&p);
            juce::MemoryBlock mb;

            if (ep == nullptr || ep->getAudioPluginInstance() == nullptr || ! e.file.loadFileAsData (mb))
                return "cannot read " + e.file.getFileName();

            // .vstpreset: 'VST3', Version, Klassen-Id (32), Offset der
            // Chunk-Liste (int64 LE); dort 'List', Anzahl, je Eintrag Id,
            // Offset, Groesse. 'Comp' = Zustand der Komponente, 'Cont' = des
            // Controllers — JUCEs VST3-Host nimmt beide als XML-Zustand
            auto* d = static_cast<const char*> (mb.getData());
            auto size = (juce::int64) mb.getSize();

            if (size < 48 || juce::String (d, 4) != "VST3")
                return "not a vstpreset: " + e.file.getFileName();

            auto listAt = (juce::int64) juce::ByteOrder::littleEndianInt64 (d + 40);

            if (listAt < 48 || listAt + 8 > size || juce::String (d + listAt, 4) != "List")
                return "broken vstpreset: " + e.file.getFileName();

            auto count = (int) juce::ByteOrder::littleEndianInt (d + listAt + 4);
            juce::XmlElement state ("VST3PluginState");

            for (int i = 0; i < count; ++i)
            {
                auto at = listAt + 8 + (juce::int64) i * 20;

                if (at + 20 > size)
                    return "broken vstpreset: " + e.file.getFileName();

                auto id = juce::String (d + at, 4);
                auto off = (juce::int64) juce::ByteOrder::littleEndianInt64 (d + at + 4);
                auto len = (juce::int64) juce::ByteOrder::littleEndianInt64 (d + at + 12);

                if (off < 0 || len < 0 || off + len > size)
                    return "broken vstpreset: " + e.file.getFileName();

                if (id == "Comp" || id == "Cont")
                    state.createNewChildElement (id == "Comp" ? "IComponent" : "IEditController")
                        ->addTextElement (juce::MemoryBlock (d + off, (size_t) len).toBase64Encoding());
            }

            if (state.getChildByName ("IComponent") == nullptr)
                return "vstpreset without component state: " + e.file.getFileName();

            juce::MemoryBlock blob;
            juce::AudioProcessor::copyXmlToBinary (state, blob);
            err = applyState (p, blob);
        }
        else if (e.file.hasFileExtension ("fxp") && isSurgeSynth (p))
        {
            // fxChunkSetCustom: 56 Byte Kopf ('CcnK', Groesse, 'FPCh', …,
            // Name[28]), dann die Laenge (Big Endian) und der Patch selbst —
            // genau der Zustand, den Surge XT als Plugin-State nimmt
            juce::MemoryBlock mb;

            if (! e.file.loadFileAsData (mb) || mb.getSize() < 60)
                return "cannot read " + e.file.getFileName();

            auto* d = static_cast<const char*> (mb.getData());

            if (juce::String (d, 4) != "CcnK" || juce::String (d + 8, 4) != "FPCh")
                return "not a chunk preset: " + e.file.getFileName();

            auto size = (size_t) juce::ByteOrder::bigEndianInt (d + 56);

            if (size == 0 || 60 + size > mb.getSize())
                return "broken preset: " + e.file.getFileName();

            err = applyComponentState (p, juce::MemoryBlock (d + 60, size));
        }
        else if (e.file.hasFileExtension ("sfz"))
        {
            juce::String kind;
            err = setPluginFile (p, e.file, kind);
        }
        else if (e.file.hasFileExtension ("syx") && isDexed (p))
        {
            err = loadDx7Voice (*dynamic_cast<te::ExternalPlugin*> (&p), e.file, juce::jmax (0, e.program));
        }
        else if (e.file.hasFileExtension ("h2p") && isUhe (p))
        {
            // u-he speichert als Zustand "#pgm=<Datei>" und den Preset-Text
            juce::MemoryBlock mb;
            juce::MemoryBlock text;

            if (! e.file.loadFileAsData (text))
                return "cannot read " + e.file.getFileName();

            auto head = "#pgm=" + e.file.getFileName() + "\n";
            mb.append (head.toRawUTF8(), head.getNumBytesAsUTF8());
            mb.append (text.getData(), text.getSize());
            err = applyComponentState (p, mb);
        }
        else
            return "cannot load " + e.file.getFileName() + " into " + pluginNameOf (p);

        if (err.isNotEmpty())
            return err;

        // Plugins melden die neuen Werte zum Teil asynchron (VST3: Komponente
        // -> Controller -> Host) — kurz die Message-Loop laufen lassen, bis
        // sich nichts mehr tut, damit listParameters und die Edit-Datei den
        // geladenen Stand zeigen
        // Surge XT laedt den Patch erst im naechsten Audio-Block, sobald die
        // Engine einmal lief, und meldet die Werte danach ueber einen Timer —
        // bis zu 1 s auf die erste Aenderung warten (Praxistest 1c:
        // changedParameters 0, obwohl sich der Klang aenderte), danach nur,
        // bis drei Runden lang nichts mehr kommt
        //
        // Rechnet die Engine das Plugin gerade nicht (Transport steht),
        // kommt der Patch nie an — nach 100 ms ohne Aenderung stille Bloecke
        // durchschicken (applyToBuffer sperrt gegen den Audio-Thread); die
        // Werte meldet Surge erst in einem spaeteren Block an den Host
        //
        // Surge XT meldet die Werte des neuen Patches gar nicht (VST3: der
        // Controller wurde beim setState abgeglichen, bevor der Block den
        // Patch lud) — ohne Graph sofort Bloecke geben und den Controller
        // abgleichen (settlePluginNow), mit Graph alle 100 ms, solange sich
        // nichts zeigt (Todo 2026-10-27-surge-werte-nach-loadpreset)
        settlePluginNow (p);
        auto after = paramValues (p);
        int stable = 0;

        for (int i = 0; i < 50; ++i)
        {
            if (i >= 5 && i < 30 && after == before)
                processSilentBlock (p);

            if (i % 5 == 4 && after == before)
                syncPluginParameters (p);

            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            auto now = paramValues (p);
            stable = now == after ? stable + 1 : 0;
            after = now;

            if (stable >= 3 && after != before)
                break;
        }

        if (auto* ep = dynamic_cast<te::ExternalPlugin*> (&p))
            ep->flushPluginStateToValueTree();

        changed = 0;

        for (size_t i = 0; i < juce::jmin (before.size(), after.size()); ++i)
            if (std::abs (before[i] - after[i]) > 1.0e-4f)
                ++changed;

        if (stateChanged != nullptr)
            *stateChanged = changed > 0 || stateOf() != stateBefore;

        return {};
    }

    juce::String writePresetFile (te::Plugin& p, const juce::String& name, const juce::File& file)
    {
        juce::XmlElement xml ("DAWDPRESET");
        xml.setAttribute ("plugin", pluginNameOf (p));
        xml.setAttribute ("name", name);

        if (auto* ep = dynamic_cast<te::ExternalPlugin*> (&p))
        {
            if (ep->getAudioPluginInstance() == nullptr)
                return "plugin is not loaded";

            xml.setAttribute ("format", ep->desc.pluginFormatName);
            juce::MemoryBlock mb;
            ep->getAudioPluginInstance()->getStateInformation (mb);
            xml.addTextElement (mb.toBase64Encoding());
        }
        else
        {
            p.flushPluginStateToValueTree();
            xml.setAttribute ("format", "builtin");

            if (auto state = p.state.createXml())
                xml.addChildElement (state.release());
        }

        if (! xml.writeTo (file))
            return "cannot write " + file.getFullPathName();

        return {};
    }

    juce::String savePresetFile (DawdContext& ctx, te::Plugin& p, const juce::String& name,
                                 const juce::String& category, juce::File& file)
    {
        if (safeName (name).isEmpty())
            return "need a name";

        auto dir = dawdPresetDir (ctx, p);

        if (category.isNotEmpty())
            dir = dir.getChildFile (safeName (category));

        if (! dir.createDirectory())
            return "cannot create " + dir.getFullPathName();

        file = dir.getChildFile (safeName (name) + ".dawdpreset");
        return writePresetFile (p, name, file);
    }
}
