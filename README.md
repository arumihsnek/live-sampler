# live-sampler

**Status: PILOT / EXPERIMENTAL**

`live-sampler` is a minimal headless JACK sampler driven by MIDI and JACK transport. Its pilot concept is:

> MIDI channel REC + MIDI channel PLAY + note number = sample slot + JACK transport/BBT follower + elastic time-stretch preserving pitch

It is intentionally a small experimental client, not a production-ready instrument.

## Architecture

- A JACK client exposes stereo input/output ports and one MIDI input.
- MIDI channel 1 records or replaces a slot; MIDI channel 2 gates playback.
- MIDI note numbers `0..127` are permanent sample-slot identities.
- The JACK callback follows transport and `JackPositionBBT`; it does not become a transport master.
- Capture buffers, voices, queues, and Rubber Band processing capacity are prepared before activation.
- Rubber Band performs elastic playback with pitch scale `1.0`.
- Sample buffers are immutable after publication; retirement and destruction are kept outside the JACK callback by the current candidate design.

## Build

The build requires a C++20 compiler, CMake, pkg-config, a JACK-compatible development package, and Rubber Band development headers/library.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

On the audited Kahuku/CachyOS system the relevant packages were `pipewire-jack`, `rubberband`, `fftw`, and `libsamplerate`. Package names differ by distribution; use the package manager's JACK-compatible development package and the upstream Rubber Band package for your system.

## Run

Start a JACK-compatible server, then launch:

```sh
./build/live-sampler --rec-channel 1 --play-channel 2 --max-capture-seconds 30 --voices 8
```

Options:

- `--rec-channel N`: user MIDI channel for record/replace, default `1`.
- `--play-channel N`: user MIDI channel for gated playback, default `2`.
- `--max-capture-seconds N`: preallocated maximum capture length, default `30`.
- `--voices N`: maximum simultaneous playback voices, `1..8`, default `8`.
- `--name NAME`: JACK client name, default `live-sampler`.

The client provides these ports (with the selected client name):

- `audio_in_1`, `audio_in_2`
- `audio_out_1`, `audio_out_2`
- `midi_in`

## MIDI semantics

- Channel 1 Note On starts recording/replacing the note-number slot.
- Channel 1 Note Off ends that capture. Note On with velocity zero is treated as Note Off.
- Channel 2 Note On starts gated playback of the selected slot; velocity controls gain.
- Channel 2 Note Off stops the matching playback voice.
- Empty slots, invalid channels, active-slot replacement conflicts, unmatched events, and voice exhaustion are handled as diagnostics rather than undefined behavior.
- Record and playback activation require rolling JACK transport and valid `JackPositionBBT` with a positive BPM.
- The note number is the slot identity; replacing a slot does not renumber other slots.

## Tests

Run the registered offline tests:

```sh
ctest --test-dir build --output-on-failure
```

The repository currently contains core behavior tests and a lifetime-saturation test. The pilot also includes the JACK driver and Rubber Band probe under `tools/`; they require a running JACK-compatible server and are not automatic production certification.

## What is implemented, tested, and pending

| Area | State | Meaning |
|---|---|---|
| MIDI channel mapping and note-slot identity | **IMPLEMENTED / TESTED** | Covered by the source and offline tests. |
| Basic capture/playback and elastic ratio calculation | **IMPLEMENTED / TESTED** | Covered by the current pilot tests. |
| JACK BBT-following behavior | **IMPLEMENTED / PILOT-TESTED** | Requires a JACK transport environment for runtime evidence. |
| Runtime ownership/lifetime behavior | **IMPLEMENTED / TESTED** | The candidate has offline lifetime coverage; inspect the source before relying on stronger real-time claims. |
| Controlled frame-accurate, gated, and multi-tempo pilot gates | **PENDING FINAL EVIDENCE** | Not represented as a production guarantee by this README. |
| Seq66 end-to-end routing | **PENDING** | This repository is independent of Seq66 and does not modify it. |
| Production readiness, complete elastic certification, and sample-accurate guarantees | **NOT CLAIMED** | The project remains PILOT / EXPERIMENTAL. |

## Licensing

Original `live-sampler` code and documentation are released under the [Zero-Clause BSD License (0BSD)](LICENSE). Third-party dependencies retain their own licenses. See [`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md) for the provenance and dependency boundary; in particular, Rubber Band is **not** 0BSD and executable distribution may carry additional upstream obligations.
