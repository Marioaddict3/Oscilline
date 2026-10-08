#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Oscilline contributors
#
# Builds Oscilline-<version>-<arch>.AppImage in the build directory.
# Usage: packaging/linux/build_appimage.sh [build-dir]
# Build on the oldest distribution you want to support; the AppImage needs a
# glibc at least as new as the one it was built against. Extra CMake arguments
# can be passed in OSCILLINE_CMAKE_ARGS.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$(realpath -m "${1:-${root}/build/appimage}")"
app_id="io.github.marioaddict3.Oscilline"
arch="$(uname -m)"
# Pinned so a packaged build does not change with linuxdeploy's moving tag.
linuxdeploy_tag="1-alpha-20251107-1"

# Fail early instead of after the build. linuxdeploy's appimagetool needs `file`.
missing=()
for tool in cmake ninja git curl file; do
    command -v "${tool}" >/dev/null 2>&1 || missing+=("${tool}")
done
if (( ${#missing[@]} > 0 )); then
    echo "build_appimage.sh: missing ${missing[*]}; see docs/packaging.md" >&2
    exit 1
fi

read -r -a extra_args <<< "${OSCILLINE_CMAKE_ARGS:-}"
# A system SDL3 would be linked as a shared library and bundled. Building it
# in statically keeps the AppImage independent of the host's SDL.
cmake -S "${root}" -B "${build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DOSCILLINE_BUILD_TESTS=OFF \
    -DOSCILLINE_WARNINGS_AS_ERRORS=OFF \
    "${extra_args[@]}"
cmake --build "${build}"

appdir="${build}/AppDir"
rm -rf "${appdir}"
DESTDIR="${appdir}" cmake --install "${build}"

tool="${build}/linuxdeploy-${arch}.AppImage"
if [[ ! -x "${tool}" ]]; then
    curl -fsSL -o "${tool}" \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/${linuxdeploy_tag}/linuxdeploy-${arch}.AppImage"
    chmod +x "${tool}"
fi

version="$(sed -n 's/^project(Oscilline$//;s/^ *VERSION \([0-9.]*\)$/\1/p' "${root}/CMakeLists.txt" | head -n 1)"
cd "${build}"
# Runs without FUSE, which containers and CI runners often lack. NO_STRIP: the
# strip inside linuxdeploy cannot read the packed relocations (.relr.dyn) that
# newer distributions such as Fedora use, and a Release build has no debug info.
APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=1 LDAI_OUTPUT="Oscilline-${version}-${arch}.AppImage" "${tool}" \
    --appdir "${appdir}" \
    --desktop-file "${appdir}/usr/share/applications/${app_id}.desktop" \
    --icon-file "${appdir}/usr/share/icons/hicolor/256x256/apps/${app_id}.png" \
    --output appimage
echo "${build}/Oscilline-${version}-${arch}.AppImage"
