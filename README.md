# Steam Game Capture for OBS (Linux)

An OBS Studio source that captures a running Steam game, **video and audio
together**, on Linux. Add it to a scene and it finds the game on its own.
It keeps working when you restart the game or switch to another one.

- **Video (GPU, zero-copy):** finds the game's X11/XWayland window by the
  `STEAM_GAME` window property or the `steam_app_<AppID>` window class. Both
  Proton and native games set these, and the Steam client window is ignored.
  Each frame, the plugin asks the X server for the window's current buffer as
  a DMA-BUF (DRI3) and imports it into OBS as a GPU texture. No pixels go
  through the CPU. The game's swapchain buffers are imported once and reused,
  and each frame is snapshotted with a GPU copy so the game can render the
  next one without tearing.
- **CPU fallback:** if DMA-BUF export or import fails, for example with a
  driver that can't import the buffer, the plugin falls back to an MIT-SHM
  copy for that window.
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

The install script also grants OBS access to the X11 display with
`flatpak override --user --socket=x11 com.obsproject.Studio`. On Wayland,
Flatpak OBS only gets `fallback-x11`, which hides X11, but Steam games are X11
clients. Pass `--no-x11-override` to skip this step. To undo it later, run
`flatpak override --user --nosocket=x11 com.obsproject.Studio`.

Restart OBS, then add **Sources → + → Steam Game Capture**.

## Build & install (native OBS)

Install the OBS development package first (for example `obs-studio-devel`,
`libobs-dev`, or `obs-studio` on Arch). You also need the `xcb`,
`xcb-composite`, `xcb-shm`, `xcb-res`, `xcb-dri3`, `libdrm` and
`libpipewire-0.3` development files. Then:

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
| Capture cursor | Draws the mouse cursor while it is over the game and the game has focus (on by default). Games that draw their own cursor already include it in the frame |
| Only capture video while visible | Skips frame grabs when the source isn't shown anywhere |
| Capture game audio | PipeWire per-game audio |
| Extra audio match | Also captures streams whose app name, node name or binary contains this text. Use it for games that play sound from a helper process |
| X11 display | Empty means `$DISPLAY` |

Video is captured once per OBS frame. The status line at the top of the
properties shows the captured game, the capture path (*GPU zero-copy* or
*CPU copy*) and the number of linked audio streams.

## Troubleshooting

`sgc-probe` uses the same capture code without OBS:

```sh
./scripts/probe-flatpak.sh                      # list detected game windows
./scripts/probe-flatpak.sh --grab /tmp/f.ppm    # save one frame
./scripts/probe-flatpak.sh --bench 120          # time CPU frame grabs
./scripts/probe-flatpak.sh --dmabuf 120         # check zero-copy DMA-BUF export
./scripts/probe-flatpak.sh --cursor 10          # report cursor position/visibility
./scripts/probe-flatpak.sh --audio 5            # show captured audio level per second
```

(`build/sgc-probe` works the same way for native builds.)

- **"Cannot connect to the X11 display"**: OBS needs X11 access. Re-run
  `scripts/install-flatpak.sh` without `--no-x11-override`, then restart OBS.
- **Video works but there are 0 audio streams**: the game plays audio from a
  different process. Find its name with `pw-dump` or `qpwgraph` and enter it
  under *Extra audio match*.
- **Black or corrupted image with *GPU zero-copy***: start OBS with
  `SGC_DISABLE_DMABUF=1` to force the CPU path, and please report your GPU
  and driver. For Flatpak:
  `flatpak override --user --env=SGC_DISABLE_DMABUF=1 com.obsproject.Studio`.
- **No game detected**: the game may be running as a native Wayland client,
  for example with `PROTON_ENABLE_WAYLAND=1` or SDL3 with the Wayland driver.
  This plugin does not capture those. Use OBS's *Screen Capture (PipeWire)*
  instead.

## Limitations

- The zero-copy path needs DRI3 with DMA-BUF export, which XWayland with
  Mesa (AMD/Intel) provides. It is tested on AMD (RX 6700 XT) with Mesa.
  NVIDIA's proprietary driver may end up on the CPU path.
- No HDR. 10-bit windows are only captured on the GPU path.
- Native Wayland games and games inside gamescope are not supported.

## Layout

```
src/plugin-main.c          module entry, PipeWire init
src/steam-game-capture.c   OBS source: settings, capture thread, frame/audio output
src/sgc-x11.[ch]           game window discovery, DRI3 DMA-BUF export, SHM fallback (xcb)
src/sgc-pipewire.[ch]      per-game PipeWire audio capture and linking
tools/sgc-probe.c          diagnostics CLI
scripts/                   Flatpak build / install / probe helpers
```

## License

Released into the public domain under [The Unlicense](LICENSE).

The compiled plugin links against libobs, which is GPL-2.0-or-later, so
binaries you distribute together with OBS also fall under the GPL's terms.
