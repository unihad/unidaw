# Third-party licenses

unidaw is licensed under the GNU Affero General Public License, version 3
(AGPL-3.0-only), see `LICENSE`. It is built on the components below. Each
keeps its own license; the full license texts ship in the folders named
here (paths relative to the source tree).

## Built into the unidaw binary

| Component | Version | License | License text |
| --- | --- | --- | --- |
| JUCE | 8.0.13 | AGPL-3.0 (dual-licensed with the commercial JUCE licence) | `tracktion_engine/modules/juce/LICENSE.md` |
| Tracktion Engine | commit e42d82f (3.5.0) | GPL-3.0-or-later (dual-licensed with a commercial licence) | `tracktion_engine/LICENSE.md` |
| Rubber Band Library | 4.0.0 | GPL-2.0-or-later | `dawd/third_party/rubberband/COPYING` (fetched by `tools/fetch_ml.sh`) |

Third-party code inside JUCE that unidaw compiles. unidaw does not build the
JUCE modules for OpenGL, JavaScript, Box2D or plugin formats other than
VST3 and LV2, and no ARA SDK; their dependencies (GLEW, QuickJS, Box2D, AU
SDK, AAX SDK, ASIO, Oboe) are not part of unidaw.

| Component | License | License text (under `tracktion_engine/modules/juce/modules/`) |
| --- | --- | --- |
| VST3 SDK | MIT | `juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt` |
| LV2 SDK | ISC | `juce_audio_processors_headless/format_types/LV2_SDK/lv2/COPYING` |
| pslextensions | Public domain | `juce_audio_processors_headless/format_types/pslextensions/ipslcontextinfo.h` |
| FLAC | BSD-3-Clause | `juce_audio_formats/codecs/flac/Flac Licence.txt` |
| Ogg Vorbis | BSD-3-Clause | `juce_audio_formats/codecs/oggvorbis/Ogg Vorbis Licence.txt` |
| zlib | Zlib | `juce_core/zip/zlib/README` |
| libpng | Zlib-style libpng license | `juce_graphics/image_formats/pnglib/LICENSE` |
| jpeglib | Independent JPEG Group License | `juce_graphics/image_formats/jpglib/README` |
| HarfBuzz | Old MIT | `juce_graphics/fonts/harfbuzz/COPYING` |
| SheenBidi | Apache-2.0 | `juce_graphics/unicode/sheenbidi/LICENSE` |
| MP3 decoder (JUCE's own, `JUCE_USE_MP3AUDIOFORMAT=1`) | Same as JUCE (AGPL-3.0); JUCE disclaims any patent or copyright responsibility for this code | `juce_audio_formats/codecs/juce_MP3AudioFormat.cpp` |

Third-party code inside Tracktion Engine (under
`tracktion_engine/modules/`):

| Component | License | Where |
| --- | --- | --- |
| CHOC | ISC | `3rd_party/choc/LICENSE.md` |
| crill | BSL-1.0 | `3rd_party/crill/LICENSE.txt` |
| doctest | MIT | `3rd_party/doctest/doctest.h` |
| expected (TartanLlama) | CC0-1.0 | `3rd_party/expected/expected.hpp` |
| libsamplerate | BSD-2-Clause | `3rd_party/libsamplerate/samplerate.h` |
| magic_enum | MIT | `3rd_party/magic_enum/magic_enum.hpp` |
| MPMCQueue (rigtorp) | MIT | `3rd_party/rigtorp/MPMCQueue.h` |
| rpmalloc | Public domain | `3rd_party/rpmalloc/rpmalloc.c` |
| farbot | MIT | `tracktion_graph/3rd_party/farbot/LICENSE` |
| moodycamel ConcurrentQueue (Cameron Desrochers) | BSD-2-Clause (Simplified BSD), also Boost Software License | `tracktion_graph/3rd_party/concurrentqueue.h` (license in the file header) |
| moodycamel LightweightSemaphore (Cameron Desrochers, based on Jeff Preshing's semaphore) | BSD-2-Clause; Jeff Preshing's part zlib | `tracktion_graph/3rd_party/lightweightsemaphore.h` (license in the file header) |
| Airwindows plugins | MIT (Copyright (c) 2018 Chris Johnson, upstream `github.com/airwindows/airwindows`) | `tracktion_engine/3rd_party/airwindows/` |

Not compiled into unidaw (switched off in the engine configuration):
SoundTouch (LGPL-2.1), Signalsmith Stretch (MIT), Ableton Link, REX.

## System libraries

unidaw links these libraries of the operating system dynamically. They are
not part of this source tree. Licenses as stated by the Ubuntu packages
(`/usr/share/doc/<package>/copyright`). The AppImage bundles the GCC
runtime, FreeType and Fontconfig together with the libraries they load
(Expat, zlib, bzip2, libpng, Brotli); the copyright files of these
packages are in the AppImage under `usr/share/doc/unidaw/licenses/system/`.
alsa-lib, JACK and glibc always come from the operating system.

| Library | License |
| --- | --- |
| alsa-lib (`libasound.so.2`) | LGPL-2.1-or-later |
| FreeType (`libfreetype.so.6`) | FreeType License (FTL) |
| Fontconfig (`libfontconfig.so.1`) | MIT-style permission notice (Keith Packard) |
| GCC runtime (`libstdc++`, `libgcc_s`, `libatomic`) | GPL-3.0 with GCC Runtime Library Exception |
| JACK (`libjack.so.0`, loaded at runtime only if installed; JACK backend for `UNIDAW_AUDIO=jack`) | LGPL-2.1-or-later |

## Shipped next to the binary

| Component | Version | License | License text |
| --- | --- | --- | --- |
| ONNX Runtime | 1.31.0 | MIT, plus the notices of its own dependencies | `LICENSE` and `ThirdPartyNotices.txt` in `dawd/third_party/onnxruntime-linux-x64-1.31.0/` |
| Web Awesome (UI components) | 3.14.0 | MIT, Copyright (c) 2025 Fonticons, Inc. | `dawd/ui/vendor/webawesome/LICENSE.md` |
| Basic drum kit | — | Part of unidaw (synthesized by `dawd/kits/make_basic_kit.py`, no third-party samples) | `LICENSE` |

## AppImage

| Component | Version | License | License text |
| --- | --- | --- | --- |
| AppImage type2-runtime (the executable part of the AppImage) | 20251108 | MIT; statically links musl libc (MIT), libfuse 3 (LGPL-2.1) and squashfuse (BSD-2-Clause) | in the AppImage: `usr/share/doc/unidaw/licenses/appimage-runtime/`; source: `github.com/AppImage/type2-runtime`, tag `20251108` |

In the AppImage, `usr/share/doc/unidaw/licenses/` holds the license texts
named in this file (same paths as in the source tree).

## Models (downloaded, not shipped)

unidaw does not ship model files. `unidaw-fetch-models` (in the AppImage:
`fetch-models`; in the source tree also `tools/fetch_ml.sh`) downloads
them from their original source; each keeps its own license.

| Model | Used for | License | Source |
| --- | --- | --- | --- |
| basic-pitch (Spotify) | audio to MIDI | Apache-2.0 (repository license) | `github.com/spotify/basic-pitch` |
| HT-Demucs v4, ONNX export | 4-stem separation | MIT (model card) | `huggingface.co/adowu/htdemucs-onnx` |
| HT-Demucs 6s, ONNX export | 6-stem separation | MIT (model card) | `huggingface.co/StemSplitio/htdemucs-6s-onnx` |
| LAION CLAP `larger_clap_general`, ONNX export by Xenova | sound search | Apache-2.0 (base model card) | `huggingface.co/Xenova/larger_clap_general` |
| MVSep Mega 53 (BS-RoFormer) | 53-class separation, optional (`MEGA53=1`) | MIT for the weights (author, 2026-09-25); training data not owned by the author | `github.com/ZFTurbo/Music-Source-Separation-Training`, issue #245 |
