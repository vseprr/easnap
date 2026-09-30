// Owns recording finalization until the UI consumes its durable result.
#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <optional>
#include <thread>

namespace easnap::app {

struct RecordingFinalizationResult final {
    bool finalized{false};
    bool worker_exception{false};
    DWORD notification_error{ERROR_SUCCESS};
};

// All public operations belong to the owner/UI thread. A window message is only
// a wakeup: polling can consume the same result if delivery fails. Work and its
// captured session must outlive Join; no timeout abandons an in-process worker.
class RecordingFinalizer final {
public:
    using Generation = UINT_PTR;

    RecordingFinalizer() = default;
    ~RecordingFinalizer();
    RecordingFinalizer(const RecordingFinalizer&) = delete;
    RecordingFinalizer& operator=(const RecordingFinalizer&) = delete;

    [[nodiscard]] bool Start(HWND owner, UINT message, std::function<bool()> stop);
    [[nodiscard]] std::optional<RecordingFinalizationResult> TryTake(Generation generation);
    void Join();
    [[nodiscard]] bool has_worker() const noexcept { return worker_.joinable(); }
    [[nodiscard]] Generation generation() const noexcept { return generation_; }

private:
    std::thread worker_;
    Generation generation_{0};
    std::atomic<Generation> completed_{0};
    RecordingFinalizationResult result_{};
};

}  // namespace easnap::app
