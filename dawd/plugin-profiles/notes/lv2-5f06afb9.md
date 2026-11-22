LSP Sidechain Compressor Stereo — the LSP compressor with an external sidechain input (ducking, pumping).

- dawd cannot feed a sidechain input yet (routing has no sidechain edges), so Sidechain type External hears nothing; for now use LSP Compressor Stereo or automate gain for ducking.
- Otherwise identical to LSP Compressor Stereo. Gains are linear factors, not dB (1 = 0 dB, 0.5 ≈ -6 dB, 2 ≈ +6 dB); thresholds likewise (0.25 ≈ -12 dB, 0.1 = -20 dB) — listParameters shows them only in the text (real is just 0–1), so set them by text: setParameter {name, text: "0.5"}.
