# Packaging

Only the `oscilline` game is packaged. The developer tools (`oscilline-viewer`,
`-extract`, `-chart`, `-inspect`) are not installed. SDL3 is linked statically,
so every package is the game binary plus metadata.

The app ID is `io.github.marioaddict3.Oscilline`. The icon is original art:
`packaging/icons/make_icons.py` writes the SVG, PNG sizes, `.ico`, and `.icns`
from one geometry (needs Pillow). Edit the script, not the generated files.

## CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `OSCILLINE_PACKAGING` | `OFF` | Windows: GUI subsystem, so no console window opens. macOS: an `oscilline.app` bundle. Leave it off for development, where command-line output should stay visible. |
| `OSCILLINE_FETCH_SDL3` | `ON` | Downloads SDL3 when it is not installed. `OFF` requires an installed SDL3 and fails at configure time otherwise, for offline builds. |
| `OSCILLINE_BUILD_TESTS` | `ON` | Set `OFF` for packages; tests also download doctest. |
| `OSCILLINE_WARNINGS_AS_ERRORS` | `ON` | Set `OFF` for packages, so a newer or older distribution compiler's warnings cannot fail a release build. CI keeps it on. |

The Windows icon and version resource is built on every Windows configuration.
`cmake --install` lays out the package:

- **Linux:** `bin/oscilline`, the desktop entry, AppStream metainfo, hicolor
  icons (SVG and 16–512 px PNG), and `LICENSE` and `README.md` under `share/doc`.
- **Windows:** `oscilline.exe`, `LICENSE.txt`, and `README.md` in one folder.
- **macOS:** `oscilline.app`, `LICENSE`, and `README.md`.

## Signing

Packages are built locally; CI does not build or upload them. Signing is not
set up:

- Windows SmartScreen warns about an unsigned `.exe` until it has a code-signing
  certificate and some download reputation.
- macOS Gatekeeper blocks an ad-hoc signed app downloaded from the internet. Users
  must right-click the app and choose Open, or the app needs a Developer ID
  signature and Apple notarization (an Apple Developer Program membership).

## Building locally

Windows (Developer PowerShell):

```
cmake -S . -B build/package -A x64 -DOSCILLINE_PACKAGING=ON -DOSCILLINE_BUILD_TESTS=OFF
cmake --build build/package --config Release
cd build/package; cpack -C Release
```

macOS:

```
cmake -S . -B build/package -G Ninja -DCMAKE_BUILD_TYPE=Release -DOSCILLINE_PACKAGING=ON \
  -DOSCILLINE_BUILD_TESTS=OFF "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build/package
cmake --install build/package --prefix build/stage
codesign --force --deep --sign - build/stage/oscilline.app
```

Linux AppImage:

```
packaging/linux/build_appimage.sh build/package
```

The script configures a Release build with `/usr` as the prefix, installs it
into an AppDir, and runs a pinned `linuxdeploy` release to produce the AppImage.
It always builds SDL3 into the binary, even when the system has SDL3, and skips
stripping, which fails on distributions that use packed relocations (i.e. Fedora). 
Extra CMake arguments go in `OSCILLINE_CMAKE_ARGS`.

The AppImage needs a glibc at least as new as the build system's. On a recent
or immutable distribution, build inside an older container so
it runs elsewhere too:

```
distrobox create --name oscilline-build --image ubuntu:22.04
distrobox enter oscilline-build
sudo apt-get install -y git file ninja-build g++-12 curl pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev \
  libxss-dev libxrender-dev libxtst-dev libxkbcommon-dev libwayland-dev \
  libdecor-0-dev libgl1-mesa-dev libegl1-mesa-dev libgles2-mesa-dev \
  libasound2-dev libpulse-dev \
  libudev-dev libdbus-1-dev libdrm-dev libgbm-dev
# Ubuntu 22.04's CMake is 3.22; this project needs 3.25. /opt stays in the container.
curl -fsSL https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz \
  | sudo tar -xz -C /opt
export PATH=/opt/cmake-3.31.6-linux-x86_64/bin:$PATH
CC=gcc-12 CXX=g++-12 packaging/linux/build_appimage.sh build/package
```

`export PATH` lasts for one shell; repeat it after each `distrobox enter`.
