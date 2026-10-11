#!/usr/bin/env bash
# Installs a build from build-flatpak.sh into Flatpak OBS's per-user plugin dir.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${OBS_FLATPAK_APP:-com.obsproject.Studio}"
BUILD_DIR="${BUILD_DIR:-build-flatpak}"
DEST="$HOME/.var/app/$APP/config/obs-studio/plugins/steam-game-capture"

SO="$ROOT/$BUILD_DIR/steam-game-capture.so"
if [[ ! -f "$SO" ]]; then
	echo "Missing $SO, run scripts/build-flatpak.sh first" >&2
	exit 1
fi

install -Dm755 "$SO" "$DEST/bin/64bit/steam-game-capture.so"
mkdir -p "$DEST/data"
cp -r "$ROOT/data/." "$DEST/data/"
echo "Installed to $DEST"

# Steam games run under XWayland; Flatpak only exposes X11 to OBS when no
# Wayland socket is available unless it is granted explicitly.
if ! flatpak info --show-permissions "$APP" | grep -Eq '^sockets=.*(^|;|=)x11;'; then
	echo
	echo "OBS has no X11 access yet. Grant it with:"
	echo "  flatpak override --user --socket=x11 $APP"
fi
