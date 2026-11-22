#include "Ops.h"

#include <array>

// Audio-Einstellungen (audioSettings): Rate und Bit-Tiefe neuer Aufnahmen
// gehoeren dem Song (tree.json) — alle Takes eines Songs haben dieselbe Rate;
// die Puffergroesse gehoert dem Geraet bzw. Live-Betrieb (audio.json im
// --config-Ordner). Unter JACK (UNIDAW_AUDIO=jack, pipewire-jack) bestimmt der
// PipeWire-Graph Rate und Block: dawd stellt ihn per pw-metadata
// clock.force-rate / clock.force-quantum um (gilt fuer den ganzen Rechner,
// solange dawd laeuft) und oeffnet das Geraet neu — JUCEs JACK-Geraet liest
// Rate und Block nur beim Oeffnen. Unter ALSA (PipeWire-ALSA-Plugin, Test-
// Suite) aendert dawd nur das eigene Geraete-Setup, nichts Globales.
namespace dawd
{
    namespace
    {
        constexpr std::array<int, 5> sampleRates { 44100, 48000, 88200, 96000, 192000 };   // UC: 176,4 kHz kaputt
        constexpr std::array<int, 6> bufferSizes { 32, 64, 128, 256, 512, 1024 };
        constexpr std::array<int, 3> bitDepths { 16, 24, 32 };

        template <size_t N>
        bool allowed (const std::array<int, N>& a, int v)
        {
            return std::find (a.begin(), a.end(), v) != a.end();
        }

        template <size_t N>
        juce::var listVar (const std::array<int, N>& a)
        {
            juce::Array<juce::var> l;

            for (auto v : a)
                l.add (v);

            return l;
        }

        juce::AudioDeviceManager& adm (DawdContext& ctx)
        {
            return ctx.engine->getDeviceManager().deviceManager;
        }

        bool isJack (DawdContext& ctx)
        {
            return ctx.audioOpen && adm (ctx).getCurrentAudioDeviceType() == "JACK";
        }

        juce::File settingsFile (DawdContext& ctx)
        {
            return ctx.engine->getPropertyStorage().getAppPrefsFolder().getChildFile ("audio.json");
        }

        // pw-metadata -n settings 0 <key> <value>; value 0 loest die Vorgabe
        bool pwForce (const juce::String& key, int value)
        {
            juce::ChildProcess p;

            if (! p.start (juce::StringArray { "pw-metadata", "-n", "settings", "0", key, juce::String (value) }))
                return false;

            p.waitForProcessToFinish (3000);
            return p.getExitCode() == 0;
        }

        // Geraet neu oeffnen, bis es die gewuenschte Rate/Blockgroesse meldet
        // (PipeWire stellt den Treiber nach force-* erst um); 0 = egal
        void reopenJack (DawdContext& ctx, int rate, int block)
        {
            auto& m = adm (ctx);

            for (int i = 0; i < 30; ++i)
            {
                m.closeAudioDevice();

                if (i > 0)
                    juce::Thread::sleep (100);

                m.restartLastAudioDevice();
                auto* d = m.getCurrentAudioDevice();

                if (d == nullptr)
                    continue;

                if ((rate == 0 || (int) d->getCurrentSampleRate() == rate)
                    && (block == 0 || d->getCurrentBufferSizeSamples() == block))
                    return;
            }

            std::cerr << "[audio] JACK meldet nach dem Umstellen nicht " << rate << " Hz / " << block << " Frames\n";
        }

        // Bit-Tiefe der Aufnahmen auf alle Wave-Eingaenge (auch nach einem
        // Neuaufbau der Geraete und vor jeder Aufnahme)
        void applyBitDepth (DawdContext& ctx)
        {
            auto bits = ctx.tree ? ctx.tree->recordBitDepth : 24;

            for (auto* wi : ctx.engine->getDeviceManager().getWaveInputDevices())
                if (wi->getBitDepth() != bits)
                    wi->setBitDepth (bits);
        }

        // Rate des Songs und Puffer anwenden; "" = ok
        juce::String applyDevice (DawdContext& ctx)
        {
            if (! ctx.audioOpen)
                return {};

            auto rate = ctx.tree ? ctx.tree->sampleRate : 0;
            auto block = ctx.bufferSetting;

            if (isJack (ctx))
            {
                bool changed = false;

                if (rate != ctx.forcedRate)
                {
                    if (! pwForce ("clock.force-rate", rate))
                        return "pw-metadata failed (clock.force-rate)";

                    ctx.forcedRate = rate;
                    changed = true;
                }

                if (block != ctx.forcedQuantum)
                {
                    if (! pwForce ("clock.force-quantum", block))
                        return "pw-metadata failed (clock.force-quantum)";

                    ctx.forcedQuantum = block;
                    changed = true;
                }

                if (changed)
                    reopenJack (ctx, rate, block);
            }
            else if (adm (ctx).getCurrentAudioDevice() != nullptr)   // ohne Geraet: nur gespeichert
            {
                auto& m = adm (ctx);
                auto setup = m.getAudioDeviceSetup();
                auto old = setup;

                // 0 = zurueck auf das, was das Geraet beim Start hatte
                setup.sampleRate = rate > 0 ? rate : ctx.deviceRate;
                setup.bufferSize = block > 0 ? block : ctx.deviceBuffer;

                if (setup.sampleRate != old.sampleRate || setup.bufferSize != old.bufferSize)
                {
                    auto err = m.setAudioDeviceSetup (setup, true);

                    if (err.isNotEmpty())
                        return "audio device: " + err;
                }
            }

            applyBitDepth (ctx);
            return {};
        }

        juce::var info (DawdContext& ctx)
        {
            auto* d = ctx.audioOpen ? adm (ctx).getCurrentAudioDevice() : nullptr;
            auto rate = d ? d->getCurrentSampleRate() : 0.0;
            auto block = d ? d->getCurrentBufferSizeSamples() : 0;

            auto r = resultObj (true, {
                { "api", ! ctx.audioOpen ? "none" : isJack (ctx) ? "jack" : "alsa" },
                { "device", d ? d->getName() : juce::String() },
                { "sampleRate", rate },
                { "bufferSize", block },
                { "bufferMs", rate > 0 ? std::round (block * 10000.0 / rate) / 10.0 : 0.0 },
                { "bufferSetting", ctx.bufferSetting },
                { "bufferSizes", listVar (bufferSizes) },
                { "sampleRates", listVar (sampleRates) },
                { "bitDepths", listVar (bitDepths) } });

            auto* o = r.getDynamicObject();

            // Audio-Graph angelegt? Im Leerlauf gibt releaseIdleGraph ihn frei
            o->setProperty ("graphActive", ctx.edit != nullptr && ctx.edit->getTransport().isPlayContextActive());

            if (ctx.tree)
            {
                auto p = new juce::DynamicObject();
                p->setProperty ("sampleRate", ctx.tree->sampleRate);
                p->setProperty ("recordBitDepth", ctx.tree->recordBitDepth);
                o->setProperty ("project", juce::var (p));
            }

            return r;
        }

        juce::var fail (const juce::String& e) { return resultObj (false, { { "error", e } }); }
    }

    void audioStartup (DawdContext& ctx)
    {
        if (ctx.audioOpen)
        {
            auto setup = adm (ctx).getAudioDeviceSetup();
            ctx.deviceRate = setup.sampleRate;
            ctx.deviceBuffer = setup.bufferSize;
        }

        auto json = juce::JSON::parse (settingsFile (ctx));
        auto b = (int) json.getProperty ("bufferSize", 0);
        ctx.bufferSetting = allowed (bufferSizes, b) ? b : 0;

        if (auto err = applyDevice (ctx); err.isNotEmpty())
            std::cerr << "[audio] " << err << "\n";
    }

    void audioProjectChanged (DawdContext& ctx)
    {
        if (auto err = applyDevice (ctx); err.isNotEmpty())
            std::cerr << "[audio] " << err << "\n";
    }

    void audioShutdown (DawdContext& ctx)
    {
        // Nur loesen, was dawd selbst gesetzt hat
        if (ctx.forcedRate != 0)
            pwForce ("clock.force-rate", 0);

        if (ctx.forcedQuantum != 0)
            pwForce ("clock.force-quantum", 0);

        ctx.forcedRate = ctx.forcedQuantum = 0;
    }

    // audioSettings {bufferSize?, sampleRate?, recordBitDepth?} — ohne
    // Parameter nur lesen. sampleRate 0 = was das Geraet hat. Erst alles
    // pruefen, ein Fehler aendert nichts.
    juce::var opAudioSettings (DawdContext& ctx, const juce::var& cmd)
    {
        auto has = [&cmd] (const char* k) { return cmd.hasProperty (k); };
        auto b = (int) cmd.getProperty ("bufferSize", ctx.bufferSetting);
        auto rate = (int) cmd.getProperty ("sampleRate", ctx.tree ? ctx.tree->sampleRate : 0);
        auto bits = (int) cmd.getProperty ("recordBitDepth", ctx.tree ? ctx.tree->recordBitDepth : 24);

        if (has ("bufferSize") && b != 0 && ! allowed (bufferSizes, b))
            return fail ("bufferSize must be 0 (default) or one of 32, 64, 128, 256, 512, 1024");

        if ((has ("sampleRate") || has ("recordBitDepth")) && ! ctx.tree)
            return fail ("no tree (sampleRate and recordBitDepth belong to the song)");

        if (has ("sampleRate") && rate != 0 && ! allowed (sampleRates, rate))
            return fail ("sampleRate must be 0 (device) or one of 44100, 48000, 88200, 96000, 192000");

        if (has ("recordBitDepth") && ! allowed (bitDepths, bits))
            return fail ("recordBitDepth must be 16, 24 or 32");

        if (has ("bufferSize") && b != ctx.bufferSetting)
        {
            auto o = new juce::DynamicObject();
            o->setProperty ("bufferSize", b);

            if (! settingsFile (ctx).replaceWithText (juce::JSON::toString (juce::var (o))))
                return fail ("cannot write " + settingsFile (ctx).getFullPathName());

            ctx.bufferSetting = b;
        }

        if (ctx.tree && (has ("sampleRate") || has ("recordBitDepth")))
        {
            ctx.tree->sampleRate = rate;
            ctx.tree->recordBitDepth = bits;

            if (! ctx.tree->save())
                return fail ("cannot write tree.json");
        }

        if (auto err = applyDevice (ctx); err.isNotEmpty())
            return fail (err);

        return info (ctx);
    }
}
