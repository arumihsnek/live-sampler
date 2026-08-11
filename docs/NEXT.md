# Deferred features

The pilot intentionally does not implement CC start/end position, reverse, x2/x1/2/x1/4, ABSOLUTE or TAPE modes, multi-output buses, per-slot output, effects, pitch transpose, one-shot/loop modes, WAV persistence, session restore, GUI, waveform editing, LV2, OSC, MIDI learn, or MPD218.

Future fit:

- Start/end position: normalize CC 0..127 to sample start/end frame bounds in the voice reader; keep `SampleBuffer` immutable.
- Reverse: add a voice read direction, without changing slot identity or ownership.
- Speed multipliers: multiply `targetBeats = capturedBeats * lengthMultiplier`, while keeping pitch scale 1.0 in ELASTIC.
- Multi-output: map play MIDI channels to preallocated JACK output buses (for example CH2 bus1, CH3 bus2, CH4 bus3) without changing slot storage or tempo ownership.
- Persistence: serialize immutable samples outside RT only; reload and prepare buffers before JACK activation.
