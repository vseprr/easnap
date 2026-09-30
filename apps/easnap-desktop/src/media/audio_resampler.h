// Easnap - Windows Audio Resampler DSP wrapper used by dual-source capture.
#pragma once

#include <cstdint>
#include <vector>

#include <mftransform.h>
#include <wrl/client.h>

#include "media/audio_format.h"

namespace easnap::media::detail {

/// Stateful, high-quality Windows Audio Resampler DSP conversion. Instances
/// are initialized, used, flushed, and destroyed on one COM-initialized worker.
class AudioResampler final {
public:
    AudioResampler() = default;
    ~AudioResampler();

    AudioResampler(const AudioResampler&) = delete;
    AudioResampler& operator=(const AudioResampler&) = delete;

    [[nodiscard]] bool Initialize(const AudioFormat& input,
                                  const AudioFormat& output) noexcept;
    [[nodiscard]] bool Convert(const AudioPacket& input,
                               std::vector<AudioPacket>& output) noexcept;
    [[nodiscard]] bool Drain(std::vector<AudioPacket>& output) noexcept;
    void Flush() noexcept;

    [[nodiscard]] long last_hresult() const noexcept { return last_hresult_; }

private:
    [[nodiscard]] bool Pull(std::vector<AudioPacket>& output,
                            bool draining) noexcept;

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    AudioFormat input_format_{};
    AudioFormat output_format_{};
    MediaTime next_output_timestamp_{0};
    AudioTimestampSource output_timestamp_source_{
        AudioTimestampSource::CaptureQpcFallback};
    bool has_next_output_timestamp_{false};
    bool discontinuity_pending_{false};
    bool mf_started_{false};
    long last_hresult_{0};
};

}  // namespace easnap::media::detail
