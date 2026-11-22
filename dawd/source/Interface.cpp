#include "Interface.h"

#include <alsa/asoundlib.h>

#include <cmath>
#include <limits>

namespace dawd
{
    namespace
    {
        constexpr double minusInf = -std::numeric_limits<double>::infinity();

        // ── ALSA ────────────────────────────────────────────────────────

        // Sucht die Karte des Fireface UC (Name aus dem snd-usb-audio-Quirk)
        struct AlsaInterface : InterfaceBackend
        {
            int card = -1;
            juce::String cardToken, name;
            std::map<unsigned int, std::shared_ptr<const std::vector<double>>> dbByNumid;   // gilt fuer cardToken

            bool simulated() const override { return false; }

            juce::String token() override
            {
                int c = -1, found = -1;
                juce::String foundName;

                while (snd_card_next (&c) == 0 && c >= 0)
                {
                    char* n = nullptr;

                    if (snd_card_get_name (c, &n) == 0 && n != nullptr)
                    {
                        juce::String s (n);
                        free (n);

                        if (s.contains ("Fireface UC"))
                        {
                            found = c;
                            foundName = s;
                            break;
                        }
                    }
                }

                if (found < 0)
                {
                    card = -1;
                    cardToken = {};
                    dbByNumid.clear();
                    return {};
                }

                // USB-Bus/Geraetenummer: neu nach jedem Einstecken
                auto bus = juce::File ("/proc/asound/card" + juce::String (found) + "/usbbus")
                               .loadFileAsString().trim();
                auto t = juce::String (found) + ":" + bus;

                if (t != cardToken)
                    dbByNumid.clear();

                card = found;
                cardToken = t;
                name = foundName;
                return t;
            }

            juce::String deviceName() override { return name; }

            bool read (std::vector<IfControl>& out) override
            {
                out.clear();

                if (token().isEmpty())
                    return false;

                snd_ctl_t* h = nullptr;

                if (snd_ctl_open (&h, ("hw:" + juce::String (card)).toRawUTF8(), 0) < 0)
                    return false;

                snd_ctl_elem_list_t* list = nullptr;
                snd_ctl_elem_id_t* id = nullptr;
                snd_ctl_elem_info_t* info = nullptr;
                snd_ctl_elem_value_t* val = nullptr;
                snd_ctl_elem_list_malloc (&list);
                snd_ctl_elem_id_malloc (&id);
                snd_ctl_elem_info_malloc (&info);
                snd_ctl_elem_value_malloc (&val);
                bool ok = false;

                if (snd_ctl_elem_list (h, list) == 0)
                {
                    auto count = snd_ctl_elem_list_get_count (list);

                    if (snd_ctl_elem_list_alloc_space (list, count) == 0 && snd_ctl_elem_list (h, list) == 0)
                    {
                        ok = true;

                        for (unsigned int i = 0; i < snd_ctl_elem_list_get_used (list); ++i)
                        {
                            snd_ctl_elem_list_get_id (list, i, id);

                            if (snd_ctl_elem_id_get_interface (id) != SND_CTL_ELEM_IFACE_MIXER)
                                continue;

                            snd_ctl_elem_info_set_id (info, id);

                            if (snd_ctl_elem_info (h, info) < 0 || snd_ctl_elem_info_get_count (info) != 1)
                                continue;

                            auto type = snd_ctl_elem_info_get_type (info);

                            if (type != SND_CTL_ELEM_TYPE_INTEGER && type != SND_CTL_ELEM_TYPE_BOOLEAN)
                                continue;

                            snd_ctl_elem_value_set_id (val, id);

                            if (snd_ctl_elem_read (h, val) < 0)
                                continue;

                            IfControl c;
                            c.name = juce::String (snd_ctl_elem_id_get_name (id));
                            c.isBool = type == SND_CTL_ELEM_TYPE_BOOLEAN;

                            if (c.isBool)
                            {
                                c.max = 1;
                                c.value = snd_ctl_elem_value_get_boolean (val, 0) ? 1 : 0;
                            }
                            else
                            {
                                c.min = (int) snd_ctl_elem_info_get_min (info);
                                c.max = (int) snd_ctl_elem_info_get_max (info);
                                c.value = (int) snd_ctl_elem_value_get_integer (val, 0);
                                c.db = dbTable (h, id, info, c.min, c.max);
                            }

                            out.push_back (std::move (c));
                        }
                    }
                }

                snd_ctl_elem_value_free (val);
                snd_ctl_elem_info_free (info);
                snd_ctl_elem_id_free (id);
                snd_ctl_elem_list_free (list);
                snd_ctl_close (h);
                return ok;
            }

            // dB je Stufe aus der TLV-Beschreibung — einmal je Regler und
            // Einstecken gelesen; gleiche Beschreibungen teilen sich die Tabelle
            std::shared_ptr<const std::vector<double>> dbTable (snd_ctl_t* h, snd_ctl_elem_id_t* id,
                                                                snd_ctl_elem_info_t* info, int min, int max)
            {
                auto numid = snd_ctl_elem_id_get_numid (id);
                auto it = dbByNumid.find (numid);

                if (it != dbByNumid.end())
                    return it->second;

                std::shared_ptr<const std::vector<double>> table;

                if (snd_ctl_elem_info_is_tlv_readable (info))
                {
                    unsigned int tlv[64] = {};

                    if (snd_ctl_elem_tlv_read (h, id, tlv, sizeof (tlv)) == 0)
                    {
                        static std::map<std::string, std::shared_ptr<const std::vector<double>>> shared;
                        auto key = std::string ((const char*) tlv, sizeof (tlv))
                                 + std::to_string (min) + "/" + std::to_string (max);
                        auto& s = shared[key];

                        if (s == nullptr)
                        {
                            auto t = std::make_shared<std::vector<double>>();

                            for (int v = min; v <= max; ++v)
                            {
                                long db100 = 0;

                                if (snd_tlv_convert_to_dB (tlv, min, max, v, &db100) < 0)
                                {
                                    t.reset();
                                    break;
                                }

                                t->push_back (db100 <= SND_CTL_TLV_DB_GAIN_MUTE ? minusInf : db100 / 100.0);
                            }

                            s = t;
                        }

                        table = s;
                    }
                }

                dbByNumid[numid] = table;
                return table;
            }

            juce::String write (const std::vector<std::pair<juce::String, int>>& values) override
            {
                if (token().isEmpty())
                    return "no interface";

                snd_ctl_t* h = nullptr;

                if (snd_ctl_open (&h, ("hw:" + juce::String (card)).toRawUTF8(), 0) < 0)
                    return "cannot open interface";

                snd_ctl_elem_id_t* id = nullptr;
                snd_ctl_elem_info_t* info = nullptr;
                snd_ctl_elem_value_t* val = nullptr;
                snd_ctl_elem_id_malloc (&id);
                snd_ctl_elem_info_malloc (&info);
                snd_ctl_elem_value_malloc (&val);
                juce::String error;

                for (auto& [n, v] : values)
                {
                    snd_ctl_elem_id_clear (id);
                    snd_ctl_elem_id_set_interface (id, SND_CTL_ELEM_IFACE_MIXER);
                    snd_ctl_elem_id_set_name (id, n.toRawUTF8());
                    snd_ctl_elem_info_set_id (info, id);

                    if (snd_ctl_elem_info (h, info) < 0)
                    {
                        error = "no such control: " + n;
                        break;
                    }

                    snd_ctl_elem_value_clear (val);
                    snd_ctl_elem_value_set_id (val, id);

                    if (snd_ctl_elem_info_get_type (info) == SND_CTL_ELEM_TYPE_BOOLEAN)
                        snd_ctl_elem_value_set_boolean (val, 0, v != 0);
                    else
                        snd_ctl_elem_value_set_integer (val, 0, v);

                    if (auto err = snd_ctl_elem_write (h, val); err < 0)
                    {
                        error = "cannot write " + n + ": " + juce::String (snd_strerror (err));
                        break;
                    }
                }

                snd_ctl_elem_value_free (val);
                snd_ctl_elem_info_free (info);
                snd_ctl_elem_id_free (id);
                snd_ctl_close (h);
                return error;
            }

            bool meters (IfMeters& out) override
            {
                if (token().isEmpty())
                    return false;

                snd_ctl_t* h = nullptr;

                if (snd_ctl_open (&h, ("hw:" + juce::String (card)).toRawUTF8(), 0) < 0)
                    return false;

                snd_ctl_elem_value_t* val = nullptr;
                snd_ctl_elem_value_malloc (&val);
                bool ok = true;

                // Treiber: 24 Bit linear, 0x7fffff = 0 dBFS
                for (auto [n, dst] : { std::pair { "Level Meter Peak", &out.peak },
                                       std::pair { "Level Meter RMS", &out.rms } })
                {
                    snd_ctl_elem_value_clear (val);
                    snd_ctl_elem_value_set_interface (val, SND_CTL_ELEM_IFACE_MIXER);
                    snd_ctl_elem_value_set_name (val, n);

                    if (snd_ctl_elem_read (h, val) < 0)
                    {
                        ok = false;
                        break;
                    }

                    dst->resize (IfMeters::channels);

                    for (int i = 0; i < IfMeters::channels; ++i)
                        (*dst)[(size_t) i] = (float) snd_ctl_elem_value_get_integer (val, (unsigned int) i) / (float) 0x7fffff;
                }

                snd_ctl_elem_value_free (val);
                snd_ctl_close (h);
                return ok;
            }
        };

        // ── Simulation ─────────────────────────────────────────────────

        // Die Regler des Treibers mit denselben Namen, Bereichen,
        // dB-Stufen und Startwerten (kernel/0001-…patch in fireface-uc)
        struct SimInterface : InterfaceBackend
        {
            std::vector<IfControl> controls;
            IfMeters meterValues;   // Stille, bis simulateMeter etwas setzt
            bool present = true;
            int plugCount = 1;

            SimInterface()
            {
                meterValues.peak.assign (IfMeters::channels, 0.0f);
                meterValues.rms.assign (IfMeters::channels, 0.0f);
                powerOn();
            }

            static std::shared_ptr<const std::vector<double>> scale (int min, int max, double minDb, double step, bool mute)
            {
                auto t = std::make_shared<std::vector<double>>();

                for (int v = min; v <= max; ++v)
                    t->push_back (mute && v == min ? minusInf : minDb + (v - min) * step);

                return t;
            }

            void powerOn()
            {
                controls.clear();

                auto mic = scale (10, 65, 10.0, 1.0, false);
                auto line = scale (0, 36, 0.0, 0.5, false);

                // Ausgangspegel: 1-dB-Stufen bis -52 dB, darunter grober
                auto out = std::make_shared<std::vector<double>>();

                for (int v = 0; v <= 63; ++v)
                    out->push_back (v == 0 ? minusInf : v <= 4 ? -59.6 + v * 1.57 : -52.0 + (v - 5));

                // Matrix: 0 = stumm, 1 = -65 dB, 0,5-dB-Stufen bis +6 dB
                auto mix = scale (0, 143, -65.5, 0.5, true);

                auto add = [this] (const juce::String& n, int min, int max, int value,
                                   std::shared_ptr<const std::vector<double>> db, bool isBool = false)
                {
                    controls.push_back ({ n, isBool, min, max, value, std::move (db) });
                };

                add ("Mic 1 Capture Volume", 10, 65, 10, mic);
                add ("Mic 2 Capture Volume", 10, 65, 10, mic);
                add ("Line 3 Capture Volume", 0, 36, 0, line);
                add ("Line 4 Capture Volume", 0, 36, 0, line);
                add ("Mic 1 48V Phantom Power Switch", 0, 1, 0, nullptr, true);
                add ("Mic 2 48V Phantom Power Switch", 0, 1, 0, nullptr, true);
                add ("Line 3 Hi-Z Switch", 0, 1, 0, nullptr, true);
                add ("Line 4 Hi-Z Switch", 0, 1, 0, nullptr, true);
                add ("Line 3 PAD Switch", 0, 1, 0, nullptr, true);
                add ("Line 4 PAD Switch", 0, 1, 0, nullptr, true);

                for (int i = 1; i <= 6; ++i)
                    add ("Line Out " + juce::String (i) + " Playback Volume", 0, 63, 57, out);

                add ("Phones Left Playback Volume", 0, 63, 57, out);
                add ("Phones Right Playback Volume", 0, 63, 57, out);

                auto& outs = interfaceOutputNames();
                auto& ins = interfaceInputNames();

                for (int o = 0; o < 18; ++o)
                {
                    for (int i = 0; i < 18; ++i)
                        add (InterfaceModel::matrixName (outs[o], ins[i]), 0, 143, 0, mix);

                    for (int p = 0; p < 18; ++p)
                    {
                        auto on = p == o || (o == 6 && p == 0) || (o == 7 && p == 1);
                        add (InterfaceModel::matrixName (outs[o], "PCM " + juce::String (p + 1)), 0, 143, on ? 131 : 0, mix);
                    }

                    add (InterfaceModel::faderName (outs[o]), 0, 143, 131, mix);
                }
            }

            bool simulated() const override { return true; }
            juce::String token() override { return present ? "sim:" + juce::String (plugCount) : juce::String(); }
            juce::String deviceName() override { return "Fireface UC (simulated)"; }

            bool read (std::vector<IfControl>& out) override
            {
                if (! present)
                {
                    out.clear();
                    return false;
                }

                out = controls;
                return true;
            }

            juce::String write (const std::vector<std::pair<juce::String, int>>& values) override
            {
                if (! present)
                    return "no interface";

                for (auto& [n, v] : values)
                {
                    auto it = std::find_if (controls.begin(), controls.end(), [&] (auto& c) { return c.name == n; });

                    if (it == controls.end())
                        return "no such control: " + n;

                    it->value = juce::jlimit (it->min, it->max, v);
                }

                return {};
            }

            bool meters (IfMeters& out) override
            {
                if (! present)
                    return false;

                out = meterValues;
                return true;
            }

            void simulatePresent (bool p) override
            {
                if (p && ! present)
                {
                    ++plugCount;
                    powerOn();
                }

                present = p;
            }

            void simulateMeter (int channel, float peak, float rms) override
            {
                if (channel >= 0 && channel < IfMeters::channels)
                {
                    meterValues.peak[(size_t) channel] = peak;
                    meterValues.rms[(size_t) channel] = rms;
                }
            }
        };

        // Paarname zweier benachbarter Kanaele: "Phones L" + "Phones R" ->
        // "Phones", "Line 1" + "Line 2" -> "Line 1/2" (ungerade zuerst); "" = kein Paar
        juce::String pairName (const juce::String& a, const juce::String& b)
        {
            if (a.endsWith (" L") && b == a.dropLastCharacters (1) + "R")
                return a.dropLastCharacters (2);

            auto baseA = a.upToLastOccurrenceOf (" ", false, false);
            auto baseB = b.upToLastOccurrenceOf (" ", false, false);
            auto na = a.fromLastOccurrenceOf (" ", false, false);
            auto nb = b.fromLastOccurrenceOf (" ", false, false);

            if (baseA.isNotEmpty() && baseA == baseB && na.containsOnly ("0123456789") && nb.containsOnly ("0123456789"))
                if (na.getIntValue() % 2 == 1 && nb.getIntValue() == na.getIntValue() + 1)
                    return baseA + " " + na + "/" + nb;

            // Mic 1 / Mic 2 haben dieselbe Basis; Mic 2 / Line 3 nicht
            return {};
        }

        // Kanaele zu Paaren (oder einzeln) zusammenfassen
        std::vector<std::pair<juce::String, juce::StringArray>> groupPairs (const juce::StringArray& chans)
        {
            std::vector<std::pair<juce::String, juce::StringArray>> out;

            for (int i = 0; i < chans.size(); ++i)
            {
                if (i + 1 < chans.size())
                    if (auto p = pairName (chans[i], chans[i + 1]); p.isNotEmpty())
                    {
                        out.push_back ({ p, { chans[i], chans[i + 1] } });
                        ++i;
                        continue;
                    }

                out.push_back ({ chans[i], { chans[i] } });
            }

            return out;
        }
    }

    const juce::StringArray& interfaceInputNames()
    {
        static const juce::StringArray names { "Mic 1", "Mic 2", "Line 3", "Line 4", "Line 5", "Line 6",
                                               "Line 7", "Line 8", "SPDIF L", "SPDIF R",
                                               "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4",
                                               "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8" };
        return names;
    }

    const juce::StringArray& interfaceOutputNames()
    {
        static const juce::StringArray names { "Line 1", "Line 2", "Line 3", "Line 4", "Line 5", "Line 6",
                                               "Phones L", "Phones R", "SPDIF L", "SPDIF R",
                                               "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4",
                                               "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8" };
        return names;
    }

    std::unique_ptr<InterfaceBackend> makeAlsaInterface() { return std::make_unique<AlsaInterface>(); }
    std::unique_ptr<InterfaceBackend> makeSimInterface() { return std::make_unique<SimInterface>(); }

    // ── Modell ─────────────────────────────────────────────────────────

    juce::String InterfaceModel::matrixName (const juce::String& out, const juce::String& src)
    {
        return out + " from " + src + " Playback Volume";
    }

    juce::String InterfaceModel::faderName (const juce::String& out)
    {
        return out + " Mix Playback Volume";
    }

    bool InterfaceModel::isMixControl (const juce::String& name)
    {
        return name.endsWith (" Playback Volume")
            && (name.contains (" from ") || name.endsWith (" Mix Playback Volume"));
    }

    InterfaceModel InterfaceModel::build (std::vector<IfControl> controls)
    {
        InterfaceModel m;
        m.controls = std::move (controls);

        for (size_t i = 0; i < m.controls.size(); ++i)
            m.index[m.controls[i].name] = i;

        // Ausgaenge: jeder mit Fader; Quellen in Reihenfolge des ersten Ausgangs
        for (auto& c : m.controls)
            if (c.name.endsWith (" Mix Playback Volume"))
                m.outputs.add (c.name.dropLastCharacters (juce::String (" Mix Playback Volume").length()));

        if (! m.outputs.isEmpty())
        {
            auto prefix = m.outputs[0] + " from ";

            for (auto& c : m.controls)
                if (c.name.startsWith (prefix) && c.name.endsWith (" Playback Volume"))
                    m.sources.add (c.name.fromFirstOccurrenceOf (prefix, false, false)
                                         .dropLastCharacters (juce::String (" Playback Volume").length()));
        }

        return m;
    }

    const IfControl* InterfaceModel::find (const juce::String& name) const
    {
        auto it = index.find (name);
        return it == index.end() ? nullptr : &controls[it->second];
    }

    juce::String InterfaceModel::outputLevelName (const juce::String& out) const
    {
        // "Line 1" -> "Line Out 1", "Phones L" -> "Phones Left" (Treiber-Namen)
        juce::String n;

        if (out.startsWith ("Line "))
            n = "Line Out " + out.fromFirstOccurrenceOf ("Line ", false, false) + " Playback Volume";
        else if (out == "Phones L")
            n = "Phones Left Playback Volume";
        else if (out == "Phones R")
            n = "Phones Right Playback Volume";

        return find (n) != nullptr ? n : juce::String();
    }

    juce::StringArray InterfaceModel::resolve (const juce::String& name, bool output) const
    {
        auto& chans = output ? outputs : sources;

        if (chans.contains (name))
            return { name };

        for (auto& [p, members] : groupPairs (chans))
            if (p == name)
                return members;

        return {};
    }

    juce::StringArray InterfaceModel::pairs (bool output) const
    {
        juce::StringArray out;

        for (auto& [p, members] : groupPairs (output ? outputs : sources))
            out.add (p);

        return out;
    }

    std::map<juce::String, int> InterfaceModel::defaultMix() const
    {
        std::map<juce::String, int> m;

        for (int o = 0; o < outputs.size(); ++o)
        {
            for (auto& src : sources)
            {
                auto* c = find (matrixName (outputs[o], src));

                if (c == nullptr)
                    continue;

                auto on = src == "PCM " + juce::String (o + 1)
                       || (outputs[o] == "Phones L" && src == "PCM 1")
                       || (outputs[o] == "Phones R" && src == "PCM 2");
                m[c->name] = on ? fromDb (*c, 0.0) : juce::jmax (0, muteValue (*c));
            }

            if (auto* f = find (faderName (outputs[o])))
                m[f->name] = fromDb (*f, 0.0);
        }

        return m;
    }

    double InterfaceModel::toDb (const IfControl& c, int value)
    {
        if (c.db == nullptr)
            return 0.0;

        auto i = juce::jlimit (0, (int) c.db->size() - 1, value - c.min);
        return (*c.db)[(size_t) i];
    }

    int InterfaceModel::fromDb (const IfControl& c, double db)
    {
        if (c.db == nullptr)
            return juce::jlimit (c.min, c.max, (int) std::lround (db));

        int best = -1;
        double bestDist = 0.0;

        for (size_t i = 0; i < c.db->size(); ++i)
        {
            auto d = (*c.db)[i];

            if (std::isinf (d))
                continue;

            auto dist = std::abs (d - db);

            if (best < 0 || dist < bestDist - 1e-9)
            {
                best = (int) i;
                bestDist = dist;
            }
        }

        return best < 0 ? c.min : c.min + best;
    }

    int InterfaceModel::muteValue (const IfControl& c)
    {
        if (c.db != nullptr)
            for (size_t i = 0; i < c.db->size(); ++i)
                if (std::isinf ((*c.db)[i]))
                    return c.min + (int) i;

        return -1;
    }
}
