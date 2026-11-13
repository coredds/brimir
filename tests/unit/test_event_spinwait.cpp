// Brimir - util::Event::SpinWait (spin-then-block hand-off of the threaded deinterlacer, performance round Task 2b)
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"

#include <ymir/util/event.hpp>

#include <atomic>
#include <chrono>
#include <thread>

TEST_CASE("Event::SpinWait returns at once when the event is set", "[util][event]") {
    util::Event e{true};
    e.SpinWait(0);
    e.SpinWait(1000);
    SUCCEED();
}

TEST_CASE("Event::SpinWait sees a signal set while spinning or after blocking", "[util][event]") {
    // Short delays end within the spin, long ones after the thread blocked in Wait(); both must wake up and see what
    // the setter wrote before Set().
    for (const int delayUs : {0, 5, 50, 2000, 20000}) {
        for (const uint32_t spins : {0u, 16u, 4096u}) {
            util::Event e{false};
            int payload = 0;
            std::thread setter([&] {
                std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
                payload = delayUs + 1;
                e.Set();
            });
            e.SpinWait(spins);
            CHECK(payload == delayUs + 1);
            setter.join();
        }
    }
}

TEST_CASE("Event::SpinWait ping-pong keeps every hand-off", "[util][event]") {
    // The deinterlacer's pattern: begin/end events, each reset by its waiter.
    util::Event begin{false}, end{false};
    std::atomic<int> rendered{0};
    constexpr int kLines = 2000;
    std::thread worker([&] {
        for (int i = 0; i < kLines; ++i) {
            begin.SpinWait(4096);
            begin.Reset();
            rendered.fetch_add(1, std::memory_order_relaxed);
            end.Set();
        }
    });
    for (int i = 0; i < kLines; ++i) {
        begin.Set();
        end.SpinWait(4096);
        end.Reset();
        REQUIRE(rendered.load(std::memory_order_relaxed) == i + 1);
    }
    worker.join();
}
