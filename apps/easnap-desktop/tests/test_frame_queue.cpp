#include <chrono>
#include <cstdio>
#include <future>
#include <memory>

#include "media/frame_queue.h"

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

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

void TestEmptyAndPushPop() {
    easnap::media::FrameQueue<int> queue(2);
    CHECK(queue.capacity() == 2);
    CHECK(queue.empty());
    CHECK(!queue.try_pop().has_value());
    CHECK(queue.try_push(42));
    CHECK(queue.size() == 1);
    auto value = queue.try_pop();
    CHECK(value.has_value());
    CHECK(*value == 42);
    CHECK(queue.empty());
}

void TestFifoFullAndReuse() {
    easnap::media::FrameQueue<int> queue(2);
    CHECK(queue.try_push(1));
    CHECK(queue.try_push(2));
    CHECK(queue.full());
    CHECK(!queue.try_push(3));
    CHECK(*queue.try_pop() == 1);
    CHECK(queue.try_push(3));
    CHECK(*queue.try_pop() == 2);
    CHECK(*queue.try_pop() == 3);
    CHECK(queue.empty());
}

void TestMoveOnlyOwnership() {
    easnap::media::FrameQueue<std::unique_ptr<int>> queue(1);
    auto first = std::make_unique<int>(7);
    CHECK(queue.try_push(std::move(first)));
    CHECK(first == nullptr);

    auto rejected = std::make_unique<int>(9);
    CHECK(!queue.try_push(std::move(rejected)));
    CHECK(rejected != nullptr);
    CHECK(*rejected == 9);

    auto popped = queue.try_pop();
    CHECK(popped.has_value());
    CHECK(**popped == 7);
}

void TestBlockingDrainAndClose() {
    using namespace std::chrono_literals;

    easnap::media::FrameQueue<int> queue(2);
    auto waiting_value = std::async(std::launch::async, [&queue] { return queue.wait_pop(); });
    CHECK(waiting_value.wait_for(20ms) == std::future_status::timeout);
    CHECK(queue.try_push(7));
    CHECK(waiting_value.wait_for(1s) == std::future_status::ready);
    CHECK(*waiting_value.get() == 7);

    CHECK(queue.try_push(11));
    CHECK(queue.try_push(13));
    CHECK(queue.peak_size() == 2);
    queue.close();
    CHECK(queue.closed());
    int rejected = 17;
    CHECK(!queue.try_push(std::move(rejected)));
    CHECK(rejected == 17);
    CHECK(*queue.wait_pop() == 11);
    CHECK(*queue.wait_pop() == 13);
    CHECK(!queue.wait_pop().has_value());
}

}  // namespace

int main() {
    TestEmptyAndPushPop();
    TestFifoFullAndReuse();
    TestMoveOnlyOwnership();
    TestBlockingDrainAndClose();
    if (g_fail == 0) {
        std::printf("PASSED  %d/%d tests\n", g_pass, g_pass + g_fail);
        return 0;
    }
    std::printf("FAILED  %d passed, %d FAILED\n", g_pass, g_fail);
    return 1;
}
