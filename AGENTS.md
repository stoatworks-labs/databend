# AGENTS.md — Databend

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

The raster as a PCM stream through audio effects — echo, flanger, phaser, pitch
shift, run on the picture in the order an audio editor would read the file — as
an FFGL 2.1 effect (`DB01`, shown as `SW Databend`) for Resolume Arena and
Avenue. C++17 + GLSL 4.10, CMake, universal macOS `.bundle` and a Windows
`.dll`. MIT, intended home `github.com/stoatworks-labs/databend`.

Built 2026-09-24 in one session (tranche four) from the fleet's templates and
`specs/SPEC-databend.md` (with `BRIEF.md` and `BRIEF-ADDENDUM.md`): slope for
the harness, verify, `--pipe`, the negative-control pattern, the state buffer and
the CI; ferric and compander for one-dimensional processing in scan order; clamp
and slope for a GPU recurrence held to a serial double reference; tinsel for
`PassBuffer` and the trap list; graticule for the notes and the provisional About.

---

## The one idea

**The editor sees one stream of samples in the file's own layout, and a
delay-line effect on that stream does not know where the lines are.**

With Lp pixels on a line and p samples of padding, a Planar file is three planes
of L = Lp + p samples a line, one after another (n = c·lines·L + l·L + x); an
Interleaved file is R, G, B per pixel with L = 3Lp + p (n = l·L + 3x + c).
Padding is silence; so is everything before the file's start. An unsigned
format's silence is black and its samples run 0..1; a signed format's silence is
mid-grey and its samples run −1..1. On that stream, in a fixed chain, each with a
bypass:

    echo      e = x + m_e Σ_{k≥1} g^k x[n − kD],     D = dl·L + ds
    flanger   f = (1 − m_f) e + m_f e[n − d(n)],     d = b + w·tri(φ(n))
    phaser    q = (1 − m_p) f + m_p A_S(f),          S lattice all-pass stages, pole k(n)
    pitch     t = (1 − m_t) q + m_t (w_A q[n − d_A] + w_B q[n − d_B]),  a sawtooth delay, two taps
    export    quantise to the Format; wrap or clip; back to the pixel

| what the stream does | what comes out |
| --- | --- |
| an echo of D = k·L + m samples | k lines down, m samples across; with feedback the echoes **march on a slant**, m gᵏ each |
| a delay that is not a multiple of 3 in an interleaved stream | **the wrong colour**: R's echo in G, G's in B, B's in the next pixel's R |
| a delay past a plane's start in a planar stream | the first lines of G carry echoes of the **last lines of R** (a faint tinted line at the top of a frame) |
| an LFO in stream time | its value changes line by line, so the flanger is **a comb across the scanlines** |
| an all-pass with a positive pole | smooth content arrives late, edges on time: **a rightward smear with ringing**, at unity gain |
| any effect on an interleaved stream | **hue rotates**, because hue lives at the stream's period-3 frequency |
| the format past full scale | 8-bit **wraps** (the harsh look) or clips; float clips |

### What does not fall out, and is the honest limit

- **Each frame is its own file** for the delay memories: nothing of frame f
  reaches frame f + 1. Databending a video file as one stream would let an echo
  reach the previous frame, which needs a frame history this plugin does not
  keep. The LFOs, alone, carry their phase across frames as if the clip were one
  file (a frame counter, exact in 64-bit integers), so the comb moves.
- **No phaser feedback.** The spec asks for it; it is not offered. See the
  window section for why.
- **The all-pass sweep's top pole is fixed at 0.9**; Phaser Depth sets how far
  below it the sweep goes. One number less to prove a bound for.
- **The alpha channel is not part of the stream.** The file the editor sees is
  RGB. The output alpha is max( the source's, the exported pixel's brightest
  channel ), so content the effects move over a transparent area is visible.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Model.h` | The stream and the effects, described; the `Perturb` bits; the option tables; `EchoTaps`; `PhaserWindow` (the bound). No code that runs an effect — that is GLSL. |
| `source/Controls.{h,cpp}` | Every slider to its physical unit, dyadic where the harness needs exactness. |
| `source/Shaders.{h,cpp}` | The vertex shader, a stream library, and seven passes assembled from it: stream, echo, flanger, phaser state, phaser, pitch, display. **The shaders are the effects.** |
| `source/StateBuffer.{h,cpp}` | A three-attachment RGBA32F framebuffer: twelve cascade states a chunk boundary. |
| `source/Databend.{h,cpp}` | The plugin: parameters, buffers, the pass schedule, the test hooks. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed, for the two R32F stream buffers. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/dbtest/` | The offline harness: renders, measures, benchmarks, pipes, dumps shaders. |
| `tools/check-shaders.sh` | glslc on the dumped shaders; verify.sh and CI both call it. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters, plus the release-time checks done locally. |
| `demo/` | The browser demo: `plugin.js` holds the plugin's ten shader bodies verbatim (assembled as `Shaders.cpp` assembles them) and a hand PORT of the CPU half (`Controls.cpp`, `Model.h`'s `GeometryOf`/`EchoTaps`/`PhaserWindow`, the per-frame arithmetic and pass order of `Databend::ProcessOpenGL`); `tools/check_shaders.py` keeps the shaders identical (verify.sh runs it); `vendor/` is the shared kit from `stoatworks-backend/resolume-demo` (never edit it, re-run `sync.sh`). Served by this repo's own Worker at `databend-demo.stoatworks-labs.com` through a DNS record + route (the zone is out of custom domains); `deploy.yml` redeploys it on a push to main. The four integer controls are dropdowns there (the kit has no integer type). The port reproduces `dbtest --window`'s table and the 207-sample default window; on the same 960×540 colour-bars frame the page and `dbtest --pipe` agree on every pixel exactly (echo, export, three layouts) and within 1/255 with the pitch shifter on, measured once 2026-09-24 with the two LFO effects off (their phase rides on a frame counter the page and the pipe do not share). |

Per frame: the stream pass writes the file into an R32F buffer (Stride × Rows
texels, one per sample); each effect that is on reads one stream buffer and
writes the other (a bypassed effect is skipped, not copied); the phaser is two
passes, a state pass into the state buffer and the main pass; the display
exports the last buffer to the Format and puts it on the host's framebuffer.

### How the phaser runs on a GPU with no image stores

The phaser is IIR over a stream of 2–25 million samples. Slope's chunked-exact
form — one draw per 32 samples, each continuing from the previous draw's true
end state — is exact because slope's lines are independent and run side by
side; here there is one stream, and that form is N/32 **serial** draws a frame
(65,000 at 1080p). Not possible.

What is possible is the spec's **windowed restart**: the fragment for sample n
re-runs the cascade from zero state K samples earlier. Slope rejected that for
its coder because the coder does not forget — the idle pattern's phase is a
memory no leak erases. The phaser does forget, and the difference is that it is
**linear**: the error of a wrong start state is the homogeneous response, which
does not depend on the signal, and in the normalised lattice form

    y = −k x + s w_prev,   w = s x + k w_prev,   s = √(1 − k²)

each stage's state error obeys e_w' = k e_w + s e_in, so |e_w| shrinks by
|k| ≤ κ every sample whatever the input and however k moves. That is the proof
of exponential forgetting for one stage. Across the cascade the errors couple —
stage i's output error feeds stage i + 1 — polynomially, and `PhaserWindow()`
iterates the rigorous worst case (every |k| at κ, every s at its maximum for
the sweep, the triangle inequality at every step) from a bound on the states at
the restart point. That bound is min of two rigorous ones: the sup chain
|w_i| ≤ X_{i−1}/(1 − κ), X_i = κ X_{i−1} + s W_i, which grows about tenfold a
stage at κ = 0.9; and **losslessness** — each lattice section is a rotation, so
the total state energy never exceeds the input energy so far, |w_i| ≤ X√N.
The window K is the first after which the bound stays under half a code of the
format for a whole chunk (below). At the defaults K = 207 (1080p); over the
whole control range at most 682.

The cost of K steps a sample was 16 ms at 720p. So the phaser is **two
passes**: a state pass with one fragment per chunk boundary (every 16 samples)
that re-runs the K-sample window and writes the twelve states, and a main pass
that continues from its chunk's boundary state for at most 16 steps. The error
at a sample is the boundary's state error carried j ≤ 16 steps further, which is
the restart bound at K + j — hence "stays under half a code for a chunk". Cost
K/16 + 16 steps a sample instead of K: six times faster, the same arithmetic.

**Why no feedback.** A phaser's feedback returns the last stage's output to the
first stage's input. The loop is stable for a frozen coefficient (|A| = 1 on the
unit circle, |fb| < 1), but its decay is the modulus of the roots of
(1 + a z⁻¹)ˢ − fb·z⁻¹(a + z⁻¹)ˢ, which sits near the unit circle as fb grows,
and for a SWEPT coefficient the frequency-domain argument does not apply at all
— the triangle-inequality iteration diverges for any useful fb. So there is no
window with a proof, and the exact alternative is the serial pass above. The
brief says use slope's chunked form where the windowed bound does not hold; it
cannot be used here; the control is left out and said so.

---

## Traps

Roughly in the order they will bite.

### ☠️ A Scoped binding made inside a lambda is gone before the draw

The phaser's main pass first bound its three state textures with
`ScopedSamplerActivation` / `Scoped2DTextureBinding` inside the uniform-setting
lambda that `effect()` calls before `quad.Draw()`. Every ffglex `Scoped*`
binding CLEARS to 0 on scope exit, so the states were unbound at the draw; the
driver read a zero texture and said so only in a log line
(`unit 2 ... is unloadable ... using zero texture`). `--allpass` failed with the
energy at a quarter of the input. The phaser's two passes use raw binds, cleared
afterwards.

### ☠️ Every effect on an interleaved stream rotates hue

The hue of an interleaved RGB stream lives at its period-3 frequency. An
all-pass delays that component by a non-multiple of three samples; a fractional
flanger delay does the same; an echo ≡ 1 (mod 3) lands one channel over. With
Interleaved as the default, twelve of Resolume's demo clips came out pink. That
is the mechanism working, and it is now one click away: the default is Planar.

### ☠️ Wrap floods

With Wrap as the default, every phaser overshoot past full scale wrapped to the
other end of the range, and eight of twelve demo clips flooded with wrapped
fringes at every edge. Also the mechanism. Default Clip; Wrap one click away.

### ☠️ The output alpha has to carry what the effects move

Resolume's demo clips are DXV with alpha. Passing the source alpha through
(what slope does) made every echo over a transparent area invisible. The alpha
is now max( source alpha, the exported pixel's brightest channel ), checked on
the Trinity clip's wireframe: the echo trail is in the alpha plane.

### The flanger was a sum and brightened everything

`e + m_f e[n − d]` adds up to m_f of the picture to itself, and on the demo
clips pushed most of the picture past full scale. It is a crossfade now,
`(1 − m_f) e + m_f e[n − d]`; the comb is deepest at m_f = ½ and the phaser's
input magnitude bound no longer carries a flanger term.

### A line under a block sees two echoes

`--wrap`'s first statement predicted the line under a white block at g·white;
it is g + g² (the block's second line already carries the first's echo). The
plugin was right, the statement was wrong — the measured 160/255 said so.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first
and restored before the display, with the host's FBO bound explicitly); every
`Ensure()` happens before anything binds a texture; `FFGLFBO::Release()` leaks
the colour texture (`PassBuffer::Destroy()` deletes it first); `SetParamInfo`
clamps a STANDARD default into 0..1 and `SetParamInfof` reads its default out
of `params[]`; an option's range reads back 0..1 whatever its element count;
the core is an **OBJECT** library; `SetTextParameter` must return `FF_SUCCESS`
for the About block; `nm | grep -q` fails under pipefail when grep succeeds; a
closed stdout must be a failed write, so `--pipe` ignores SIGPIPE; `packed`,
`half`, `layout` and the rest are reserved in GLSL and none is an identifier
here; `far` and `near` are MSVC macros and are none either; `<cmath>` is
included where `std::lround` is used. Resolume's clock overflowing a float does
not arise: the plugin has no clock.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at
320×180 and 1280×720 in `verify.sh`.

What makes them rasteriser-proof by construction: **every coordinate is an
integer computed in integers** (`gl_FragCoord`, never an interpolated uv);
**every read is `texelFetch`**; **every coefficient is computed on the CPU in
double** and handed over as a float or an int; **the LFOs are triangles of an
integer counter**, exact in float, never a GPU `sin`; the echo checks use a
float file (no quantisation) with dyadic inputs and a dyadic gain, and the
export checks use dyadic values whose products with 255 and 32768 are exact.

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--echo` | the brightest pixel on the predicted line, and the whole picture against the closed form (an impulse, its ghosts, grey elsewhere), four delay/padding cases | 2(K + 1) float ULPs for the Horner sum + m × one code × the impulse for the truncated tail + 4 ULPs; **measured 0** | the delay in samples includes Lp, so D changes with the raster; the offsets do not |
| `--feedback` | the j-th ghost's amplitude at (j, 7j) for j = 1..K | the same | none; K is the file's code, not the raster |
| `--interleave` | the channel and pixel of the first ghost, and the whole picture, six cases | the same; **measured 0** | none |
| `--flanger` | on every line, the two values the ghost lands on, and its offset as a centroid, against the LFO at that stream time | values: 2 ULPs of (base + depth) through the interpolation weight, × the impulse, + 8 ULPs; offset: 2·depth/P — how far the LFO moves in one sample, which is exactly what a centroid of two adjacent taps cannot resolve — + the ULPs; the measured worst is 0.96 of that because the slope is the true difference | one line is skipped where the LFO stepped between the two taps (179/180 at 320 wide, 720/720 at 1280) |
| `--allpass` (a) | the output energy of the whole stream against the input's, after ¾ of a picture of silence | 2δ√(N E) + Nδ² with δ = half a code (the restart bound) + 32 ULPs (a MODEL of the float roundings per sample, not a bound); refuses a clipped reading | N and E scale with the raster, the tolerance with them |
| `--allpass` (b) | every visible sample against the serial double cascade | half a code (the rigorous restart bound) + 32 ULPs × √(K + 16) (the same float model, a random walk over the re-run); measured 0.004–0.012 of it | the window K depends on N through the lossless bound: 282/659 at 320×180, 283/660 at 1280×720 |
| `--wrap` | the exported value of 1.4375 and 0.6289 of full scale in six format/overflow cases | 2 ULPs, the final division; every product with 255 or 32768 is exact and none lands within ⅛ of a half code | none |
| `--pitch` | the autocorrelation of a line at the stretched period and its half | none: a square wave's autocorrelation is 1 at its period and −1 at its half, asserted as > 0.5 and < 0 | the line is 320 or 1280 samples; the grain of 512 straddles lines either way |
| `--laws`, `--window` | every control law at 21 points; the window over the control range | 1e-12 relative; exact | none (no GL) |

Deliberately NOT relied on: any GPU transcendental in a checked value (the
phaser's `sqrt` is inside the float model); round-to-nearest at a half code;
`mix(a, b, 1) == b` (the display returns early at Mix 1); interpolated
varyings; a texture unit's filtering; GLSL integer `%` on a negative operand
(none occurs; the export wraps in float with `floor`); the 8-bit readback (the
harness reads floats).

What is a model, not a bound, and said so: the float term of the two
`--allpass` tolerances. The rigorous part is the restart bound, half a code,
which is 7.6e-6 in a float file; the float term is 32 ULPs × √(K + 16) ≈
3e-5, and the measured error is 1.6e-7 — 0.004 of the tolerance. A worst-case
bound on the float roundings through a cascade of contractions with the
sup-norm coupling comes out at 1e-2, useless; the model is stated instead.

### The negative controls

`dbtest --negative` runs seven against the rendered checks; `--perturb BITS`
runs any check verbosely against one. Each perturbs the *plugin* — its shaders,
or the geometry it computes in `Databend.cpp` — never the harness's expectation.

| perturbation | what fails, measured at 320×180 |
| --- | --- |
| the stride ignores Line Padding | `--echo`: the two padding cases land 5 samples off |
| every echo at gain g instead of gⁿ | `--feedback`: worst 0.22 of signal against 1e-5 |
| the stream laid out Planar whatever Layout says (the spec's) | `--interleave`: R's echo found in R, four pixels on, not in G |
| both LFOs frozen at phase 0 | `--flanger`: the ghost sits at the base delay on every line, 0..32 samples off |
| the window cut to a quarter of its bound (the spec's) | `--allpass` reference: worst 0.012 (4 stages) and 0.033 (12), 290–580 tolerances; the energy check still passes, correctly — a wrong start state is still a rotation |
| an integer format clips whatever Overflow says | `--wrap`: the two Wrap cases read 1 and 1 where 111/255 and 0.21875 were predicted |
| the pitch shifter reads at 1/r | `--pitch`: period 8 where 32 was predicted, R(16) positive |

### The mutation

One character of the shipped GLSL, on a clean committed tree: in the echo pass,
`acc = Gain * ( fetch( In, n - k * Delay ) + acc )` → `n + k * Delay` (the echo
reads the future). Caught at 320×180 and 1280×720 by every case of `--echo`
(the brightest pixel on the predicted line is grey; the ghost is a line up),
`--feedback` (worst 0.22 of signal), `--interleave` (six cases) and `--wrap`
(six cases: the block's second line no longer sees its first): 17 of 25 checks.
`--flanger`, `--allpass` and `--pitch` passed, correctly: none of them reads
the echo. Reverted with `git checkout source/Shaders.cpp`; the tree was clean
before and after.

---

## Decisions taken without asking

- **The format decides the zero point.** An unsigned 8-bit file's silence is
  black; a signed 16-bit or float file's is mid-grey. Both are what the bytes
  mean. The effects run in the editor's float domain and the format quantises
  and overflows ONCE, on export — the spec's pipeline order, and what keeps the
  echo linear so its finite sum is valid. A float file has no code; one 16-bit
  code is taken for its truncation and restart bounds.
- **No phaser feedback** (above).
- **Planar is one plane-major stream.** A delay past a plane's start reads the
  previous plane; that is what a planar file is.
- **Interleaved is R, G, B per pixel, three samples**; the alpha channel is not
  in the stream.
- **Triangle LFOs at a nominal 44.1 kHz** with an integer period, so their
  values are exact and the flanger's tolerance can be derived. A sine would
  have been the usual choice and has no accuracy requirement in GLSL 4.10.
- **The LFOs' phase carries across frames**, the delay memories do not (above).
- **The flanger is a crossfade**, not a sum (above). The phaser and pitch mixes
  are crossfades too, because an all-pass has unity DC gain and a picture is
  mostly DC: x + A(x) would double the brightness.
- **Feedback is 7/8 v**: dyadic at every dyadic slider (7/16 at the default),
  never 1, so every echo train ends and the tap count is finite.
- **Delay Samples runs to 4095**, so a whole line's worth of samples can be
  expressed without Delay Lines (the padding cases of `--echo` need Lp + k).
- **Ratio is 2^(2v − 1)** and **Grain is 64 × 2^(6v)**, even, so ½, 1, 2 and
  64, 512, 4096 are exact and the `--pitch` taps fall in phase.
- **The pole's top is 0.9** and Phaser Depth sweeps below it.
- **kChunk = 16**: K/16 + 16 is near the minimum of K/C + C for K ≈ 200–700.
- **Defaults: Planar, Clip, 3 lines + 24 samples at 7/16 and half mix, a
  flanger at 4 + 24 samples and 0.3, a four-stage phaser at 0.35, the pitch
  shifter off** — judged on twelve of Resolume's bundled demo clips through
  `--pipe` at 640×360 and 1280×720 (the traps above).
- **No resize check.** Nothing carries across frames but the frame counter;
  every buffer is rewritten from the start every frame.
- **`--pipe` steps options, booleans and integers between cues** and ramps
  sliders; the fleet's harnesses ramp everything, and a Layout interpolated
  halfway is not a layout.
- **`--fps` is accepted and does nothing**, so the fleet's video renderer can
  pass it.
- **`--fail-render-at N`** is a harness-only hook so `verify.sh` can prove
  `--pipe` exits 1 on a failed render.
- **About and attributions were provisional hand copies** until registration;
  they are generated now (`sync-about.py`, `sync-attributions.py`), and the
  User guide button makes four About buttons, so `--list` shows 30 parameters.
- **The FFGL submodule was dissociated from the reference clone** so this repo
  does not depend on a path in `~/Projects`.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-24)

Every number is `tools/verify.sh` on this machine against a fresh universal
Release build, at 320×180 and 1280×720.

- **Echo.** A delay of one line + 7 samples ghosts at (+1, +7); Lp + 7 samples
  lands there too; with 5 of padding at (+1, +2), as the stride predicts; the
  whole picture matches the closed form with worst **0** (tolerance 9.7e-6),
  four cases, both rasters.
- **Feedback.** 14 echoes at (j, 7j), amplitude m gʲ, worst 5.7e-8; the plugin's
  14 taps are the count one float-file code predicts.
- **Interleave.** D = 4: R → G one pixel on, G → B, B → the next pixel's R;
  D = 3 keeps colour; D = L + 1 lands a line down in G; whole-picture worst
  **0**, six cases.
- **Flanger.** 179/180 and 720/720 lines: the ghost's offset is the LFO's value
  at that stream time, 16.1–48.0 samples across the picture; worst 0.0014 of
  0.0015 (the LFO's slope over one sample); values within 1.7e-6 of 3.3e-6.
- **All-pass.** 4 and 12 stages sweeping 0..0.9: energy kept to 1e-5 of 226
  (320×180) and 4e-4 of 3596 (1280×720); every sample within 1.6e-7 / 7e-7 of
  the serial double cascade against 4e-5; the plugin's window (283, 660) is
  the stated one; at a quarter of it the error is 290–670 tolerances.
- **Wrap.** 1.4375 of full scale exports as 111/255, 1, 0.21875, 32767/32768
  and 1 across the six cases; 0.6289 quantises exactly; worst 3.4e-8.
- **Pitch.** Period 16 → 32 at Ratio ½ and → 8 at Ratio 2; autocorrelation
  ≥ 0.88 at the period, ≤ −0.94 at the half; 16 when bypassed.
- **Negative controls.** All seven fail their check.
- **Mutation.** Caught (above), with the three checks that correctly passed it.
- **Laws, names, window.** Every law at 21 points; the dyadic promises; 29
  names unique and ≤ 16; `SW Databend` / `DB01` / effect; K = 207 at the
  defaults, ≤ 682 over the range, limit 1024.
- **No dead controls**, all 25, with the four About buttons skipped.
- **Every shader compiles** through `glslc`, all 8, as the plugin hands them
  to the driver.
- **`--pipe`** returns exactly two frames for two and a half, refuses an unknown
  cue with 2, exits 1 on a failed render and on a closed stdout (`| head -c 1`).
- **The bundle** is universal (`x86_64 arm64`), exports `_plugMain`, carries
  `com.stoatworks.ffgl.databend`, ad-hoc signs, and `oxbow` reports
  `SW Databend` / `DB01` / `effect` and renders 120 frames through `plugMain`.
- **Render cost**, best of three runs of 60 frames after a warm-up, `glFinish`
  both sides, on a shared GPU:

  | | ms/frame | % of a 60fps frame | every effect on |
  | --- | --- | --- | --- |
  | 1280×720 | 2.86 | 17.2% | 2.90 |
  | 1920×1080 | 5.83 | 35.0% | 4.98 |
  | 3840×2160 | 18.7 | 112% | 21.9 |

  The phaser's window is the cost: 207 samples re-run per 16-sample chunk at
  the defaults. The bound is rigorous and about twice what the error needs
  (at K/4 the error is hundreds of tolerances; at K/2, in an earlier run, it
  was under one). 4K is not real time with the phaser on. The 1080p "every
  effect on" figure being lower than the defaults' is the shared GPU.

### Assumed, or not done

- ☠️ **Never loaded into Resolume on macOS.** Everything was compiled, rendered
  and measured offline against the real plugin class in a headless CGL context,
  plus an `oxbow` load.
- On Windows, in Resolume Arena 7.27.1 (win-lab, Mesa llvmpipe, no GPU, 2026-09-24): this release's DLL loads from Extra Effects, registers as `SW Databend` / `DB01` / effect, all 31 host controls match the declaration, it renders and Arena's log stays clean: 9 of the fleet gate's 9 checks, with 21 of the 26 controls moving the picture (ten under a precondition) and five inconclusive on the gate's single frames (Rate, Depth, Flanger On, Phaser Rate and Line Padding: the LFOs' phase advances every frame, so the gate's noise floor covers them; the harness measures each against its closed form). Software rendering says nothing about a GPU or about speed.
- **Windows** is built by CI (release.yml; the first run compiled clean on MSVC
  because `kPi`, `<cmath>` and the `far`/`near` rename were in from the start).
- **Footage has only been seen through `--pipe`** — twelve demo clips, one frame
  each, at two rasters, for the defaults. Nothing has been through a show.
- **Not verified at 4K**, only benchmarked there.
- **The float part of the `--allpass` tolerances is a model**, said above.
- **No OpenFX port, no factory presets.** There is a user guide (`docs/USER-GUIDE.md`,
  built to the site by the website's `build_guides.py`). The browser demo's CPU
  half is a port that only a reader checks; its shaders are held to the C++ by
  `check_shaders.py`.

---

## Open questions

- **Phaser feedback.** The linear-algebra route would make it exact: per chunk,
  the cascade's transition Φ_b (S×S) and forced response c_b computed in
  parallel, a scan over the chunks (on the CPU after a readback, or a parallel
  prefix on the GPU), then the main pass from exact boundary states. Feasible
  for small S; the readback or the S² matrix traffic is the cost.
- **A tighter window.** The bound is about 2× what the error needs, and the
  phaser is most of the render cost. The pessimism is in the state magnitude at
  the restart: a rigorous per-stage bound better than min(sup chain, X√N) would
  halve the cost.
- **Should the delay memories cross frames?** Truthful to a video file as one
  stream; needs a ring of previous frames and makes the first lines of every
  frame echoes of the last lines of the previous one.
- **RGBA as a fourth layout**, with the alpha in the stream: a real file layout,
  and a fourth channel that the delays would land in.
- **Should Interleaved be the default after all?** It is the signature look and
  it is pink on everything; the mechanism is honest either way.

---

## Siblings

- **slope** — the harness, verify, CI and `--pipe` shapes, the negative
  controls, `--offline`, the state buffer, the finding that a windowed restart
  needs a proof, and the AGENTS.md shape.
- **clamp**, **compander** — a recurrence the GPU does, checked against the
  serial one.
- **ferric** — one-dimensional processing in scan order.
- **astronaught** — the frame-level tape echo; this is the sample-level one.
- **tinsel** — `PassBuffer`, `sweep.py`, and the fleet's trap list.
- **graticule** — the notes, and the provisional About.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
