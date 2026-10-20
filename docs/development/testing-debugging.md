---
title: Testing & Debugging
parent: Development
nav_order: 4
---

# Testing and Debugging

CrossInk runs on real hardware, so debugging usually combines local build checks, simulator checks, and on-device logs.

## Local checks

Make sure `clang-format` 21+ is installed and available in `PATH` before running the formatting step.
If needed, see [Getting Started](./getting-started.md).

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run -e simulator
pio run -e default
```

`pio run` without `-e` builds the X3/X4 and Sticky firmware targets from `platformio.ini`. Use it for a comprehensive build check, but prefer explicit environments while iterating.

## Native performance profiling

`simulator-X3` uses `-O2`, debug symbols, and frame pointers. Compare firmware
changes against an equally optimized baseline; the previous unoptimized native
build is not a valid proxy for ESP32 performance.

Use a disposable SD-card directory with public fixtures, never the reader's real
card or private books. `CROSSPOINT_SIM_SD` selects its `fs_` directory. Normal
reader-resume workloads need `/.crosspoint/state.json` with `openEpubPath` set to
the logical book path, `lastSleepFromReader: true`, and `readerActivityLoadCount: 0`.
Settings belong in `/.crosspoint/crossink-settings.json`. Check the activity log:
a warm run that opens Home instead of the reader is not a page-turn benchmark.

Setting `CROSSINK_SIMULATOR_PERF_TRACE` enables native-only `PERF render_us` logs.
They measure the activity's render call, not boot time, complete input latency, or
physical e-ink refresh. Leave the variable unset to disable tracing.

On macOS, record the actual application with Instruments:

```sh
pio run -e simulator-X3
xcrun xctrace record --template 'Time Profiler' --time-limit 15s \
  --output /tmp/crossink-profile.trace --no-prompt \
  --target-stdout /tmp/crossink-profile.log \
  --env SDL_VIDEODRIVER=dummy --env SDL_AUDIODRIVER=dummy \
  --env CROSSPOINT_SIM_SD=/absolute/path/to/disposable-card/fs_ \
  --env CROSSPOINT_SIM_WAKE_REASON=power \
  --env CROSSINK_SIMULATOR_PERF_TRACE=1 \
  --env 'CROSSPOINT_SIM_INPUT_SCRIPT=2000:DOWN;2300:DOWN;4000:QUIT' \
  --launch -- "$PWD/.pio/build/simulator-X3/program"
```

Do not pass empty `xctrace --env` values. Use a fresh trace output path each time.
Keep input scripts, settings, file timestamps, and cold/warm cache state identical.
Freeze both binaries before comparison and run them sequentially without
concurrent builds. Normal runs can use `/usr/bin/time -l` for process peak RSS;
the simulator's fixed `MEM`/`SIMRESOURCE` values are not measured ESP32 heap use.

### Measured native optimization results

On macOS arm64, three alternating before/after pairs compared the same optimized
`8c282578` reader against the library-yield and EPUB-write changes. The simulator
dependency was `6c768461`. Medians below are host measurements, not X3 timings.

| Workload | Before | After | Speedup |
| --- | ---: | ---: | ---: |
| Cold title-sorted library, metadata off, 100 EPUB files | 110 ms | 6 ms | 18.3× |
| Cold title-sorted library, metadata on, 100 EPUB files | 268 ms | 46 ms | 5.8× |
| Cold title-sorted library, metadata off, 1,000 EPUB files | 1,099 ms | 51 ms | 21.5× |
| First mixed-style EPUB page, cold cache, antialiasing on, render call | 63.714 ms | 18.708 ms | 3.4× |

The EPUB fixture is `test/epubs/font-prewarm-benchmark.epub` with its mixed-style
spine moved before the instructions; its SHA-256 is
`4d0e621bdca0f66112175fa19adf285903dbcbfec54376f414fbc3a7743c0b67`.
Library fixtures use public test EPUB bytes under distinct filenames.

Time Profiler identified repeated tiny filesystem writes during EPUB layout.
A separate wall-stack sample showed the index builder sleeping throughout sorting
and emission. The fixes coalesce adjacent scalar writes and yield after an
elapsed-work budget, preserving cancellation and idle-task opportunities.
They do not change text layout or cache formats.

SDL presentation and host filesystem behavior differ from the reader's display
and SD card. Actual ESP32-C3 speed and e-ink latency require separate hardware
measurements; these results do not establish either.

Verification also exercised normal startup, cold and warm TXT/EPUB reading, JPEG
and PNG pages, and cached library navigation. All 60 paired screenshot comparisons
and all persisted SD-cache/settings/progress bytes matched. Both full auto-rotation
replays passed, with 43 corresponding grayscale captures identical.

The ESP32-C3 build passed at 6,318,304 image bytes, with 235,296 bytes of OTA
partition headroom and 64,832 bytes of static RAM use, unchanged from baseline.
No device was flashed, and bootloader, partition, and OTA paths were not changed.

All 985 native tests passed. A separate AddressSanitizer/UndefinedBehaviorSanitizer
build passed 87 text-layout, section/progress-persistence, and library-builder tests,
including short writes, truncated records, cancellation, and retained old caches.

### UI rendering optimization

The next pass compares the optimized `cbc3a061` baseline with glyph-resolution,
width-only measurement, bounded text-fit, and checked kerning-search changes.
Instruments profiles separate the activity render thread from main-thread SDL
presentation. The same 80-step long-metadata Library scroll window produced:

| Sampled CPU in the scrolling window | Before | After |
| --- | ---: | ---: |
| Activity render thread | 378 ms | 214 ms |
| Kerning, inclusive | 130 ms | 70 ms |
| Text truncation, inclusive | 91 ms | 34 ms |

Inclusive function times overlap; do not add them. The render thread also contains
the simulator's synchronous framebuffer conversion. Neither it nor main-thread
SDL presentation is a measured ESP32-C3/e-ink response time.

Normal, unprofiled application runs replayed Home, Library scrolling and sorting,
Settings, and reader-drawer navigation. Each case used three alternating
before/after pairs with identical public books, settings, and common warm caches.
Medians are the median of per-run navigation-frame medians; p95 uses the pooled
navigation frames. All times below measure activity-render calls, in milliseconds.

| UI workload | Before median | After median | Before p95 | After p95 |
| --- | ---: | ---: | ---: | ---: |
| Minimal Library, 1,000 short filename titles | 0.643 | 0.585 | 1.111 | 1.386 |
| Lyra expanded Library, 100 long-metadata titles | 3.942 | 2.431 | 8.074 | 4.938 |
| Settings tabs | 0.581 | 0.533 | 1.326 | 1.242 |
| Settings rows | 0.597 | 0.607 | 1.189 | 1.168 |
| Reader drawer | 0.681 | 0.620 | 1.499 | 1.358 |
| Home with recent-book card | 0.744 | 0.579 | 1.189 | 1.251 |
| Library sort picker and sorted view | 3.136 | 2.138 | 4.688 | 4.119 |
| Arabic Settings, large UI | 0.751 | 0.655 | 1.302 | 1.261 |

The long-metadata fixtures use public `font-prewarm-benchmark.epub` content under
distinct filenames, with synthetic long titles, authors, series, and genre.
Library settings are title sort ascending, metadata on, expanded rows, and
series/genre shown; the short-filename case uses Minimal with metadata off.
Other screens use Lyra. The Arabic case sets `language: "AR"` and `uiScale: 2`.
Navigation replays include actual button events, not direct activity injection.

The long Library median improves by 1.62×; smaller views remain sub-millisecond
and noisy. Some p95 values increased, so these measurements do not establish a
general tail-latency improvement. No polling-delay, display-waveform, input,
appearance, persistent-cache, bootloader, partition, or OTA changes were made.

All 48 application runs passed their intended-activity checks; 72 corresponding
screenshots and all persisted cache/settings/progress bytes matched exactly.
All 1,001 native CTest entries passed. The standalone rounding runner executes
its 21 regression groups once rather than being misregistered as GoogleTest
output. Forty kerning-map cases, that runner, and the real scalable-font adapter
also passed AddressSanitizer/UndefinedBehaviorSanitizer checks.
The complete reader auto-rotation replay also passed; all 43 corresponding
grayscale captures matched the pre-UI baseline exactly.

The ESP32-C3 candidate is 6,318,928 bytes, with 234,672 bytes of partition
headroom. Static RAM remains 64,832 bytes. No device was accessed or flashed;
physical X3 UI latency and peak heap remain unmeasured.

#### Complexity report

Manual cyclomatic counts include runtime decisions, ternaries, and Boolean
operators; preprocessor branches and compile-time assertions are excluded.

| Function | Before | After |
| --- | ---: | ---: |
| `EpdFontFamily::getTextDimensions` | 31 | 6 |
| `EpdFontFamily::getFallbackCodepoint` | 7 | 2 |
| `GfxRenderer::getTextWidth` | 10 | 3 |
| Split / packed kerning lookup helpers | 7 / 6 | 10 / 9 |

Extracted helpers separate codepoint normalization, scalar glyph snapshots,
cursor/bounds accumulation, prepared-width dispatch, and bounded fitting.
New measurement helpers have counts at most 9. Truncation's outer routine stays
at 8, with its unchanged UTF-8 candidate lambda at 5.

## Flash and monitor

Flash firmware:

```sh
pio run -e default --target upload
```

Open serial monitor:

```sh
pio device monitor
```

Optional enhanced monitor:

```sh
python3 -m pip install pyserial colorama matplotlib
python3 scripts/debugging_monitor.py
```

## Useful bug report contents

- Firmware version and build environment
- Exact steps to reproduce
- Expected vs actual behavior
- Serial logs from boot through failure
- Whether issue reproduces after clearing the affected book cache or using **Clear Reading Cache**

## Common troubleshooting references

- [Common Issues](../troubleshooting.md)
