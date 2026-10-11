#!/usr/bin/env bash
# Installs a build from build-flatpak.sh into Flatpak OBS's per-user plugin dir
# and grants OBS access to the X11 display, which the plugin needs to see
# Steam games (they run under XWayland).
#
#   --no-x11-override   only install the plugin, don't change OBS permissions
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${OBS_FLATPAK_APP:-com.obsproject.Studio}"
BUILD_DIR="${BUILD_DIR:-build-flatpak}"
DEST="$HOME/.var/app/$APP/config/obs-studio/plugins/steam-game-capture"

x11_override=1
for arg in "$@"; do
	case "$arg" in
	--no-x11-override) x11_override=0 ;;
	*)
		echo "usage: $0 [--no-x11-override]" >&2
		exit 2
		;;
	esac
done

SO="$ROOT/$BUILD_DIR/steam-game-capture.so"
if [[ ! -f "$SO" ]]; then
	echo "Missing $SO, run scripts/build-flatpak.sh first" >&2
	exit 1
fi

# Write a new file and rename it over the old one: overwriting the library
# in place would corrupt it for an OBS instance that has it loaded.
install -Dm755 "$SO" "$DEST/bin/64bit/.steam-game-capture.so.new"
mv -f "$DEST/bin/64bit/.steam-game-capture.so.new" "$DEST/bin/64bit/steam-game-capture.so"
mkdir -p "$DEST/data"
cp -r "$ROOT/data/." "$DEST/data/"
echo "Installed to $DEST"

# On Wayland sessions Flatpak only gives OBS "fallback-x11", which hides the
# X11 display as long as a Wayland socket exists.
if flatpak info --show-permissions "$APP" | grep -Eq '^sockets=(.*;)?x11;'; then
	echo "OBS already has X11 access"
elif ((x11_override)); then
	flatpak override --user --socket=x11 "$APP"
	echo "Granted OBS X11 access (undo: flatpak override --user --nosocket=x11 $APP)"
else
	echo
	echo "OBS has no X11 access, the plugin won't find games until you run:"
	echo "  flatpak override --user --socket=x11 $APP"
fi

if pgrep -f "$APP" >/dev/null; then
	echo
	echo "Restart OBS to load the plugin."
fi
