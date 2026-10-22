# CrossInk — Personal X3 Fork

This is of [CrossInk](https://github.com/uxjulia/CrossInk), which itself builds on [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).

## What’s Different in This Fork
### Natural, Sensor-Calibrated Auto-Rotation
Reading in bed or switching hands was awkward without rotating the screen.

We added an orientation filter that waits for ~0.5 seconds of a stable, unambiguous hold before rotating, explicitly ignoring flat desk rests, diagonal angles, and mid-transition tilts. The axes are re-mapped specifically for the X3's hardware mounting, and long-pressing the Power button now rotates 90° clockwise on demand (cleanly suppressing the short-press sleep action upon release). The result is reliable 4-way rotation for EPUB and TXT books that only turns when you intend it to, never wobbles on a table, and includes an instant one-handed physical button shortcut.

See [automatic orientation](./docs/controls.md#automatic-reading-orientation) and [Power button actions](./docs/controls.md#power-back-and-menu-button-actions) for setup details.

### Exact Reading Position Retention
Rotating or reflowing shouldn't make you lose your place or force you to hunt for the paragraph you were reading. Instead of tracking fragile page numbers, the reader anchors your exact reading position using text anchors for EPUBs and byte anchors for TXT files.

When you flip orientation or reflow text, the exact sentence you were reading stays right before your eyes. Viewport layouts are cached so flipping orientation back and forth is instantaneous, and legacy TXT bookmarks are automatically migrated and preserved across index rebuilds without corruption.

### Instant Sleep Transitions & Fast Cover Art
Putting the reader down should feel immediate—not flash an "entering sleep" modal or grind away processing the cover art. We optimized 2-bit grayscale bitmap rendering by normalizing palettes once per row and writing packed bytes directly to display buffers, bypassing per-pixel conversion overhead. On eligible screens, intermediate popups are skipped entirely while preserving safe power-manager handoffs and durable progress saves.

On physical X3 hardware, total sleep preparation dropped from **3.35s to 1.21s (2.77× faster)**, while decoding and rendering cached cover art plummeted from **1.84s to just 0.23s (nearly 8× faster)**—giving you a clean, near-instant transition straight to your book cover.

### Smoother UI Scrolling & Text Rendering
Recalculating font metrics, kerning pairs, and layout geometry on every single frame put unnecessary load on the 160 MHz CPU, making list browsing feel sluggish. We cached and reused resolved glyph metrics and text-fit boundaries across render passes, introduced read-only ASCII kerning lookups for built-in fonts, and optimized glyph drawing with packed-bit writes to the framebuffer.

During continuous library scrolling, sampled render-thread CPU time dropped by **~43%** (from 378 ms to 214 ms), and list item renders dropped from 3.94 ms to 2.43 ms—making library browsing noticeably more fluid without altering visual geometry or font clarity.

### Fast Library Indexing & Less SD Card Wear
Scanning hundreds of books used to freeze the interface and batter the SD card with thousands of tiny, fragmented I/O calls. We replaced fixed, arbitrary indexing pauses with a cooperative work-budget yield that keeps the UI responsive to button presses, and bundled adjacent metadata, ZIP directory headers, and page offsets into bounded batch reads.

Cold library scans on 1,000 EPUBs dropped from over a second to **~50 ms** in testing (**95% faster**), eliminating thousands of redundant disk accesses and keeping library navigation snappy.

### Background Next-Page Preparation (Zero-Wait Page Turns)
Readers spend 30 to 60 seconds on a page; we put that idle time to work so page turns feel instantaneous. The EPUB engine quietly decodes and prepares the upcoming page in the background while you read. Under strict free-heap and contiguous-block gates, it automatically releases speculative memory before foreground fallback, font changes, or user reflows.

When you tap next, the page appears immediately—slashing foreground filesystem operations from **3,458 to just 182** over an 18-page run (a 95% reduction). If a page is unusually complex or memory runs low, it seamlessly falls back to normal foreground loading with zero risk of crashes.

### Instant Home Dashboard & Lighter Startup
Booting up or jumping Home should feel instant. We deferred non-essential dashboard context queries (like OPDS catalogs and bookmarks) until you actually open the dashboard menu, and eliminated redundant settings rewrites on wake when using automatic device naming. Initial Home startup requires zero optional background calls, navigation stays responsive, and flash storage is spared from unnecessary write cycles during wake-up.

The Home and next-page optimizations adapt ideas from [YACP](https://github.com/Sichroteph/YACP) and [CrossDiTo](https://github.com/dito94/CrossDiTo); see the [attribution and scope notes](./docs/development/testing-debugging.md#x3-bounded-home-and-next-page-ports) for X3-specific adaptations.

## Performance & Measurements

To make sure these optimizations delivered real-world gains without compromising battery life or stability, we benchmarked and verified each change on both physical X3 hardware and instrumented native builds.

### Real-World Hardware (Xteink X3)

To test cold sleep performance, we timed putting an X3 to sleep while displaying a cached 515×792 full-screen book cover (`COVER / NO_FILTER / FIT` mode), measuring from `enterDeepSleep()` entry to the safe power-manager handoff:

| Workload | Stock / Baseline | Optimized | Impact |
| --- | ---: | ---: | --- |
| **Total sleep preparation** | 3.349 s | **1.211 s** | **2.77× faster** (near-instant transition) |
| **Decode & draw (both grayscale planes)** | 1.840 s | **0.230 s** | **7.98× faster** (smooth cover rendering) |
| **Panel activation handshake** | 0.821 s | 0.820 s | Controller-bound hardware timing (unchanged) |

In daily use, putting the reader to sleep no longer feels like a chore. The device immediately paints the cover and shuts down cleanly, while preserving full progress persistence, controller safety thresholds, and power-down sequencing. Detailed logs and timing breakdowns are documented in [physical measurements](./docs/development/testing-debugging.md#physical-x3-measurement).

### Native Profiling & Host Workloads

We used optimized native builds (macOS arm64) with frame pointers to isolate CPU bottlenecks and compare identical workloads against their direct baselines without SD card hardware variance:

| Workload | Baseline | Optimized | Impact |
| --- | ---: | ---: | --- |
| **80-step Library scroll (render CPU)** | 378 ms | **214 ms** | **~43% less CPU time** via glyph & kerning reuse |
| **Library item render (median)** | 3.942 ms | **2.431 ms** | **38% faster** UI list rendering |
| **Cold library index (1,000 EPUBs)** | 1,099 ms | **51 ms** | **95% faster** cold scan via work-budget yields |
| **Incremental index (1 changed, 999 reused)** | 101 ms | **67 ms** | **34% faster** cached library check |
| **Warm EPUB page render (median)** | 6.581 ms | **5.677 ms** | **14% faster** text rendering |
| **Warm TXT page render (median)** | 5.586 ms | **5.335 ms** | Snappier TXT page display |
| **Rotation replay (render CPU)** | 365 ms | **345 ms** | Smoother orientation flips |

*Tradeoffs and edge cases:* Not every path sees dramatic acceleration—cold TXT page renders and physical button-to-presentation latencies are bottlenecked by display and storage hardware and remain essentially even. Some uncached image or small-font cases show modest tradeoffs, which is why optimizations are conservatively bounded to prioritize stability over raw synthetic scores.

### Filesystem & I/O Reduction

Consolidating fragmented disk reads and moving I/O out of the foreground path reduces flash storage wear and eliminates micro-stutters:

| Workload | Baseline | Optimized | Real-World Impact |
| --- | ---: | ---: | --- |
| **Foreground page loads (18 EPUB turns)** | 3,458 calls | **182 calls** | **95% fewer foreground calls** via idle page prep |
| **Dashboard Home startup calls** | 17 calls | **0 calls** | Deferred non-essential checks until menu opens |
| **Wake-up settings saves** | 20 calls | **0 calls** | Eliminated redundant flash writes on boot/wake |

### Rigorous Testing & Safety Gates

Performance wins mean nothing if books crash or bookmarks get corrupted. Every change was held to strict safety gates:

- **1,171 automated tests** passed with AddressSanitizer and UndefinedBehaviorSanitizer enabled.
- **17 paired native sleep/wake cases** verified pixel-perfect buffer consistency (34 SDL surface pairs, 17 B/W buffers), ensuring settings, reading progress, and bookmarks survive intact.
- **On-device verification:** Flashed and tested on physical X3 hardware with zero crashes, reliable auto-rotation, intact reading anchors, and verified power management.
- **Memory footprint:** The final C3 firmware image is **6.35 MB** with **64.8 KB** static RAM, preserving over **201 KB** of free headroom within the configured OTA flash budget.

See the [changelog](./CHANGELOG.md#unreleased) and [profiling documentation](./docs/development/testing-debugging.md#native-performance-profiling) for full benchmark logs, workloads, and methodology.

## Device Coverage

CrossInk includes hardware profiles for several devices:

- Xteink X3
- Xteink X4
- Xteink X4 Pro
- Xteink X4 Classic
- Seeed Studio Sticky

> **Important:** While the firmware compiles for all supported targets, the auto-rotation sensor calibrations and physical timing checks in this fork were specifically developed and verified on the **Xteink X3**.

## Inherited CrossInk Features

Credit where credit is due: the typography, reading statistics, custom themes, and core reader features below are the wonderful work of [upstream CrossInk](https://github.com/uxjulia/CrossInk) by uxjulia. They are fully included here alongside our X3-specific improvements.

<table>
  <tr>
    <td align="center">
      <img src="./docs/images/bitter-small-15-margin.jpg" alt="Font: Bitter, Size: 12 pt, Margin: 15" /><br/>
      <em>Font: Bitter, Size: 12 pt, Margin: 15</em>
    </td>
    <td align="center">
      <img src="./docs/images/reading-stats.jpg" alt="Reading Stats with custom front button mapping shown" /><br/>
      <em>Reading Stats with custom front button mapping shown</em>
    </td>
  </tr>
</table>

### Highlights

- New reader fonts: Lexend Deca and Bitter.
- Music notation and selected supplemental Unicode glyph support to be able to render Project Hail Mary accurately.
- Added a custom `Minimal` theme and sleep screen option for the minimalists out there.
- Added a custom `Dashboard` theme and sleep screen option for reading stats enthusiasts.
- Reader font sizes: 10 pt, 12 pt, 14 pt, and 16 pt.
- Added ~~strikethrough~~ support.
- Made <u>underlines</u> thicker for better visibility.
- Added support for `<hr>` section breaks.
- Added support for "redaction" style rendering.
- Added improved support for tables with simple markup.
- Added ability to add bookmarks.
- Added ability to remap front buttons that only applies in the reader.
- Added Focus Reading and Guide Dots as optional reader modes.
- Added Force Paragraph Indents for books that render as one giant wall of text.
- Added ability to pin a sleep image as a favorite. The favorited image will always be displayed when your sleep settings are set to `Custom` or `Cover + Custom` (when no cover is available).
- Added more in-reader control remapping options for side buttons, short power button clicks, and long-press menu actions, and more.
- Added ability to mark a book as finished from the in-book menu. A pop-up will also display once 99% of the book is reached. This status allows tracking of total books read.
- Added ability to move finished books to "Read" folder.
- In-book menu to quickly adjust reader options without having to exit the book.
- Reading stats: total books read, total reading time, number of sessions, pages turned, average session time, pages turned per minute. You can also set your reading stats as your sleep screen.
- All-time reading stats [syncing](./docs/reading-stats-sync.md) between two CrossInk devices.
- Reading [progress sync](./docs/nearby-position-sync.md) between two CrossInk devices.
- Added customizable Auto Page Turn Interval (anything between 5-120 seconds).
- Added ability to view Recent Books as a 3x3 grid view.
- For inherited CrossInk release notes, see the [upstream releases](https://github.com/uxjulia/CrossInk/releases).

---

### Reader Fonts

The default fonts have been replaced with Lexend Deca and Bitter. These fonts have been chosen specifically to improve reading fluency and e-ink performance. These 'sturdier' typefaces feature uniform stroke weights and open geometries, allowing the X4/X3 to render crisp, high-contrast text with font-aliasing on while significantly reducing ghosting and artifacts.

- [Lexend Deca](https://fonts.google.com/specimen/Lexend+Deca) - A research-backed sans-serif typeface designed to improve reading fluency. Lexend was engineered based on the theory that reading issues are often a design problem (visual crowding) rather than a cognitive one.
- [Bitter](https://fonts.google.com/specimen/Bitter) - A "contemporary" slab serif typeface for text, it is specially designed for comfortably reading on digital screens. The consistent stroke weight of Bitter helps it render particularly well on e-ink devices. The medium weight has been chosen specifically for improved rendering on the X4/X3.

The UI now uses [Inter](https://fonts.google.com/specimen/Inter) as the display font which has improved readability at smaller sizes.

### Music and Supplemental Glyphs

- Built-in reader fonts include music notation, selected Cyrillic glyphs, and the Project Hail Mary CJK fallback ranges. Additional SD-card fonts retain emoji fallback support.

---

### Font Sizes

CrossInk includes 10 pt, 12 pt, 14 pt, and 16 pt built-in reader font sizes.

See [SD Card Fonts](./docs/sd-card-fonts.md) for installing additional font families and size ranges.

---

### Reader features

Reader Options, Focus Reading, Guide Dots, Force Paragraph Indents, reading stats, and finished-book behavior are documented in [Reader Features](./docs/reader-features.md).

### Custom button actions

CrossInk adds configurable button shortcuts.

See [Controls](./docs/controls.md) for the full action list and defaults.

---

## Tips for the best reading experience

The Xteink X3 runs on an ESP32-C3 with limited RAM, so very large folders or complex EPUBs can be slow.

- Keep folders under about 200 files. For the smoothest browsing, aim for 50-100 files per folder.
- Having 1000+ books on the SD card is fine if they are split into smaller folders, such as by author, series, genre, or read/unread status.
- Avoid putting every book in the SD card root. The file browser has to scan and sort the current folder before it can show it.
- Text-first EPUBs are the best fit. Large image-heavy EPUBs, scanned books, comics, and omnibus files with thousands of sections may load slowly or fail under memory pressure.
- As a rough target, EPUBs under 20 MB tend to work the best. Files over 50 MB may still work, but they are more likely to be slow or memory-sensitive, especially if they contain many large images.
- If an EPUB is unusually slow, try [optimizing](./docs/webserver.md#epub-optimization) it with the built-in web optimizer (via File Transfer) before copying it to the SD card: remove unused high-resolution images, split very large omnibus files, and avoid embedding multiple full font families when possible.
- Use a reliable SD card and leave some free space. CrossInk stores settings, reading progress, cache files, stats, and generated book data on the card.

---

## Installation

After a version tag passes CI, download the matching firmware from [this fork's releases](https://github.com/neomoto/CrossInk/releases).

Development binaries are available from the [CI workflow](https://github.com/neomoto/CrossInk/actions/workflows/ci.yml). These builds are not releases.

To build from source, use this repository's [`main` branch](https://github.com/neomoto/CrossInk/tree/main) and the [build instructions](./docs/development/getting-started.md).

See [Versioned releases](./docs/development/testing-debugging.md#versioned-releases) for release tags and required CI checks.

> **Note:** Pre-built binaries from [upstream CrossInk releases](https://github.com/uxjulia/CrossInk/releases) and the [Inky web installer](https://inky.crossink.dev/#flash-tools) deliver official upstream firmware, not this fork's changes. See the [installation guide](./docs/installation.md) for standard flashing and revert workflows.

---

## Guides & Documentation

Visit [https://www.crossink.dev](https://www.crossink.dev) for more user guides and additional documentation.

---

## Development quick start

CrossInk uses PlatformIO for building and flashing firmware. See [Getting Started](./docs/development/getting-started.md) for prerequisites, clone setup, and validation commands.

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

Connect your device to your computer via a USB cable. Before the first build, initialize the repository's submodules (including `freeink-sdk`):

```sh
git submodule update --init --recursive
```

Then flash the firmware using the correct environment for the device. The `default` environment is for the X3/X4 devices. ESP32-S3 devices have their own named environments.

> [!CAUTION]
> Always keep a full recovery backup and check your partition layout before flashing custom firmware. The generic `upload` command below performs a direct flash without automated recovery safeguards.

```sh
pio run -e default --target upload
```

If PlatformIO reports `PackageException: Can not create a symbolic link for freeink-sdk/libs/hardware/BatteryMonitor, not a directory`, the `freeink-sdk` submodule is not initialized. Run the submodule command above and retry.

See [Testing and Debugging](./docs/development/testing-debugging.md) for serial logging, simulator checks, static analysis, and bug-report guidance.

---

## Upstream & Community

Upstream CrossInk does not accept pull requests; feature requests and ideas can be shared in the [CrossInk discussions](https://github.com/uxjulia/CrossInk/discussions), while broader architecture features are generally directed upstream to [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).

---

If you enjoy CrossInk and want to support the core upstream project, please consider supporting Julia on Ko-fi:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/Q5Q01M6S7)
