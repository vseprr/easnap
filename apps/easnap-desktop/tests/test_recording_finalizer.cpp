#include "app/recording_finalizer.h"

#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>

using easnap::app::RecordingFinalizer;
using namespace std::chrono_literals;
namespace {
int failures = 0;
int checks = 0;
void Check(bool condition, int line) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL finalizer:%d\n", line); }
}
#define CHECK(value) Check((value), __LINE__)
constexpr UINT kComplete = WM_APP + 23;

std::optional<easnap::app::RecordingFinalizationResult> Poll(
    RecordingFinalizer& worker, std::chrono::milliseconds interval = 1ms) {
    const auto start = std::chrono::steady_clock::now();
    do {
        if (const auto result = worker.TryTake(worker.generation())) return result;
        std::this_thread::sleep_for(interval);
    } while (std::chrono::steady_clock::now() - start < 1s);
    return std::nullopt;
}

void TestMessageAndPolling(HWND owner) {
    RecordingFinalizer worker;
    CHECK(worker.Start(owner, kComplete, [] { return true; }));
    const auto generation = worker.generation();
    MSG message{};
    bool delivered = false;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (::PeekMessageW(&message, owner, kComplete, kComplete, PM_REMOVE)) {
            delivered = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    CHECK(delivered);
    CHECK(message.wParam == generation);
    const auto result = worker.TryTake(static_cast<UINT_PTR>(message.wParam));
    CHECK(result.has_value() && result->finalized && !result->worker_exception &&
          result->notification_error == ERROR_SUCCESS);
    CHECK(!worker.TryTake(generation)); // duplicate message cannot finish twice
    CHECK(!worker.has_worker());
}

void TestLostNotificationAndStaleGeneration(HWND destroyed_owner, HWND owner) {
    RecordingFinalizer worker;
    CHECK(worker.Start(destroyed_owner, kComplete, [] { return true; }));
    const auto old_generation = worker.generation();
    const auto started = std::chrono::steady_clock::now();
    const auto recovered = Poll(worker, 250ms); // real production recovery cadence
    CHECK(recovered.has_value() && recovered->finalized &&
          recovered->notification_error == ERROR_INVALID_WINDOW_HANDLE);
    CHECK(std::chrono::steady_clock::now() - started < 1s);
    CHECK(!worker.TryTake(old_generation));

    std::promise<void> release;
    auto gate = release.get_future().share();
    CHECK(worker.Start(owner, kComplete, [gate] { gate.wait(); return false; }));
    CHECK(worker.generation() != old_generation);
    const auto before = std::chrono::steady_clock::now();
    CHECK(!worker.TryTake(old_generation)); // late prior-session message
    CHECK(!worker.TryTake(worker.generation())); // early current-session message
    CHECK(std::chrono::steady_clock::now() - before < 100ms);
    CHECK(!worker.Start(owner, kComplete, [] { return true; }));
    release.set_value();
    const auto failed = Poll(worker);
    CHECK(failed.has_value() && !failed->finalized && !failed->worker_exception);
}

void TestJoinAndRepeatedSessions(HWND owner) {
    RecordingFinalizer worker;
    for (int i = 0; i < 40; ++i) {
        CHECK(worker.Start(owner, kComplete, [i] { return i % 2 == 0; }));
        const auto result = Poll(worker);
        CHECK(result.has_value() && result->finalized == (i % 2 == 0));
        CHECK(!worker.TryTake(worker.generation()));
    }
    CHECK(worker.Start(owner, kComplete, []() -> bool { throw std::runtime_error("failure"); }));
    const auto exception = Poll(worker);
    CHECK(exception.has_value() && !exception->finalized && exception->worker_exception);

    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<bool> finished{false};
    CHECK(worker.Start(owner, kComplete, [gate, &finished] {
        gate.wait(); finished.store(true); return true;
    }));
    std::thread unblock([&release] { std::this_thread::sleep_for(20ms); release.set_value(); });
    worker.Join(); // shutdown must drain before releasing captured owner state
    unblock.join();
    CHECK(finished.load());
    CHECK(!worker.has_worker());
    CHECK(!worker.TryTake(worker.generation()));
}
} // namespace

int main() {
    const HWND owner = ::CreateWindowExW(0, L"STATIC", L"finalizer-test", 0,
                                         0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    const HWND dead = ::CreateWindowExW(0, L"STATIC", L"finalizer-dead", 0,
                                        0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!owner || !dead) return 2;
    ::DestroyWindow(dead);
    TestMessageAndPolling(owner);
    TestLostNotificationAndStaleGeneration(dead, owner);
    TestJoinAndRepeatedSessions(owner);
    ::DestroyWindow(owner);
    std::printf("Finalizer: %d/%d passed\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
