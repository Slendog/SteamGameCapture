#!/usr/bin/env bash
# Builds the plugin inside the OBS Flatpak's SDK so it links against the same
# runtime (glibc, PipeWire, xcb) and libobs headers that Flatpak OBS uses.
# Needs the matching SDK: flatpak install flathub org.freedesktop.Sdk//25.08
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${OBS_FLATPAK_APP:-com.obsproject.Studio}"
BUILD_DIR="${BUILD_DIR:-build-flatpak}"

flatpak run --devel --filesystem="$ROOT" --command=bash "$APP" -c "
	set -euo pipefail
	cd '$ROOT'
	cmake -S . -B '$BUILD_DIR' -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=/app
	cmake --build '$BUILD_DIR'
"
