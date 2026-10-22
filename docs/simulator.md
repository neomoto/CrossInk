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
edges, and supplies gravity through the simulator HAL. The X3 profile replays
independently measured raw portrait/left/right samples from one physical device;
inverted remains an ideal derived fixture. Other profiles retain canonical
synthetic inputs. It exercises EPUB and TXT four-way auto-rotation, one rotation
per Power hold, short-action suppression on long release, invalid/flat/noisy
input, and foreground overlays.
Rendered content ranges keep a fixed nonzero reading anchor visible. TXT sleep
reconstruction is compared against the live rotated text body. PGM captures and
JSON orientation/viewport/content receipts are saved in the capture directory.

Check preference retention with no IMU, and active-touch deferral on Sticky's
touch+IMU profile:

```sh
pio run -e simulator -e sticky-simulator
CROSSINK_SIMULATOR_SMOKE_AUTO_ROTATION_PERSISTENCE=1 \
  python3 scripts/run_simulator_smoke_test.py --env simulator --no-build
CROSSINK_SIMULATOR_SMOKE_AUTO_ROTATION=1 \
CROSSINK_SIMULATOR_SMOKE_AUTO_ROTATION_TOUCH=1 \
  python3 scripts/run_simulator_smoke_test.py --env sticky-simulator --timeout 180 --no-build
```

The no-IMU scenario uses the production JSON load/save path and checks that
auto-rotation, tilt enable/direction, and physical-button tilt shortcuts survive
an unrelated settings save. Sensor-only controls and shortcut choices stay hidden
without changing their saved values. Run the same persistence scenario with
`--env simulator-X3` to check the sensor-available path. The Sticky scenario drives
raw HAL contacts while actual EPUB and TXT readers receive synthetic gravity.
It covers two fingers with pinch resizing disabled, three with it enabled,
partial releases down to one finger, and a suppressed single contact. Rotation
must wait for the final release and a fresh stable pose, preserving the passage.

Run the native policy, HAL, and input-release regressions separately:

```sh
cmake -S test -B /tmp/crossink-rotation-tests -G 'Unix Makefiles'
cmake --build /tmp/crossink-rotation-tests \
  --target AutoRotationTest TxtPageLayoutTest ReleaseSuppressionTest InputReleaseGuardTest -j 6
ctest --test-dir /tmp/crossink-rotation-tests --output-on-failure \
  -R 'AutoRotation|HalTiltSensor|TxtPageLayout|ReleaseSuppression|InputReleaseGuard'
```

### Inclined Poses and TXT Reflow

The posture gate requires at least 40% of gravity in the assumed display plane.
It accepts synthetic 30-degree reading poses with small pitch variation, but
rejects flat poses.

Use a normal resumed reader and a nonzero reading page. Check both landscape
directions, direct left/right switching, and small motion jitter. Check flat poses
while already in landscape so that an incorrect portrait change is visible.

For TXT responsiveness, use long physical paragraphs with real font measurement.
Keep a fixed input schedule to expose blocked reader loops. Check reading anchors
and screenshots, not successful orientation assignments alone.

### Physical X3 Check

Native replay does **not** independently establish physical mounting signs.
The X3 fixture now uses raw median acceleration measured on one physical X3
during user-labelled portrait → case-top-left → portrait → case-top-right poses,
plus an explicitly confirmed natural portrait hold:

| Physical pose | Raw ax, ay, az (g) | Required reading orientation |
| --- | --- | --- |
| Natural portrait | `-0.9424, +0.1135, -0.2975` | Portrait (`0`) |
| Case top left | `+0.0298, -0.8653, -0.2571` | Landscape CCW (`3`) |
| Case top right | `-0.0237, +1.0777, -0.1244` | Landscape CW (`1`) |
| Inverted (derived, not measured) | `+1, 0, 0` | Portrait 180 (`2`) |

`ReaderUtils` negates raw X and Y only for X3 auto-rotation, leaving Z unchanged.
The generic classifier still uses canonical `+X` Portrait, `-Y` Landscape CW,
`-X` Portrait 180, and `+Y` Landscape CCW. Raw diagnostic samples, gyro page turns,
manual controls, and other device profiles are unchanged.

The permanent native regression replays the labelled portrait → left → portrait
→ right sequence before the existing synthetic rejection controls. It checks
the actual reader path, not a second copy of the correction. With the old reader
code, this raw replay failed while awaiting left's required Landscape CCW (`3`).
With the X3 correction, the full native scenario passed: both
EPUB and TXT reached left `3` → portrait `0` → right `1` → portrait `0`, retained
their reading anchors, and passed the existing manual Power, disabled/flat/noisy
input, overlay and TXT sleep-reconstruction checks. The relevant native test
selection also passed all 78 tests. These are host-only results, not physical
screen acceptance. The measurements establish input signs on one device, not
fleet-wide hardware qualification or proof that the corrected firmware renders
upright on its physical screen.
Before treating the corrected build as hardware-verified:

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

### Bounded Rotation Diagnostics

Define `CROSSINK_ROTATION_DIAGNOSTICS=1` in a local diagnostic build, retaining
the environment's existing build flags. Serial logging must be enabled with
`LOG_LEVEL` at least `1`. For the native X3 profile, the define can be added
through `build_src_flags` in `platformio.local.ini`.

`ROT` records contain consumed **raw** acceleration in g (before the X3-only
auto-rotation correction), current/candidate/accepted orientations, rejection
or gating reasons, and stable-hold time. Ordinary
sample/gate/gap records share a four-per-second limit; accepted decisions and
applied rotations are recorded immediately. EPUB/TXT application records include
the actual and expected renderer orientation plus numeric reading anchors.
They do not include book paths, titles, or text.

These diagnostics do not change the rotation policy or calibrate sensor axes.
The diagnostic state and calls are omitted when the define is absent. Opening
the X3 USB serial port can reset it even with DTR/RTS disabled: save the reading
position first, and close any monitor before using the flasher.

## Cache Note

On first open of an EPUB, an **Indexing...** popup appears while the section cache is built in `.crosspoint/`.

EPUB keeps the active layout at its original cache path and one alternate layout
for reuse when rotating back. TXT keeps narrow and wide indexes separately.
Both validate the full layout key before reuse; a first visit to a new geometry
can still require reflow. See [Data and Cache](./data-cache.md) for file details.

TXT layout caches rebuild automatically after a version change. Keep legacy TXT
indexes until the reader upgrades their page-only bookmarks.

CAUTION: Removing `./fs_/.crosspoint/` also removes emulator settings and reading
progress. Use a disposable emulator card for a complete reset.
