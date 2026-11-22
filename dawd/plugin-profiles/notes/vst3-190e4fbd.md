Surge XT — hybrid synth: 2 scenes (A/B) × 3 oscillators (classic, wavetable, FM, sine, …), 2 filters, many LFOs/envelopes, 4 FX slots.

How to work:
- Start from a preset: listPresets {plugin, query: "bass" | "pad" | "lead" | "pluck" | "keys"}, factory categories (Basses, Brass, Chords, FX, Keys, Leads, MPE, Pads, Percussion, Plucks, Polysynths, Sequences, Splits, Templates, Tutorials, Vocoder, Winds) plus many 3rd-party authors; loadPreset, then adjust.
- Parameters are named "<Scene> <Section> <Param>" — "A Filter 1 Cutoff", "A Amp EG Attack", "A Osc 1 Type". Edit scene A unless the patch uses B (scene mode split/layer).
- Use VST3 (texts with units: '523.25 Hz'); set values by text: setParameter {name: "A Filter 1 Cutoff", text: "1.2 kHz"}.
- Templates/Init Saw and Init Square are clean starting points.
