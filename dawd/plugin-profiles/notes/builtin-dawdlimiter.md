Limiter — dawd's true-peak limiter for the master chain (4× oversampled peak detection, lookahead).

- Put it last: addPlugin {name: "Limiter", at: "master"}.
- Gain (dB) drives into the limiter (loudness up), Ceiling (dB, e.g. -1) is the true-peak ceiling, Release (ms) 50–200 for mixes.
- Aim for the reference's LUFS with compareReference; true peak stays at the ceiling.
