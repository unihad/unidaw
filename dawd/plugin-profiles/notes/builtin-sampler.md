Sampler / Drum Kit — dawd's built-in sampler (Drum Kit = Sampler with dawd's basic kit in GM layout: 36 kick, 38 snare, 42 closed hat, 46 open hat, 49 crash …).

- Map samples with setSamples: note = one key, one-shot (drums); root/low/high = a key range played pitched (melodic); start/length cut a slice.
- One sample stretched over many keys sounds dull (down) or thin (up) — keep ranges within about ±12 semitones of the root, or map several samples.
- No velocity layers: velocity only changes level.
- Change a single sample's gain/pan/range with setSamples {update: [...]} instead of resending the list.
