# unidaw

unidaw is a digital audio workstation (DAW) for people and AI agents. The
engine runs as a headless daemon with one command API. You work in a
browser UI; an agent works through the same API over MCP. Both see the
same song.

- **One API for all:** the browser UI, MCP agents and scripts send the
  same commands to `http://127.0.0.1:7400`.
- **Song tree:** every idea is a node with its own version of the song.
  Branch, compare, keep the favorites.
- **Built on Tracktion Engine and JUCE.** Plugins (VST3, LV2) run as
  modules in a routing matrix, by default in a separate host process.
- **Analysis for agents:** tempo, key, structure, loudness, stems,
  audio to MIDI, compare with a reference track (some need the ML models,
  see below).

> **Status: early.** Version 0.1.0 is the first public release. Expect
> rough edges.

## No support

unidaw is made by a one-person company. There is **no support** and **no
promise of a reply** to issues or questions. Bug reports are welcome
(see [CONTRIBUTING.md](CONTRIBUTING.md)), but they may stay unanswered.

## Requirements

- **Linux only**, x86_64. There is no macOS or Windows version.
- glibc 2.35 or newer (Ubuntu 22.04 or newer, or another distribution
  with that glibc). The AppImage is built and tested on Ubuntu 22.04.
- `curl` (the starter uses it to check whether unidaw is up).
- An audio output via ALSA (PipeWire and PulseAudio through their ALSA
  plugin) or JACK. unidaw also runs
  without audio (`--no-audio`), for example on a server.
- A web browser for the UI.
- For the MCP server: Python 3 (no extra packages).

## Install

1. Download `unidaw-<version>-x86_64.AppImage` and its `.sha256` file from
   the [Releases](../../releases) page.
2. Check the file and make it executable:

   ```bash
   sha256sum -c unidaw-0.1.0-x86_64.AppImage.sha256
   chmod +x unidaw-0.1.0-x86_64.AppImage
   ```

3. Start it:

   ```bash
   ./unidaw-0.1.0-x86_64.AppImage
   ```

   unidaw starts on `http://127.0.0.1:7400` and opens the UI in your
   browser. Press Ctrl+C in the terminal to stop it. Without FUSE on your
   system, start it with `--appimage-extract-and-run`.

Optional: some analysis commands (audio to MIDI, stem separation, search
by sound) need ML models (about 1.2 GB). The AppImage does not contain
them. Download them once; the command lists each model with its license:

```bash
./unidaw-0.1.0-x86_64.AppImage fetch-models
```

More commands:

```bash
./unidaw-0.1.0-x86_64.AppImage --help
./unidaw-0.1.0-x86_64.AppImage --port 7401 --no-open -- --no-audio   # other port, no audio
./unidaw-0.1.0-x86_64.AppImage licenses                               # license and source code
```

## First steps with an agent (MCP)

The MCP server is one Python file: [`mcp/unidaw_mcp.py`](mcp/unidaw_mcp.py).
It talks to the unidaw that runs on port 7400 and gives the agent two
tools: `unidaw_start` and `unidaw_command` (one command of the API; the tool
description lists all commands).

1. Start unidaw with the AppImage (see above) and keep it running.
   Optional: set `UNIDAW_BIN` to the path of the AppImage before you start
   your agent. Then the `unidaw_start` tool can start unidaw itself (without
   the browser UI).
2. Add the server to your agent.

   **Claude Code**, as a plugin from this repository:

   ```bash
   claude plugin marketplace add unihad/unidaw
   claude plugin install unidaw@unihad
   ```

   **Claude Desktop and other clients that take MCP bundles:** download
   `unidaw-mcp-<version>.mcpb` from the [Releases](../../releases) page and
   open it with the client.

   **Any MCP client, by hand:** get the source code of the same release
   (Releases page, "Source code"), unpack it, and note the path of
   `mcp/unidaw_mcp.py`. For Claude Code:

   ```bash
   claude mcp add unidaw -- python3 /path/to/unidaw/mcp/unidaw_mcp.py
   ```

   Other MCP clients take the same command in their config, for example:

   ```json
   {
     "mcpServers": {
       "unidaw": {
         "command": "python3",
         "args": ["/path/to/unidaw/mcp/unidaw_mcp.py"]
       }
     }
   }
   ```

3. Ask the agent for something small, for example: *"Start a new song in
   ~/music/first-song with a drum beat at 120 BPM and render it to
   ~/music/first-song/beat.wav."* Watch the song change in the browser UI
   while the agent works.

The API is protected: unidaw only answers on 127.0.0.1, and every command
needs the token of the running session. The MCP server reads it from
`~/.config/unidaw/api-token-7400`.

## Build from source

The source needs the Tracktion Engine submodule. unidaw applies a few
patches to it before the build:

```bash
git submodule update --init --recursive
bash dawd/patches/apply.sh
cmake -S unidaw -B dawd/build -DCMAKE_BUILD_TYPE=Release
cmake --build dawd/build -j4
```

Tracktion Engine loads JUCE as a submodule over SSH
(`git@github.com:…`). Without an SSH key for GitHub, run this once before
the submodule step:

```bash
git config --global url."https://github.com/".insteadOf git@github.com:
```

Time stretching and the ML commands need extra libraries:
`bash tools/fetch_ml.sh` downloads them, then build again. The AppImage
build is described in [`docs/appimage.md`](docs/appimage.md) (German).
Most developer docs in this repository are in German.

## License

unidaw is free software under the GNU Affero General Public License,
version 3 only (AGPL-3.0-only), see [LICENSE](LICENSE). The components
it is built on and their licenses: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Support the project

unidaw is free. If it is useful to you, you can support its development
with a donation: [GitHub Sponsors](https://github.com/sponsors/unihad).
A donation does not buy support or features.
