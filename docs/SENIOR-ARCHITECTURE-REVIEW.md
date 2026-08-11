# Senior architecture review — live-sampler

## Gate context

- Mission: independent `live-sampler` pilot; no changes to Seq66, SooperLooper, PipeWire, WirePlumber, JACK configuration, or existing control planes.
- Review execution: `9a96e286-8863-4587-9f17-ea8cd29b6d85`.
- Review status: `COMPLETED / VALID_ADVISORY_VERDICT`, verdict `changes_required`.
- Consultation model: `gpt-5.6-sol`, low effort; one model process, one observed turn, zero tools.

## Decisions

1. **Stretch library: Rubber Band 4.0.0.** It is installed on Kahuku (`pkg-config rubberband=4.0.0`, package `4.0.0-2.1`, GPL-2.0-or-later). Signalsmith Stretch was not installed or vendored in the inspected roots, so it is not introduced for this pilot.
2. **Realtime mode:** use Rubber Band realtime mode only, with a single explicit option set and `OptionThreadingNever`. Construction, option selection, max process size, scratch buffers, and warm-up happen outside JACK. The callback may use only the exact proven-safe subset: same-thread ratio update, process, available, and retrieve, within the configured process bound.
3. **Ownership:** the worker constructs an immutable `SampleBuffer`; a bounded preallocated publish queue transfers a pointer/handle to the audio thread. The audio thread owns slot replacement and voice references. A bounded retire queue transfers no-longer-referenced samples back to the worker for destruction. Queue-full behavior is non-destructive and diagnostic.
4. **Capture:** consume JACK MIDI events in frame order and process segments `[cursor,event.time)`. Start/stop boundaries use the event frame exactly. Finalization is worker-only.
5. **Tempo:** `capturedBeats` is authoritative; `timeRatio=(capturedBeats*60/currentBPM)/physicalSeconds`, pitch scale `1.0`. Ratio updates happen on the same thread as Rubber Band processing, including BPM changes during playback. Invalid BBT must not invent a BPM fallback.
6. **Latency:** use `getPreferredStartPad()` and `getStartDelay()` after setting initial ratios, pad from preallocated memory, discard documented initial delay, and measure first audible output as `PLAY_ONSET_ERROR_FRAMES`. The <= one-quantum gate requires an actual impulse measurement; it cannot be inferred.
7. **Voice lifetime:** fixed-capacity voices; voice completion releases its sample reference on the audio thread. A worker must never destroy a published sample based only on a concurrently observed counter.

## Required implementation gates

- An exact Rubber Band 4.0.0 harness must instrument construction/preparation/reset/setTimeRatio/process/available/retrieve/teardown and verify the callback subset and bounded process size.
- Define reset policy explicitly. If reset is not proven callback-safe, prepare idle stretchers outside RT and publish them through fixed queues.
- Define same-frame MIDI ordering, retrigger, release, voice exhaustion, replacement conflicts, and queue-full diagnostics.
- Measure onset across supported sample rates, quanta, ratio extremes, retrigger, and dynamic `120 -> 90 -> 150 BPM`.
- Add ARM64 qualification as a separate gate; this Kahuku build is currently x86_64_v3.

## Review conclusion

Rubber Band is the chosen library, but implementation is not accepted by this review until the exact API call subset, lifetime protocol, and onset measurements are evidenced. The pilot must fail closed on unresolved realtime or lifetime blockers.
