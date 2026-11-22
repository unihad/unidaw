Surge XT Effects — Surge's effect units as one plugin: FX Type picks the effect (Delay, Reverb 1, Phaser, Rotary Speaker, Distortion, EQ, Chorus, Flanger, …).

- Set FX Type first, then read listParameters again: the twelve effect controls take the names and values of the chosen effect (Reverb 1: Pre-Delay, Room Shape, Size, Decay Time, HF Damping, EQ Low Cut/Peak/High Cut, Output Width, Output Mix). Unused slots show as "- -".
- The names only update once the plugin has processed audio. If listParameters still shows "FX Parameter n" after changing FX Type, render once (always needed in an offline-only instance, --no-audio).
- Output Mix sets wet/dry inside the effect (default 50 %); for a parallel wet path set it to 100 %.
- The measured profile covers the default type (Delay) only.
