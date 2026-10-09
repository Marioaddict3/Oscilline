<h1 align="center"><picture>
  <source media="(prefers-color-scheme: dark)" srcset="/images/Oscilline-Text-Transparent.gif">
  <source media="(prefers-color-scheme: light)" srcset="/images/Oscilline-Text-Transparent-Black.gif">
  <img alt="Oscilline" src="/images/Oscilline-Text-Transparent.gif">
</picture></h1>

A clean-room behavioral reimplementation engine for a 1999 PlayStation vector rhythm game, written in C++20 with SDL3.

**Oscilline ships no original game code, assets, music, BIOS or SDK files.** It reads data from a disc image you own (BIN/CUE or ISO).

The primary supported disc is PAL `SCES-02873`. Japanese `SCPS-45469` is also accepted (Experimental).

Oscilline is an unofficial project and is not affiliated with Sony or NanaOn-Sha.

## Clean-room rules

- Public format documentation, emulator observation and our own analysis are the references. **No code is copied from any decompilation or disassembly.**
- No copyrighted data (game files, extracted assets, music, screenshots of assets) is committed. Tests use synthetic fixtures we generate ourselves.
- See [CONTRIBUTING.md](CONTRIBUTING.md).

## Build

Requires CMake 3.25+, a C++20 compiler, and Ninja (the Visual Studio generator also works on Windows). SDL3 is located with `find_package` and, if that fails, fetched at configure time.

On Linux, fetching SDL3 needs the usual window-system development packages (libx11-dev, Xext, Xrandr, Xcursor, Xi, Xfixes, Xss, Xrender, Xtst, plus Wayland, GLES, ALSA, PulseAudio, udev and DBus if you want those backends). The full package list is in [CONTRIBUTING.md](CONTRIBUTING.md#ci-runner). A system SDL3 from the package manager is used instead when `find_package(SDL3)` succeeds.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Other presets: `release`, `asan` (AddressSanitizer and UndefinedBehaviorSanitizer, for memory bugs), and `tsan` (ThreadSanitizer, for data races). Both sanitizer presets work with Clang or GCC. `-DOSCILLINE_ENABLE_CLANG_TIDY=ON` runs the `.clang-tidy` checks on project sources while they compile; it is off by default and not run in CI. Windows, macOS, and AppImage packages are described in [docs/packaging.md](docs/packaging.md).

```sh
oscilline
oscilline --disc disc.cue
oscilline --disc disc.cue --play --course 1
oscilline --disc disc.cue --play --difficulty bronze
oscilline --play --music song.mp3 --difficulty silver
oscilline --frames 2
oscilline --disc disc.cue --asset-report
oscilline --disc disc.cue --asset-report --asset-confidence medium
oscilline --disc disc.cue --no-disc-assets --play --course 1
oscilline --clips-test
oscilline --sfx-test
oscilline --disc disc.cue --sfx-test --sfx-experimental
```

## Launch options

Saved settings apply unless a flag overrides them for that run. `oscilline --help` lists every flag.

`--disc <file>` loads a disc image you own (a .cue, or an ISO). It also becomes the saved last-used disc.

`--play` skips the title and starts right away: course 1 of the disc, a difficulty pair, or a music chart. With no disc and no `--music` it exits with an error.

`--course <N>` with `--play` plays one disc course, 1 to 6.

`--difficulty <bronze|silver|gold>` with `--play` plays that pair of disc courses, or sets the density of a music chart. Without `--play` the title opens on that row. Do not combine it with `--course`.

`--music <file>` loads a WAV, FLAC, or MP3 for Play My Music. With `--play` it starts at launch and defaults to bronze.

`--language <name>` selects the disc's language archive. Only English is currently implemented.

`--disc-camera` (the default) follows the disc's camera paths. `--builtin-camera` uses the built-in camera instead. The last of the two wins.

`--no-disc-assets` keeps the placeholder figure, built-in camera, and debug text even when a disc is mounted. It does not change the saved setting.

`--asset-report` prints, for each asset slot, the disc path it uses or that it fell back to a placeholder.

`--asset-confidence <high|medium|low>` sets how confident a disc mapping must be before it replaces a placeholder. High is the default.

`--ribbon-guides` shows beat and hit guides on the ribbon. These can be helpful for debugging hitboxes or obstacle placement.

`--clips-test` reports all mapped animations, their original ANM file path, and their confidence.

`--sfx-test` reports all 48 event cues and auditions the one-shot cues. It skips menu and course music cues so they cannot stack during the sweep. Each line prints bank, program and tone or a VAG index, rate, and confidence. Anything with a confidence below high is gated. See [SFX comparison notes](docs/sfx-comparison.md).

`--sfx-experimental` permits low- and medium-confidence sound effects. No disc-derived audio or other copyrighted data is committed.

`--gen-experimental` turns on medium-confidence custom-music placement rules: track density and a 2-second, 4–7 beat stride. Disc courses are unchanged.

`--frames <N>` exits after N presented frames. CI uses it to run without a display.

`--version` prints the version, and `--help` prints the full usage.

## Playing your own music

Oscilline supports WAV, FLAC, and MP3 files.

- Custom music needs to be at least 20 seconds long.
- Songs longer than 15 minutes will be cut at 15 minutes.

Oscilline decodes with [dr_wav](third_party/dr_libs/dr_wav.h) 0.14.6, [dr_flac](third_party/dr_libs/dr_flac.h) 0.13.4, and [dr_mp3](third_party/dr_libs/dr_mp3.h) 0.7.4 (David Reid). Audio is downmixed to stereo and resampled to 44100 Hz.

Timing constants that a real disc may disagree with are listed in [docs/course-mapping.md](docs/course-mapping.md).

## Additional tools

`oscilline-viewer` draws TMD models from a disc you own. The default view is the side profile (`--view side`); `--view front` is the debug view. Left and right change the model and pick an animation from its folder, unless `--anm` was given. Up and down change the animation. Drag with the left mouse button to orbit around that framed model. The wheel zooms. Q and E yaw, R and F pitch, `-` and `=` zoom, and P resets the orbit and zoom. Tab toggles between the SFX and model viewers. `--anc <name>` draws that camera path instead. `--sfx` starts in the SFX browser. There are many sound effects that have not been mapped yet.

```sh
oscilline-viewer --disc disc.cue
oscilline-viewer --disc disc.cue --view front
oscilline-viewer --disc disc.cue --pak GAME/02_FILES.PAK --tmd CHARA/PEELOO/MODEL.TMD
oscilline-viewer --disc disc.cue --frames 2
oscilline-viewer --sfx
oscilline-viewer --sfx --disc disc.cue
```

`oscilline-extract` lists or copies files out of an original disc image that you own:

```sh
oscilline-extract list disc.cue
oscilline-extract extract disc.cue ./out
oscilline-extract extract disc.iso ./out SYSTEM.CNF
```

`oscilline-chart <cue> <course>` prints the obstacle hit times a disc course charts. `--attacks` prints the picked attack times instead. `oscilline-inspect` prints a JSON dump of one parsed file (PAK, TIM, TMD, ANM, ANC, VH/VB, or FSL) so a disc you own can be checked locally. Notes on the layout are in [docs/architecture.md](docs/architecture.md), [docs/disc-map.md](docs/disc-map.md), and [docs/course-mapping.md](docs/course-mapping.md).

```sh
oscilline-inspect file.pak
oscilline-inspect bank.vh --vb bank.vb
oscilline-inspect --full-pcm bank.vh --vb bank.vb
```

## AI Disclosure

Oscilline was developed with heavy assistance from AI tools. Most frameworks in this project were built by AI, then tweaked by hand. Reviews and bug reports are welcome.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
