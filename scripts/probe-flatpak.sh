#!/usr/bin/env bash
# Runs sgc-probe inside the OBS Flatpak sandbox (with X11 access) to check
# what the plugin will see. Arguments are passed through, e.g. --audio 5.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${OBS_FLATPAK_APP:-com.obsproject.Studio}"
BUILD_DIR="${BUILD_DIR:-build-flatpak}"

exec flatpak run --socket=x11 --filesystem="$ROOT" --command="$ROOT/$BUILD_DIR/sgc-probe" "$APP" "$@"
