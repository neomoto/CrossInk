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

For input-to-presentation correlation, a paired local simulator SDK checkout can
enable application hooks with `-DCROSSPOINT_SIM_PERF_TRACE_API=1`, then set
`CROSSPOINT_SIM_PERF_TRACE=1` at runtime. These hooks are native-only and tracing
is off by default. The public dependency pin is not changed to an unpublished
SDK commit; a local symlink override validates paired checkouts, not a portable
Git dependency install.

`SIMPERF` records use actual applied input edges, observed phases, request IDs,
render generations, and SDL presentation IDs. A release-driven action's
down-to-presentation time includes the actual button hold. Only use a
`present_return` with `valid=1`; overlapping conversion or failed SDL operations
make the frame association ambiguous. Coalesced requests are not automatically
lost input. Use identical tracing settings for latency comparisons, and leave
this trace off for ordinary render-call benchmarks.

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

### Shared pipeline and rotation optimization

This pass compares the optimized `b7d1052a` application baseline with shared
glyph drawing, ASCII kerning, metadata/ZIP I/O batching, and bounded layout
reuse. Both native binaries use `-O2`. Three alternating pairs for each of
16 normal-application workloads produced 96 runs; causal tracing was disabled
and the existing render-duration logger was enabled on both sides.

The following are macOS arm64 **host milliseconds**, not ESP32 or e-ink timings.
Rendering values are medians of the three per-run navigation medians. Indexing
values measure the complete logged index operation; cold means a cold application
cache, not a flushed operating-system disk cache.

| Workload | Before (ms) | After (ms) |
| --- | ---: | ---: |
| Short library rows | 1.388 | 1.268 |
| Long metadata library rows | 3.702 | 3.598 |
| Settings tabs | 1.178 | 1.072 |
| Settings rows | 1.180 | 1.078 |
| Reader menu | 1.246 | 1.111 |
| Home | 1.268 | 1.129 |
| Library sort menu | 3.354 | 3.240 |
| Large Arabic settings | 1.338 | 1.292 |
| EPUB turns, cold-start run | 7.032 | 6.540 |
| EPUB turns, warm-start run | 7.132 | 6.489 |
| TXT turns, cold-start run | 5.966 | 5.540 |
| TXT turns, warm-start run | 6.075 | 5.575 |
| TXT cold-start maximum render | 32.887 | 21.812 |
| Mixed JPEG/PNG/text page turns | 7.430 | 6.528 |
| Cold metadata indexing, 1,000 books | 356 | 258 |
| Explicit unchanged rescan, 1,000 books | 89 | 52 |
| One changed book among 1,000 | 96 | 91 |

The TXT startup row is the median of each run's maximum startup render.
There is substantial host scheduling variation, especially in the long-library
case; do not interpret the small improvements as precise device latency claims.
All 129 paired full SDL captures are pixel-identical. Book bytes, settings,
application state, and persisted progress match between each pair. Warm metadata
controls parse no books; changed-one controls parse exactly one and reuse 999.

Input latency has a separate control: 18 contemporaneous alternating normal
runs with causal tracing enabled and the render-duration logger unset. All 432
isolated input/render associations have valid presentations and are
release-driven. The table reports medians of the three per-run medians:

| Release to SDL presentation return | Before (ms) | After (ms) |
| --- | ---: | ---: |
| EPUB | 17.057 | 17.231 |
| TXT | 17.945 | 18.012 |
| Library | 17.614 | 18.013 |

There is **no meaningful end-to-end button-latency improvement** in this control,
despite lower render work. After-change request-to-render medians are 19–20 us;
down-to-presentation also includes the approximately 79 ms actual button hold.
Do not report a shorter render call as a faster physical button response.
Button hold, debounce, rotation-stability intervals, and e-ink drivers are
unchanged.

Default Inter UI fonts are **1-bpp**. Optimizing only the 2-bpp reader path does
not optimize those screens. Monochrome drawing now decodes source-byte chunks;
2-bpp drawing merges framebuffer masks in both physical directions. Read-only
ASCII kerning tables retain the original Unicode and callback fallbacks. The
37 generated headers preserve all 434 original arrays byte-for-byte; all 9,472
new class IDs were independently checked against the original maps. This was
metadata-only regeneration, not font substitution or rerasterization.

Index metadata fields share one checked prefix traversal. ZIP central-directory
headers use one checked 46-byte read rather than eight small reads and three
seeks. Neither change introduces a whole-directory cache or a persistent value
cache. EPUB retains the last activated layout at its original path plus one
alternate; TXT retains narrow/wide indexes with the original file reserved for
legacy page-anchor migration. File formats and full-key validation are unchanged.

A separate controlled four-direction production-reader replay passes with all
43 grayscale captures identical to the baseline:

| Repeated-rotation metric | Before | After |
| --- | ---: | ---: |
| EPUB section builds | 15 | 2 |
| Sampled render-thread CPU (ms) | 966 | 459 |

This is one profiled replay pair, not a twofold improvement in wall-clock
rotation latency: sensor holds, scripted waits, and e-ink refresh are not removed.

A normal TXT P0-to-landscape scenario also exits to Home and reopens the reader.
All four full SDL surfaces match the baseline; the canonical six-byte zero
anchor remains unchanged, the original narrow index is retained, and the wide
index is byte-identical to the baseline's active wide index. Initial portrait,
auto-rotation-enabled settings, and reader-resume load count are explicit
controls, not inferred from a previously completed card.

The full native suite passes 1,092 CTest entries. Targeted ASan/UBSan coverage
includes glyph/kerning bounds, ZIP/index I/O failures, interrupted layout
transactions, legacy TXT anchors, and constructor-only EPUB sync consumers.
The ESP32-C3 image is 6,335,488 bytes with 218,112 bytes of OTA-partition headroom;
reported static RAM is 64,832 bytes. Its offline image checksum and validation
hash pass. No reader, serial port, or physical SD card was accessed; physical
latency, peak heap, and peak stack remain unmeasured.

Local evidence is under `crossink-global-performance-3ctrxh4n` in the macOS
temporary directory: `final-paired-runs/results.json`,
`final-paired-analysis.json`, the frozen native binaries, source/image identity
receipts, and Instruments traces. Normal baseline SHA-256 starts `3b375d0c`;
candidate starts `d7c271bf`. The paired simulator SDK is local commit `43dbb83`;
the public dependency pin remains `6c768461`. A local symlink build is not proof
that an unpublished SDK Git pin can be fetched.

Under the manual complexity convention above, the raster dispatcher is 10,
new monochrome helpers are 1–6, and the kerning wrapper is 9 for bitmap fonts or
10 with scalable-font dispatch. The ASCII shortcut adds one decision while
avoiding CJK checks for proven ASCII pairs.

### Bounded read and font-lookup optimization

This follow-on pass uses eight implementation agents running
`openai-codex/gpt-6.1-sol` at `xhigh`, plus a safety reviewer. Runtime model
receipts are retained; an inherited prompt header is not model provenance.

Changes reuse resolved glyphs within one draw call, narrow long sparse kerning
rows, retain accepted UI line widths, and group adjacent serialized reads.
Library metadata and sort keys use sequential reads. XML metadata sizing passes
a bounded ZIP entry descriptor to streaming; the reader closes the file between
operations and validates the entry again before using it. There is no new
whole-font or whole-archive cache.

The device-code comparison keeps the native SDK fixed at local `43dbb83`.
It compares reader baseline `b114c587` against the new reader and FreeInkUI
sources. Native binaries start with SHA-256 `d7c271bf` and `9d8c85d1`.
The separate packed-pixel simulator optimization is excluded from this table.
There are three alternating before/after pairs for each of 16 workloads:
96 normal app launches, no smoke-driver shortcuts. Render numbers are the
median of three run medians, measured inside the native app with the same
instrumentation on both sides. They are not physical e-ink timings.

| Workload | Before render (ms) | After render (ms) |
| --- | ---: | ---: |
| Library, short titles | 1.275 | 1.261 |
| Library, long titles | 3.547 | 3.493 |
| Settings tabs | 1.072 | 1.044 |
| Settings rows | 1.075 | 1.046 |
| Reader menu | 1.138 | 1.146 |
| Home | 1.140 | 1.122 |
| Library sort picker | 2.836 | 2.919 |
| Arabic settings, large text | 1.261 | 1.263 |
| EPUB, cold cache | 6.553 | 5.735 |
| EPUB, warm cache | 6.581 | 5.677 |
| TXT, cold cache | 5.290 | 5.374 |
| TXT, warm cache | 5.586 | 5.335 |
| EPUB with JPEG and PNG images | 7.120 | 6.801 |

| Index operation, 1,000 books | Before (ms) | After (ms) |
| --- | ---: | ---: |
| Cold scan: 1,000 parsed | 272 | 262 |
| Unchanged scan: 1,000 reused | 66 | 69 |
| One changed book: 1 parsed, 999 reused | 101 | 67 |

Small differences and mixed image/sort results are noisy; this is not an
across-the-board speedup claim. In particular, unchanged indexing does not
improve in these runs. Every one of 129 full SDL surface comparisons matches,
and settings, progress, and bookmarks have no before/after differences.
Public fixture book hashes are unchanged.

The full native suite passes 1,137 CTest entries; the targeted ASan/UBSan suite
passes 278. FreeInkUI passes 296,126 host checks. An independent packed-pixel
oracle passes ASan/UBSan checks of byte/plane combinations, inversion,
incomplete planes, and tail boundaries.

The safety review found and closed a batched SD-read error case. SdFat can
copy a valid prefix and advance its cursor before returning `-1`. The inverse
page lookup now retains complete position matches in that prefix, rather than
discarding them. A fail-before/pass-after regression covers matches before
the failed sector and across the 32-entry batch boundary.

The ESP32-C3 image is 6,337,456 bytes: 1,968 bytes larger, with 216,144 bytes
of OTA-partition headroom. Static RAM remains 64,832 bytes. Offline checksum
and validation hash pass. New scratch storage is bounded: 32 bytes for UI
widths, 99 bytes for a sort-key prefix, 128 bytes for TXT offsets, and 64 bytes
for EPUB inverse offsets. Production grouped POD reads use at most 26 bytes
of payload scratch. These are separate call paths, not a measured peak stack.
No physical reader was accessed; peak device heap, stack, and latency remain
unmeasured.

Evidence is retained under `crossink-sol-performance-5hh69hvu` in the macOS
temporary directory, including paired results, source and model receipts,
frozen binaries, and the offline image. Firmware SHA-256 is
`bfe70173a252dcdf603bcda514261b52f77a97fab1fc7a768f10c9165d81124d`.
The local SDK builds do not establish that unpublished dependency commits
can be fetched by someone else.
FreeInkUI is retained in local submodule commit `2cad542`; the native simulator
change is local commit `8deaedc`. Neither was pushed. The reader's public native
simulator pin remains `6c768461`; testing uses the ignored local symlink config.
Before publishing the reader branch, its new FreeInkUI submodule commit must
be available from the configured submodule remote.

#### Simulator-only conversion attribution

A separate 24-launch comparison keeps the new reader and FreeInkUI fixed.
Only the native packed-pixel converter changes, from binary `9d8c85d1` to
`00dd8b09`. All 36 full SDL surface pairs and retained app states match.

| Native workload | Old converter render (ms) | New converter render (ms) |
| --- | ---: | ---: |
| Library, long titles | 2.037 | 1.877 |
| EPUB, warm cache | 3.933 | 3.063 |
| TXT, warm cache | 3.658 | 2.534 |
| EPUB with JPEG and PNG images | 4.772 | 3.523 |

These are paired native-app results, not X3 gains. Absolute host timings drift
between batches; do not combine this table with the device-code table to
calculate a cumulative speedup. The converter adds no frame buffer and leaves
SDL pacing and shared-buffer ownership unchanged.

#### Input-to-presentation check

Eighteen normal launches compare baseline `d7c271bf` with the complete native
candidate `00dd8b09`. Each launch has 24 isolated release-triggered page/list
actions. All 432 input-to-render-to-valid-present joins succeed, with no invalid
present records. The older render logger is disabled for this experiment.

| Activity | Release to SDL present, before (ms) | After (ms) |
| --- | ---: | ---: |
| EPUB | 17.281 | 18.096 |
| TXT | 18.606 | 18.376 |
| Library | 18.643 | 18.179 |

Values are pooled medians of 72 actions per activity and variant. This pass
reduces rendering work but does not establish a button-to-presentation latency
win. The requested 80 ms hold is separate from these release-edge timings;
press-to-present medians remain about 96–98 ms. SDL present return is not
physical panel visibility. Input polling, debounce, refresh waveforms, and
presentation pacing have not been shortened.

Matched Instruments profiles retain the before/after call stacks for long-title
UI, warm EPUB, cold indexing, and one-book reconciliation. In warm EPUB, the
unchanged native grayscale converter remains a major sampled cost in this
device-code-only comparison. Profile samples are diagnostic, not substitutes
for the repeated unprofiled timing results above.

#### Rotation and resume safety

Fresh production-reader replays pass for the baseline, device-code candidate,
and complete native candidate. Each produces the same 43 grayscale captures
and performs only two EPUB section builds. Four orientations, content anchors,
settings persistence, Power hold/release, and disabled/flat/noise/overlay gates
remain intact.

| Replay variant | Sampled render-thread CPU (ms) | Section builds |
| --- | ---: | ---: |
| Previous optimized baseline | 365 | 2 |
| New device code, unchanged native converter | 345 | 2 |
| Complete native candidate | 296 | 2 |

This is one profiled replay per variant. It does not establish a corresponding
wall-clock or physical rotation-latency improvement.

A separate normal TXT scenario rotates page zero from portrait to landscape,
exits to Home, and reopens. All four complete SDL captures match. The six-byte
zero progress anchor and original narrow index remain intact; the wide index
is byte-identical to the baseline. This scenario does not use a smoke driver.

### X3-bounded Home and next-page ports

Two ideas were adapted without importing PSRAM requirements or changing input,
power, SPI, refresh waveforms, or persistent formats:

- [YACP, `23568ad3`](https://github.com/Sichroteph/YACP/tree/23568ad3580740068cd3466f400b6900f08a1a15):
  defer OPDS, bookmark, and clipping presence checks while the Minimal/Dashboard
  Home menu is hidden. Refresh them when the menu opens. Visible-menu themes,
  an explicit initial menu selection, and visible Home statistics stay eager.
- [CrossDiTo, `4506e2e9`](https://github.com/dito94/CrossDiTo/tree/4506e2e9516dc1330695cb6f4c337e7d12a7fe55):
  prepare one exact next EPUB page during the existing idle window and consume
  it once. X3 uses stricter byte, allocation, and heap gates rather than the
  PSRAM reader's page-count-only cache. See [cache policy](../data-cache.md#idle-next-page-preparation).

The projects' existing MIT copyright/license is retained, with pinned source
attribution at the adapted implementations. Two implementation workers and one
safety reviewer ran on `openai-codex/gpt-6.1-sol` without fallback. The request
was xhigh; this runtime records the reasoning level as `max`.

#### What the profiler establishes

Instruments File Activity follows real calls in the native application.
For a Dashboard fixture containing all optional data, hidden-menu startup
performs 17 optional-context filesystem calls before the change and zero
afterward. Opening the menu performs the deferred checks; reopening refreshes
presence again. The visible Lyra menu still performs its 17 startup calls.
This moves work, rather than eliminating every future metadata read.

On 18 paced forward EPUB turns, foreground `Section::loadPageAt` filesystem
calls fall from 3,458 to 182: 19 page loads become the initial load plus
18 consumed prepared pages. A preparation log alone is not evidence of a hit;
foreground call stacks establish consumption.

Total filesystem calls in that trace increase from 4,637 to 7,574. Bounded
decoding adds native cursor queries, and an unused final successor is prepared.
Time Profiler does not establish a total CPU reduction. Final-binary File
Activity replays reproduce these Home and page-call counts after the empty-cache
fast path was added.

#### Final paired native measurements

The frozen baseline and final binary ran 54 normal launches: three pairs for
each of six reader scenarios, plus one pair for each of nine Home/menu variants.
All 103 complete SDL surface pairs match. Source books, settings, progress,
bookmarks, and other retained primary state have no before/after differences.

Each change below is the median of three within-pair changes in run-median
render-call time; negative means faster. Execution order alternates between
pairs. Reader scenarios contain 18 turns, or 12 for alternating/image cases.

| Reader scenario | Median paired change | Range across pairs |
| --- | ---: | ---: |
| Styled EPUB, paced forward | -10.7% | -11.2% to +2.9% |
| Rapid forward control | -14.3% | -29.0% to -3.2% |
| Paced forward/backward | -6.0% | -12.8% to -1.8% |
| Dense prose, paced forward | -10.7% | -11.9% to -9.8% |
| Dense small-font control | +2.9% | +1.8% to +3.5% |
| JPEG/PNG control | +5.0% | -3.3% to +38.5% |

Small-font pages were not prepared. Rapid runs prepare only before and after
the burst, and image runs log just two preparations; do not attribute their
timing changes to sustained page reuse. Home rendering differences are small
and mixed. Tail timings also vary, particularly for alternating and image
cases. This is not an across-the-board CPU, button-latency, or panel-speed win.

#### Safety and retained state

The independent review caught speculative ownership surviving into font
discovery/replacement. The fix releases it before all six font-size feedback
routes and before replacement-font loading, using the existing lock ownership
rather than nesting locks. The reviewer closed the finding.

The final native binary passes these actual-application scenarios:

- Four-way EPUB/TXT rotation, Power hold/release, disabled/flat/noise/overlay
  gates, settings, content anchors, and TXT sleep reconstruction: all 43 PGM
  pairs and 30 anchor records match the baseline.
- Front-button long-hold and side-button short-press font changes: six complete
  SDL surface pairs match; each larger-font frame differs from its initial
  frame, and each restored frame matches it. Settings and progress match.
- TXT page-zero rotation, Home exit, and reopen: all four surfaces match,
  with the original narrow index and retained primary state unchanged.
- A malformed optional successor is rejected once without changing the current
  page, source book, or cache bytes. Both applications exit normally.

Every reader probe asserts actual `EpubReader`/`TxtReader` entry. Normal entry
clears `lastSleepFromReader` for crash-loop protection, so a copied post-run seed
can otherwise resume Home. Early font probes and two profiles with the wrong
initial activity were rejected, not counted as reader evidence.

Process RSS is not a page-allocation measurement. Separate live `heap` and
`malloc_history` snapshots after the same 18 turns find 7,584 bytes in 82 live
allocations under the prepared page's decoder, and a 7,696-byte total live-heap
delta. This is one public native fixture, not a measured C3 peak, allocator
fragmentation bound, or physical-device safety guarantee. Diagnostic binaries
were disposable copies; debug entitlements were not applied to firmware.

The final source passes 1,158 CTest entries and 86 affected ASan/UBSan checks.
The ESP32-C3 image is 6,340,640 bytes, with 212,960 bytes of OTA headroom;
static RAM remains 64,832 bytes. The image grows 3,184 bytes over the preceding
optimized baseline. Static RAM does not account for dynamic page ownership.
Offline image checksum and validation hash pass. Firmware SHA-256 is
`13a65f8262a86bb8b826da21ef5bfab202c04a83c91495d255aaa30e2bc845a7`.
The cached IDF app descriptor is not the source identity; the receipt includes
the frozen binary hash and all 20 implementation/test source hashes.

Evidence is retained under `crossink-x3-ports-40518ip0` in the macOS temporary
directory. Native baseline SHA-256 starts `00dd8b09`; final SHA-256 starts
`d794a28a`. The local SDK revisions and public dependency-pin caveat above still
apply; this pass changes neither SDK. The optimization runs did not access a
physical reader or private card. The subsequent authorized installation is
documented below; neither pass pushed code or published a PR.

#### Subsequent physical X3 installation

The same firmware hash above was installed in the designated app1 test slot.
Full readback matched the image; the original app0, bootloader, partition table,
NVS, recovery selector, and protected flash suffix passed preservation checks.
The final live OTA record selects app1 with sequence 12, state VALID, and a valid
CRC. The original selector sector remains byte-identical.

The first trial became ABORTED after an immediate USB-serial reset. Retrying
only the test selector, with serial closed through the first working UI,
succeeded without rewriting the application. Confirm the trial is VALID before
opening a serial monitor; a reset during pending verification can trigger rollback.

A subsequent 20-second capture from the confirmed image observed X3/UC8253,
initialized IMU, detected SD, and boot/periodic memory reports, with no observed
panic, watchdog, or brownout. The user confirmed a responsive screen and the
original book and saved passage. The final official reset left the app running
with serial closed.

Host flashing did not access, format, or clear the SD card; no independent SD
backup was taken. These checks establish installation, bounded startup health,
and user-reported state preservation, not sustained heap or physical latency.

### Sleep-entry and wake work

On X3, plain Dark/Light/Blank sleep screens now reach their final refresh without
an intermediate Entering Sleep popup. A cached cover also skips that popup when
the existing open, header parse, and decode succeed. Missing covers still show
feedback before preparation. Unreadable or malformed covers still show feedback
before the existing default/custom fallback. Other devices retain their popup
policy.

The cover path reuses its existing lookup, file handle, and row scratch storage.
Native two-bit BMP rows with unchanged geometry and no active ditherer normalize
their four palette entries once per row. Already-normalized pixels use a packed
copy; other native palette orders use a 16-byte two-pixel lookup. Tail bits are
cleared, BMP padding is excluded, and read/error/counter/rewind behavior is retained.

Eligible unscaled bitmap rows resolve plane, orientation, and clipping outside
the pixel loop. Packed writes preserve monochrome transparency, overlay and
absolute grayscale, all four rotations, physical strip bounds, and untouched
destination bits. Existing algorithms remain for inputs outside these guards.
These paths add no heap allocation, full framebuffer, or persistent image cache.
Refresh waveforms, final pixels, reader-exit saves, storage shutdown, and board
power sequencing are unchanged.

Wake settings loading now treats an already-empty automatic device name as
canonical. Invalid nonempty short names still require repair and persistence.
The existing settings smoke checks canonical empty/valid names, persisted repair,
stable reload, and genuine legacy migration.

#### Local evidence

Normal application runs exercise 17 paired sleep/wake cases: direct screens,
valid, missing, invalid-header and truncated covers, landscape, cover/custom
from Home, Quick Resume in night mode, TXT byte-anchor preservation, and four
packed-image fixtures covering odd widths, top-down/bottom-up rows, permuted
palettes, panel-width rows, and downscaling. All 34 complete before/wake SDL
surface pairs and 17 sleep B/W backing buffers match the baseline. Source books
and retained settings/progress/bookmarks match.

The B/W buffer is not proof of both grayscale planes; native rendering also does
not exercise the physical controller's Absolute/Direct grayscale paths. Independent
scalar byte oracles cover all five plane modes, rotations, alignments, cropped
prefixes, clipping, strips, packed tails, padding, and exact allocation boundaries.

Use an isolated working directory containing its own `fs_`, and set
`CROSSPOINT_SIM_SD` to that same absolute directory. Some existing full-smoke
checks use literal `fs_` paths with `std::filesystem`; the environment override
alone does not isolate them. Never launch that smoke against a checkout's real
card directory. Reader fixtures explicitly set a healthy resume counter and
assign short Power to Sleep before replay.

File Activity follows two startup passes in one normal public cover lifecycle:

| Native filesystem attribution | Baseline | Candidate |
| --- | ---: | ---: |
| Settings-save calls during setup | 20 | 0 |
| All card filesystem calls in the lifecycle | 4,812 | 4,772 |

These counts show removal of one settings rewrite per startup, not a measured
physical wake-latency gain. The normal application smoke passes, including the
settings persistence regressions, and all 1,171 ASan/UBSan-enabled CTest entries
pass. Independent safety reviews found no outstanding defect; static review
does not establish universal hardware safety.

The clean production simulator and ESP32-C3 build pass after removing the
temporary timing/drain/raw-capture scaffolding. A fresh uninstrumented Power
sleep/reader-resume run retains both visible surfaces and primary state.
The production image is 6,351,920 bytes, with 201,680 bytes of repository OTA
headroom and unchanged 64,832-byte static RAM use. Offline checksum and image
validation pass. Its SHA-256 is
`a6fa064951010a7d9009f909eef9617a95c02bacb535db47a3f4c011b6b86372`.
It is installed in app1 and confirmed sequence 24, VALID, with exact app readback
and protected-region verification before boot. Original app0 and its E000
selector remain intact. With serial closed, the user confirmed the normal
cover, saved passage, and controls through a production sleep/wake cycle.
Static RAM is not a measurement of peak dynamic memory.

#### Physical X3 measurement

After the user authorized resumption, complete host-received lifecycle traces
were collected on the same cached native two-bit 515×792 cover, with COVER,
NO_FILTER, FIT, and a settled reader page. Earlier missing/incomplete traces
remain excluded.

| Physical configuration | Preparation (s) | Decode/draw (s) | Activation lap (s) |
| --- | ---: | ---: | ---: |
| Baseline `16da21ec`, lifecycle timing | 3.348728 | — | — |
| Popup/settings optimization, lifecycle timing | 2.876382 | — | — |
| Same first optimization, detailed profiling | 2.881112 | 1.840145 | 0.820760 |
| Packed-row optimization, detailed profiling | 1.211026 | 0.230481 | 0.819715 |

Preparation is 2.765× faster than the baseline, or 63.84% shorter, for this
cover. Each configuration has one accepted sample; this is not a guarantee
for every book or image. Decode/draw includes file reads. Activation laps include
the enclosed BUSY diagnostics; both detailed runs observed qualified refresh
and power-off completion with no BUSY timeout.

The boundary is `enterDeepSleep()` entry through safe application power-manager
handoff, before diagnostic delivery. It is not a physical button edge,
electrical-off time, or wake-latency measurement. Diagnostic logging contributes
to enclosing timings; the bounded post-timestamp completion drain is excluded.
All temporary lifecycle, cover, BUSY, drain, and raw-capture probes were removed
from the installed normal image. Controller waveforms, wait thresholds, image
quality, progress durability, and power sequencing were not shortened or skipped.

Local public-fixture evidence is retained under `crossink-sleep-wake-he1s9ao5`
in the macOS temporary directory. Physical logs, frozen diagnostic images, and
recovery receipts remain in the private device-backup directory; no private
book data is included in the repository.


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
