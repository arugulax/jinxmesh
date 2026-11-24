# Building JinxMesh

You need this page only if you want to change the code or build the firmware yourself. To
install a finished build, see [INSTALL.md](INSTALL.md).

## What there is to build

JinxMesh is two source trees that are built together:

| Tree | Branch of this repository | What it is |
|---|---|---|
| firmware | `firmware-m9-kb` | Meshtastic firmware 2.8.3 with the changes JinxMesh needs |
| device-ui | `device-ui-kbview` | The interface. Most of JinxMesh is here, in `source/graphics/TFT/KeyboardView*.cpp` |

The firmware build pulls device-ui in as a library from a folder next to it, so check the
three branches out side by side:

```
git clone -b main             https://github.com/arugulax/jinxmesh.git jinxmesh
git clone -b firmware-m9-kb   --recurse-submodules https://github.com/arugulax/jinxmesh.git firmware
git clone -b device-ui-kbview https://github.com/arugulax/jinxmesh.git device-ui
```

The firmware's `platformio.ini` (section `[device-ui_base]`) points at `../device-ui`. If
your folders are named differently, change that line. Editing any `platformio.ini` makes
PlatformIO rebuild everything.

## Requirements

- Linux. The M9 image may build on other systems; the simulator and the scripts are
  Linux-only.
- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/). The scripts
  look for it at `~/.platformio-venv/bin/pio`; set `KBVIEW_PIO` if yours is elsewhere.
- For the simulator: an X display (`Xvfb :99 -screen 0 320x240x24 &` is enough), the build
  dependencies of Meshtastic's native target (see the Meshtastic documentation for
  `meshtasticd`), and for the test scripts `xdotool` and ImageMagick.
- About 16 GB of memory builds comfortably. With less, set `KBVIEW_JOBS=2`.

## Building

Two PlatformIO environments are added to the firmware:

| Environment | Builds |
|---|---|
| `thinknode_m9-tft-kb` | The image for the M9 |
| `native-tft-kb` | The simulator: the real firmware and interface as a Linux program with an X11 window |

Both are the stock environments of the same name without `-kb`, plus `-D USE_KEYBOARD_VIEW`.

Directly with PlatformIO, from the firmware folder:

```
pio run -e thinknode_m9-tft-kb
```

The images land in `.pio/build/thinknode_m9-tft-kb/`: `firmware-...bin` for an update and
`firmware-....factory.bin` for a first install.

Or with the scripts on `main`, which run the build in the background and write a log:

```
bash build-m9.sh     # M9 image; log in ~/.cache/kbview/build-m9.log
bash build.sh        # simulator; log in ~/.cache/kbview/build-kb.log
bash build-asan.sh   # simulator with the address sanitizer
```

A log ends with `BUILD_EXIT=<code>` when the build is over. `env.sh` holds the paths the
scripts use; override them with `KBVIEW_FIRMWARE`, `KBVIEW_DEVICE_UI`, `KBVIEW_WORK` and
`KBVIEW_DISPLAY`.

Things worth knowing:

- **One build at a time.** Both environments write into the same `.pio` folder.
- **The version string** ends in the short commit id of the firmware checkout. A new firmware
  commit therefore changes the file names and rebuilds everything. Commit, then build.
- **The build date** shown under Settings, System, About comes from one small file that the
  scripts touch before each build. A plain `pio run` shows the date that file was last
  compiled.
- **Temporary files.** The link step needs more room than a small `/tmp` has. The scripts set
  `TMPDIR` to `~/.cache/pio-tmp`.

## The simulator

```
bash build.sh
bash run.sh                 # the interface with built-in demo data
bash run-real.sh            # against the real firmware; `bash run-real.sh 150` adds 150 made-up nodes
bash shot.sh home Home      # send keys, save a screenshot to ~/.cache/kbview/kb/home.png
python3 monkey.py 6000 3 dive   # 6,000 random keys, seed 3
```

The simulator's file system is `~/.cache/kbview/sim/fs`. It stands in for the SD card, so
maps go below `sim/fs/maps/<style>/`. The firmware's API listens on TCP port 4403, which lets
the Meshtastic command line talk to it: `meshtastic --host 127.0.0.1 --info`.

Keys in the simulator window:

| X key | M9 key |
|---|---|
| arrows | d-pad |
| `Return` | OK |
| `Delete` | OK held |
| `Escape` | Back |
| `Home` | Home |
| `End` | next tab |
| `BackSpace` | Del |
| `ctrl+e` | control center |
| `ctrl+g` | `@` |

What the simulator cannot show: it has no radio, no keyboard chip, no SD card driver, no
Wi-Fi chip and a display that never sleeps. The M9's Chat, Preset, Location and MAP keys have
no stand-in. Xvfb also drops keys sent less than about 60 ms apart, so a test that types
faster proves nothing.

Switches the interface reads in the simulator only: `KBVIEW_DEMO` (demo data), `KBVIEW_SAFE`
(skip actions that restart), `KBVIEW_STRESS=<n>` (add n nodes).

## Tests

- device-ui has unit tests for the parts without a screen (route search, town lookup, MECP
  codec, declination) under `tests/`, built with its own CMake setup.
- `monkey.py` presses random keys and reports whether the simulator survived and how its
  memory moved.
- `build-asan.sh` builds the simulator with the address sanitizer; run it with
  `bash run-real.sh 0 ~/.cache/pio-asan/native-tft-kb/meshtasticd`. Use it for any change
  that touches buffers.

## Code style

device-ui is formatted with clang-format and the style file in the tree
(`--style=file:.trunk/configs/.clang-format`). Format the code you add, not whole upstream
files: some of them do not match the style file, and reformatting them makes later merges
from upstream harder.

## Keeping up with upstream

The two source branches are ordinary branches on top of an upstream commit, so a newer
Meshtastic is a rebase or merge away. Changes to upstream files are kept few on purpose: the
interface is almost entirely new files, switched on by one define.
