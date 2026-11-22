Dexed — DX7 FM synth (6 operators, 32 algorithms): electric pianos, bells, FM basses, brass, plucks.

- Sounds come from DX7 cartridges (32 voices each). The built-in one shows as 32 programs (listPresets → loadPreset {plugin, program: n}; patches like 'SAW EM UP', 'ARP 2600', 'Slow3D Pad').
- ~1000 more voices: the cartridges in ~/.local/share/DigitalSuburban/Dexed/Cartridges (Dexed_01, SynprezFM_01…32) are file presets — category = cartridge, one entry per voice: loadPreset {plugin, path, program: voice} or name 'SynprezFM_01/E.PIANO 1'. Names are short DX7 names — try a few and compare with analyze; findPresets finds them by sound once catalogued.
- After loading a cartridge voice the program list still shows the built-in cartridge (the host caches the names) — use the file presets, not program numbers, to pick cartridge voices.
- Editing FM from scratch is hard: change a preset gently — Cutoff/Resonance (global filter), Output, OPx OUTPUT LEVEL (operator levels: carriers = loudness, modulators = brightness), FEEDBACK, ALGORITHM.
- MonoMode POLY/MONO for basses and leads.
