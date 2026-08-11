# Third-party software and provenance

## Scope

This document records the provenance audit for the public `live-sampler` candidate. The repository contains no vendored third-party source tree. The `0BSD` license in the repository root applies only to original `live-sampler` source and documentation; it does not relicense dependencies.

The audit covered the two commits present before publication:

- `5b0e73ca53fd136876edff2a12ca24c8197428c2` — `docs: freeze pilot and record architecture review`
- `3f5ce4ff99ebe4920cfb91668c4238246af7be1e` — `fix: make sample buffer ownership and shutdown safe`

No copied or adapted implementation from SooperLooper, `drumkv1`, `samplv1`, Ardour, LinuxSampler, or JUCE was identified in the inspected history and source tree. Those projects informed architectural research only; that research is not itself third-party code.

## Original project code

The `src/`, `tests/`, `tools/`, `CMakeLists.txt`, and project documentation are original `live-sampler` material for licensing purposes unless a dependency is explicitly identified below. No third-party source was copied into the repository.

## Runtime and build dependencies

| Component | Candidate/probe evidence | Upstream/provenance | License status |
|---|---|---|---|
| Rubber Band | `pkg-config rubberband` reported `4.0.0`; CMake links `PkgConfig::RUBBERBAND`; the candidate includes `RubberBandStretcher.h` | [Breakfast Quay / Rubber Band](https://www.breakfastquay.com/rubberband/) | `GPL-2.0-or-later` according to the installed package metadata. **Not 0BSD.** |
| JACK API | CMake requires `pkg-config jack`; the candidate uses JACK client, port, MIDI, and transport APIs | JACK-compatible system implementation. The audited Kahuku environment provides the API through `pipewire-jack` 1.6.8 | The installed `pipewire-jack` package reports `MIT`, `GPL-2.0-only`, and `LGPL-2.1-or-later`. The project does not include that implementation. |
| FFTW | Transitive dependency reported by Rubber Band pkg-config/package metadata; installed version `3.3.11-1.1` | [FFTW](http://www.fftw.org/) | `GPL-2.0-or-later` according to the installed package metadata |
| libsamplerate | Transitive dependency reported by Rubber Band package metadata; installed version `0.2.2-3.1` | [libsamplerate](https://libsndfile.github.io/libsamplerate/) | `BSD` according to the installed package metadata |

Versions and package metadata above describe the audited build machine, not a vendored lockfile. Other systems may provide compatible versions or implementations. Consult the upstream license texts and package notices when distributing an executable that links these libraries.

## Distribution boundary

- `live-sampler` original source: `0BSD`.
- Rubber Band: upstream `GPL-2.0-or-later`; it remains under its own license.
- Other linked or transitive libraries: retain their upstream licenses and obligations.
- No release binaries are included in this repository by the publication commit.
- This repository does not grant permission to redistribute third-party software beyond the permissions provided by its respective upstream license.
