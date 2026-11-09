// The support table and the probe's pure rules (ADR-040): extension lookups,
// per-container subtypes, the names v1 refuses, the path rules and every
// answer of the classifier, including those no test file can produce
// (protected content, a missing codec extension, an 8K picture, a network
// error).
#include "media/video_probe.h"
#include "media/video_support.h"

#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

using easnap::media::ClassifyVideoProbe;
using easnap::media::VideoContainer;
using easnap::media::VideoNotSupportedYetCause;
using easnap::media::VideoPathProblem;
using easnap::media::VideoProbeFacts;
using easnap::media::VideoProbeStep;
using easnap::media::VideoProbeVerdict;
using easnap::media::VideoRefusal;

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

void TestExtensions() {
    using easnap::media::FindVideoContainer;
    using easnap::media::FindVideoContainerForPath;
    using easnap::media::IsKnownUnsupportedVideoExtension;
    using easnap::media::SupportedVideoExtensions;
    using easnap::media::VideoFileExtension;

    const auto expect = [](std::wstring_view extension, VideoContainer container) {
        const auto* entry = FindVideoContainer(extension);
        return entry != nullptr && entry->container == container;
    };
    CHECK(expect(L".mp4", VideoContainer::Mpeg4));
    CHECK(expect(L"mp4", VideoContainer::Mpeg4));
    CHECK(expect(L".MP4", VideoContainer::Mpeg4));
    CHECK(expect(L"M4v", VideoContainer::Mpeg4));
    CHECK(expect(L".mov", VideoContainer::Mpeg4));
    CHECK(expect(L".wmv", VideoContainer::Asf));
    CHECK(expect(L"ASF", VideoContainer::Asf));
    CHECK(expect(L".avi", VideoContainer::Avi));
    // ASCII case folding only: a Turkish dotted capital I is not "i".
    CHECK(expect(L".AVI", VideoContainer::Avi));
    CHECK(FindVideoContainer(L".av\u0130") == nullptr);
    CHECK(FindVideoContainer(L"") == nullptr);
    CHECK(FindVideoContainer(L".") == nullptr);
    CHECK(FindVideoContainer(L"..mp4") == nullptr);
    CHECK(FindVideoContainer(L".mp4 ") == nullptr);
    CHECK(FindVideoContainer(L".mp") == nullptr);
    CHECK(FindVideoContainer(L".mp44") == nullptr);
    CHECK(FindVideoContainer(L".mkv") == nullptr);
    CHECK(FindVideoContainer(L".xyz") == nullptr);

    CHECK(VideoFileExtension(L"C:\\videos\\clip.final.MOV") == L".MOV");
    CHECK(VideoFileExtension(L"C:\\videos.d\\clip").empty());
    CHECK(VideoFileExtension(L"C:/videos/clip.avi") == L".avi");
    CHECK(VideoFileExtension(L"clip.") == L".");
    CHECK(FindVideoContainerForPath(L"C:\\videos.mp4\\clip") == nullptr);
    CHECK(FindVideoContainerForPath(L"C:\\v\\clip.WMV") != nullptr);
    CHECK(FindVideoContainerForPath(L"C:\\v\\clip.WMV")->container == VideoContainer::Asf);

    CHECK(IsKnownUnsupportedVideoExtension(L".mkv"));
    CHECK(IsKnownUnsupportedVideoExtension(L"WEBM"));
    CHECK(!IsKnownUnsupportedVideoExtension(L".mp4"));
    CHECK(!IsKnownUnsupportedVideoExtension(L""));
    CHECK(!IsKnownUnsupportedVideoExtension(L"."));

    // The enumeration the installer test reads: exactly the table's rows,
    // lower case, with the dot, each once.
    const auto extensions = SupportedVideoExtensions();
    const std::wstring_view expected[]{L".mp4", L".m4v", L".mov", L".wmv", L".asf", L".avi"};
    CHECK(extensions.size() == std::size(expected));
    for (const std::wstring_view extension : expected) {
        CHECK(std::count(extensions.begin(), extensions.end(), extension) == 1);
        CHECK(FindVideoContainer(extension) != nullptr);
    }
    for (const std::wstring_view extension : easnap::media::KnownUnsupportedVideoExtensions()) {
        CHECK(FindVideoContainer(extension) == nullptr);
        CHECK(std::find(extensions.begin(), extensions.end(), extension) == extensions.end());
    }
}

void TestSubtypes() {
    using easnap::media::IsAudioSubtypeAllowed;
    using easnap::media::IsVideoSubtypeAllowed;
    using easnap::media::KnownUnsupportedVideoCodec;
    using easnap::media::UnsupportedVideoCodec;

    for (const VideoContainer container : {VideoContainer::Mpeg4, VideoContainer::Avi}) {
        for (const GUID& subtype : {MFVideoFormat_H264, MFVideoFormat_MP4V, MFVideoFormat_M4S2,
                                    MFVideoFormat_MJPG}) {
            CHECK(IsVideoSubtypeAllowed(container, subtype));
        }
        for (const GUID& subtype : {MFVideoFormat_WMV3, MFVideoFormat_WVC1, MFVideoFormat_HEVC,
                                    MFVideoFormat_VP90, MFVideoFormat_AV1, MFVideoFormat_NV12}) {
            CHECK(!IsVideoSubtypeAllowed(container, subtype));
        }
        for (const GUID& subtype : {MFAudioFormat_AAC, MFAudioFormat_MP3, MFAudioFormat_PCM}) {
            CHECK(IsAudioSubtypeAllowed(container, subtype));
        }
        for (const GUID& subtype : {MFAudioFormat_WMAudioV8, MFAudioFormat_WMAudioV9,
                                    MFAudioFormat_Float, MFAudioFormat_Dolby_AC3}) {
            CHECK(!IsAudioSubtypeAllowed(container, subtype));
        }
    }
    for (const GUID& subtype : {MFVideoFormat_WMV3, MFVideoFormat_WVC1}) {
        CHECK(IsVideoSubtypeAllowed(VideoContainer::Asf, subtype));
    }
    for (const GUID& subtype : {MFVideoFormat_H264, MFVideoFormat_MP4V, MFVideoFormat_MJPG,
                                MFVideoFormat_WMV2, MFVideoFormat_HEVC}) {
        CHECK(!IsVideoSubtypeAllowed(VideoContainer::Asf, subtype));
    }
    for (const GUID& subtype : {MFAudioFormat_WMAudioV8, MFAudioFormat_WMAudioV9}) {
        CHECK(IsAudioSubtypeAllowed(VideoContainer::Asf, subtype));
    }
    for (const GUID& subtype : {MFAudioFormat_AAC, MFAudioFormat_MP3, MFAudioFormat_PCM,
                                MFAudioFormat_WMAudio_Lossless}) {
        CHECK(!IsAudioSubtypeAllowed(VideoContainer::Asf, subtype));
    }

    CHECK(KnownUnsupportedVideoCodec(MFVideoFormat_HEVC) == UnsupportedVideoCodec::Hevc);
    CHECK(KnownUnsupportedVideoCodec(MFVideoFormat_HEVC_ES) == UnsupportedVideoCodec::Hevc);
    CHECK(KnownUnsupportedVideoCodec(MFVideoFormat_H265) == UnsupportedVideoCodec::Hevc);
    CHECK(KnownUnsupportedVideoCodec(MFVideoFormat_VP90) == UnsupportedVideoCodec::Vp9);
    CHECK(KnownUnsupportedVideoCodec(MFVideoFormat_AV1) == UnsupportedVideoCodec::Av1);
    CHECK(KnownUnsupportedVideoCodec(MFVideoFormat_H264) == UnsupportedVideoCodec::None);

    CHECK(easnap::media::VideoContainerEntry(VideoContainer::Mpeg4).content_type == L"video/mp4");
    CHECK(easnap::media::VideoContainerEntry(VideoContainer::Asf).content_type == L"video/x-ms-asf");
    CHECK(easnap::media::VideoContainerEntry(VideoContainer::Avi).content_type == L"video/avi");
}

void TestH264Size() {
    using easnap::media::FrameFitsH264;
    static_assert(FrameFitsH264(3840, 2160));
    CHECK(FrameFitsH264(1920, 1080));
    CHECK(FrameFitsH264(4096, 2304));
    CHECK(!FrameFitsH264(4096, 2320));
    CHECK(!FrameFitsH264(7680, 4320));
    CHECK(FrameFitsH264(16, 8688));
    CHECK(!FrameFitsH264(16, 8690));
    CHECK(!FrameFitsH264(0, 100));
}

void TestPathSyntax() {
    using easnap::media::CheckVideoPathSyntax;
    CHECK(CheckVideoPathSyntax(L"C:\\videos\\clip.mp4") == VideoPathProblem::None);
    CHECK(CheckVideoPathSyntax(L"d:/videos/clip.mp4") == VideoPathProblem::None);
    CHECK(CheckVideoPathSyntax(L"\\\\?\\C:\\videos\\clip.mp4") == VideoPathProblem::None);
    CHECK(CheckVideoPathSyntax(L"") == VideoPathProblem::NotAbsolute);
    CHECK(CheckVideoPathSyntax(L"clip.mp4") == VideoPathProblem::NotAbsolute);
    CHECK(CheckVideoPathSyntax(L"C:clip.mp4") == VideoPathProblem::NotAbsolute);
    CHECK(CheckVideoPathSyntax(L"\\videos\\clip.mp4") == VideoPathProblem::NotAbsolute);
    CHECK(CheckVideoPathSyntax(std::wstring_view(L"C:\\a\0b.mp4", 10)) ==
          VideoPathProblem::NotAbsolute);
    CHECK(CheckVideoPathSyntax(L"\\\\server\\share\\clip.mp4") == VideoPathProblem::Remote);
    CHECK(CheckVideoPathSyntax(L"//server/share/clip.mp4") == VideoPathProblem::Remote);
    CHECK(CheckVideoPathSyntax(L"\\\\?\\UNC\\server\\share\\clip.mp4") ==
          VideoPathProblem::Remote);
    CHECK(CheckVideoPathSyntax(L"\\\\?\\unc\\server\\share\\clip.mp4") ==
          VideoPathProblem::Remote);
    CHECK(CheckVideoPathSyntax(L"\\\\.\\pipe\\clip.mp4") == VideoPathProblem::NotRegularFile);
    CHECK(CheckVideoPathSyntax(L"\\\\.\\C:\\clip.mp4") == VideoPathProblem::NotRegularFile);
    CHECK(CheckVideoPathSyntax(L"\\\\?\\Volume{0}\\clip.mp4") ==
          VideoPathProblem::NotRegularFile);
    CHECK(CheckVideoPathSyntax(L"https://example.invalid/clip.mp4") == VideoPathProblem::Url);
    CHECK(CheckVideoPathSyntax(L"file:///C:/clip.mp4") == VideoPathProblem::Url);
    CHECK(CheckVideoPathSyntax(L"ms-photos:clip.mp4") == VideoPathProblem::Url);
}

// ---------------------------------------------------------------------------
// The classifier: one row per answer, from facts and an HRESULT
// ---------------------------------------------------------------------------

/// The facts of a file every check accepted.
VideoProbeFacts AcceptedFacts() {
    VideoProbeFacts facts;
    facts.step = VideoProbeStep::Complete;
    facts.container = VideoContainer::Mpeg4;
    facts.has_video = true;
    facts.video_subtype = MFVideoFormat_H264;
    facts.width = 1920;
    facts.height = 1080;
    facts.frame_rate_numerator = 30;
    facts.frame_rate_denominator = 1;
    facts.interlace_mode = MFVideoInterlace_MixedInterlaceOrProgressive;
    facts.video_profile = 100;  // High
    facts.has_audio = true;
    facts.audio_subtype = MFAudioFormat_AAC;
    return facts;
}

VideoProbeFacts StoppedAt(VideoProbeStep step) {
    VideoProbeFacts facts = AcceptedFacts();
    facts.step = step;
    return facts;
}

VideoProbeVerdict Verdict(VideoRefusal reason,
                          VideoNotSupportedYetCause cause = VideoNotSupportedYetCause::None) {
    return VideoProbeVerdict{reason, cause};
}

void TestClassifierAccepts() {
    CHECK(ClassifyVideoProbe(AcceptedFacts(), S_OK) == Verdict(VideoRefusal::None));
    VideoProbeFacts facts = AcceptedFacts();
    facts.has_audio = false;
    facts.audio_subtype = MFAudioFormat_WMAudioV8;  // Ignored without an audio stream.
    CHECK(ClassifyVideoProbe(facts, S_OK) == Verdict(VideoRefusal::None));
    facts = AcceptedFacts();
    facts.container = VideoContainer::Asf;
    facts.video_subtype = MFVideoFormat_WVC1;
    facts.audio_subtype = MFAudioFormat_WMAudioV9;
    facts.interlace_mode = MFVideoInterlace_Progressive;
    facts.video_profile = 0;
    CHECK(ClassifyVideoProbe(facts, S_OK) == Verdict(VideoRefusal::None));
    facts = AcceptedFacts();
    facts.container = VideoContainer::Avi;
    facts.video_subtype = MFVideoFormat_MJPG;
    facts.audio_subtype = MFAudioFormat_PCM;
    facts.pixel_aspect_numerator = 0;  // Undeclared counts as square.
    facts.pixel_aspect_denominator = 0;
    CHECK(ClassifyVideoProbe(facts, S_OK) == Verdict(VideoRefusal::None));
    facts = AcceptedFacts();
    facts.rotation_degrees = 360;
    facts.pixel_aspect_numerator = 4;
    facts.pixel_aspect_denominator = 4;
    CHECK(ClassifyVideoProbe(facts, S_OK) == Verdict(VideoRefusal::None));
}

void TestClassifierLocationAndAccess() {
    CHECK(ClassifyVideoProbe(StoppedAt(VideoProbeStep::MediaFoundation), S_OK) ==
          Verdict(VideoRefusal::MediaFoundationUnavailable));
    CHECK(ClassifyVideoProbe(StoppedAt(VideoProbeStep::MediaFoundation), E_OUTOFMEMORY) ==
          Verdict(VideoRefusal::MediaFoundationUnavailable));

    struct PathRow final {
        VideoPathProblem problem;
        VideoRefusal reason;
    };
    const PathRow path_rows[]{
        {VideoPathProblem::NotAbsolute, VideoRefusal::NotFound},
        {VideoPathProblem::NoSuchDrive, VideoRefusal::NotFound},
        {VideoPathProblem::Url, VideoRefusal::NetworkOrOffline},
        {VideoPathProblem::Remote, VideoRefusal::NetworkOrOffline},
        {VideoPathProblem::Offline, VideoRefusal::NetworkOrOffline},
        {VideoPathProblem::NotRegularFile, VideoRefusal::UnsupportedExtension},
    };
    for (const VideoProbeStep step : {VideoProbeStep::Path, VideoProbeStep::OpenFile}) {
        for (const PathRow& row : path_rows) {
            VideoProbeFacts facts = StoppedAt(step);
            facts.path_problem = row.problem;
            CHECK(ClassifyVideoProbe(facts, S_OK) == Verdict(row.reason));
        }
    }

    struct ErrorRow final {
        DWORD error;
        VideoRefusal reason;
    };
    const ErrorRow error_rows[]{
        {ERROR_FILE_NOT_FOUND, VideoRefusal::NotFound},
        {ERROR_PATH_NOT_FOUND, VideoRefusal::NotFound},
        {ERROR_INVALID_NAME, VideoRefusal::NotFound},
        {ERROR_NOT_READY, VideoRefusal::NotFound},
        {ERROR_ACCESS_DENIED, VideoRefusal::NoAccess},
        {ERROR_SHARING_VIOLATION, VideoRefusal::InUse},
        {ERROR_LOCK_VIOLATION, VideoRefusal::InUse},
        {ERROR_BAD_NETPATH, VideoRefusal::NetworkOrOffline},
        {ERROR_NETNAME_DELETED, VideoRefusal::NetworkOrOffline},
        {ERROR_CLOUD_FILE_PROVIDER_NOT_RUNNING, VideoRefusal::NetworkOrOffline},
        {ERROR_FILE_OFFLINE, VideoRefusal::NetworkOrOffline},
        {ERROR_CRC, VideoRefusal::Damaged},
        {ERROR_INVALID_FUNCTION, VideoRefusal::Damaged},
    };
    for (const ErrorRow& row : error_rows) {
        for (const VideoProbeStep step : {VideoProbeStep::Path, VideoProbeStep::OpenFile}) {
            CHECK(ClassifyVideoProbe(StoppedAt(step), HRESULT_FROM_WIN32(row.error)) ==
                  Verdict(row.reason));
        }
    }
    // A read Media Foundation passes on keeps its file-system meaning.
    CHECK(ClassifyVideoProbe(StoppedAt(VideoProbeStep::OpenSource),
                             HRESULT_FROM_WIN32(ERROR_NETNAME_DELETED)) ==
          Verdict(VideoRefusal::NetworkOrOffline));
    CHECK(ClassifyVideoProbe(StoppedAt(VideoProbeStep::DecodeFirstFrame),
                             HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) ==
          Verdict(VideoRefusal::InUse));
    // ERROR_SHARING_VIOLATION is 0x80070020, as ADR-040 says.
    CHECK(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) == static_cast<HRESULT>(0x80070020));

    VideoProbeFacts extension = StoppedAt(VideoProbeStep::Extension);
    extension.container.reset();
    CHECK(ClassifyVideoProbe(extension, S_OK) == Verdict(VideoRefusal::UnsupportedExtension));
    extension.extension_known_unsupported = true;
    CHECK(ClassifyVideoProbe(extension, S_OK) == Verdict(VideoRefusal::FormatNotSupported));
}

void TestClassifierContent() {
    // Not a container the handler can read.
    for (const HRESULT error : {MF_E_UNSUPPORTED_BYTESTREAM_TYPE, MF_E_INVALID_FILE_FORMAT,
                                E_FAIL}) {
        CHECK(ClassifyVideoProbe(StoppedAt(VideoProbeStep::OpenSource), error) ==
              Verdict(VideoRefusal::Damaged));
    }
    CHECK(MF_E_UNSUPPORTED_BYTESTREAM_TYPE == static_cast<HRESULT>(0xC00D36C4));
    CHECK(MF_E_INVALID_FILE_FORMAT == static_cast<HRESULT>(0xC00D36BE));

    // Protected, whether a stream says so or Media Foundation refuses it.
    VideoProbeFacts protected_facts = StoppedAt(VideoProbeStep::OpenSource);
    protected_facts.is_protected = true;
    CHECK(ClassifyVideoProbe(protected_facts, S_OK) == Verdict(VideoRefusal::Protected));
    for (const HRESULT error : {MF_E_DRM_UNSUPPORTED, MF_E_LICENSE_REQUIRED,
                                MF_E_LICENSE_OUTOFDATE, MF_E_NO_CONTENT_PROTECTION_MANAGER,
                                MF_E_UNSUPPORTED_CONTENT_PROTECTION_SYSTEM}) {
        CHECK(ClassifyVideoProbe(StoppedAt(VideoProbeStep::OpenSource), error) ==
              Verdict(VideoRefusal::Protected));
        CHECK(ClassifyVideoProbe(StoppedAt(VideoProbeStep::DecodeFirstFrame), error) ==
              Verdict(VideoRefusal::Protected));
    }
    CHECK(MF_E_DRM_UNSUPPORTED == static_cast<HRESULT>(0xC00D3700));

    VideoProbeFacts no_video = StoppedAt(VideoProbeStep::FindVideo);
    no_video.has_video = false;
    CHECK(ClassifyVideoProbe(no_video, S_OK) == Verdict(VideoRefusal::NoVideo));
    no_video.is_protected = true;
    CHECK(ClassifyVideoProbe(no_video, S_OK) == Verdict(VideoRefusal::Protected));

    // Formats the table does not list.
    VideoProbeFacts format = StoppedAt(VideoProbeStep::ReadFormat);
    format.video_subtype = MFVideoFormat_VP90;
    CHECK(ClassifyVideoProbe(format, S_OK) == Verdict(VideoRefusal::FormatNotSupported));
    format.video_subtype = MFVideoFormat_WMV3;  // Windows decodes it, but not in MPEG-4 here.
    CHECK(ClassifyVideoProbe(format, S_OK) == Verdict(VideoRefusal::FormatNotSupported));
    format = StoppedAt(VideoProbeStep::ReadFormat);
    format.audio_subtype = MFAudioFormat_Dolby_AC3;
    CHECK(ClassifyVideoProbe(format, S_OK) == Verdict(VideoRefusal::FormatNotSupported));
    format = StoppedAt(VideoProbeStep::ReadFormat);
    format.container = VideoContainer::Asf;
    format.video_subtype = MFVideoFormat_WMV3;
    format.audio_subtype = MFAudioFormat_AAC;
    CHECK(ClassifyVideoProbe(format, S_OK) == Verdict(VideoRefusal::FormatNotSupported));
    // The format answer comes before a later failure.
    format = StoppedAt(VideoProbeStep::DecodeFirstFrame);
    format.video_subtype = MFVideoFormat_AV1;
    CHECK(ClassifyVideoProbe(format, MF_E_INVALID_STREAM_DATA) ==
          Verdict(VideoRefusal::FormatNotSupported));

    // HEVC: whether its decoder exists only changes the wording.
    VideoProbeFacts hevc = StoppedAt(VideoProbeStep::ConfigureDecoder);
    hevc.video_subtype = MFVideoFormat_HEVC;
    CHECK(ClassifyVideoProbe(hevc, MF_E_TOPO_CODEC_NOT_FOUND) ==
          Verdict(VideoRefusal::NeedsCodecExtension));
    CHECK(ClassifyVideoProbe(hevc, MF_E_INVALIDMEDIATYPE) ==
          Verdict(VideoRefusal::NeedsCodecExtension));
    CHECK(ClassifyVideoProbe(hevc, S_OK) == Verdict(VideoRefusal::FormatNotSupported));
    hevc.step = VideoProbeStep::ReadFormat;
    CHECK(ClassifyVideoProbe(hevc, S_OK) == Verdict(VideoRefusal::FormatNotSupported));
    CHECK(MF_E_TOPO_CODEC_NOT_FOUND == static_cast<HRESULT>(0xC00D5212));

    // A codec of the table whose decoder Windows lacks (an N edition with a
    // partial pack, a removed codec).
    for (const HRESULT error : {MF_E_TOPO_CODEC_NOT_FOUND, MF_E_INVALIDMEDIATYPE}) {
        CHECK(ClassifyVideoProbe(StoppedAt(VideoProbeStep::ConfigureDecoder), error) ==
              Verdict(VideoRefusal::NeedsCodecExtension));
    }
    CHECK(ClassifyVideoProbe(StoppedAt(VideoProbeStep::ConfigureDecoder), E_FAIL) ==
          Verdict(VideoRefusal::Damaged));

    // The first frame does not decode, or the file has no usable duration.
    for (const HRESULT error : {MF_E_INVALID_STREAM_DATA, MF_E_END_OF_STREAM, E_FAIL}) {
        CHECK(ClassifyVideoProbe(StoppedAt(VideoProbeStep::DecodeFirstFrame), error) ==
              Verdict(VideoRefusal::Damaged));
    }
    CHECK(ClassifyVideoProbe(StoppedAt(VideoProbeStep::ReadDuration), MF_E_ATTRIBUTENOTFOUND) ==
          Verdict(VideoRefusal::Damaged));
    CHECK(ClassifyVideoProbe(StoppedAt(VideoProbeStep::ReadFormat), MF_E_INVALIDMEDIATYPE) ==
          Verdict(VideoRefusal::Damaged));
}

void TestClassifierNotSupportedYet() {
    VideoProbeFacts facts = StoppedAt(VideoProbeStep::ReadFormat);
    facts.rotation_degrees = 90;
    CHECK(ClassifyVideoProbe(facts, S_OK) ==
          Verdict(VideoRefusal::NotSupportedYet, VideoNotSupportedYetCause::Rotation));
    facts.rotation_degrees = 270;
    CHECK(ClassifyVideoProbe(facts, S_OK) ==
          Verdict(VideoRefusal::NotSupportedYet, VideoNotSupportedYetCause::Rotation));

    facts = StoppedAt(VideoProbeStep::ReadFormat);
    facts.pixel_aspect_numerator = 4;
    facts.pixel_aspect_denominator = 3;
    CHECK(ClassifyVideoProbe(facts, S_OK) ==
          Verdict(VideoRefusal::NotSupportedYet, VideoNotSupportedYetCause::PixelAspect));

    for (const uint32_t mode : {static_cast<uint32_t>(MFVideoInterlace_FieldInterleavedUpperFirst),
                                static_cast<uint32_t>(MFVideoInterlace_FieldInterleavedLowerFirst),
                                static_cast<uint32_t>(MFVideoInterlace_FieldSingleUpper),
                                static_cast<uint32_t>(MFVideoInterlace_FieldSingleLower)}) {
        facts = StoppedAt(VideoProbeStep::ReadFormat);
        facts.interlace_mode = mode;
        CHECK(ClassifyVideoProbe(facts, S_OK) ==
              Verdict(VideoRefusal::NotSupportedYet, VideoNotSupportedYetCause::Interlaced));
    }
    // "Mixed" is what an H.264 file declares; the decoded frame decides.
    facts = AcceptedFacts();
    facts.first_frame_interlaced = true;
    CHECK(ClassifyVideoProbe(facts, S_OK) ==
          Verdict(VideoRefusal::NotSupportedYet, VideoNotSupportedYetCause::Interlaced));

    for (const uint32_t profile : {110u, 122u, 244u, 44u}) {
        facts = StoppedAt(VideoProbeStep::ReadFormat);
        facts.video_profile = profile;
        CHECK(ClassifyVideoProbe(facts, S_OK) ==
              Verdict(VideoRefusal::NotSupportedYet, VideoNotSupportedYetCause::HighBitDepth));
    }
    // The profile number means something else for another codec.
    facts = StoppedAt(VideoProbeStep::ReadFormat);
    facts.video_subtype = MFVideoFormat_MP4V;
    facts.video_profile = 244;
    CHECK(ClassifyVideoProbe(facts, MF_E_INVALIDMEDIATYPE) == Verdict(VideoRefusal::Damaged));
    for (const uint32_t transfer : {static_cast<uint32_t>(MFVideoTransFunc_2084),
                                    static_cast<uint32_t>(MFVideoTransFunc_HLG)}) {
        facts = StoppedAt(VideoProbeStep::ReadFormat);
        facts.transfer_function = transfer;
        CHECK(ClassifyVideoProbe(facts, S_OK) ==
              Verdict(VideoRefusal::NotSupportedYet, VideoNotSupportedYetCause::HighBitDepth));
    }
}

void TestClassifierLimitsAndDeadline() {
    VideoProbeFacts facts = AcceptedFacts();
    facts.width = 7680;
    facts.height = 4320;
    CHECK(ClassifyVideoProbe(facts, S_OK) == Verdict(VideoRefusal::TooLargeForMp4));
    facts.width = 4096;
    facts.height = 2304;
    CHECK(ClassifyVideoProbe(facts, S_OK) == Verdict(VideoRefusal::None));

    // Whatever step was next, a passed deadline answers Timeout.
    for (const VideoProbeStep step : {VideoProbeStep::Path, VideoProbeStep::OpenFile,
                                      VideoProbeStep::OpenSource, VideoProbeStep::DecodeFirstFrame,
                                      VideoProbeStep::Complete}) {
        facts = StoppedAt(step);
        facts.deadline_passed = true;
        CHECK(ClassifyVideoProbe(facts, S_OK) == Verdict(VideoRefusal::Timeout));
    }
}

}  // namespace

int main() {
    TestExtensions();
    TestSubtypes();
    TestH264Size();
    TestPathSyntax();
    TestClassifierAccepts();
    TestClassifierLocationAndAccess();
    TestClassifierContent();
    TestClassifierNotSupportedYet();
    TestClassifierLimitsAndDeadline();
    if (g_fail == 0) {
        std::printf("PASSED  %d/%d checks\n", g_pass, g_pass + g_fail);
        return 0;
    }
    std::printf("FAILED  %d passed, %d FAILED\n", g_pass, g_fail);
    return 1;
}
