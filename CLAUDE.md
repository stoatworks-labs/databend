# databend

The raster as a PCM stream through audio effects, as an FFGL **effect** for
Resolume Arena/Avenue: echo, flanger, phaser and pitch shift run on the picture
in scan order, as an audio editor would run them on the file. C++/GLSL, CMake
MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the stream's layout (`Shaders.cpp`, the stream
and display passes), the phaser's window (`Model.h`, `PhaserWindow`), the
control laws or the harness's tolerances.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships, and what `verify.sh` builds): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel 4`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/dbtest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Layout=1" --set "Delay Samples=61" --set "Feedback=0.7"`
  (0..1 for sliders, the element index for options, the integer for the counts)
- List parameters, kinds, defaults and ranges: `./build/dbtest --list`
- The exact GLSL the plugin compiles: `./build/dbtest --dump-shaders DIR` (8 files:
  the vertex shader and seven assembled fragment shaders)
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (accepted for the fleet's contract; this plugin has
  no clock, so it changes nothing) and an optional `--script` of
  `frame Parameter Name value` cues. A slider ramps between its cues; an option, a
  boolean or an integer STEPS, holding each cue until the next cue's frame. A cue
  naming no parameter exits 2 before any frame; a partial frame at the end of stdin
  ends the stream with exit 0; a failed render or a closed stdout exits 1 (SIGPIPE is
  ignored so a closed stdout is a failed write, not a 141):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/dbtest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the offline checks +
  every rendered check at 320x180 AND 1280x720 + the --pipe contract + the sweep + the
  bundle, ~3 min)
- An echo of a line + k samples lands a line down and k across; padding moves it as the
  stride predicts: `./build/dbtest --echo`
- The nth echo at n·(k, 1) with amplitude m gⁿ: `./build/dbtest --feedback`
- An interleaved delay ≡ 1 (mod 3) puts R's echo in G, G's in B, B's in the next R:
  `./build/dbtest --interleave`
- The flanger's ghost on every line sits at the LFO's value at that stream time:
  `./build/dbtest --flanger`
- The phaser keeps a noise line's energy; the windowed GPU form matches the serial double
  cascade: `./build/dbtest --allpass`
- 8-bit wraps past full scale, 16-bit wraps, float clips: `./build/dbtest --wrap`
- A bar pattern's period is stretched by 1/r inside a grain: `./build/dbtest --pitch`
- The checks can fail: `./build/dbtest --negative`; one perturbation verbosely:
  `./build/dbtest --echo --perturb 2` (bits in `Model.h`)
- No GL (what CI runs first): `./build/dbtest --offline` = `--laws --names --window`
- Every rendered check takes `--size WxH`; CI runs them at 320x180 with `--allow-no-gl`
- Shaders through glslc: `tools/check-shaders.sh build/dbtest`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/dbtest --bench` (best of three; the GPU is shared, so run it twice)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Databend.bundle`

## Notes
- **The shaders ARE the effects.** Each effect lives once, in GLSL (`Shaders.cpp`);
  the C++ converts sliders to uniforms (`Controls.cpp`), computes the echo's tap
  count and the phaser's window (`Model.h`) and schedules the passes
  (`Databend.cpp`). The harness restates the effects in double from `Model.h`'s
  description and holds the shaders to them.
- **The stream is one stream.** Planar is plane-major (all of R, then G, then B), so a
  delay past a plane's start reads the previous plane; the first lines of G carry
  echoes of the last lines of R. Interleaved is R, G, B per pixel, and every effect
  on it rotates hue (the hue lives at the stream's period-3 frequency).
- **The format decides the zero point.** An unsigned file's silence is black; a signed
  file's (16-bit, float) is mid-grey. The effects run in float and the format
  quantises and wraps or clips once, on export.
- **The phaser is a windowed restart with a proved bound** (`Model.h`), run once per
  chunk of 16 samples into a three-attachment state buffer and continued from there.
  Its window is most of the render cost. No phaser feedback: see AGENTS.md.
- **The LFOs are triangles in stream time** at a nominal 44.1 kHz, with an integer
  period, so their values are exact in float; their phase carries across frames on a
  frame counter (`SetTimeSupported(false)`), the delay memories do not.
- **The output alpha is max( source alpha, the exported pixel's brightest channel )**:
  the effects move content, and an echo over a transparent area must be visible.
- **Every ffglex `Scoped*` binding clears to 0 on scope exit**; a Scoped binding made
  inside a lambda is gone before the draw. The phaser's passes use raw binds.
- **Parameter names must be unique and 16 characters or under**; every group's Mix is
  its own name (Echo Mix, Flanger Mix, Phaser Mix, Pitch Mix, Mix).
- `SetParamInfo` clamps a STANDARD default into 0..1; `SetParamInfof` reads its default
  out of `params[]`, so fill `params[]` first. Options are mapped by index in
  `Controls.cpp` (an option's range reads back 0..1); the four counts are real
  `FF_TYPE_INTEGER`s with real ranges.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `databend_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- GLSL reserved words (`packed` among them) are none of our identifiers; `far` and
  `near` are MSVC macros and are none either.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `DB01`, display name `SW Databend`.

## Not done yet
- **Never loaded into Resolume.** Everything numeric is measured offline on macOS,
  plus an `oxbow` load. Footage only through `--pipe` (the defaults were judged on
  Resolume's bundled demo clips that way).
- No Windows build has run; no Arena gate; no release, no website, no user guide.
- No OpenFX port, no browser demo, no factory presets, no audio input.
- No phaser feedback (AGENTS.md says why).
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies in the shape
  the backend's syncs generate; register the project and re-sync before release.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/databend/databend.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\databend\logs\databend.YYYY-MM-DD.log   (Windows)
