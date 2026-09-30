// Easnap - Windows-native video-only H.264 MP4 writer.
#pragma once

#include <mfapi.h>
#include <mfidl.h>

#include <memory>

#include "media/media_interfaces.h"

namespace easnap::media {

/// Creates a writer whose dedicated MTA thread owns Media Foundation startup,
/// colour conversion, the sink writer, WriteSample, Finalize, and shutdown.
std::unique_ptr<IMediaWriter> CreateMediaFoundationWriter();

/// Internal Media Foundation writer boundary used by trim export. It accepts
/// decoded NV12 samples directly, including DXGI-backed samples owned by the
/// shared device manager, avoiding BGRA materialization and CPU conversion.
class IMediaFoundationNv12Writer {
public:
    virtual ~IMediaFoundationNv12Writer() = default;
    virtual bool Open(const std::wstring& output_path,
                      const RecordingOptions& options,
                      const AudioFormat* audio_format,
                      IMFDXGIDeviceManager* device_manager) = 0;
    // Rebases the decoded sample's time/duration and submits that same sample.
    // The caller may release its reference on return; the sink retains the
    // sample (and its tracked decoder-pool lease) while encoding is in flight.
    virtual bool WriteVideoSample(IMFSample* sample, MediaTime timestamp,
                                  MediaTime duration) = 0;
    virtual bool WriteAudioPacket(const MediaChunk& packet) = 0;
    virtual bool Finalize(std::wstring& out_path) = 0;
    virtual void Abort() = 0;
    [[nodiscard]] virtual std::wstring last_error() const = 0;
};

std::unique_ptr<IMediaFoundationNv12Writer>
CreateMediaFoundationNv12Writer();

}  // namespace easnap::media
