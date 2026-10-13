// Easnap - region capture provider contracts.
//
// The capture layer only knows about desktop pixels: it never talks to the UI,
// never owns session state and never writes files. Concrete providers are chosen
// by capability probing (Desktop Duplication first, GDI as documented fallback),
// which keeps the Windows 11 / future-API work inside this file.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "media/media_interfaces.h"
#include "platform/platform_utils.h"

namespace easnap::capture {

using platform::BgraImage;

/// One monitor's worth of pixels together with its virtual-desktop placement.
struct MonitorFrame {
    RECT desktop_rect{};  ///< Position of `image` in virtual-screen coordinates.
    BgraImage image;
    /// False when the monitor could not be read and `image` is black.
    bool captured{true};
};

/// A frozen picture of every monitor, taken once when the overlay opens.
///
/// Freezing the desktop keeps the overlay itself out of the exported image and
/// lets the editor crop without a second capture round-trip.
struct DesktopSnapshot {
    RECT virtual_bounds{};
    std::vector<MonitorFrame> monitors;
    std::chrono::steady_clock::time_point timestamp{};

    [[nodiscard]] bool empty() const noexcept { return monitors.empty(); }
    void Clear();

    /// Composes `region` (virtual-screen coordinates) out of the captured
    /// monitor frames. Areas not covered by a monitor stay opaque black.
    [[nodiscard]] bool CropRegion(const RECT& region, BgraImage& out) const;
    /// True when every monitor that `region` touches was actually read. A
    /// picture of a region that is not complete has a black part.
    [[nodiscard]] bool RegionComplete(const RECT& region) const noexcept;
};

enum class RegionCaptureStatus { FrameAcquired, NoNewFrame, Failed };

/// Privacy-safe stage identifiers for native desktop-capture failures. The
/// associated code is retained separately so diagnostics never contain pixels,
/// window titles, paths, or other user content.
enum class RegionCaptureFailureStage : uint8_t {
    None,
    InvalidRegion,
    NoOverlappingMonitor,
    GraphicsInitialization,
    D3dDeviceCreation,
    DuplicateOutput,
    DuplicationOutputNotFound,
    DuplicationRotationUnsupported,
    DuplicationAcquire,
    DuplicationFrameTexture,
    DuplicationInvalidTexture,
    DuplicationStagingTexture,
    DuplicationMapTexture,
    DuplicationBlankFrame,
    GdiInvalidRegion,
    GdiGetScreenDc,
    GdiCreateMemoryDc,
    GdiCreateDibSection,
    GdiSelectBitmap,
    GdiBitBlt,
};

enum class RegionCaptureFailureCodeKind : uint8_t { None, HResult, Win32 };

struct RegionCaptureFailureDiagnostic {
    RegionCaptureFailureStage stage{RegionCaptureFailureStage::None};
    RegionCaptureFailureCodeKind code_kind{RegionCaptureFailureCodeKind::None};
    uint32_t native_code{0};
};

/// Aggregate-only timings for one production capture attempt. Values contain
/// no screen content and are expressed in microseconds.
struct RegionCaptureMetrics {
    uint64_t acquisition_microseconds{0};
    uint64_t region_copy_microseconds{0};
};

struct RegionCaptureResult {
    RegionCaptureStatus status{RegionCaptureStatus::Failed};
    RegionCaptureMetrics metrics;
    RegionCaptureFailureDiagnostic primary_failure;
    RegionCaptureFailureDiagnostic fallback_failure;
};

[[nodiscard]] const wchar_t* RegionCaptureFailureStageName(
    RegionCaptureFailureStage stage) noexcept;

/// How one region is captured as a plain image.
struct RegionImageOptions {
    bool include_cursor{false};
    /// How long to wait for the desktop to change before answering
    /// `NoNewFrame`. Only a region on one duplicated output can wait; every
    /// other path answers at once with the current pixels.
    uint32_t wait_milliseconds{0};
    /// A monitor's part that cannot be read fails the capture instead of
    /// staying black in an otherwise successful image.
    bool require_every_output{false};
    /// A duplication that has not been handed a composed frame yet returns a
    /// black surface. With this set, such a picture is read through GDI and
    /// duplication is asked again next time, instead of the output being
    /// given up for the life of the provider. For a capture that starts on a
    /// desktop that is standing still.
    bool await_first_composed_frame{false};
};

/// Captures bounded desktop regions synchronously. Recording calls this only
/// from its dedicated worker and transports results through a bounded queue.
class IRegionCaptureProvider {
public:
    virtual ~IRegionCaptureProvider() = default;

    /// Captures every monitor once.
    virtual bool CaptureDesktop(bool include_cursor, DesktopSnapshot& out) = 0;

    /// Captures a single region in virtual-screen coordinates.
    virtual bool CaptureRegion(const RECT& region, bool include_cursor, BgraImage& out) = 0;

    /// Captures one owned frame for a fixed recording region. The caller owns
    /// timeline generation and supplies the session-relative timestamp.
    virtual RegionCaptureResult CaptureRegionFrame(
        const RECT& region, bool include_cursor, media::MediaTime timestamp,
        media::MediaTime duration, media::VideoFrame& out) = 0;

    /// Captures one region as a plain image through the same per-output path.
    /// `NoNewFrame` leaves `out` untouched.
    virtual RegionCaptureResult CaptureRegionImage(
        const RECT& region, const RegionImageOptions& options, BgraImage& out) = 0;

    /// Releases duplication interfaces and GPU resources; safe to call any time.
    virtual void Release() noexcept = 0;

    /// Diagnostic name of the active strategy (never contains user data).
    [[nodiscard]] virtual const wchar_t* name() const = 0;
};

std::unique_ptr<IRegionCaptureProvider> CreateRegionCaptureProvider();

/// Draws the live mouse cursor into `image`, which maps to `image_rect` on the
/// virtual desktop. Exposed for reuse by the Phase 2 recording pipeline.
bool CompositeCursor(BgraImage& image, const RECT& image_rect);

}  // namespace easnap::capture
