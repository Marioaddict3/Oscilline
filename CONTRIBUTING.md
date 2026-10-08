# Contributing to Oscilline

Oscilline is a clean-room behavioral reimplementation. These rules are not optional (at least if you want your PR accepted).

## Codename

Use the name **Oscilline** in code, identifiers, branding, binaries, commit messages, and documentation. The original commercial title may be named only where a reader needs it to recognize a disc they already own (supported serials in the README and `docs/`). It must not appear in identifiers, type names, file names, or log strings that are not identifying a disc.

## Useful resources

- The open format notes at <https://open-ribbon.github.io/documentation/> (PAK, TMD, ANM, TIM, VB/VH, FSL, and the disc layout described there).
- Public PlayStation format specifications: [psx-spx](https://psx-spx.consoledev.net/), ISO 9660, CD-ROM XA / Mode 2 sector layouts, and CUE sheets.
- VibRipper's documented command-line behavior (its README), not its source.

## What you must not do

- Do not copy, translate, or mechanically rewrite code from the open-ribbon decompilation, from VibRipper, or from any disassembly or decompiled listing.
- Do not add game data, extracted assets, music, BIOS images, or SDK/PsyQ files to the repository, including test fixtures and screenshots of those assets.
- Write parsers from the format documentation. Cite the document section in a comment on each parser.
- Tests use small synthetic fixtures: bytes produced by our own test code, or hand-written bytes that follow the documented layouts.

## Discs

Players supply their own disc image. Oscilline ships no disc data. The primary serial is PAL `SCES-02873`. Japanese `SCPS-45469` is supported as well (Experimental). Tests and examples assume the PAL boot file unless they are specifically covering the Japanese disc. An image with any other boot file is refused.

## Original art

Do not recreate or trace the original game's character, logo, or other distinctive art. Icons, placeholders, and splash screens drawn for Oscilline must be original. Geometry, images, and samples that came from a disc may be rendered or played only at runtime from the user's own image. Those bytes must never be committed.

## CI runner

Linux CI runs on GitHub's `ubuntu-latest` runner and installs these packages with `apt-get` when they are missing:

```sh
sudo apt-get install -y \
  ninja-build clang-format pkg-config cmake g++ clang libclang-rt-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
  libxfixes-dev libxss-dev libxrender-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev libdecor-0-dev \
  libgl1-mesa-dev libegl1-mesa-dev libgles2-mesa-dev \
  libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
  libdrm-dev libgbm-dev
```

The same list works for a local Ubuntu or Debian build. `libclang-rt-dev` is needed only for the sanitizer presets.

macOS and Windows builds are in `.github/workflows/ci-hosted.yml`. That workflow is manual `workflow_dispatch` only. It does not run on push or pull request. There is no scheduled workflow, and pull requests do not upload artifacts. `push` runs on `main` only. A change outside the path allow-list in `.github/workflows/ci.yml` (docs, README, and other non-build files) does not start a build.

## License

Contributions are licensed under GPL-2.0-or-later, the same license as the rest of the tree. New source files carry an SPDX header:

```
SPDX-License-Identifier: GPL-2.0-or-later
```
