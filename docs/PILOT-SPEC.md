# live-sampler pilot specification

## Scope freeze

`live-sampler` is an independent JACK client. It exposes stereo `audio_in_1/2`, stereo `audio_out_1/2`, and `midi_in`; it has no MIDI OUT, OSC, GUI, server, LV2, persistence, or dependency on Seq66 internals.

- User-facing MIDI channel 1: record/replace.
- User-facing MIDI channel 2: gated playback.
- MIDI note 0..127 is the permanent slot identity.
- One simultaneous capture; eight preallocated playback voices.
- Empty slots, wrong channels, unmatched events, active-slot capture conflicts, and voice exhaustion are safe diagnostics.
- NoteOn velocity zero is NoteOff.

## Timing

The client is a JACK follower. It queries transport and `JackPositionBBT`/`beats_per_minute`; it never calls JACK transport or timebase setters. Without valid BBT, record/play activation is rejected with `NO_VALID_JACK_BBT`.

Capture is segmented by JACK MIDI `event.time`. A start at frame `t` copies from `t`; a stop at frame `t` excludes `t`. `capturedBeats` is computed from the continuous beat counter.

Elastic playback uses:

```text
targetSeconds = capturedBeats * 60 / currentBPM
physicalSeconds = sampleFrames / sampleRate
timeRatio = targetSeconds / physicalSeconds
pitchScale = 1.0
```

The ratio is updated while a voice is active. No custom time-stretch DSP is used.

## Realtime contract

The JACK callback performs no allocation, deallocation, locks, filesystem I/O, logging, blocking, thread operations, vector resizing, or decoding. All fixed arrays, queues, sample storage, voice state, and Rubber Band processing capacity are prepared before activation. Diagnostics use atomics or bounded preallocated records and are drained outside RT.

Samples are immutable after worker publication. The audio thread is the authority for slot pointer replacement and active voice references; the worker is the only owner that destroys retired samples.

## Pilot acceptance

Offline tests must prove ratio direction, permanent slot identity across replace, MIDI semantics, and sine pitch/duration. Controlled JACK tests must prove frame-accurate capture, gated playback, elastic 60/120/240 BPM, dynamic BPM, and onset error. Exact-candidate Seq66 E2E is downstream of those gates and must use an isolated Seq66 home and `-0`.
