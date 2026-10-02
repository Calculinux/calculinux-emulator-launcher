# calculinux-emulator-launcher

The window that the [Calculinux](https://github.com/Calculinux/meta-calculinux)
emulator AppImage opens before it boots: it lists the published system images,
downloads the one you pick (with progress, and resuming an interrupted
download), and deletes the ones you no longer want.

It is a small C++17 program using Dear ImGui on SDL2's software renderer, plus
libcurl. The emulator's `AppRun` script runs it, reads the chosen image id
from its output, and starts QEMU on that image. The same binary also does the
emulator's headless downloads (`fetch`, `list`, `remove`).

## Build

Needs CMake, a C++17 compiler, and the SDL2 and libcurl development packages.
Dear ImGui and nlohmann/json are vendored in `third_party/`.

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`-DLAUNCHER_BUILD_GUI=OFF` builds only the headless commands (no SDL2 needed).

## Try it without the emulator

```sh
tools/make-test-repo.sh /tmp/repo http://127.0.0.1:8000
tools/range_server.py --dir /tmp/repo --port 8000 --rate 2000000 &
build/calculinux-emulator-launcher --state /tmp/state \
    --index-url http://127.0.0.1:8000/index.json gui
```

`make-test-repo.sh` writes a few fake images and an index; `range_server.py`
serves them slowly enough to watch a download. Without `--index-url` the
launcher uses the real index at
`https://opkg.calculinux.org/emulator/calculinux-qemuarm/index.json`.

## Commands

```
calculinux-emulator-launcher [options] <command>

  gui [--ssh-port N]                     choose an image in a window
  fetch [ID|--auto|--url URL] [--update] make sure an image is downloaded
  list [--json] [--offline]              show images and their status
  remove ID                              delete an image and its saved state
  reset ID                               delete an image's saved state
  migrate                                convert an old state directory
```

`gui` and `fetch` print `id=<image id>` on success (`gui` also
`ssh_port=<port or off>`).

| Exit code | Meaning |
|---|---|
| 0 | An image was chosen, or the command succeeded |
| 1 | Error |
| 2 | Usage |
| 3 | Unknown image, or not downloaded and the index is unreachable |
| 4 | Network error |
| 5 | Checksum mismatch |
| 6 | Not enough disk space |
| 10 | `gui`: the window was closed without choosing |
| 20 | `gui`: no window could be opened |

Options: `--state DIR`, `--index-url URL`, `--default-id ID`,
`--fallback-url URL`, `--app-version STR`, `--ca-bundle FILE`; see `--help`.

In the window: Up/Down select, Enter boots (downloading first if needed),
Delete removes, F5 refreshes, Esc quits.

## State directory

`$CALCULINUX_EMULATOR_HOME`, else `~/.local/share/calculinux-emulator`:

```
layout                  "2"
settings                last_id=..., ssh_port=...
index.json              last index fetched, for labels when offline
images/<id>/
  base.qcow2  base.qcow2.sha256  meta.json
  base.qcow2.part  base.qcow2.part.meta     download in progress
  disk.qcow2  uboot-env.img  image-id       written by AppRun
local/                  AppRun's state for --image FILE
```

An image id is the release tag, or `continuous-main` / `continuous-develop`
for the latest build of those branches. Each image has its own overlay
(`disk.qcow2`) and U-Boot environment, so switching images keeps the changes
made in each. Updating an image to a newer build discards its overlay, which
only fits the base it was created on.

A state directory from before the launcher (one image and one overlay in the
top level) is converted on first run; its image appears under the id the
index gives it.

## Environment

| Variable | Effect |
|---|---|
| `CALCULINUX_EMULATOR_HOME` | State directory |
| `CALCULINUX_EMULATOR_INDEX_URL` | Image index (`file://` works) |
| `CALCULINUX_EMULATOR_LAUNCHER_SCALE` | UI scale factor, instead of the display's |
| `SSL_CERT_FILE`, `CURL_CA_BUNDLE` | CA certificates, instead of the host's |

For tests: `CALCULINUX_LAUNCHER_SMOKE=N` closes the window after N idle
frames, `CALCULINUX_LAUNCHER_SMOKE_KEYS=down,enter,...` presses keys first,
and `CALCULINUX_LAUNCHER_SCREENSHOT=file.bmp` saves the last frame. With
`SDL_VIDEODRIVER=dummy` this runs without a display.

## Updating the vendored libraries

```sh
tools/update-imgui.sh v1.92.9b v3.12.0   # Dear ImGui tag, nlohmann/json tag
```

## License

MIT. Dear ImGui (`third_party/imgui`) and nlohmann/json
(`third_party/nlohmann`) are MIT licensed; see the license files there.
