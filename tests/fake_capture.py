#!/usr/bin/env python3
"""Ersatz fuer pw-record in der Suite (UNIDAW_CAPTURE_CMD): fake_capture.py
<quelle.wav> <ziel.wav>. Schreibt die Quelle (s24 stereo) als "laufenden
Mitschnitt" nach ziel — Kopf mit Groesse 0 wie ein Schreiber, der noch
nicht fertig ist — und wartet auf das Signal. Den Kopf repariert dawd beim
Stop (repairWavHeader); fehlt die Quelle, endet das Programm sofort mit
einer Meldung (der Weg "capture ended by itself"). Liegt <quelle>.crash
daneben, endet es einmal nach dem Schreiben mit einer Meldung (wie
pw-record beim PipeWire-Neustart) und loescht die Marke — dawd startet es
neu (Wiederverbinden)."""
import os
import signal
import struct
import sys
import time

src, dest = sys.argv[1], sys.argv[2]
signal.signal(signal.SIGINT, lambda *_: sys.exit(0))

if not os.path.exists(src):
    print(f"no source: {src}", file=sys.stderr)
    sys.exit(1)

with open(src, "rb") as f:
    data = f.read()
pos = data.index(b"data") + 8
fmt = data[data.index(b"fmt ") + 8:]
channels, rate = struct.unpack("<HI", fmt[2:8])
block = struct.unpack("<H", fmt[12:14])[0]

with open(dest, "wb") as out:
    out.write(b"RIFF" + struct.pack("<I", 0) + b"WAVE")
    out.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * block, block, block * 8 // channels))
    out.write(b"data" + struct.pack("<I", 0))
    out.write(data[pos:])
    out.flush()
    if os.path.exists(src + ".crash"):
        os.remove(src + ".crash")
        print("remote error: id=0 seq:126 res:-32 (Broken pipe): connection error", file=sys.stderr)
        sys.exit(1)
    while True:
        time.sleep(0.05)
