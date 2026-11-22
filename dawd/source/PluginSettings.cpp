#include "PluginSettings.h"
#include <map>

namespace dawd
{
    namespace
    {
        // "reverbMix" -> "Reverb Mix"
        juce::String prettyId (const juce::String& id)
        {
            juce::String out;

            for (int i = 0; i < id.length(); ++i)
            {
                auto c = id[i];

                if (i == 0)
                    out << juce::CharacterFunctions::toUpperCase (c);
                else if (juce::CharacterFunctions::isUpperCase (c) || (juce::CharacterFunctions::isDigit (c) && ! juce::CharacterFunctions::isDigit (id[i - 1])))
                    out << ' ' << c;
                else
                    out << c;
            }

            return out;
        }

        double round4 (double v) { return std::round (v * 10000.0) / 10000.0; }

        PluginSetting choice (const juce::String& name, juce::StringArray options, juce::CachedValue<int>& cv, std::vector<int> values = {})
        {
            PluginSetting s;
            s.name = name;
            s.kind = "choice";
            s.options = options;
            auto vals = values.empty() ? [&] { std::vector<int> v; for (int i = 0; i < options.size(); ++i) v.push_back (i); return v; }() : values;
            s.get = [&cv, options, vals]
            {
                for (size_t i = 0; i < vals.size(); ++i)
                    if (vals[i] == cv.get())
                        return juce::var (options[(int) i]);

                return juce::var (cv.get());
            };
            s.set = [&cv, options, vals] (const juce::var& v) { cv = vals[(size_t) options.indexOf (v.toString())]; };
            return s;
        }

        PluginSetting integer (const juce::String& name, int min, int max, juce::CachedValue<int>& cv)
        {
            PluginSetting s;
            s.name = name;
            s.kind = "int";
            s.min = min;
            s.max = max;
            s.get = [&cv] { return juce::var (cv.get()); };
            s.set = [&cv] (const juce::var& v) { cv = (int) v; };
            return s;
        }

        PluginSetting number (const juce::String& name, double min, double max, const juce::String& unit, juce::CachedValue<float>& cv)
        {
            PluginSetting s;
            s.name = name;
            s.kind = "float";
            s.fmin = min;
            s.fmax = max;
            s.unit = unit;
            s.get = [&cv] { return juce::var (std::round ((double) cv.get() * 1000.0) / 1000.0); };
            s.set = [&cv] (const juce::var& v) { cv = (float) (double) v; };
            return s;
        }

        PluginSetting boolean (const juce::String& name, juce::CachedValue<bool>& cv)
        {
            PluginSetting s;
            s.name = name;
            s.kind = "bool";
            s.get = [&cv] { return juce::var (cv.get()); };
            s.set = [&cv] (const juce::var& v) { cv = (bool) v; };
            return s;
        }
    }

    juce::String parameterLabel (te::Plugin& plugin, te::AutomatableParameter& p)
    {
        auto name = p.getParameterName();
        int same = 0;

        for (auto* q : plugin.getAutomatableParameters())
            if (q->getParameterName() == name)
                ++same;

        if (same <= 1)
            return name;

        // Aus der Id ("reverbMix" -> "Reverb Mix"), wenn sie Buchstaben hat;
        // numerische Ids (VST3: "2") sagen nichts — dann "Name 2", "Name 3"
        // in der Reihenfolge (Dragonfly "Dry Level" neben dem der Engine)
        auto pretty = prettyId (p.paramID);

        if (pretty.isNotEmpty() && pretty != name && pretty.containsAnyOf ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"))
            return pretty;

        int nth = 0;

        for (auto* q : plugin.getAutomatableParameters())
        {
            if (q->getParameterName() == name)
                ++nth;

            if (q == &p)
                break;
        }

        return nth <= 1 ? name : name + " " + juce::String (nth);
    }

    juce::StringArray parameterLabels (te::Plugin& plugin)
    {
        auto params = plugin.getAutomatableParameters();
        juce::StringArray names, out;
        std::map<juce::String, int> count, seen;

        for (auto* p : params)
        {
            names.add (p->getParameterName());
            ++count[names[names.size() - 1]];
        }

        for (int i = 0; i < params.size(); ++i)
        {
            const auto& name = names[i];
            const int nth = ++seen[name];

            if (count[name] <= 1)
            {
                out.add (name);
                continue;
            }

            auto pretty = prettyId (params[i]->paramID);

            if (pretty.isNotEmpty() && pretty != name && pretty.containsAnyOf ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"))
                out.add (pretty);
            else
                out.add (nth <= 1 ? name : name + " " + juce::String (nth));
        }

        return out;
    }

    namespace
    {
        // Zahl aus einem Anzeigetext, k/m vor einer Einheit als Faktor
        // ("1.20 kHz" -> 1200, "500 ms" -> 0.5, "-INF dB" -> -1e9)
        bool parseNumber (const juce::String& t, double& out, bool scaleMilli = true)
        {
            auto s = t.trim();

            if (s.startsWithIgnoreCase ("-inf"))
                { out = -1.0e9; return true; }

            if (s.startsWithIgnoreCase ("inf") || s.startsWithIgnoreCase ("+inf"))
                { out = 1.0e9; return true; }

            int i = 0;

            if (s[i] == '+' || s[i] == '-')
                ++i;

            const int digitsStart = i;

            while (juce::CharacterFunctions::isDigit (s[i]) || s[i] == '.')
                ++i;

            if (i == digitsStart)
                return false;

            out = s.substring (0, i).getDoubleValue();
            auto unit = s.substring (i).trim();

            if (unit.length() >= 2 && juce::CharacterFunctions::isLetter (unit[1]))
            {
                if (unit[0] == 'k' || unit[0] == 'K')
                    out *= 1000.0;
                else if (scaleMilli && unit[0] == 'm' && unit[1] != 'i')   // ms, mV — nicht "min"
                    out *= 0.001;
            }

            return true;
        }
    }

    namespace
    {
        // Zahl im Anzeigetext in der angezeigten Einheit ("500 ms" -> 500,
        // "1.20 kHz" -> 1200 Hz) — das, was "real" fuer Agenten heisst
        bool displayNumber (const juce::String& t, double& out) { return parseNumber (t, out, false); }

        juce::String unitOf (const juce::String& text)
        {
            auto s = text.trim();
            int i = 0;

            while (i < s.length() && (juce::CharacterFunctions::isDigit (s[i]) || s[i] == '.' || s[i] == '-' || s[i] == '+'))
                ++i;

            auto u = s.substring (i).trim();
            return u.startsWithChar ('k') && u.length() > 1 && juce::CharacterFunctions::isLetter (u[1]) ? u.substring (1) : u;
        }
    }

    namespace
    {
        // Zeit-Parameter, deren Text zwischen ms und s wechselt (4OSC Amp
        // Decay: "1ms" .. "60s") — real gilt dort in ms (Praxistest 1f:
        // "real must be 1..60 s" bei Text "300ms"), auch wo der Wertebereich
        // in Sekunden zum Text passt (4OSC Filter-/Mod-Huellkurve 0..60,
        // Praxistest 1h P12; Decision 2026-10-24-einheiten-real)
        bool mixedTime (te::AutomatableParameter& p)
        {
            auto at = [&p] (float v) { return unitOf (p.valueToString (p.valueRange.convertFrom0to1 (v))); };
            const auto a = at (0.0f), b = at (1.0f);
            return (a == "ms" && b == "s") || (a == "s" && b == "ms");
        }

        // Zahl im Text in ms (fuer mixedTime)
        bool msNumber (const juce::String& t, double& out)
        {
            if (! parseNumber (t, out, true))
                return false;

            out *= 1000.0;
            return true;
        }

        // mixedTime mit Wertebereich in Sekunden (4OSC): real direkt umrechnen
        // — die Suche ueber den gerundeten Text traf die Kante ("50" -> "49ms")
        bool secondsInside (te::AutomatableParameter& p)
        {
            for (float v : { 0.0f, 0.5f, 1.0f })
            {
                const float x = p.valueRange.convertFrom0to1 (v);
                double ms = 0.0;

                if (! msNumber (p.valueToString (x), ms) || std::abs (ms / 1000.0 - x) > 0.011 * juce::jmax (0.001, std::abs ((double) x)))
                    return false;
            }

            return true;
        }

        // Anteil 0..1 (oder -1..1) als ganze Prozent im Text (4OSC Pan, Detune,
        // Pulse Width): real direkt umrechnen — die Suche ueber den gerundeten
        // Text traf die Kante ("-50" -> "-51%")
        bool percentInside (te::AutomatableParameter& p)
        {
            for (float v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                const float x = p.valueRange.convertFrom0to1 (v);
                const auto t = p.valueToString (x);
                double d = 0.0;

                if (! t.trim().endsWithChar ('%') || ! displayNumber (t, d) || std::abs (d - 100.0 * x) > 0.51)
                    return false;
            }

            return true;
        }

        // Text haengt nicht vom Wert ab (Surge-Makros "M1".."M8": valueToString
        // zeigt immer den aktuellen Wert): real/text lassen sich nicht
        // umrechnen — die Suche ueber den flachen Text setzte still 0
        // (Praxistest 1m P12, "M5: Distortion Gain"). value 0..1 wirkt.
        bool textIgnoresValue (te::AutomatableParameter& p)
        {
            auto textAt = [&p] (float v) { return p.valueToString (p.valueRange.convertFrom0to1 (v)).trim(); };
            const auto t0 = textAt (0.0f);
            return t0 == textAt (0.5f) && t0 == textAt (1.0f);
        }

        juce::String fixedTextError()
        {
            return juce::String::fromUTF8 ("its text shows only the current value, not the range — set it with value 0..1");
        }
    }

    bool realIsDisplay (te::AutomatableParameter& p)
    {
        if (mixedTime (p))
            return false;

        for (float v : { 0.0f, 0.5f, 1.0f })
        {
            const float x = p.valueRange.convertFrom0to1 (v);
            double d = 0.0;

            if (! displayNumber (p.valueToString (x), d) || std::abs (d - x) > 0.011 * juce::jmax (1.0, std::abs ((double) x)))
                return false;
        }

        return true;
    }

    juce::String valueForText (te::AutomatableParameter& p, const juce::String& text, float& normalised)
    {
        return valueForNumberText (p, text, normalised, true);
    }

    juce::String valueForReal (te::AutomatableParameter& p, double real, float& normalised)
    {
        if (realIsDisplay (p))
        {
            if (real < p.valueRange.start - 1.0e-6 || real > p.valueRange.end + 1.0e-6)
            {
                auto unit = p.getLabel().isNotEmpty() ? p.getLabel() : unitOf (p.getCurrentValueAsString());
                return "real must be " + juce::String (p.valueRange.start) + ".." + juce::String (p.valueRange.end) + (unit.isNotEmpty() ? " " + unit : juce::String());
            }

            normalised = p.valueRange.convertTo0to1 ((float) real);
            return {};
        }

        if (textIgnoresValue (p))
            return fixedTextError();

        auto textAt = [&p] (float v) { return p.valueToString (p.valueRange.convertFrom0to1 (v)); };
        double lo = 0.0, hi = 0.0;

        if (mixedTime (p))
        {
            msNumber (textAt (0.0f), lo);
            msNumber (textAt (1.0f), hi);

            if (real < juce::jmin (lo, hi) - 1.0e-6 || real > juce::jmax (lo, hi) + 1.0e-6)
                return "real must be " + juce::String (juce::jmin (lo, hi)) + ".." + juce::String (juce::jmax (lo, hi)) + " ms";

            if (secondsInside (p))
            {
                normalised = p.valueRange.convertTo0to1 ((float) (real / 1000.0));
                return {};
            }

            return valueForNumberText (p, juce::String (real) + " ms", normalised, true);
        }

        if (! displayNumber (textAt (0.0f), lo) || ! displayNumber (textAt (1.0f), hi))
            return "no numeric value (" + textAt (0.0f) + " .. " + textAt (1.0f) + juce::String::fromUTF8 (") — use text");

        if (real < juce::jmin (lo, hi) - 1.0e-6 || real > juce::jmax (lo, hi) + 1.0e-6)
            return "real must be " + juce::String (juce::jmin (lo, hi)) + ".." + juce::String (juce::jmax (lo, hi)) + " " + unitOf (textAt (1.0f));

        if (percentInside (p))
        {
            normalised = p.valueRange.convertTo0to1 ((float) (real / 100.0));
            return {};
        }

        return valueForNumberText (p, juce::String (real), normalised, false);
    }

    juce::String valueForNumberText (te::AutomatableParameter& p, const juce::String& text, float& normalised, bool scaleMilli)
    {
        auto textAt = [&p] (float v) { return p.valueToString (p.valueRange.convertFrom0to1 (v)); };
        auto parse = [scaleMilli] (const juce::String& t, double& out) { return parseNumber (t, out, scaleMilli); };
        double target = 0.0;

        if (textIgnoresValue (p))
            return fixedTextError();

        // Auswahl mit Woertern an den Raendern (LSP "High-pass filter mode":
        // off .. 36 dB/oct): "12 dB/oct" ist ein Optionsname, keine Zahl —
        // erst unten bei den Woertern suchen (Todo 2026-10-15-plugin-steckbriefe)
        double lo = 0.0, hi = 0.0;
        const bool numericText = parse (text, target);
        const bool numericEnds = parse (textAt (0.0f), lo) && parse (textAt (1.0f), hi);

        if (numericText && numericEnds)
        {
            const bool rising = hi >= lo;

            if (target < juce::jmin (lo, hi) - 1.0e-9 || target > juce::jmax (lo, hi) + 1.0e-9)
                return "text value out of range: " + textAt (0.0f) + " .. " + textAt (1.0f);

            float a = 0.0f, b = 1.0f;

            for (int i = 0; i < 30; ++i)
            {
                auto mid = (a + b) * 0.5f;
                double v = 0.0;
                parse (textAt (mid), v);

                if ((v < target) == rising)
                    a = mid;
                else
                    b = mid;
            }

            normalised = (a + b) * 0.5f;

            // Gestufte Anzeige (Diva Transpose: ganze Halbtoene): die Suche
            // endet an der Kante zur Zielstufe und traf je nach Rundung die
            // Stufe daneben ("0" -> "-1", "24" -> "23"; Praxistest 1h P11).
            // Zeigt eine Seite der Kante genau das Ziel: die Mitte der Stufe
            auto shows = [&] (float v)
            {
                double x = 0.0;
                return parse (textAt (v), x) && std::abs (x - target) <= 1.0e-9 * juce::jmax (1.0, std::abs (target));
            };

            for (float edge : { a, b })
            {
                if (! shows (edge))
                    continue;

                auto bound = [&] (float in, float out)   // Grenze zwischen Treffer und Nicht-Treffer
                {
                    if (shows (out))
                        return out;

                    for (int k = 0; k < 24; ++k)
                    {
                        auto mid = (in + out) * 0.5f;
                        (shows (mid) ? in : out) = mid;
                    }

                    return in;
                };

                normalised = (bound (edge, 0.0f) + bound (edge, 1.0f)) * 0.5f;
                break;
            }

            return {};
        }

        // Woerter (Auswahl-Parameter): den Text abtasten. Nicht den ersten
        // Treffer nehmen — das ist der Rand des Bereichs, und LV2-Enums
        // (LSP "Filter type": 0..11, ganzzahlig) schneiden ab: "Lo-pass" ab
        // Portwert 3.5 wird im Plugin zu 3 = Hi-shelf. Beide Raender genau
        // suchen und knapp ueber der Mitte setzen (60 %) — passt fuers
        // Abschneiden wie fuers Runden.
        auto matches = [&] (float v) { return textAt (v).trim().equalsIgnoreCase (text.trim()); };

        for (int i = 0; i <= 512; ++i)
        {
            auto v = (float) i / 512.0f;

            if (! matches (v))
                continue;

            int j = i;

            while (j < 512 && matches ((float) (j + 1) / 512.0f))
                ++j;

            auto edge = [&] (float in, float out)   // Grenze zwischen Treffer und Nicht-Treffer
            {
                for (int k = 0; k < 24; ++k)
                {
                    auto mid = (in + out) * 0.5f;
                    (matches (mid) ? in : out) = mid;
                }

                return in;
            };

            auto from = i > 0 ? edge (v, (float) (i - 1) / 512.0f) : 0.0f;
            auto to = j < 512 ? edge ((float) j / 512.0f, (float) (j + 1) / 512.0f) : 1.0f;
            normalised = from + 0.6f * (to - from);
            return {};
        }

        if (numericText)
            return "parameter text is not numeric (" + textAt (0.0f) + " .. " + textAt (1.0f) + ")";

        return "no value shows as '" + text + "'";
    }

    juce::var parameterShape (te::AutomatableParameter& p, int samples)
    {
        samples = juce::jlimit (2, 65, samples);
        auto textAt = [&p] (double v) { return p.valueToString (p.valueRange.convertFrom0to1 ((float) v)).trim(); };
        juce::Array<juce::var> texts;
        juce::StringArray distinct;
        std::vector<double> nums;
        bool numeric = true;

        for (int i = 0; i < samples; ++i)
        {
            auto v = (double) i / (samples - 1);
            auto t = textAt (v);
            texts.add (juce::Array<juce::var> { round4 (v), t });
            distinct.addIfNotAlreadyThere (t);
            double x = 0.0;

            if (parseNumber (t, x))
            {
                if (std::abs (x) < 1.0e8)   // -INF dB zaehlt als Zahl, nicht fuer die Skala
                    nums.push_back (x);
            }
            else
                numeric = false;
        }

        auto o = new juce::DynamicObject();
        o->setProperty ("texts", texts);

        if (! numeric)
        {
            // Woerter: Optionen fein abtasten (Auswahl-Parameter)
            juce::StringArray options;

            for (int i = 0; i <= 256; ++i)
                options.addIfNotAlreadyThere (textAt (i / 256.0));

            o->setProperty ("scale", options.size() <= 64 ? "choice" : "text");

            if (options.size() <= 64)
            {
                juce::Array<juce::var> opts;

                for (auto& x : options)
                    opts.add (x);

                o->setProperty ("options", opts);
            }
        }
        else if (nums.size() < 2)
            o->setProperty ("scale", "linear");
        else if (distinct.size() < samples && distinct.size() <= 8)
            o->setProperty ("scale", "stepped");
        else
        {
            auto lo = nums.front(), hi = nums.back(), mid = nums[nums.size() / 2];
            const bool logLike = lo > 0.0 && hi > 0.0 && std::abs (mid - std::sqrt (lo * hi)) < std::abs (mid - (lo + hi) * 0.5);
            o->setProperty ("scale", logLike ? "log" : "linear");
        }

        auto def = p.getDefaultValue();
        o->setProperty ("default", def.has_value() ? juce::var (round4 ((double) p.valueRange.convertTo0to1 (*def))) : juce::var());
        return juce::var (o);
    }

    namespace
    {
        // Namen, die ein Parameter in diesem Prozess schon hatte: Plugin und
        // paramID (bei externen Plugins der Index beim Plugin) -> Namen
        std::map<juce::String, juce::StringArray>& seenNames()
        {
            static std::map<juce::String, juce::StringArray> names;
            return names;
        }

        juce::String nameKey (te::Plugin& plugin, te::AutomatableParameter& p)
        {
            return plugin.getPluginType() + "|" + plugin.getName() + "|" + p.paramID;
        }

        void rememberName (te::Plugin& plugin, te::AutomatableParameter& p, const juce::String& name)
        {
            seenNames()[nameKey (plugin, p)].addIfNotAlreadyThere (name);
        }
    }

    void rememberParameterNames (te::Plugin& plugin)
    {
        // wie parameterLabel, ohne n^2 fuer Surge (776 Parameter)
        std::map<juce::String, int> count;
        auto params = plugin.getAutomatableParameters();

        for (auto* p : params)
            ++count[p->getParameterName()];

        for (auto* p : params)
        {
            auto name = p->getParameterName();
            rememberName (plugin, *p, count[name] <= 1 ? name : parameterLabel (plugin, *p));
        }
    }

    te::AutomatableParameter* findParameter (te::Plugin& plugin, const juce::String& name)
    {
        auto params = plugin.getAutomatableParameters();

        if (auto i = parameterLabels (plugin).indexOf (name); i >= 0)
            return params[i];

        // Eine Zahl ist der Index aus listParameters, nie die paramID: VST3-
        // Ids sind auch Zahlen, aber andere (Praxistest 1h: Surge "39" traf
        // Index 41, FX-Gain auf +24 dB)
        if (name.isNotEmpty() && name.containsOnly ("0123456789"))
            return params[name.getIntValue()];

        for (auto* p : params)
            if (p->getParameterName() == name || p->paramID == name)
                return p;

        // Fruehere Namen desselben Parameters (Praxistest 1m: Surge meldet nach
        // jedem Neuladen des Edits Rohnamen wie "FX A2 Param 5", erst nach dem
        // ersten Verarbeiten die echten wie "FX A2 Distortion - Drive" — ein
        // Auto-Node zwischen listParameters und setParameter genuegte)
        for (auto* p : params)
            if (auto it = seenNames().find (nameKey (plugin, *p)); it != seenNames().end() && it->second.contains (name))
                return p;

        return nullptr;
    }

    juce::var parameterVar (te::Plugin& plugin, te::AutomatableParameter& p, int index)
    {
        auto o = new juce::DynamicObject();
        const auto label = parameterLabel (plugin, p);
        rememberName (plugin, p, label);
        o->setProperty ("index", index);
        o->setProperty ("name", label);
        o->setProperty ("value", round4 ((double) p.getCurrentNormalisedValue()));
        o->setProperty ("text", p.getCurrentValueAsString());

        // real/min/max in der angezeigten Einheit — auch wo der Wertebereich
        // intern 0..1 ist (externe Plugins, Kompressor-Threshold als Faktor;
        // Praxistest 1d: "real must be 0..1" bei Dragonfly)
        auto textAt = [&p] (float v) { return p.valueToString (p.valueRange.convertFrom0to1 (v)); };
        auto inf = [] (double v) { return std::abs (v) >= 1.0e8 ? juce::var() : juce::var (round4 (v)); };
        double cur = 0.0, lo = 0.0, hi = 0.0;

        if (realIsDisplay (p))
        {
            o->setProperty ("real", round4 ((double) p.getCurrentValue()));
            o->setProperty ("unit", p.getLabel().isNotEmpty() ? p.getLabel() : unitOf (p.getCurrentValueAsString()));
            o->setProperty ("min", round4 ((double) p.valueRange.start));
            o->setProperty ("max", round4 ((double) p.valueRange.end));
        }
        else if (mixedTime (p) && msNumber (p.getCurrentValueAsString(), cur) && msNumber (textAt (0.0f), lo) && msNumber (textAt (1.0f), hi))
        {
            o->setProperty ("real", round4 (cur));
            o->setProperty ("unit", "ms");
            o->setProperty ("min", round4 (juce::jmin (lo, hi)));
            o->setProperty ("max", round4 (juce::jmax (lo, hi)));
        }
        else if (displayNumber (p.getCurrentValueAsString(), cur) && displayNumber (textAt (0.0f), lo) && displayNumber (textAt (1.0f), hi))
        {
            // Text ohne Bezug zum Wert (Surge-Makros): kein Bereich — min = max
            // sah aus wie ein fester Parameter (Praxistest 1m P12)
            const bool fixed = textIgnoresValue (p);
            o->setProperty ("real", inf (cur));
            o->setProperty ("unit", unitOf (textAt (1.0f)));
            o->setProperty ("min", fixed ? juce::var() : inf (juce::jmin (lo, hi)));
            o->setProperty ("max", fixed ? juce::var() : inf (juce::jmax (lo, hi)));
        }
        else
        {
            // Woerter ("Saw", "On"): kein Zahlenwert — value/text benutzen
            o->setProperty ("real", juce::var());
            o->setProperty ("unit", "");
            o->setProperty ("min", juce::var());
            o->setProperty ("max", juce::var());
        }

        return juce::var (o);
    }

    std::vector<PluginSetting> pluginSettings (te::Plugin& plugin)
    {
        std::vector<PluginSetting> out;

        // Chorus, Phaser: die Engine fuehrt ihre Regler nur als Zustand,
        // nicht als Parameter (Praxistest 1d: "Chorus macht nicht breiter" —
        // es liess sich gar nichts einstellen). Breite kommt aus Width und
        // Mix: Mix 0,5 laesst die Haelfte trocken und korreliert.
        if (auto* ch = dynamic_cast<te::ChorusPlugin*> (&plugin))
        {
            out.push_back (number ("Depth", 0.0, 20.0, "ms", ch->depthMs));
            out.push_back (number ("Speed", 0.05, 10.0, "Hz", ch->speedHz));
            out.push_back (number ("Width", 0.0, 1.0, "", ch->width));
            out.push_back (number ("Mix", 0.0, 1.0, "", ch->mixProportion));
            return out;
        }

        if (auto* ph = dynamic_cast<te::PhaserPlugin*> (&plugin))
        {
            out.push_back (number ("Depth", 0.0, 12.0, "oct", ph->depth));
            out.push_back (number ("Rate", 0.0, 10.0, "", ph->rate));
            out.push_back (number ("Feedback", 0.0, 0.99, "", ph->feedbackGain));
            return out;
        }

        if (auto* de = dynamic_cast<te::DelayPlugin*> (&plugin))
        {
            out.push_back (integer ("Length", 1, 2000, de->lengthMs));
            out.back().unit = "ms";
            return out;
        }

        // LPF/HPF: der Modus ist Zustand, kein Parameter (Praxistest 1g:
        // "bietet keinen Hochpass an")
        if (auto* lp = dynamic_cast<te::LowPassPlugin*> (&plugin))
        {
            PluginSetting s;
            s.name = "Mode";
            s.kind = "choice";
            s.options = { "lowpass", "highpass" };
            s.get = [lp] { return juce::var (lp->isLowPass() ? "lowpass" : "highpass"); };
            s.set = [lp] (const juce::var& v) { lp->mode = v.toString(); };
            out.push_back (s);
            return out;
        }

        if (auto* fo = dynamic_cast<te::FourOscPlugin*> (&plugin))
        {
            const juce::StringArray waves { "none", "sine", "triangle", "sawUp", "sawDown", "square", "random" };

            // Oszillator-Wellen nach Oscillator::Waves (tracktion_Oscillators.h:
            // none, sine, square, saw, triangle, noise) — die LFO-Liste oben
            // passte nicht: "square" spielte Rauschen, "triangle" Rechteck,
            // "sawDown" Dreieck, "random" nichts (2026-10-25)
            const juce::StringArray oscWaves { "none", "sine", "triangle", "sawUp", "square", "noise" };

            for (int i = 0; i < fo->oscParams.size(); ++i)
            {
                auto n = juce::String (i + 1);
                out.push_back (choice ("Wave " + n, oscWaves, fo->oscParams[i]->waveShapeValue, { 0, 1, 4, 3, 2, 5 }));
                out.push_back (integer ("Unison " + n, 1, 8, fo->oscParams[i]->voicesValue));
            }

            for (int i = 0; i < fo->lfoParams.size(); ++i)
            {
                auto n = juce::String (i + 1);
                out.push_back (choice ("LFO Wave " + n, waves, fo->lfoParams[i]->waveShapeValue));
                out.push_back (boolean ("LFO Sync " + n, fo->lfoParams[i]->syncValue));
            }

            out.push_back (choice ("Filter Type", { "none", "lowpass", "highpass", "bandpass", "notch" }, fo->filterTypeValue));
            out.push_back (choice ("Filter Slope", { "12", "24" }, fo->filterSlopeValue, { 12, 24 }));
            out.push_back (boolean ("Analog Amp", fo->ampAnalogValue));
            out.push_back (boolean ("Distortion On", fo->distortionOnValue));
            out.push_back (boolean ("Reverb On", fo->reverbOnValue));
            out.push_back (boolean ("Delay On", fo->delayOnValue));
            out.push_back (boolean ("Chorus On", fo->chorusOnValue));
            out.push_back (choice ("Voice Mode", { "mono", "legato", "poly" }, fo->voiceModeValue));
            out.push_back (integer ("Voices", 1, 32, fo->voicesValue));
        }

        return out;
    }

    juce::String checkSetting (const PluginSetting& s, const juce::var& in, juce::var& out)
    {
        if (s.kind == "choice")
        {
            if (! s.options.contains (in.toString()))
                return s.name + " must be one of: " + s.options.joinIntoString (", ");

            out = in.toString();
            return {};
        }

        if (s.kind == "bool")
        {
            // Als Text (setParameter texts): "true"/"on"/"1", "false"/"off"/"0"
            // (Praxistest 1n)
            if (in.isString())
            {
                auto t = in.toString().trim().toLowerCase();

                if (t == "true" || t == "on" || t == "1")
                    out = true;
                else if (t == "false" || t == "off" || t == "0")
                    out = false;
                else
                    return s.name + " must be true or false (or on/off, 1/0)";

                return {};
            }

            if (! (in.isBool() || in.isInt() || in.isDouble()))
                return s.name + " must be true or false (or on/off, 1/0)";

            out = (bool) in;
            return {};
        }

        auto range = s.kind == "float" ? juce::String (s.fmin) + ".." + juce::String (s.fmax) : juce::String (s.min) + ".." + juce::String (s.max);

        if (s.unit.isNotEmpty())
            range << " " << s.unit;

        // Zahl als Text (setParameter texts: {"Unison 1": "4"}, Praxistest
        // 1n), auch mit der eigenen Einheit ("7.5 ms")
        auto num = in;

        if (in.isString())
        {
            auto t = in.toString().trim();
            auto str = (s.unit.isNotEmpty() && t.endsWithIgnoreCase (s.unit) ? t.dropLastCharacters (s.unit.length()).trim() : t).toStdString();
            char* end = nullptr;
            auto d = std::strtod (str.c_str(), &end);

            if (str.empty() || *end != 0 || ! std::isfinite (d))
                return s.name + " must be a number " + range + " (got '" + t + "')";

            num = d;
        }

        if (s.kind == "float")
        {
            auto f = (double) num;

            if (! (num.isInt() || num.isDouble() || num.isInt64()) || f < s.fmin - 1.0e-9 || f > s.fmax + 1.0e-9)
                return s.name + " must be " + range;

            out = f;
            return {};
        }

        auto v = (int) (double) num;

        if (! (num.isInt() || num.isDouble() || num.isInt64()) || v < s.min || v > s.max)
            return s.name + " must be " + range;

        out = v;
        return {};
    }

    juce::var settingVar (const PluginSetting& s)
    {
        auto o = new juce::DynamicObject();
        o->setProperty ("name", s.name);
        o->setProperty ("kind", s.kind);
        o->setProperty ("value", s.get());

        if (s.kind == "choice")
        {
            juce::Array<juce::var> opts;

            for (auto& x : s.options)
                opts.add (x);

            o->setProperty ("options", opts);
        }
        else if (s.kind == "int")
        {
            o->setProperty ("min", s.min);
            o->setProperty ("max", s.max);
        }
        else if (s.kind == "float")
        {
            o->setProperty ("min", s.fmin);
            o->setProperty ("max", s.fmax);
        }

        if (s.unit.isNotEmpty())
            o->setProperty ("unit", s.unit);

        return juce::var (o);
    }
}
