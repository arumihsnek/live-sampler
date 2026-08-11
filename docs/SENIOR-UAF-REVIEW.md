# Senior UAF / ownership review

## Scope

This review covers only `SampleBuffer` lifetime and JACK shutdown. It does not change DSP, MIDI, BBT, Seq66, or PipeWire behavior.

## Root cause

The original `retire_if_unused()` checked active voices but not `Slot::current`. On PLAY NoteOff, `release_voice()` cleared the voice and could enqueue the same buffer that the slot still published. The worker freed it, and `SamplerEngine::~SamplerEngine()` later freed the dangling slot pointer.

A second reachable issue was loss of ownership when both retirement stores were full. A third issue was worker shutdown spinning forever trying to publish a pending buffer after JACK callbacks had stopped. A fourth issue was writing retirement flags after publishing the pointer to the worker.

## Ownership state machine

| State | Owner | Readers | Transition | Free authority |
|---|---|---|---|---|
| allocated/finalizing | worker | worker | worker creates and fills | worker, if publication is abandoned |
| publish queue | publish queue / worker transfer | audio thread after pop | worker `push`, audio `pop` | audio shutdown drain if never installed |
| slot current | audio-thread slot retention | audio thread and voices | audio thread installs/replaces | audio shutdown after callbacks and worker stop |
| active voice | slot remains authoritative; voice is borrowed | audio callback | audio thread starts/stops voice | slot or retirement owner, never voice directly |
| deferred retire | audio thread fixed table | no reader after handoff eligibility | audio thread records when queue full | worker after later queue transfer, or shutdown |
| retire queue | worker | worker | audio thread publishes after ownership handoff | worker only |
| blocked publish | audio thread fixed pointer | no reader until retry | audio thread holds replacement if old cannot retire | audio shutdown if never installed |
| freed | none | none | owning path only | exactly once |

## Required invariants

1. `slot.current` and `retire_queue_` are mutually exclusive for every pointer.
2. An active voice prevents replacement of its slot; voices never own or free samples.
3. Retirement metadata is written before a successful SPSC publication. No `SampleBuffer` field is touched after the worker can observe the pointer.
4. If retirement capacity is unavailable, the old slot reference is restored and the replacement remains in `blocked_publish_`.
5. Worker shutdown never waits for a stopped audio consumer: pending publication is freed if it cannot be transferred.
6. JACK is deactivated and closed before `engine_` member destruction; callbacks therefore cannot access the engine during destruction.

## Exact fix

- `sample_referenced()` now checks all slots and active voices.
- `deferred_retire_` retains failed retirement transfers without allocation.
- `blocked_publish_` makes replacement transactional under total retirement saturation.
- `publish_sample()` clears `slot.current` before publishing the old pointer, restores it only when publication fails, and installs the new sample only after successful transfer.
- retirement flags are set before `retire_queue_.push()` and are never written after successful publication.
- the worker frees pending publication instead of spinning after stop.
- the destructor drains mutually exclusive slot, publish, retire, deferred, and blocked holdings.

## Evidence

- `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ctest --test-dir build-debug --output-on-failure`: 2/2 PASS.
- Same CTest suite repeated 50 times: PASS.
- Saturation test fills all retirement stores and verifies old slot plus blocked replacement remain reachable: 30/30 PASS under ASan/UBSan.
- JACK activate/callback/deactivate/SIGINT shutdown under Debug ASan: `sampler_rc=0`, no UAF/invalid free, all sampler diagnostics zero.
- External LeakSanitizer reports observed in JACK runs are rooted in `jack_client_open` / PipeWire shared-library frames, not live-sampler allocations. Offline core and saturation tests report no leaks.

## Senior consultations

- r1 execution `9bbe4367-3277-4375-b4d6-7b88a7fdc623`: blocked; identified slot retention, retirement overflow, and pending-publish shutdown risks.
- r2 execution `dc3c42b1-bfca-4fa1-aded-3307ce83663e`: changes required; identified bounded overflow loss.
- r3 execution `68d867af-de55-4970-9f60-597c6b76e30d`: changes required; identified post-publication metadata race.
- r4 execution `ea850edb-b55b-4723-a1ff-479c9292468e`: changes required; identified slot/retire publication overlap.
- r5 execution `445e098d-525d-4b4c-9be0-532fdbd65b95`: **accept**, no blocking findings.
