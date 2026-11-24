# Thor Browser decisions and handoff

Last reviewed on 25 November 2026. Thor Browser is an experiment in using ThorVG to paint real web pages. It has no long-term maintenance or release commitment.

## Architecture

- We considered Chromium, Firefox, and Ladybird. We chose Ladybird because its compositor already replays a page display list through a raster painter interface. The fork translates those operations to ThorVG and paints directly into the shared page surface. Ladybird still handles networking, layout, text shaping, video decoding, scrolling, tile caching, damage tracking, and presentation. Start with [the display-list player](../Services/Compositor/DisplayListPlayerThorVG.cpp).
- Keep both macOS ThorVG presets Skia-free. Software rendering paints shared bitmaps. The OpenGL variant creates an accelerated OpenGL 4.1 context and paints shared IOSurfaces. It also includes the software engine for bitmap work. Build ThorVG as a shared library to keep its GL dispatch symbols separate from Apple's. No ThorVG fork is required. The [workflow](../.github/workflows/thorvg.yml) pins the exact upstream revision.
- Preserve text. Use Ladybird's shaped glyph IDs, positions, and font outlines, then let ThorVG paint the outlines. For video, convert the current decoded frame on the CPU and draw it as an image. These are separate paths, so successful video playback says little about surrounding page text.
- Prefer explicit limitations to a hidden Skia fallback. Shapes, intersecting clips, gradients, images, masks, and common blend modes have implementations. Filters, repeated patterns, and bitmap-only color glyphs have gaps. Unsupported layer blends use source-over. Canvas 2D and WebGL contexts currently return null in the Skia-free builds. See the [renderer notes and build commands](BuildInstructionsLadybird.md#experimental-thorvg-page-painting).

## Product and distribution

| Variant | Installed name | macOS bundle ID |
| --- | --- | --- |
| Software | Thor Browser.app | sh.karthikeyan.thor-browser |
| OpenGL | Thor Browser GL.app | sh.karthikeyan.thor-browser.gl |

Use the Icelandic red phalarope (þórshani) as the mascot. The approved icon is the head-and-shoulders portrait. Both variants use separate profiles and install alongside upstream Ladybird. Keep upstream's macOS deployment target (14.4 at this revision). These builds target Apple Silicon, use ad hoc signing, and disable Sparkle.

Retain Ladybird's BSD notice, ThorVG's MIT notice, and the FreeType acknowledgment for the CPU rasterizer present in both builds. Identify KarthikeyanRanasthala as the publisher and make independence from the Ladybird Browser Initiative clear. Each DMG carries its own matching LGPL library sources, patches, build recipes, and instructions under `Sources/`. Check the notices and sources again when updating dependencies. [Ladybird trademark policy](https://ladybird.org/trademark/)

Use the **Thor Browser macOS builds** workflow for both variants. Ordinary CI runs produce installable DMGs. For lasting distribution, an explicit `thorvg-v*` tag runs the additional macOS 15 smoke test and publishes both images and their checksums to GitHub Releases. A normal push does not create a release. See [installation and release instructions](BuildInstructionsLadybird.md#installing-and-publishing-the-thorvg-macos-builds).

## Evidence and where to resume

1. Both DMGs were built locally. The bundle IDs, mascot, signatures, license text, and hashes for all six LGPL source packages and their port files were checked. The software painter tests passed for both presets. On the restricted build host the GPU test skipped because it could not create an accelerated pixel format, and the headless browser could not start its sandboxed RequestServer. Repeat those checks on a Mac with a usable GPU and browser sandbox. [GPU test](../Tests/Compositor/TestThorVGGL.cpp)
2. An earlier six-second animation benchmark ran three trials per renderer at a 1200 × 824 viewport and device pixel ratio 2. CPU produced 118.5–119.8 changed compositor frames/s and GL produced 70.3–104.2. Page animation callbacks stayed near 120/s in both. This single scene is a baseline. Repeat it on the same hardware, recording changed frames and `gpu.thorvg_painted_passes`, before judging a GPU optimization. The original local harness and measurements were kept under `Build/thorvg-gl/paint-benchmark/`; a fresh clone will need a new harness.
3. Recheck Google text and a YouTube watch page in each renderer. Test playback and the expanded video description separately. The missing description was reported by the user and its cause is unresolved. Check whether the text reaches the DOM and display list before changing glyph painting. Start with the [painter tests](../Tests/Compositor/TestDisplayListPlayerThorVG.cpp).
4. For a fresh checkout, follow the pinned workflow and the build guide. A shallow Ladybird or ThorVG checkout is sufficient, but vcpkg needs access to historical port trees. If dependency resolution reports a missing tree object, fetch the full vcpkg history. The local successful setup used Rust 1.98.1 and fetched Cargo dependencies with `cargo +1.98.1 fetch --locked`. The macOS ThorVG presets avoid the Sparkle dependency.
