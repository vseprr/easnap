# ADR-007 — A/V muxing and active-time audio mapping

**Status:** Accepted  
**Date:** 2026-08-28

## Context

The production WASAPI provider captures native-format PCM on its own QPC-based
timeline and already owns a bounded 500 ms queue. The Media Foundation sink
writer must declare every stream and input media type before `BeginWriting`,
while the recording session must exclude paused wall time from both streams and
must finalize only after every accepted sample has drained.

## Decision

1. A recording session owns at most one `IAudioCaptureProvider`: a single-source
   WASAPI provider for SystemAudio/Microphone, or the composed dual provider
   for Both (ADR-012). It submits one canonical audio stream to the writer.
2. For an audio-enabled session, the audio provider starts first so its `AudioFormat`
   is known before the sink writer opens. Audio callbacks are gated until the
   video worker and writer are ready, so startup packets are not submitted to an
   unopened stream.
3. `MediaFoundationWriter` declares H.264 video and AAC-LC audio streams before
   `BeginWriting`. The sink writer's bounded/throttled transform chain converts
   native PCM or float input to encoder-compatible 16-bit PCM, resampling to
   44.1 kHz when the native rate is 44.1 kHz and otherwise to 48 kHz, and emits
   mono or stereo AAC.
4. `RecordingSession` does not add a second audio queue. The provider's bounded
   delivery path invokes the session callback, which maps and synchronously
   submits one non-owning packet view to the writer's serialized MTA command
   thread. Media Foundation copies that packet into one bounded sample buffer.
5. WASAPI remains active during pause. The session gates and discards paused
   callbacks, then anchors a new segment with current QPC and the frozen video
   active-time endpoint on resume. A first packet that arrives later retains its
   QPC-measured delay from start/resume instead of being collapsed onto the
   segment boundary. Paused wall time remains excluded without restarting the
   device or changing its negotiated format.
6. Within a continuous segment, PCM timestamps and durations are derived from
   cumulative captured frame counts and sample rate. Device positions and
   WASAPI flags detect gaps; QPC re-anchors only a new segment or genuine
   discontinuity. Callback scheduling jitter therefore cannot create PCM gaps,
   overlaps, or variable packet cadence.
7. Media Foundation throttling remains enabled to keep its internal transform
   pipeline bounded. To avoid one stream blocking the serialized writer while
   the other catches up, the video consumer writes PCM silence in 1024-frame
   packets before each video frame, up to 750 ms behind that frame and never
   past a captured packet that is waiting for submission (`WriteSilenceUpTo`),
   and real audio submission is held to at most 100 ms ahead of
   written video. Waiting remains backed by the existing bounded 500 ms WASAPI
   queue; no duration-growing mux queue is introduced. If written video does
   not advance for 10 s while a packet waits, the session fails instead of
   blocking Pause, Stop or Abort (v1.0.1). Separate diagnostics
   measure video writes, audio writes, silence writes, and inter-stream waits.
8. Capture stop first closes audio acceptance, waits for any in-flight callback,
   stops both producers, drains the video queue, and joins all worker threads.
   `FinalizeAudioTimeline` then fills the interval from the last committed
   audio sample to the active Stop endpoint with PCM silence in 1024-frame
   packets, so even an entirely silent loopback session finalizes with
   valid-duration AAC. That interval is normally no longer than the 750 ms
   lag of item 7. The fill reuses one 1024-frame buffer and its memory
   does not grow with recording duration.
9. A gap between the committed audio end and the next captured packet,
   including one caused by a WASAPI discontinuity, is filled with PCM silence
   before that packet is submitted (`FillSilenceGap`). The encoded audio
   timeline is therefore continuous. Writer validation rejects overlapping or non-monotonic
   samples and finalization requires at least one video sample and, when enabled,
   at least one audio sample.

## Consequences

- Audio and video use the same active-time media domain, and paused wall time is
  absent from both streams.
- Audio buffering remains bounded by the existing 500 ms WASAPI queue, one
  callback packet, one writer command, and Media Foundation's throttled pipeline.
- Initial source silence is written as PCM silence, and continuous
  PCM presentation timing is sample-accurate rather than callback-timed.
- Silence written behind video and the 100 ms audio lead gate prevent Media Foundation's
  default cross-stream throttling from starving the other bounded producer.
- Stop, failure rollback, abort, drain, and finalization have a deterministic
  ownership order across both streams.
- AAC frame granularity can make the decoded audio end or presentation duration
  differ from the active Stop endpoint by roughly one AAC frame.
- Native formats outside the validated Media Foundation conversion envelope
  (8–192 kHz, up to eight channels, with a channel mask for multichannel input)
  fail transactionally with an actionable writer error rather than silently
  producing a partial A/V success.
