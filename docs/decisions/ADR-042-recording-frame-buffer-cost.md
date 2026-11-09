# ADR-042 — What a recorded frame's buffers cost

**Status:** Accepted (BGRA pool and no-fill resize implemented and validated
in the lab; NV12 sample reuse deferred)  
**Date:** 2026-11-09

## Context

Before this decision, every frame a recording captured and wrote got new
memory:

- **The BGRA picture.** `CaptureRegionFrame` read each frame into a new
  `BgraImage` and wrapped it in a new `shared_ptr<const BgraImage>`
  (`capture_provider.cpp`). Inside it, `AcquireFrame` read into a local
  `candidate` image and moved it into the caller's, so the caller's buffer
  would be freed even if the caller kept its image. `BgraImage::Reset`
  allocated the pixels of an empty image and always filled them with opaque
  black before the row copy overwrote them; `MakeOpaque` then wrote every
  pixel a third time. At 1920 × 1080 the picture is 8,294,400 bytes.
- **The NV12 buffer.** `CoreWriteVideoFrame` (`media_foundation_writer.cpp`)
  calls `MFCreateMemoryBuffer` with the NV12 size for each frame, converts the
  picture straight into it and hands it to the sink writer in a new
  `MFCreateSample`. At 1920 × 1080 the buffer is 3,110,400 bytes. This is
  unchanged.

Already reused: the Direct3D staging texture of a steady region, the frame
queue's slots, the picture of a cadence slot without a new frame (ADR-005
rule 2) and the frame passed to the writer by pointer.

A large allocation on the Windows heap comes from fresh pages, so each new
picture also costs about one page fault per 4 KiB the first time it is
written. Nothing had measured whether any of this matters next to the
H.264 encoder.

## Decision

Measure first, inside Easnap, then decide by a threshold set in advance:
reuse the buffers if reuse would save **at least 1 ms per frame at 1080p30 on
Desktop Duplication**, or **at least 5 % of the process CPU time per frame**,
or if `pacing_missed` or `queue_full` would drop.

The figures below met it for the BGRA picture: **the recording worker reuses
its pictures from a pool, and a picture that is overwritten in full is no
longer filled first.** They did not meet it for the NV12 buffer, whose reuse
is deferred.

### The measurement

- **`frame_buffer_cost`** (fast tier, `unit;recording;performance`,
  `tests/test_frame_buffer_cost.cpp`) times on the machine it runs on, 200
  iterations each at 1920 × 1080 and 3840 × 2160: the production
  `BgraImage::Reset` on a new image every iteration (allocation, fill and
  release: what a frame cost before) against one image reset to the same size
  (the fill alone), a full frame copy (writing every pixel once) and
  `MakeOpaque`; then what a frame's readback costs now, `ResizeForOverwrite`
  on a reused picture and `CopyOpaqueFrom` into a reused picture (a pooled
  frame) and into a new one (a pool that is still filling up); and the
  writer's `MFCreateMemoryBuffer`, `Lock`, `Unlock` and `SetCurrentLength`
  with a new buffer every iteration against one buffer reused. Both NV12
  variants write every byte, as the conversion does. It prints one `MEASURE`
  line per operation with the median time and the page faults per operation.
  It checks that sizes and calls are sound, that `CopyOpaqueFrom` gives
  exactly the bytes of `Reset`, a row copy and `MakeOpaque` (rows with padding
  between them, into new, larger, smaller and same-size pictures) and that a
  picture reused at its size keeps its storage; it never judges a time. The
  3840 × 2160 lines give a 4K figure without a 4K display. Where Windows has
  no Media Foundation it measures the images and exits 77.
- **`recording_frame_cost`** (Soak tier, visible desktop,
  `integration;real-desktop-capture;recording;performance;soak`,
  `test_recording_session_integration --frame-cost`) records the whole test
  monitor three times through the production session: content that changes
  about twice per frame slot, 30 fps, MP4 without audio and without the
  pointer, 10 active seconds each. Around `Start` to the end of `Stop`'s
  finalization it reads `GetProcessTimes` (user and kernel time, less the
  painting thread of the test window) and `GetProcessMemoryInfo` (page faults,
  peak private bytes), and prints one `RECORDING_FRAME_COST` line per
  recording with the capture path from the recording-path probe, frames,
  acquired frames and attempts, `fallback_frames`, missed pacing slots,
  queue-full rejections, the peak queue depth, `region_copy_timing`,
  `writer_submission_timing`, faults and CPU time per written frame, the
  process's peak private bytes since it started, and the pool's
  `picture_allocations` and `peak_pictures`, then a `summary` line with the
  medians. The painting thread's own CPU time is printed apart. It checks
  that the recordings are sound, that at least 90 % of the attempts acquired
  a new picture, that the pool never had more pictures in use than can be
  alive at once and that it made a picture only when every earlier one was in
  use. It runs in the lab by a Soak run or `lab.ps1 probe`; the host runs it
  only when the owner asks.

### Host figures

`frame_buffer_cost` on the development PC (AMD Ryzen 7 5800X3D, 16 logical
processors, 64 GB, Windows 10 22H2, Release build, run alone), 2026-11-09.
Median of 200 iterations; faults are page faults per operation. The first
run, before the change:

| Operation | 1920 × 1080 | Faults | 3840 × 2160 | Faults |
|---|---|---|---|---|
| `BgraImage::Reset` on a new image (allocate, fill, release) | 1,503 µs | 2,030 | 6,620 µs | 8,117 |
| `BgraImage::Reset` on the same image (fill only) | 304 µs | 0 | 1,189 µs | 0 |
| Full frame copy into that image | 300 µs | 0 | 1,192 µs | 0 |
| New NV12 buffer: create, `Lock`, write, `Unlock`, release | 422 µs | 762 | 1,489 µs | 3,044 |
| The same NV12 buffer: `Lock`, write, `Unlock` | 125 µs | 0 | 443 µs | 0 |

With the change, the same machine and the same conditions (one run; the
operations measured before gave 1,314, 291, 290, 391 and 121 µs at 1080p in
it, so single figures vary by about 10 to 15 %):

| Operation | 1920 × 1080 | Faults | 3840 × 2160 | Faults |
|---|---|---|---|---|
| `MakeOpaque` (the third pass the readback made) | 124 µs | 0 | 483 µs | 0 |
| `ResizeForOverwrite` on a reused picture | 0 µs | 0 | 0 µs | 0 |
| `CopyOpaqueFrom` into a reused picture (a pooled frame) | 144 µs | 0 | 672 µs | 0 |
| `CopyOpaqueFrom` into a new picture | 1,399 µs | 2,030 | 5,583 µs | 8,117 |

Read on this machine: the readback of a 1080p frame wrote a new picture three
times, about 1.7 ms (allocation, fill and release, copy, `MakeOpaque`), and
writes a pooled picture once, about 0.15 ms; at 2160p about 6.5 ms against
0.7 ms. The one-pass copy is not slower than `memcpy` here; it keeps the
picture in the cache where the large `memcpy` does not. A new NV12 buffer
costs about 0.3 ms more than a reused one at 1080p and 1.0 ms at 2160p. These
are the costs of the operations alone, on a fast desktop processor; the lab
figures say what they cost inside a recording.

### Lab figures

Before the change, the lab Soak run of `recording_frame_cost` (the lab's
1920 × 1080 display at 125 %, 30 fps, Desktop Duplication): about 2,160 page
faults per written frame, `region_copy_timing` about 6.7 ms on average, 0.7
to 1.3 ms of process CPU time per frame, and 15 missed pacing slots in 286.
About 2,030 of those faults are the new BGRA picture's pages, the 1.2 ms per
frame the host measured for them is above the 1 ms threshold, and a readback
that takes 6.7 ms on average at 30 fps leaves little room before a slot is
missed. The BGRA picture meets the threshold.

After the change, the lab Soak run `20261109-103322-soak` (tree `bd10347`,
the same image and the same three recordings; the run before it was
`20261109-054120-soak`):

| `recording_frame_cost`, 1080p30, Desktop Duplication | Before | After |
|---|---|---|
| Page faults per written frame | about 2,160 | 862 |
| BGRA pictures allocated per recording | one per frame | 2 (`picture_allocations`), at most 2 in use (`peak_pictures`) |
| `region_copy_timing` | 6.7 ms on average | 5.7 to 6.1 ms (median of each recording) |
| `writer_submission_timing` | 5.7 ms on average | 5.8 to 6.0 ms |
| Process CPU time per frame | 667 to 1,311 µs | 106 to 708 µs |
| Missed pacing slots | 14 to 20 of 286 (median 15) | 6 to 14 of about 290 |

`frame_buffer_cost` in the same run, in the VM, 1920 × 1080:

| Operation | Time | Faults |
|---|---|---|
| `BgraImage::Reset` on a new image | 1,633 µs | 2,030 |
| `BgraImage::Reset` on the same image | 295 µs | 0 |
| `CopyOpaqueFrom` into a new picture | 1,854 µs | 2,030 |
| `CopyOpaqueFrom` into a reused picture | 156 µs | 0 |

What the after figures say. The pool made two pictures per recording, not
one per frame, and about 1,300 page faults per frame are gone; the 862 that
remain are close to the 762 the host measured for the new NV12 buffer of
each frame. The process spends roughly half the CPU time per frame or less,
and fewer pacing slots are missed. The region copy fell by 0.6 to 1.0 ms,
less than the 1.2 ms expected, and the two figures are not the same
statistic (an average before, medians after); a copy into a pooled picture
takes 0.16 ms in the VM, so most of the remaining 5.8 ms is the readback
itself, not the picture. The writer, which this decision does not touch,
stayed at about 5.9 ms. The decision is validated in the lab: the CPU time
and the missed slots met the threshold set in advance.

### What changed

- **`BgraImage::ResizeForOverwrite`** sets the size and resizes the storage
  without filling it: pixels already there keep their old values and added
  ones are zero, so it is only for a caller that writes every pixel next. A
  picture reused at its size is neither filled nor reallocated; growing past
  the capacity drops the old pixels instead of copying them.
  **`BgraImage::CopyOpaqueFrom`** resizes that way and copies 32bpp rows with
  their pitch, making each pixel opaque in the same pass. The three places
  that overwrite every pixel use it: the full-texture readback, the region
  readback and the GDI blit. `Reset` and its opaque black stay where black
  must remain: the canvas of a region across monitors, the picture of a
  monitor that could not be read, the cropped snapshot and the cursor patch.
- **A picture pool owned by the recording worker**
  (`RecordingPicturePool`, `recording_capture_worker.h`). Every picture of a
  recording has the region's size. `Acquire` hands out a picture, a free one
  when there is one and otherwise a new one; it never blocks. The picture
  goes back through the deleter of its `shared_ptr` when its last owner lets
  it go, on whatever thread that is, never by looking at `use_count`. The
  free list is guarded by a mutex, reserved to its cap so a return never
  allocates, and capped at `RecordingPicturesAlive(capacity)`: the queue's
  capacity plus `kCaptureWorkerHeldFrames` plus `kConsumerHeldFrames`, seven
  with the default queue; a picture returned beyond the cap is freed. The
  pictures share the ownership of the pool, so a picture released after
  `Stop`, after `Release` or after the worker is gone is safe. `Start` opens
  the pool and starts its counts again; `Stop` closes it, freeing the free
  list and every picture returned later. A picture handed out as a frame is
  `const` and is not written again until every frame that holds it is gone,
  so ADR-005 rule 2 holds.
- **The worker reads through `CaptureRegionImage`** into the pooled picture
  instead of `CaptureRegionFrame`, with `RecordingFrameImageOptions`, the one
  definition both use: one poll without waiting, the pointer when asked, and
  `await_first_composed_frame`, so the 2026-11-09 amendment of ADR-004 holds.
  The timing grid, the timestamps and durations, `NoNewFrame` and `Failed`
  reusing `latest_pixels`, and the counters are unchanged; a picture the
  capture did not fill goes back to the pool at the end of its slot.
- **`AcquireFrame` reads into a provider-owned scratch picture** next to the
  staging texture and swaps it with the caller's picture when a frame is
  acquired, so `NoNewFrame` and `Failed` leave the caller's picture untouched
  and the caller's storage becomes the next scratch. A black provisional
  picture is still handed over and then overwritten by the GDI blit. With the
  pool, a recording reuses the same storage frame after frame; a caller that
  passes a new picture each time (the screenshot snapshot) gets one
  allocation per capture, as before. `Release` frees the scratch.
- `bounded_frame_payload_bytes` counts `RecordingPicturesAlive` from the same
  header, and the session diagnostics report `picture_allocations` and
  `peak_pictures_in_use`.

### The memory bound

Up to seven pictures can be alive at once when the content changes and the
consumer is behind: three in the queue; two held by the capture worker
(`kCaptureWorkerHeldFrames`: the latest picture, kept for reuse even after the
full queue rejected it, and the new one being captured while the latest is
still held); and two held by `ConsumerMain` while it writes
(`kConsumerHeldFrames`: the pending frame and the frame just taken from the
queue, whose timestamp sets the pending frame's duration). The bound counts
seven pictures, and the pool keeps at most seven. `recording_worker_foundations`
checks the worker's two against the real worker: with nothing taken from the
queue, the most pictures in use at once is the queue's capacity plus two, and
the pool made exactly that many.

The NV12 figure stays one buffer, the one the writer converts into. Samples
Media Foundation keeps after `WriteSample` (the sink writer's queue and the
encoder's own, limited by the sink writer's throttling) are not Easnap's to
bound and are not counted; a hardware encoder may hold several. The
provider's own region-sized buffers, the persistent Direct3D staging texture,
the scratch picture the readback goes through and the temporary GDI section
of the fallback, are not frame payload and are not counted either.

### NV12 sample reuse, deferred

A new NV12 buffer costs about 0.3 ms per frame at 1080p on the development PC,
below the threshold on its own. Reusing it would mean tracked samples: in
`CoreWriteVideoFrame`, a free list of `MFCreateTrackedSample` samples, each
with one `MFCreateMemoryBuffer` of the NV12 size, returned by a separate
reference-counted callback object given with `SetAllocator` before each
`WriteSample`; the same contiguous buffer, `SetCurrentLength`, sample time and
duration, no attributes; not `MFCreateVideoSampleAllocatorEx`, whose 2D
buffers have a pitch. A hardware encoder may keep its input samples after
`WriteSample`, the lab has only the software encoder, and the trim fallback
writer goes through the same function, so the change would need a run on a
physical GPU before it could ship. It is not done. It is reconsidered if the
lab's after figures still miss pacing slots or a hardware encoder run shows
the NV12 buffer's cost. The after figures still miss 6 to 14 slots of about
290; in those recordings the region copy and the writer take about 5.8 and
5.9 ms of each frame, against about 0.3 ms for a new NV12 buffer on the
development PC, so the deferral stands until a hardware-encoder run on a
physical GPU. `CoreWriteVideoSample` (the native NV12 trim path), the audio
path and the encoder are not touched by this decision.

## Consequences

- A recording of changing content allocates at most seven pictures instead
  of one per frame (two in the lab's recordings); a still recording, fewer.
  The pages of a pooled picture are touched once.
- The bound is 61,171,200 bytes at 1920 × 1080 and 244,684,800 bytes at
  3840 × 2160 (seven pictures and one NV12 buffer). A pool full to its cap
  holds the pictures from the moment the consumer first falls behind until
  `Stop`, instead of freeing them between frames; the scratch picture adds
  one region-sized buffer while the recording runs.
- `CaptureRegionFrame` stays in `IRegionCaptureProvider` with the same
  behaviour; production recording no longer calls it, and the recording-path
  probe of `real_desktop_recording_session` and other tests do. The worker's
  test providers implement `CaptureRegionImage`.
- Before and after are two lab runs of the same two tests on the same image.
- The test window's own painting is taken out of the CPU figure; the
  composition it causes runs in the Desktop Window Manager, outside the
  process.

## Not validated

- A physical GPU and a hardware encoder: the after figures come from the
  lab's VM, which has neither.
- The tests that read the real desktop (`real_desktop_recording_session`,
  `recording_pause_stop_stress`, `island_capture_exclusion`, the Scrollshot
  desktop tests) were built but not run on the host. With the pool they
  passed in the lab runs `20261109-101913-full` and `20261109-103322-soak`,
  apart from `scrollshot_realistic_desktop`
  ([ADR-041](ADR-041-scrollshot-realistic-test-application.md#validation)).
- A region across monitors, a rotated output and the GDI fallback were not
  exercised with the pool on real displays.
