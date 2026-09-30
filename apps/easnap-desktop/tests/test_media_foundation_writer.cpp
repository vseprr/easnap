#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "capture/recording_capture_worker.h"
#include "media/media_foundation_writer.h"
#include "media/temp_session.h"

namespace {

using Microsoft::WRL::ComPtr;

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

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

class MediaFoundationRuntime final {
public:
    MediaFoundationRuntime() {
        const HRESULT com_result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_initialized_ = SUCCEEDED(com_result);
        result_ = com_initialized_ ? ::MFStartup(MF_VERSION, MFSTARTUP_FULL) : com_result;
        mf_started_ = SUCCEEDED(result_);
    }

    ~MediaFoundationRuntime() {
        if (mf_started_) {
            static_cast<void>(::MFShutdown());
        }
        if (com_initialized_) {
            ::CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_{E_FAIL};
    bool com_initialized_{false};
    bool mf_started_{false};
};

void FillMovingPattern(easnap::platform::BgraImage& image, uint32_t frame_index) {
    for (int y = 0; y < image.height; ++y) {
        uint32_t* const row = image.row(y);
        for (int x = 0; x < image.width; ++x) {
            const uint32_t red = static_cast<uint32_t>((x + frame_index * 3) & 0xFF);
            const uint32_t green = static_cast<uint32_t>((y * 2 + frame_index * 5) & 0xFF);
            const uint32_t blue = static_cast<uint32_t>((x + y + frame_index * 7) & 0xFF);
            row[x] = 0xFF000000u | (red << 16) | (green << 8) | blue;
        }
    }
}

bool ValidateMp4Metadata(const std::wstring& path, UINT32 expected_width,
                         UINT32 expected_height, int64_t expected_duration,
                         int64_t expected_audio_start,
                         uint64_t& reported_duration) {
    ComPtr<IMFSourceReader> reader;
    HRESULT result = ::MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    CHECK(SUCCEEDED(result));
    if (FAILED(result)) {
        return false;
    }

    ComPtr<IMFMediaType> native_type;
    result = reader->GetNativeMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native_type);
    CHECK(SUCCEEDED(result));
    if (FAILED(result)) {
        return false;
    }

    GUID major_type{};
    GUID subtype{};
    UINT32 width = 0;
    UINT32 height = 0;
    result = native_type->GetGUID(MF_MT_MAJOR_TYPE, &major_type);
    CHECK(SUCCEEDED(result));
    result = SUCCEEDED(result) ? native_type->GetGUID(MF_MT_SUBTYPE, &subtype) : result;
    CHECK(SUCCEEDED(result));
    result = SUCCEEDED(result) ? ::MFGetAttributeSize(native_type.Get(), MF_MT_FRAME_SIZE, &width, &height)
                               : result;
    CHECK(SUCCEEDED(result));
    CHECK(major_type == MFMediaType_Video);
    CHECK(subtype == MFVideoFormat_H264);
    CHECK(width == expected_width);
    CHECK(height == expected_height);

    ComPtr<IMFMediaType> native_audio_type;
    result = reader->GetNativeMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
        &native_audio_type);
    CHECK(SUCCEEDED(result));
    if (FAILED(result)) {
        return false;
    }
    GUID audio_major_type{};
    GUID audio_subtype{};
    UINT32 audio_sample_rate = 0;
    UINT32 audio_channels = 0;
    result = native_audio_type->GetGUID(MF_MT_MAJOR_TYPE, &audio_major_type);
    result = SUCCEEDED(result)
                 ? native_audio_type->GetGUID(MF_MT_SUBTYPE, &audio_subtype)
                 : result;
    result = SUCCEEDED(result)
                 ? native_audio_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                                &audio_sample_rate)
                 : result;
    result = SUCCEEDED(result)
                 ? native_audio_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                                                &audio_channels)
                 : result;
    CHECK(SUCCEEDED(result));
    CHECK(audio_major_type == MFMediaType_Audio);
    CHECK(audio_subtype == MFAudioFormat_AAC);
    CHECK(audio_sample_rate == 48'000);
    CHECK(audio_channels == 2);

    ComPtr<IMFSourceReader> native_audio_reader;
    result = ::MFCreateSourceReaderFromURL(path.c_str(), nullptr,
                                           &native_audio_reader);
    CHECK(SUCCEEDED(result));
    LONGLONG first_aac_time = -1;
    LONGLONG previous_aac_time = -1;
    bool native_aac_timestamps_monotonic = true;
    bool native_aac_durations_valid = true;
    for (int read_count = 0; SUCCEEDED(result) && read_count < 16;
         ++read_count) {
        DWORD audio_flags = 0;
        LONGLONG audio_time = 0;
        ComPtr<IMFSample> audio_sample;
        result = native_audio_reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
            nullptr, &audio_flags, &audio_time, &audio_sample);
        if (FAILED(result) || (audio_flags & MF_SOURCE_READERF_ERROR) != 0) {
            native_aac_timestamps_monotonic = false;
            break;
        }
        if ((audio_flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if (audio_sample == nullptr) {
            continue;
        }
        if (audio_time < 0 ||
            (previous_aac_time >= 0 && audio_time <= previous_aac_time)) {
            native_aac_timestamps_monotonic = false;
        }
        LONGLONG audio_duration = 0;
        if (FAILED(audio_sample->GetSampleDuration(&audio_duration)) ||
            audio_duration <= 0) {
            native_aac_durations_valid = false;
        }
        previous_aac_time = audio_time;
        if (first_aac_time < 0) {
            first_aac_time = audio_time;
        }
    }
    constexpr int64_t kTwoAacFrames100ns = 426'667;
    CHECK(native_aac_timestamps_monotonic);
    CHECK(native_aac_durations_valid);
    // The MP4 sink may encode explicit silence from stream ticks beginning at
    // zero. Content placement is therefore verified from decoded PCM below;
    // the first native AAC PTS remains useful timeline diagnostics.
    CHECK(first_aac_time >= 0);
    CHECK(first_aac_time <= expected_audio_start);

    PROPVARIANT duration;
    ::PropVariantInit(&duration);
    result = reader->GetPresentationAttribute(
        static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration);
    CHECK(SUCCEEDED(result));
    if (SUCCEEDED(result) && duration.vt == VT_UI8) {
        reported_duration = duration.uhVal.QuadPart;
    } else if (SUCCEEDED(result) && duration.vt == VT_I8 && duration.hVal.QuadPart >= 0) {
        reported_duration = static_cast<uint64_t>(duration.hVal.QuadPart);
    } else {
        CHECK(false);
    }
    ::PropVariantClear(&duration);

    const int64_t one_frame = 10'000'000 / 30;
    CHECK(reported_duration >= static_cast<uint64_t>(expected_duration - one_frame));
    CHECK(reported_duration <= static_cast<uint64_t>(expected_duration + one_frame));

    DWORD stream_flags = 0;
    LONGLONG sample_time = 0;
    ComPtr<IMFSample> sample;
    result = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr,
                                &stream_flags, &sample_time, &sample);
    CHECK(SUCCEEDED(result));
    CHECK((stream_flags & MF_SOURCE_READERF_ERROR) == 0);
    CHECK(sample != nullptr);
    CHECK(sample_time >= 0);

    stream_flags = 0;
    sample_time = 0;
    sample.Reset();
    ComPtr<IMFMediaType> decoded_audio_type;
    result = ::MFCreateMediaType(&decoded_audio_type);
    if (SUCCEEDED(result)) {
        result = decoded_audio_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    }
    if (SUCCEEDED(result)) {
        result = decoded_audio_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    }
    if (SUCCEEDED(result)) {
        result = decoded_audio_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    }
    if (SUCCEEDED(result)) {
        result = decoded_audio_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                               48'000);
    }
    if (SUCCEEDED(result)) {
        result = decoded_audio_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    }
    if (SUCCEEDED(result)) {
        result = reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr,
            decoded_audio_type.Get());
    }
    CHECK(SUCCEEDED(result));
    double decoded_square_sum = 0.0;
    std::size_t decoded_sample_count = 0;
    bool reached_audible_sample = false;
    LONGLONG first_audible_time = -1;
    for (int read_count = 0; read_count < 512 && !reached_audible_sample;
         ++read_count) {
        stream_flags = 0;
        sample_time = 0;
        sample.Reset();
        result = reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
            nullptr, &stream_flags, &sample_time, &sample);
        CHECK(SUCCEEDED(result));
        CHECK((stream_flags & MF_SOURCE_READERF_ERROR) == 0);
        if (FAILED(result) || (stream_flags & MF_SOURCE_READERF_ERROR) != 0 ||
            (stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if (sample == nullptr) {
            continue;
        }
        CHECK(sample_time >= 0);
        ComPtr<IMFMediaBuffer> decoded_buffer;
        result = sample->ConvertToContiguousBuffer(&decoded_buffer);
        BYTE* decoded_bytes = nullptr;
        DWORD decoded_byte_count = 0;
        if (SUCCEEDED(result)) {
            result = decoded_buffer->Lock(&decoded_bytes, nullptr,
                                          &decoded_byte_count);
        }
        if (SUCCEEDED(result) && decoded_bytes != nullptr &&
            decoded_byte_count % sizeof(float) == 0) {
            const float* decoded_samples =
                reinterpret_cast<const float*>(decoded_bytes);
            const std::size_t sample_count =
                decoded_byte_count / sizeof(float);
            double packet_square_sum = 0.0;
            for (std::size_t index = 0; index < sample_count; ++index) {
                const double value = decoded_samples[index];
                decoded_square_sum += value * value;
                packet_square_sum += value * value;
            }
            decoded_sample_count += sample_count;
            if (sample_count > 0 &&
                std::sqrt(packet_square_sum / sample_count) > 0.01) {
                reached_audible_sample = true;
                first_audible_time = sample_time;
            }
        }
        if (decoded_buffer != nullptr && decoded_bytes != nullptr) {
            const HRESULT unlock_result = decoded_buffer->Unlock();
            CHECK(SUCCEEDED(unlock_result));
        }
        CHECK(SUCCEEDED(result));
    }
    CHECK(decoded_sample_count > 0);
    const double decoded_rms =
        decoded_sample_count > 0
            ? std::sqrt(decoded_square_sum / decoded_sample_count)
            : 0.0;
    CHECK(decoded_rms > 0.01);
    CHECK(reached_audible_sample);
    CHECK(first_audible_time >= expected_audio_start - kTwoAacFrames100ns);
    CHECK(first_audible_time <= expected_audio_start + kTwoAacFrames100ns);
    std::wprintf(L"AUDIO_TIMELINE first_aac_100ns=%lld "
                 L"first_audible_100ns=%lld expected_100ns=%lld\n",
                 first_aac_time, first_audible_time, expected_audio_start);
    return SUCCEEDED(result) && reached_audible_sample &&
           major_type == MFMediaType_Video && subtype == MFVideoFormat_H264 &&
           width == expected_width && height == expected_height &&
           audio_major_type == MFMediaType_Audio &&
           audio_subtype == MFAudioFormat_AAC && audio_sample_rate == 48'000 &&
           audio_channels == 2;
}

void TestRealH264AacMp4Writer() {
    constexpr UINT32 kWidth = 320;
    constexpr UINT32 kHeight = 180;
    constexpr uint32_t kFrameRate = 30;
    constexpr uint64_t kFrameCount = 150;
    constexpr int64_t kAudioStart100ns = 25'000'000;

    MediaFoundationRuntime inspection_runtime;
    CHECK(SUCCEEDED(inspection_runtime.result()));
    if (FAILED(inspection_runtime.result())) {
        return;
    }

    auto session_store = easnap::media::CreateTemporarySessionStore();
    CHECK(session_store != nullptr);
    if (session_store == nullptr) {
        return;
    }
    const auto session = session_store->CreateSession();
    CHECK(session.has_value());
    if (!session.has_value()) {
        std::fwprintf(stderr, L"Temporary session creation failed under: %ls\n",
                      session_store->root_directory().c_str());
        return;
    }
    const std::wstring output_path = session->recording_path();

    auto writer = easnap::media::CreateMediaFoundationWriter();
    CHECK(writer != nullptr);
    if (writer == nullptr) {
        return;
    }

    easnap::media::RecordingOptions options;
    options.region = RECT{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    options.format = easnap::media::RecordingFormat::Mp4;
    // The writer receives one already-mixed PCM stream for Both and must create
    // one AAC track, exactly as for either single-source option.
    options.audio = easnap::media::AudioSource::Both;
    options.target_frame_rate = kFrameRate;
    options.video_bitrate_bps = 2'000'000;

    easnap::media::AudioFormat audio_format;
    audio_format.sample_rate = 48'000;
    audio_format.channels = 2;
    audio_format.bits_per_sample = 32;
    audio_format.is_float = true;
    audio_format.channel_mask = 0x3;

    const bool opened = writer->Open(output_path, options, &audio_format);
    if (!opened) {
        std::fwprintf(stderr, L"Writer Open error: %ls\n", writer->last_error().c_str());
    }
    CHECK(opened);
    if (!opened) {
        return;
    }
    // Write continuous silence from t=0 to kAudioStart100ns in bounded
    // 1024-frame chunks, matching the production continuous-silence model.
    {
        constexpr uint32_t kSilenceFrames = 1024;
        const std::size_t silence_bytes =
            static_cast<std::size_t>(kSilenceFrames) * audio_format.block_align();
        std::vector<uint8_t> silence(silence_bytes, 0);
        int64_t silence_pos = 0;
        bool silence_ok = true;
        while (silence_pos < kAudioStart100ns) {
            const int64_t remaining = kAudioStart100ns - silence_pos;
            const uint64_t remaining_frames =
                (static_cast<uint64_t>(remaining) * audio_format.sample_rate) /
                10'000'000ULL;
            if (remaining_frames == 0) break;
            const uint32_t chunk_frames = static_cast<uint32_t>(
                (std::min)(static_cast<uint64_t>(kSilenceFrames),
                           remaining_frames));
            const int64_t chunk_dur = static_cast<int64_t>(
                (static_cast<uint64_t>(chunk_frames) * 10'000'000ULL) /
                audio_format.sample_rate);
            if (chunk_dur <= 0) break;
            easnap::media::MediaChunk chunk;
            chunk.data = silence.data();
            chunk.size =
                static_cast<std::size_t>(chunk_frames) * audio_format.block_align();
            chunk.timestamp = easnap::media::MediaTime{silence_pos};
            chunk.duration = easnap::media::MediaTime{chunk_dur};
            chunk.discontinuity = (silence_pos == 0);
            if (!writer->WriteAudioPacket(chunk)) {
                std::fwprintf(stderr, L"Silence write error: %ls\n",
                              writer->last_error().c_str());
                silence_ok = false;
                break;
            }
            silence_pos += chunk_dur;
        }
        CHECK(silence_ok);
    }

    easnap::media::VideoFrame frame;
    auto image = std::make_shared<easnap::platform::BgraImage>();
    image->Reset(static_cast<int>(kWidth), static_cast<int>(kHeight));
    frame.image = image;
    bool all_frames_written = true;
    for (uint64_t index = 0; index < kFrameCount; ++index) {
        FillMovingPattern(*image, static_cast<uint32_t>(index));
        frame.timestamp = easnap::capture::MediaTimestampForFrame(index, kFrameRate);
        frame.duration = easnap::capture::MediaTimestampForFrame(index + 1, kFrameRate) -
                         frame.timestamp;
        if (!writer->WriteVideoFrame(frame)) {
            std::fwprintf(stderr, L"WriteVideoFrame error: %ls\n", writer->last_error().c_str());
            all_frames_written = false;
            break;
        }
    }
    CHECK(all_frames_written);

    constexpr uint32_t kAudioFramesPerPacket = 480;
    constexpr uint64_t kAudioPacketCount = 150;
    constexpr double kPi = 3.14159265358979323846;
    std::vector<float> audio_samples(
        static_cast<std::size_t>(kAudioFramesPerPacket) * audio_format.channels);
    bool all_audio_written = true;
    for (uint64_t packet_index = 0; packet_index < kAudioPacketCount;
         ++packet_index) {
        for (uint32_t frame_index = 0; frame_index < kAudioFramesPerPacket;
             ++frame_index) {
            const uint64_t absolute_frame =
                packet_index * kAudioFramesPerPacket + frame_index;
            const float sample = static_cast<float>(
                0.2 * std::sin(2.0 * kPi * 440.0 *
                               static_cast<double>(absolute_frame) /
                               audio_format.sample_rate));
            const std::size_t sample_index =
                static_cast<std::size_t>(frame_index) * audio_format.channels;
            audio_samples[sample_index] = sample;
            audio_samples[sample_index + 1] = sample;
        }
        easnap::media::MediaChunk packet;
        packet.data = reinterpret_cast<const uint8_t*>(audio_samples.data());
        packet.size = audio_samples.size() * sizeof(float);
        packet.timestamp = easnap::media::MediaTime{
            kAudioStart100ns +
            static_cast<int64_t>(packet_index * 100'000)};
        packet.duration = easnap::media::MediaTime{100'000};
        if (!writer->WriteAudioPacket(packet)) {
            std::fwprintf(stderr, L"WriteAudioPacket error: %ls\n",
                          writer->last_error().c_str());
            all_audio_written = false;
            break;
        }
    }
    CHECK(all_audio_written);

    std::wstring finalized_path;
    const bool finalized = writer->Finalize(finalized_path);
    if (!finalized) {
        std::fwprintf(stderr, L"Finalize error: %ls\n", writer->last_error().c_str());
    }
    CHECK(finalized);
    CHECK(finalized_path == output_path);
    writer.reset();

    WIN32_FILE_ATTRIBUTE_DATA file_data{};
    const bool file_exists = ::GetFileAttributesExW(output_path.c_str(), GetFileExInfoStandard,
                                                     &file_data) != FALSE;
    CHECK(file_exists);
    const uint64_t file_size = (static_cast<uint64_t>(file_data.nFileSizeHigh) << 32) |
                               file_data.nFileSizeLow;
    CHECK(file_size > 0);

    const int64_t expected_duration =
        easnap::capture::MediaTimestampForFrame(kFrameCount, kFrameRate).count();
    uint64_t reported_duration = 0;
    const bool metadata_valid = ValidateMp4Metadata(output_path, kWidth, kHeight,
                                                     expected_duration,
                                                     kAudioStart100ns,
                                                     reported_duration);
    CHECK(metadata_valid);

    std::wprintf(L"MP4_VALIDATION path=%ls bytes=%llu codec=H264+AAC dimensions=%ux%u "
                 L"video_frames=%llu audio_packets=%llu duration_100ns=%llu\n",
                 output_path.c_str(), static_cast<unsigned long long>(file_size),
                 kWidth, kHeight, static_cast<unsigned long long>(kFrameCount),
                 static_cast<unsigned long long>(kAudioPacketCount),
                 static_cast<unsigned long long>(reported_duration));

    CHECK(session_store->DeleteSession(*session));
}

// ---------------------------------------------------------------------------
// STTS regression: verify no abnormally long AAC sample duration in the MP4.
// Writes 2.5 seconds of silence then 1.5 seconds of real audio, finalizes,
// and inspects the raw STTS box for the audio track.
// ---------------------------------------------------------------------------

struct SttsEntry {
    uint32_t sample_count;
    uint32_t sample_delta;
};

// Parse the STTS box for a given track from a raw MP4 file.
// Returns true if found, populating entries and timescale.
bool ParseAudioStts(const std::wstring& path,
                    std::vector<SttsEntry>& entries,
                    uint32_t& timescale) {
    entries.clear();
    timescale = 0;

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER file_size{};
    ::GetFileSizeEx(file, &file_size);
    const std::size_t size = static_cast<std::size_t>(file_size.QuadPart);
    std::vector<uint8_t> data(size);
    DWORD bytes_read = 0;
    ::ReadFile(file, data.data(), static_cast<DWORD>(size), &bytes_read, nullptr);
    ::CloseHandle(file);
    if (bytes_read != size) {
        return false;
    }

    // Big-endian 32-bit reader.
    auto read32 = [&](std::size_t offset) -> uint32_t {
        return (static_cast<uint32_t>(data[offset]) << 24) |
               (static_cast<uint32_t>(data[offset + 1]) << 16) |
               (static_cast<uint32_t>(data[offset + 2]) << 8) |
               static_cast<uint32_t>(data[offset + 3]);
    };

    // Linear scan: track whether we are inside a 'soun' handler track.
    // When we see 'hdlr' with handler_type 'soun', set is_audio_track.
    // When we see 'mdhd', record timescale.
    // When we see 'stts' while is_audio_track is true, parse it.
    // Reset is_audio_track and timescale at each new 'trak'.
    bool is_audio_track = false;
    std::size_t pos = 0;
    while (pos + 8 <= size) {
        const uint32_t atom_size = read32(pos);
        const uint32_t atom_type = read32(pos + 4);
        if (atom_size < 8 || pos + atom_size > size) {
            break;
        }
        // Container atoms: descend into children.
        const bool container =
            atom_type == 0x6D6F6F76 ||  // moov
            atom_type == 0x6D646961 ||  // mdia
            atom_type == 0x6D696E66 ||  // minf
            atom_type == 0x7374626C;    // stbl
        // 'trak' is also a container but resets track state.
        if (atom_type == 0x7472616B) {  // trak
            is_audio_track = false;
            timescale = 0;
            pos += 8;
            continue;
        }
        if (container) {
            pos += 8;
            continue;
        }
        // 'hdlr' — handler reference box: handler_type at offset 16.
        // handler_type 'soun' (0x736F756E) = audio track.
        if (atom_type == 0x68646C72 && atom_size >= 20) {  // hdlr
            const uint32_t handler_type = read32(pos + 16);
            is_audio_track = (handler_type == 0x736F756E);  // 'soun'
        }
        // 'mdhd' — media header, contains timescale.
        if (atom_type == 0x6D646864 && atom_size >= 24) {
            const uint8_t version = data[pos + 8];
            if (version == 0 && atom_size >= 32) {
                timescale = read32(pos + 20);
            } else if (version == 1 && atom_size >= 44) {
                timescale = read32(pos + 28);
            }
        }
        // 'stts' — sample-to-time box.
        if (atom_type == 0x73747473 && atom_size >= 16 && is_audio_track &&
            timescale > 0) {
            const uint32_t entry_count = read32(pos + 12);
            const std::size_t entries_start = pos + 16;
            if (entries_start + entry_count * 8 <= pos + atom_size) {
                entries.reserve(entry_count);
                for (uint32_t i = 0; i < entry_count; ++i) {
                    SttsEntry e;
                    e.sample_count = read32(entries_start + i * 8);
                    e.sample_delta = read32(entries_start + i * 8 + 4);
                    entries.push_back(e);
                }
                return true;
            }
        }
        pos += atom_size;
    }
    return false;
}

void TestDelayedAudioSttsContinuity() {
    constexpr UINT32 kWidth = 160;
    constexpr UINT32 kHeight = 90;
    constexpr uint32_t kFrameRate = 30;
    constexpr uint64_t kFrameCount = 90;  // 3 seconds
    // Audio starts at 1.5 seconds — the gap exceeds the 750ms interleave lag.
    constexpr int64_t kAudioStart100ns = 15'000'000;

    MediaFoundationRuntime inspection_runtime;
    CHECK(SUCCEEDED(inspection_runtime.result()));
    if (FAILED(inspection_runtime.result())) {
        return;
    }

    auto session_store = easnap::media::CreateTemporarySessionStore();
    CHECK(session_store != nullptr);
    if (session_store == nullptr) {
        return;
    }
    const auto session = session_store->CreateSession();
    CHECK(session.has_value());
    if (!session.has_value()) {
        return;
    }
    const std::wstring output_path = session->recording_path();

    auto writer = easnap::media::CreateMediaFoundationWriter();
    CHECK(writer != nullptr);
    if (writer == nullptr) {
        return;
    }

    easnap::media::RecordingOptions options;
    options.region = RECT{0, 0, static_cast<LONG>(kWidth),
                          static_cast<LONG>(kHeight)};
    options.format = easnap::media::RecordingFormat::Mp4;
    options.audio = easnap::media::AudioSource::SystemAudio;
    options.target_frame_rate = kFrameRate;
    options.video_bitrate_bps = 1'000'000;

    easnap::media::AudioFormat audio_format;
    audio_format.sample_rate = 44'100;
    audio_format.channels = 2;
    audio_format.bits_per_sample = 16;
    audio_format.is_float = false;
    audio_format.channel_mask = 0x3;

    const bool opened = writer->Open(output_path, options, &audio_format);
    if (!opened) {
        std::fwprintf(stderr, L"STTS test Open error: %ls\n",
                      writer->last_error().c_str());
    }
    CHECK(opened);
    if (!opened) {
        return;
    }

    // Write continuous silence from t=0 to kAudioStart100ns.
    {
        constexpr uint32_t kSilenceFrames = 1024;
        const std::size_t silence_bytes =
            static_cast<std::size_t>(kSilenceFrames) *
            audio_format.block_align();
        std::vector<uint8_t> silence(silence_bytes, 0);
        int64_t silence_pos = 0;
        bool silence_ok = true;
        while (silence_pos < kAudioStart100ns) {
            const int64_t remaining = kAudioStart100ns - silence_pos;
            const uint64_t remaining_frames =
                (static_cast<uint64_t>(remaining) * audio_format.sample_rate) /
                10'000'000ULL;
            if (remaining_frames == 0) break;
            const uint32_t chunk_frames = static_cast<uint32_t>(
                (std::min)(static_cast<uint64_t>(kSilenceFrames),
                           remaining_frames));
            const int64_t chunk_dur = static_cast<int64_t>(
                (static_cast<uint64_t>(chunk_frames) * 10'000'000ULL) /
                audio_format.sample_rate);
            if (chunk_dur <= 0) break;
            easnap::media::MediaChunk chunk;
            chunk.data = silence.data();
            chunk.size = static_cast<std::size_t>(chunk_frames) *
                         audio_format.block_align();
            chunk.timestamp = easnap::media::MediaTime{silence_pos};
            chunk.duration = easnap::media::MediaTime{chunk_dur};
            chunk.discontinuity = (silence_pos == 0);
            if (!writer->WriteAudioPacket(chunk)) {
                std::fwprintf(stderr, L"STTS silence error: %ls\n",
                              writer->last_error().c_str());
                silence_ok = false;
                break;
            }
            silence_pos += chunk_dur;
        }
        CHECK(silence_ok);
    }

    // Write video frames.
    easnap::media::VideoFrame frame;
    auto image = std::make_shared<easnap::platform::BgraImage>();
    image->Reset(static_cast<int>(kWidth), static_cast<int>(kHeight));
    frame.image = image;
    for (uint64_t index = 0; index < kFrameCount; ++index) {
        FillMovingPattern(*image, static_cast<uint32_t>(index));
        frame.timestamp =
            easnap::capture::MediaTimestampForFrame(index, kFrameRate);
        frame.duration =
            easnap::capture::MediaTimestampForFrame(index + 1, kFrameRate) -
            frame.timestamp;
        CHECK(writer->WriteVideoFrame(frame));
    }

    // Write real audio starting at kAudioStart100ns.
    constexpr uint32_t kAudioFramesPerPacket = 480;
    constexpr uint64_t kAudioPacketCount = 50;
    constexpr double kPi = 3.14159265358979323846;
    std::vector<int16_t> audio_samples(
        static_cast<std::size_t>(kAudioFramesPerPacket) * audio_format.channels);
    bool all_audio_written = true;
    for (uint64_t packet_index = 0; packet_index < kAudioPacketCount;
         ++packet_index) {
        for (uint32_t frame_index = 0; frame_index < kAudioFramesPerPacket;
             ++frame_index) {
            const uint64_t absolute_frame =
                packet_index * kAudioFramesPerPacket + frame_index;
            const int16_t sample = static_cast<int16_t>(
                6000.0 * std::sin(2.0 * kPi * 440.0 *
                                  static_cast<double>(absolute_frame) /
                                  audio_format.sample_rate));
            const std::size_t si =
                static_cast<std::size_t>(frame_index) * audio_format.channels;
            audio_samples[si] = sample;
            audio_samples[si + 1] = sample;
        }
        easnap::media::MediaChunk packet;
        packet.data = reinterpret_cast<const uint8_t*>(audio_samples.data());
        packet.size = audio_samples.size() * sizeof(int16_t);
        const int64_t packet_duration = static_cast<int64_t>(
            (static_cast<uint64_t>(kAudioFramesPerPacket) * 10'000'000ULL) /
            audio_format.sample_rate);
        packet.timestamp = easnap::media::MediaTime{
            kAudioStart100ns +
            static_cast<int64_t>(
                (packet_index * kAudioFramesPerPacket * 10'000'000ULL) /
                audio_format.sample_rate)};
        packet.duration = easnap::media::MediaTime{packet_duration};
        if (!writer->WriteAudioPacket(packet)) {
            std::fwprintf(stderr, L"STTS audio error: %ls\n",
                          writer->last_error().c_str());
            all_audio_written = false;
            break;
        }
    }
    CHECK(all_audio_written);

    std::wstring finalized_path;
    const bool finalized = writer->Finalize(finalized_path);
    if (!finalized) {
        std::fwprintf(stderr, L"STTS finalize error: %ls\n",
                      writer->last_error().c_str());
    }
    CHECK(finalized);
    writer.reset();

    // --- Raw STTS inspection ---
    std::vector<SttsEntry> stts_entries;
    uint32_t audio_timescale = 0;
    const bool stts_found =
        ParseAudioStts(finalized_path, stts_entries, audio_timescale);
    CHECK(stts_found);
    std::printf("STTS_REGRESSION timescale=%u entries=%zu\n",
                audio_timescale, stts_entries.size());

    // The normal AAC frame duration in timescale ticks.
    // For 44100 Hz: 1024 ticks. For 48000 Hz: 1024 ticks.
    // Allow up to 2× normal as the maximum acceptable duration.
    constexpr uint32_t kNormalAacDelta = 1024;
    constexpr uint32_t kMaxAcceptableDelta = kNormalAacDelta * 2;

    bool all_normal = true;
    uint64_t total_samples = 0;
    for (std::size_t i = 0; i < stts_entries.size(); ++i) {
        const auto& e = stts_entries[i];
        total_samples += e.sample_count;
        std::printf("  STTS[%zu]: %u samples x %u ticks (%.3f ms)\n",
                    i, e.sample_count, e.sample_delta,
                    static_cast<double>(e.sample_delta) * 1000.0 /
                        audio_timescale);
        if (e.sample_delta > kMaxAcceptableDelta) {
            std::fprintf(stderr,
                         "STTS DEFECT: entry %zu has %u ticks "
                         "(%.3f ms), max acceptable %u ticks (%.3f ms)\n",
                         i, e.sample_delta,
                         static_cast<double>(e.sample_delta) * 1000.0 /
                             audio_timescale,
                         kMaxAcceptableDelta,
                         static_cast<double>(kMaxAcceptableDelta) * 1000.0 /
                             audio_timescale);
            all_normal = false;
        }
    }
    CHECK(all_normal);
    CHECK(total_samples > 0);
    std::printf("STTS_RESULT: %s (%llu total samples, timescale=%u)\n",
                all_normal ? "CONTINUOUS" : "DEFECTIVE",
                static_cast<unsigned long long>(total_samples),
                audio_timescale);

    CHECK(session_store->DeleteSession(*session));
}

// ---------------------------------------------------------------------------
// STTS regression: finalization tail.
// Simulates the production scenario where WriteSilenceUpTo stops 750ms before
// the video endpoint, then FinalizeAudioTimeline fills the remaining gap.
// Verifies the raw MP4 STTS has no abnormal sample duration at the tail.
// ---------------------------------------------------------------------------
void TestFinalizationTailSttsContinuity() {
    constexpr UINT32 kWidth = 160;
    constexpr UINT32 kHeight = 90;
    constexpr uint32_t kFrameRate = 30;
    constexpr uint64_t kFrameCount = 60;  // 2 seconds
    // Video endpoint in 100ns.
    const int64_t kEndpoint100ns =
        easnap::capture::MediaTimestampForFrame(kFrameCount, kFrameRate).count();
    // Silence stops 750ms before endpoint (interleave lag).
    const int64_t kSilenceStop100ns = kEndpoint100ns - 7'500'000;

    MediaFoundationRuntime inspection_runtime;
    CHECK(SUCCEEDED(inspection_runtime.result()));
    if (FAILED(inspection_runtime.result())) return;

    auto session_store = easnap::media::CreateTemporarySessionStore();
    CHECK(session_store != nullptr);
    if (session_store == nullptr) return;
    const auto session = session_store->CreateSession();
    CHECK(session.has_value());
    if (!session.has_value()) return;
    const std::wstring output_path = session->recording_path();

    auto writer = easnap::media::CreateMediaFoundationWriter();
    CHECK(writer != nullptr);
    if (writer == nullptr) return;

    easnap::media::RecordingOptions options;
    options.region = RECT{0, 0, static_cast<LONG>(kWidth),
                          static_cast<LONG>(kHeight)};
    options.format = easnap::media::RecordingFormat::Mp4;
    options.audio = easnap::media::AudioSource::SystemAudio;
    options.target_frame_rate = kFrameRate;
    options.video_bitrate_bps = 1'000'000;

    easnap::media::AudioFormat audio_format;
    audio_format.sample_rate = 44'100;
    audio_format.channels = 2;
    audio_format.bits_per_sample = 16;
    audio_format.is_float = false;
    audio_format.channel_mask = 0x3;

    const bool opened = writer->Open(output_path, options, &audio_format);
    CHECK(opened);
    if (!opened) return;

    // Phase 1: Write silence from t=0 to kSilenceStop100ns.
    // This simulates WriteSilenceUpTo during recording.
    {
        constexpr uint32_t kSilenceFrames = 1024;
        const std::size_t silence_bytes =
            static_cast<std::size_t>(kSilenceFrames) *
            audio_format.block_align();
        std::vector<uint8_t> silence(silence_bytes, 0);
        int64_t pos = 0;
        bool ok = true;
        while (pos < kSilenceStop100ns) {
            const int64_t remaining = kSilenceStop100ns - pos;
            const uint64_t remaining_frames =
                (static_cast<uint64_t>(remaining) * audio_format.sample_rate) /
                10'000'000ULL;
            if (remaining_frames == 0) break;
            const uint32_t chunk_frames = static_cast<uint32_t>(
                (std::min)(static_cast<uint64_t>(kSilenceFrames),
                           remaining_frames));
            const int64_t chunk_dur = static_cast<int64_t>(
                (static_cast<uint64_t>(chunk_frames) * 10'000'000ULL) /
                audio_format.sample_rate);
            if (chunk_dur <= 0) break;
            easnap::media::MediaChunk chunk;
            chunk.data = silence.data();
            chunk.size = static_cast<std::size_t>(chunk_frames) *
                         audio_format.block_align();
            chunk.timestamp = easnap::media::MediaTime{pos};
            chunk.duration = easnap::media::MediaTime{chunk_dur};
            chunk.discontinuity = (pos == 0);
            if (!writer->WriteAudioPacket(chunk)) {
                ok = false;
                break;
            }
            pos += chunk_dur;
        }
        CHECK(ok);
        std::printf("TAIL_TEST silence_phase1_end=%.6f s stop_target=%.6f s\n",
                    pos / 10'000'000.0, kSilenceStop100ns / 10'000'000.0);

        // Phase 2: Fill the remaining 750ms gap continuously.
        // This simulates what FinalizeAudioTimeline now does.
        while (pos < kEndpoint100ns) {
            const int64_t remaining = kEndpoint100ns - pos;
            const uint64_t remaining_frames =
                (static_cast<uint64_t>(remaining) * audio_format.sample_rate) /
                10'000'000ULL;
            if (remaining_frames == 0) break;
            const uint32_t chunk_frames = static_cast<uint32_t>(
                (std::min)(static_cast<uint64_t>(kSilenceFrames),
                           remaining_frames));
            const int64_t chunk_dur = static_cast<int64_t>(
                (static_cast<uint64_t>(chunk_frames) * 10'000'000ULL) /
                audio_format.sample_rate);
            if (chunk_dur <= 0) break;
            easnap::media::MediaChunk chunk;
            chunk.data = silence.data();
            chunk.size = static_cast<std::size_t>(chunk_frames) *
                         audio_format.block_align();
            chunk.timestamp = easnap::media::MediaTime{pos};
            chunk.duration = easnap::media::MediaTime{chunk_dur};
            chunk.discontinuity = false;
            if (!writer->WriteAudioPacket(chunk)) {
                ok = false;
                break;
            }
            pos += chunk_dur;
        }
        CHECK(ok);
        std::printf("TAIL_TEST silence_phase2_end=%.6f s endpoint=%.6f s\n",
                    pos / 10'000'000.0, kEndpoint100ns / 10'000'000.0);
    }

    // Write video frames.
    easnap::media::VideoFrame frame;
    auto image = std::make_shared<easnap::platform::BgraImage>();
    image->Reset(static_cast<int>(kWidth), static_cast<int>(kHeight));
    frame.image = image;
    for (uint64_t index = 0; index < kFrameCount; ++index) {
        FillMovingPattern(*image, static_cast<uint32_t>(index));
        frame.timestamp =
            easnap::capture::MediaTimestampForFrame(index, kFrameRate);
        frame.duration =
            easnap::capture::MediaTimestampForFrame(index + 1, kFrameRate) -
            frame.timestamp;
        CHECK(writer->WriteVideoFrame(frame));
    }

    std::wstring finalized_path;
    const bool finalized = writer->Finalize(finalized_path);
    CHECK(finalized);
    writer.reset();

    // --- Raw STTS inspection ---
    std::vector<SttsEntry> stts_entries;
    uint32_t audio_timescale = 0;
    const bool stts_found =
        ParseAudioStts(finalized_path, stts_entries, audio_timescale);
    CHECK(stts_found);
    std::printf("STTS_TAIL_REGRESSION timescale=%u entries=%zu\n",
                audio_timescale, stts_entries.size());

    constexpr uint32_t kNormalAacDelta = 1024;
    constexpr uint32_t kMaxAcceptableDelta = kNormalAacDelta * 2;
    bool all_normal = true;
    uint64_t total_samples = 0;
    for (std::size_t i = 0; i < stts_entries.size(); ++i) {
        const auto& e = stts_entries[i];
        total_samples += e.sample_count;
        std::printf("  STTS[%zu]: %u samples x %u ticks (%.3f ms)\n",
                    i, e.sample_count, e.sample_delta,
                    static_cast<double>(e.sample_delta) * 1000.0 /
                        audio_timescale);
        if (e.sample_delta > kMaxAcceptableDelta) {
            std::fprintf(stderr,
                         "STTS TAIL DEFECT: entry %zu has %u ticks "
                         "(%.3f ms), max acceptable %u ticks\n",
                         i, e.sample_delta,
                         static_cast<double>(e.sample_delta) * 1000.0 /
                             audio_timescale,
                         kMaxAcceptableDelta);
            all_normal = false;
        }
    }
    CHECK(all_normal);
    CHECK(total_samples > 0);
    std::printf("STTS_TAIL_RESULT: %s (%llu total samples, timescale=%u)\n",
                all_normal ? "CONTINUOUS" : "DEFECTIVE",
                static_cast<unsigned long long>(total_samples),
                audio_timescale);

    CHECK(session_store->DeleteSession(*session));
}

}  // namespace

int main() {
    TestRealH264AacMp4Writer();
    TestDelayedAudioSttsContinuity();
    TestFinalizationTailSttsContinuity();
    if (g_fail == 0) {
        std::printf("PASSED  %d/%d tests\n", g_pass, g_pass + g_fail);
        return 0;
    }
    std::printf("FAILED  %d passed, %d FAILED\n", g_pass, g_fail);
    return 1;
}
