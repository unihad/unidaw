<!--
Vorbereiteter Pull Request an Tracktion (Entscheidung Matthias 2026-10-26:
vorbereiten; einreichen macht Matthias — Konto, Beitragsbedingungen von
Tracktion vorher prüfen). Patch: dawd/patches/tracktion-rubberband-start-delay.patch,
Basis: tracktion_engine e42d82f. Text unten auf Englisch für Upstream.
-->

# RubberBandStretcher: drop the whole start delay before returning output

## Summary

After every reset of the real-time Rubber Band wrapper (clip start, loop
restart, seek), `RubberBandStretcher::processData` in
`modules/tracktion_engine/timestretch/tracktion_TimeStretch.cpp` drops the
stretcher's start delay (`getStartDelay()`, 512 frames at 44.1 kHz in our
setup) only one output block at a time (`samplesPerOutputBuffer`, 256 frames),
but retrieves output in the same call. The first call therefore returns
start-delay samples (near silence) as audio, and the next call drops real
audio as "delay". A transient exactly at a clip or loop start loses up to
one output block (5.8 ms at 44.1 kHz); everything after that is
sample-accurate.

## Reproduction

- An audio clip with auto-tempo (or any clip that goes through the
  real-time Rubber Band wrapper) whose file starts with a click or kick at
  sample 0.
- Render the edit offline: the output is exactly zero up to the next
  256-frame boundary, then correct.
- A looped clip loses the same 256 frames at every loop restart.
- Rubber Band on its own (padding + `getStartDelay`) does not show this;
  driving it with the wrapper's call pattern reproduces the first onset at
  frame 238, as in the render.

## Fix

Drop the whole start delay (as far as output is available) before
retrieving audio:

```diff
--- a/modules/tracktion_engine/timestretch/tracktion_TimeStretch.cpp
+++ b/modules/tracktion_engine/timestretch/tracktion_TimeStretch.cpp
@@ -813,7 +813,11 @@
         int numAvailable = rubberBandStretcher.available();
         jassert (numAvailable >= 0);
 
-        if (numSamplesToDrop > 0)
+        // Drop the whole start delay before returning any output: dropping only one
+        // block's worth here and then retrieving would hand out start-delay samples
+        // as audio and drop real samples on the next call (a transient right at a
+        // clip or loop start lost its first samplesPerOutputBuffer frames)
+        while (numSamplesToDrop > 0 && numAvailable > 0)
         {
             auto numToDropThisTime = std::min (numSamplesToDrop, std::min (numAvailable, samplesPerOutputBuffer));
             rubberBandStretcher.retrieve (outChannels, (size_t) numToDropThisTime);
```

## Testing

Our test renders onsets at the clip start (percussive and melodic
material), at the start of a trimmed clip and at every loop restart. Without
the fix the onset window measures RMS 0.0, with the fix 0.478.
