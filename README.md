# Steam Game Capture for OBS (Linux)

An OBS Studio source that captures a running Steam game, **video and audio
together**, on Linux. Add it to a scene and it finds the game on its own.
It keeps working when you restart the game or switch to another one.

- **Video:** finds the game's X11/XWayland window by the `STEAM_GAME` window
  property or the `steam_app_<AppID>` window class. Both Proton and native
  games set these. The window contents are read with MIT-SHM, and the Steam
  client window is ignored.
- **Audio:** creates a PipeWire capture stream and links it to the game's
  playback streams. Streams are matched to the game window's process ID. Only
  the game's audio is captured, not Discord or music. Surround channels are
  folded down to stereo, and the game's own playback is unaffected.

## Requirements

- OBS Studio 30+ (tested with Flatpak OBS 32.2.2 on KDE Plasma / Wayland)
- PipeWire (with pipewire-pulse or native PipeWire clients)
- The game must run under X11/XWayland. This is the default for Proton and for
  most native games.

## Build & install (Flatpak OBS)

The plugin is built inside the OBS Flatpak SDK, so it links against the same
runtime that OBS runs in.

```sh
flatpak install flathub org.freedesktop.Sdk//25.08   # once; must match OBS's runtime
./scripts/build-flatpak.sh
./scripts/install-flatpak.sh
```

On a Wayland session, Flatpak OBS has no X11 access by default. Steam games
are X11 clients, so grant it once:

```sh
flatpak override --user --socket=x11 com.obsproject.Studio
```

Restart OBS, then add **Sources → + → Steam Game Capture**.

## Build & install (native OBS)

Install the OBS development package first (for example `obs-studio-devel`,
`libobs-dev`, or `obs-studio` on Arch). You also need the `xcb`, `xcb-composite`,
`xcb-shm`, `xcb-res` and `libpipewire-0.3` development files. Then:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
cmake --install build --prefix ~/.config/obs-studio/plugins
```

To install system-wide instead, add `-DSGC_INSTALL_LAYOUT=system` and use a
prefix such as `/usr`.

## Settings

| Setting | Meaning |
|---|---|
| Game | *Any running Steam game* (default), or a specific AppID |
| Capture frame rate | Grab rate. Defaults to the OBS output FPS |
| Only capture video while visible | Skips frame grabs when the source isn't shown anywhere |
| Capture game audio | PipeWire per-game audio |
| Extra audio match | Also captures streams whose app name, node name or binary contains this text. Use it for games that play sound from a helper process |
| X11 display | Empty means `$DISPLAY` |

The status line at the top of the properties shows the captured game and the
number of linked audio streams.

## Troubleshooting

`sgc-probe` uses the same capture code without OBS:

```sh
./scripts/probe-flatpak.sh                      # list detected game windows
./scripts/probe-flatpak.sh --grab /tmp/f.ppm    # save one frame
./scripts/probe-flatpak.sh --bench 120          # time frame grabs
./scripts/probe-flatpak.sh --audio 5            # show captured audio level per second
```

(`build/sgc-probe` works the same way for native builds.)

- **"Cannot connect to the X11 display"**: OBS needs X11 access. See the
  `flatpak override` command above.
- **Video works but there are 0 audio streams**: the game plays audio from a
  different process. Find its name with `pw-dump` or `qpwgraph` and enter it
  under *Extra audio match*.
- **No game detected**: the game may be running as a native Wayland client,
  for example with `PROTON_ENABLE_WAYLAND=1` or SDL3 with the Wayland driver.
  This plugin does not capture those. Use OBS's *Screen Capture (PipeWire)*
  instead.

## Limitations

- Frames are copied through the CPU (X GetImage over shared memory). This is
  fine for 1080p/1440p at 60 fps, but uses more CPU than a GPU capture such as
  obs-vkcapture would.
- 8-bit only, no HDR. Windows with a depth other than 24/32 are skipped.
- Native Wayland games and games inside gamescope are not supported.

## Layout

```
src/plugin-main.c          module entry, PipeWire init
src/steam-game-capture.c   OBS source: settings, capture thread, frame/audio output
src/sgc-x11.[ch]           game window discovery and SHM frame grabbing (xcb)
src/sgc-pipewire.[ch]      per-game PipeWire audio capture and linking
tools/sgc-probe.c          diagnostics CLI
scripts/                   Flatpak build / install / probe helpers
```

## License

GPL-2.0-or-later, the same license as OBS Studio.
