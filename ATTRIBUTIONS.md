# Attributions

Databend is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Harness shape, --pipe contract and verify — Stoatworks slope and clamp

<https://github.com/stoatworks-labs/slope>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness shape, the --pipe contract (SIGPIPE ignored, a closed stdout exits 1), the negative-control pattern, --offline, check-shaders.sh, the verify script, the sweep, the CI workflows and the three-attachment state buffer are slope's, by way of clamp and standards. The stepped cues for options in --pipe are databend's own.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The off-screen buffer wrapper is tinsel's, by way of standards, clamp and slope.

### A serial recurrence held to a serial reference — Stoatworks compander, clamp and slope

<https://github.com/stoatworks-labs/compander>  
Licence: MIT  
Copyright: Stoatworks Labs

The idea of computing a recurrence on the GPU and holding it to a serial double-precision one is compander's and clamp's; slope's finding that a windowed restart needs a proof, and its chunked scheduling, shaped the phaser's two-pass form. The window bound is databend's own.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Databending

The practice of opening an image file as raw audio in an audio editor and running its effects on it, as it has been done since the early 2000s. Built from the description of what the editor sees (one stream of samples in the file's own layout) and what its delay-line effects do to it; no editor's code, presets or effect designs are used.

### The normalised lattice all-pass section

The Gray-Markel normalised first-order lattice, as described in the open literature: a rotation of the input and the state, lossless for any time-varying coefficient. That losslessness is what makes the phaser's restart bound and its energy check possible.

## Standards and published specifications

What the implementation is measured against.

- **Nicholas J. Higham, Accuracy and Stability of Numerical Algorithms** — The rounding-error terms in the harness's tolerances.
- **Melissa E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation" (Harvey Mudd College, 2014)** — The pcg_hash output mix the harness's noise pictures use, written out rather than copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
