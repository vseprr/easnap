#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "media/audio_resampler.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expression);
    }
}
#define CHECK(expression) Check((expression), #expression, __FILE__, __LINE__)

easnap::media::AudioPacket MonoPacket(uint32_t packet_index) {
    constexpr uint32_t frames = 441;
    constexpr double pi = 3.14159265358979323846;
    easnap::media::AudioPacket packet;
    packet.format = {44'100, 1, 16, false, 0x4};
    packet.frame_count = frames;
    packet.timestamp = easnap::media::MediaTime{
        static_cast<int64_t>(packet_index) * 100'000};
    packet.device_position = static_cast<uint64_t>(packet_index) * frames;
    packet.data.resize(frames * sizeof(int16_t));
    std::vector<int16_t> samples(frames);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const uint64_t absolute =
            static_cast<uint64_t>(packet_index) * frames + frame;
        samples[frame] = static_cast<int16_t>(
            std::sin(2.0 * pi * 440.0 * static_cast<double>(absolute) /
                     44'100.0) *
            12'000.0);
    }
    std::memcpy(packet.data.data(), samples.data(), packet.data.size());
    return packet;
}

}  // namespace

int main() {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CHECK(SUCCEEDED(com));
    if (FAILED(com)) {
        return 1;
    }

    easnap::media::detail::AudioResampler converter;
    const easnap::media::AudioFormat input{44'100, 1, 16, false, 0x4};
    const easnap::media::AudioFormat output{48'000, 2, 32, true, 0x3};
    CHECK(converter.Initialize(input, output));

    std::vector<easnap::media::AudioPacket> all_output;
    for (uint32_t index = 0; index < 100; ++index) {
        std::vector<easnap::media::AudioPacket> converted;
        CHECK(converter.Convert(MonoPacket(index), converted));
        for (auto& packet : converted) {
            all_output.push_back(std::move(packet));
        }
    }
    std::vector<easnap::media::AudioPacket> tail;
    CHECK(converter.Drain(tail));
    for (auto& packet : tail) {
        all_output.push_back(std::move(packet));
    }

    CHECK(!all_output.empty());
    uint64_t total_frames = 0;
    easnap::media::MediaTime previous{0};
    bool nonzero = false;
    bool stereo_matches = true;
    for (const auto& packet : all_output) {
        CHECK(packet.consistent());
        CHECK(packet.format.sample_rate == 48'000);
        CHECK(packet.format.channels == 2);
        CHECK(packet.timestamp >= previous);
        previous = packet.timestamp;
        total_frames += packet.frame_count;
        std::vector<float> samples(packet.data.size() / sizeof(float));
        std::memcpy(samples.data(), packet.data.data(), packet.data.size());
        for (std::size_t index = 0; index + 1 < samples.size(); index += 2) {
            nonzero = nonzero || std::abs(samples[index]) > 0.001F;
            stereo_matches = stereo_matches &&
                             std::abs(samples[index] - samples[index + 1]) <
                                 0.0001F;
        }
    }
    CHECK(total_frames >= 47'900 && total_frames <= 48'100);
    CHECK(nonzero);
    CHECK(stereo_matches);

    converter.Flush();
    auto resumed = MonoPacket(200);
    resumed.flags = easnap::media::AudioPacketFlags::Discontinuity;
    std::vector<easnap::media::AudioPacket> resumed_output;
    CHECK(converter.Convert(resumed, resumed_output));
    CHECK(converter.Drain(tail));
    resumed_output.insert(resumed_output.end(), tail.begin(), tail.end());
    CHECK(!resumed_output.empty());
    CHECK(resumed_output.front().timestamp >=
          easnap::media::MediaTime{19'900'000});

    std::printf("audio_resampler_dsp: %d passed, %d failed\n", g_pass, g_fail);
    ::CoUninitialize();
    return g_fail == 0 ? 0 : 1;
}
