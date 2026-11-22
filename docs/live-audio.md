# Live-Audio: unidaw über JACK (pipewire-jack)

Standard ist ALSA über das PipeWire-ALSA-Plugin. Dabei puffert das Plugin vor
PipeWire noch einmal, und unidaw läuft mit 512 × 4 Frames. Für Live-Monitoring
(Gitarre durch Plugins) läuft unidaw stattdessen als JACK-Client im PipeWire-Graphen.

Gemessen am RME Fireface UC, 48 kHz, Durchschleifen Eingang → App → Ausgang
(Kabel-Loopback) mit den Mess-Clients aus `~/dev/fireface-uc/test/` (`latency.c` über
das ALSA-Plugin, `jack_latency.c` über JACK), die dawds Puffer nachbilden. Eine
eigene Verzögerung der Engine (Input-Monitoring, Plugin-Latenz) kommt noch dazu
und ist nicht gemessen:

| Weg | Durchschleifen |
| --- | --- |
| ALSA-Plugin, unidaw-Standard 512 × 4 | 69 ms |
| ALSA-Plugin, 64 × 2, Quantum 64 | 11,7 ms |
| **JACK, Quantum 64** | **8,65 ms**, fest, 0 xruns auch unter Vollast |
| JACK, Quantum 32 | 7,5 ms, vereinzelt xruns unter Last |

## Voraussetzungen

- Pakete `pipewire-jack` (PipeWires libjack) und zum Bauen `libjack-jackd2-dev`.
  CMake schaltet `JUCE_JACK` nur ein, wenn `jack/jack.h` gefunden wird; JUCE lädt
  libjack erst zur Laufzeit.
- Fireface UC: WirePlumber-Regel `51-fireface-uc-sync.conf` aus `~/dev/fireface-uc/pipewire`
  (Ein- und Ausgang synchron getaktet).
- PipeWires Audio-Thread braucht Echtzeit (`chrt -p` auf `data-loop.0` des
  pipewire-Prozesses: `SCHED_RR`/`SCHED_FIFO`, nicht `SCHED_OTHER`). Nach schnellen
  Neustarts des PipeWire-Stacks kam sie einmal nicht (rtkit) – dann
  `systemctl --user restart pipewire`.

## Start

```
UNIDAW_AUDIO=jack PIPEWIRE_LATENCY=64/48000 PIPEWIRE_RATE=1/48000 pw-jack dawd/build/unidaw_artefacts/Release/unidaw --port 7400 ...
```

- `PIPEWIRE_LATENCY` wünscht 64 Frames, `PIPEWIRE_RATE` 48 kHz – beides nur, solange unidaw
  läuft, und nur als Wunsch. **Nicht `PIPEWIRE_QUANTUM`**: Das erzwingt für den JACK-Client
  Block *und Rate*; wechselte dann die Rate des Graphen (Song-Rate, `clock.force-rate`), kam
  das Audio am Fireface UC verwürfelt an (gemessen 2026-10-17). Beim Start meldet unidaw unter
  `PIPEWIRE_LATENCY` noch das Leerlauf-Quantum (z. B. Block 1024), läuft danach aber mit 64.
- Fest als Benutzerdienst: `~/dev/fireface-uc/systemd/unidaw-7400.service`.
- **Audio-Worker:** Tracktion verteilt die Berechnung auf Worker-Threads (einer je CPU). Unter
  Linux scheiterten sie an `SCHED_RR 99` (RLIMIT_RTPRIO 95) und liefen ohne Echtzeit → unter
  CPU-Last ~5-ms-Aussetzer (etwa 1/min bei Load 7). Patch
  `dawd/patches/tracktion-linux-worker-priority.patch` + `TRACKTION_RT_PRIORITY=10` (setzt unidaw
  selbst, überschreibbar): Worker mit `SCHED_FIFO 10`, unter dem PipeWire-Daemon (20).
  Höher ist schädlich: Mit 80 verdrängten die teils spinnenden Worker den Daemon (37 ms
  Stillstand). Gemessen unter 16 Lastprozessen: vorher 2 Aussetzer in 75 s, danach 0 in 100 s.
- Ausgang: JACK-Client `UNIDAW_JACK_OUT`, Standard „Fireface UC Ausgang 1/2“.
  Eingang: `UNIDAW_JACK_IN`, Standard die Rohkanäle des UC („Fireface UC Mac (…)
  Multichannel“), davon Kanal 1/2. Fehlt ein Name, nimmt unidaw den ersten gefundenen.
- Die Rate kommt von PipeWire (48 kHz), unidaw folgt ihr.
- Das Geräte-Setup bleibt im Speicher (`DawdPropertyStorage`), nie in der
  `Settings.xml` des `--config`-Ordners: ein dort gespeichertes JACK-Setup ließ eine
  unidaw-Instanz ohne JACK ohne Audio-Gerät starten. Altlasten räumt jeder Start ohne
  `UNIDAW_AUDIO=jack` weg (Test `audio_api_flow`).
- unidaw selbst legt beim Input-Monitoring keinen Block drauf (gemessen rein digital über
  JACK: genau 1 Quantum, und das ist der Kreis Mess-Client → unidaw → Mess-Client).

## Rate, Puffer, Bit-Tiefe (audioSettings, Knopf „Audio“ im Kopf)

- **Puffer** (32…1024 Frames, „Default“ = `PIPEWIRE_LATENCY` des Starts) gilt für unidaw
  auf diesem Rechner, gespeichert in `audio.json` im `--config`-Ordner.
- **Song-Rate** (Device, 44,1/48/88,2/96/192 kHz) und **Aufnahme-Bit-Tiefe** (16/24/32 float)
  gehören dem Song (`tree.json`); beim Öffnen stellt unidaw das Gerät um.
- Unter JACK setzt unidaw dafür `clock.force-rate` bzw. `clock.force-quantum` in PipeWire
  (gilt für alle Programme, solange unidaw läuft) und öffnet sein JACK-Gerät neu. Beim
  Schließen des Songs bzw. Beenden löst unidaw die Vorgaben wieder. Andere JACK-Clients, die
  Rate/Block nur beim Start lesen (auch ein zweiter unidaw), merken den Wechsel nicht.
- Das Fireface UC schaltet dabei wirklich um, seit der Treiber bei jeder Rate 18 Kanäle hat
  (fireface-uc 8e4cf72); vorher blieb es bei 48 kHz und PipeWire rechnete um.
  `clock.allowed-rates` bleibt beim Standard `[ 48000 ]` – sonst folgt der Graph im Leerlauf
  anderen Programmen (z. B. Spotify auf 44,1 kHz).

