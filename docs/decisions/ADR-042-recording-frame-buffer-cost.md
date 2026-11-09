# ADR-042 — What a recorded frame's buffers cost

**Status:** Proposed; the measurement and the corrected memory bound are
implemented, the decision on reusing the buffers waits for the lab figures  
**Date:** 2026-11-09

## Context

Every frame a recording captures and writes gets new memory:

- **The BGRA picture.** `CaptureRegionFrame` reads each frame into a new
  `BgraImage` and wraps it in a new `shared_ptr<const BgraImage>`
  (`capture_provider.cpp`). Inside it, `AcquireFrame` reads into a local
  `candidate` image and moves it into the caller's, so the caller's buffer
  would be freed even if the caller kept its image. `BgraImage::Reset`
  allocates the pixels of an empty image and always fills them with opaque
  black before the row copy overwrites them; `MakeOpaque` then writes every
  pixel a third time. At 1920 × 1080 the picture is 8,294,400 bytes.
- **The NV12 buffer.** `CoreWriteVideoFrame` (`media_foundation_writer.cpp`)
  calls `MFCreateMemoryBuffer` with the NV12 size for each frame, converts the
  picture straight into it and hands it to the sink writer in a new
  `MFCreateSample`. At 1920 × 1080 the buffer is 3,110,400 bytes.

Already reused: the Direct3D staging texture of a steady region, the frame
queue's slots, the picture of a cadence slot without a new frame (ADR-005
rule 2) and the frame passed to the writer by pointer.

A large allocation on the Windows heap comes from fresh pages, so each new
picture also costs about one page fault per 4 KiB the first time it is
written. Nothing had measured whether any of this matters next to the
H.264 encoder.

## Decision

Measure first, inside Easnap, before any buffer changes. Two tests do it:

- **`frame_buffer_cost`** (fast tier, `unit;recording;performance`,
  `tests/test_frame_buffer_cost.cpp`) times on the machine it runs on, 200
  iterations each at 1920 × 1080 and 3840 × 2160, the production
  `BgraImage::Reset` on a new image every iteration (allocation, fill and
  release) against one image reset to the same size (the fill alone), a full
  frame copy (writing every pixel once), and the writer's
  `MFCreateMemoryBuffer`, `Lock`, `Unlock` and `SetCurrentLength` with a new
  buffer every iteration against one buffer reused; both NV12 variants write
  every byte, as the conversion does. It prints one `MEASURE` line per
  operation with the median time and the page faults per operation, and
  checks only that sizes and calls are sound. The 3840 × 2160 lines give a 4K
  figure without a 4K display. Where Windows has no Media Foundation it
  measures the images and exits 77.
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
  `writer_submission_timing`, faults and CPU time per written frame and the
  process's peak private bytes since it started, then a `summary` line with
  the medians. The painting thread's own CPU time is printed apart. It checks that the
  recordings are sound and that at least 90 % of the attempts acquired a new
  picture. It runs in the lab by a Soak run or `lab.ps1 probe`; the host runs
  it only when the owner asks.

The lab figures decide. Reuse the buffers if reuse would save **at least
1 ms per frame at 1080p30 on Desktop Duplication**, or **at least 5 % of the
process CPU time per frame**, or if `pacing_missed` or `queue_full` would
drop. Otherwise record the figures here and close the question.

### Host figures

`frame_buffer_cost` on the development PC (AMD Ryzen 7 5800X3D, 16 logical
processors, 64 GB, Windows 10 22H2, Release build, run alone), 2026-11-09.
Median of 200 iterations; faults are page faults per operation.

| Operation | 1920 × 1080 | Faults | 3840 × 2160 | Faults |
|---|---|---|---|---|
| `BgraImage::Reset` on a new image (allocate, fill, release) | 1,503 µs | 2,030 | 6,620 µs | 8,117 |
| `BgraImage::Reset` on the same image (fill only) | 304 µs | 0 | 1,189 µs | 0 |
| Full frame copy into that image | 300 µs | 0 | 1,192 µs | 0 |
| New NV12 buffer: create, `Lock`, write, `Unlock`, release | 422 µs | 762 | 1,489 µs | 3,044 |
| The same NV12 buffer: `Lock`, write, `Unlock` | 125 µs | 0 | 443 µs | 0 |

Read on this machine: a new picture costs about 1.2 ms more than a reused one
at 1080p and 5.4 ms at 2160p, nearly all of it one page fault per 4 KiB page;
a new NV12 buffer costs about 0.3 ms and 1.0 ms more. The opaque fill that the
row copy overwrites costs as much as the copy itself. An earlier run of the
same build gave 1,359 µs and 462 µs for the new picture and the new buffer at
1080p, so single figures vary by about 10 %. These are the costs of the
operations alone, on a fast desktop processor; they do not say what reuse
would save inside a recording, where the encoder runs on other threads at the
same time. That is what the lab figures below answer.

### Lab figures

To be filled from the lab Soak or probe run of `recording_frame_cost` (the
lab's 1920 × 1080 display at 125 %) and of `frame_buffer_cost`.

### The memory bound

`bounded_frame_payload_bytes` in the session diagnostics counted the queue's
capacity plus two BGRA pictures and one NV12 buffer: five pictures with the
default queue of three. Up to seven can be alive at once when the content
changes and the consumer is behind: three in the queue; two held by the
capture worker (`kCaptureWorkerHeldFrames`: the latest picture, kept for reuse
even after the full queue rejected it, and the new one being captured while
the latest is still held); and two held by `ConsumerMain` while it writes (the
pending frame and the frame just taken from the queue, whose timestamp sets
the pending frame's duration). The bound now counts seven pictures.
`recording_worker_foundations` checks the worker's two against the real
worker: with nothing taken from the queue, the peak number of pictures alive
is the queue's capacity plus two.

The NV12 figure stays one buffer, the one the writer converts into. Samples
Media Foundation keeps after `WriteSample` (the sink writer's queue and the
encoder's own, limited by the sink writer's throttling) are not Easnap's to
bound and are not counted; a hardware encoder may hold several. The
provider's own two region-sized buffers, the persistent Direct3D staging
texture and the temporary GDI section of the fallback, are not frame payload
and are not counted either.

## Candidate fix, if the figures call for one

From the 2026-11-09 research of this question; nothing of it is implemented.

- **A pool of BGRA pictures owned by the capture worker.** Sized at Start for
  the region and capped at the bound above; it hands out
  `shared_ptr<const BgraImage>` whose deleter returns the picture to a
  mutex-guarded free list in pool state the `shared_ptr`s share, so a picture
  released after Stop or `Release` is still safe. It allocates when empty and
  never blocks. The worker calls `CaptureRegionImage` into a pooled picture
  instead of `CaptureRegionFrame`, and `AcquireFrame` reads into a
  provider-owned scratch picture next to the staging texture and swaps it into
  the output on success, so "no new frame leaves the output untouched" holds.
  Pictures go back only through the deleter, never by `use_count`.
- **A Reset that resizes without filling,** used only where every pixel is
  overwritten (the full-texture and region readbacks and the GDI blit), while
  the opaque-black Reset stays for the cross-monitor canvas and the cropped
  snapshot. `MakeOpaque` can fold into the row copy.
- **Tracked NV12 samples.** In `CoreWriteVideoFrame`, a free list of
  `MFCreateTrackedSample` samples, each with one `MFCreateMemoryBuffer` of the
  NV12 size, returned by a separate reference-counted callback object given
  with `SetAllocator` before each `WriteSample`; the same contiguous buffer,
  `SetCurrentLength`, sample time and duration, no attributes. Not
  `MFCreateVideoSampleAllocatorEx`, whose 2D buffers have a pitch.
- **Unchanged:** `CoreWriteVideoSample` (the native NV12 trim path), the audio
  path, and the trim fallback writer's sample timing
  (`mp4_trim_export_backend` and `media_foundation_h264_aac_mp4` keep their
  `stts` entries and durations).

## Consequences

- Before and after a fix are two lab runs of the same two tests on the same
  image.
- The bound is 61,171,200 bytes at 1920 × 1080 and 244,684,800 bytes at
  3840 × 2160 (seven pictures and one NV12 buffer). Today that peak is reached
  only while the consumer is behind and is freed between frames; a pool full
  to its cap would hold it for the whole recording.
- A hardware encoder may keep its input samples after `WriteSample`. The lab has
  only the software encoder, so tracked samples would need a run on a physical
  GPU as well.
- The test window's own painting is taken out of the CPU figure; the
  composition it causes runs in the Desktop Window Manager, outside the
  process.

## Not validated

- `recording_frame_cost` was built and registered but not run: it needs the
  visible desktop, so it runs in the lab.
- No lab figure exists yet, so the threshold has not been applied.
