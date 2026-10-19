---
title: Simulator
nav_order: 15
---

# Development Device Simulator

CrossInk can run in the [CrossPoint simulator](https://github.com/uxjulia/crosspoint-simulator), which renders the e-ink display in an SDL2 window. Use it for quick sanity checks without flashing firmware every time.

## Platform Support

The simulator is currently configured for macOS on Apple Silicon.

The `platformio.ini` `[env:simulator]` section contains hardcoded `-arch arm64` and Homebrew paths under `/opt/homebrew`.

- Intel Mac users need to remove `-arch arm64` and change Homebrew paths to `/usr/local`.
- Linux requires similar path changes plus a replacement for `lib/simulator_mock/src/MD5Builder.h`, which uses the macOS-only `CommonCrypto` API.
- Native Windows is not supported. Use WSL and follow the Linux adjustments.

## Prerequisites

```sh
# macOS
brew install sdl2

# Linux (Debian/Ubuntu)
sudo apt install libsdl2-dev
```

## Setup

Place EPUB books in `./fs_/books/` relative to the project root. That maps to the SD-card `/books/` path on device.

## Build And Run

```sh
pio run -e simulator
.pio/build/simulator/program
```

Use the X4 Pro environment to enable its touch, frontlight, and Home-key behavior:

```sh
pio run -e x4-pro-simulator -t run_simulator
```

## Keyboard Controls

| Key | Action |
| --- | --- |
| Up / Down | Page back / forward (side buttons) |
| Left / Right | Left / right front buttons |
| Return | Confirm / Select |
| Escape | Back |
| P | Power |
| H | X4 Pro Home key (tap to go Home; hold for 700 ms to toggle the reader menu) |

The `H` mapping is active only in `x4-pro-simulator`.

## Rotation Verification

Build the X3 profile and run the rotation scenario against its isolated SD card:

```sh
pio run -e simulator-X3
mkdir -p /tmp/crossink-rotation-captures
CROSSINK_SIMULATOR_SMOKE_AUTO_ROTATION=1 \
CROSSINK_SIMULATOR_SMOKE_AUTO_ROTATION_CAPTURES=/tmp/crossink-rotation-captures \
  python3 scripts/run_simulator_smoke_test.py --env simulator-X3 --timeout 180 --no-build
```

The scenario changes and reloads the production settings, drives physical button
edges, and supplies synthetic gravity through the simulator HAL. It exercises
EPUB and TXT four-way auto-rotation, one rotation per Power hold, short-action
suppression on long release, invalid/flat/noisy input, and foreground overlays.
Rendered content ranges keep a fixed nonzero reading anchor visible. TXT sleep
reconstruction is compared against the live rotated text body. PGM captures and
JSON orientation/viewport/content receipts are saved in the capture directory.

Run the native policy, HAL, and input-release regressions separately:

```sh
cmake -S test -B /tmp/crossink-rotation-tests -G 'Unix Makefiles'
cmake --build /tmp/crossink-rotation-tests \
  --target AutoRotationTest ReleaseSuppressionTest InputReleaseGuardTest -j 6
ctest --test-dir /tmp/crossink-rotation-tests --output-on-failure \
  -R 'AutoRotation|HalTiltSensor|ReleaseSuppression|InputReleaseGuard'
```

### Physical X3 Check

Synthetic gravity does **not** establish the X3's physical mounting signs.
The current isolated mapping assumes `+ax` is Portrait, `-ay` is Landscape CW,
`-ax` is Portrait 180, and `+ay` is Landscape CCW. This mapping has not yet been
measured on a physical X3.

Before treating the build as hardware-verified:

1. With auto-rotation off, assign **Rotate Page CW** to Power long-press and
   **Next Page** to short-press. From the middle of a book, hold and release:
   rotate once, retain the passage, and do not turn a page on release.
2. Turn **Auto-rotate (EPUB/TXT)** on with tilt page turning off. Check all four
   physical upright positions in EPUB and TXT, holding each steadily. Confirm
   that text is upright and the same passage stays visible.
3. Check face-up/down, diagonal poses, button holds, reader menus, and Quick Lock.
   They must not rotate. Returning to a reader requires a fresh stable pose.
4. After a TXT rotation, sleep before paging again; its reconstructed sleep page
   and wake position must match the visible passage.

No physical sensor-noise, battery-current, e-ink refresh, or electrical wake
measurements are implied by a simulator pass.

## Cache Note

On first open of an EPUB, an **Indexing...** popup appears while the section cache is built in `.crosspoint/`.

If rendering looks stale after a code change, delete `./fs_/.crosspoint/` to clear simulator caches.
