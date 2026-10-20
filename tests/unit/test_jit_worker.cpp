// Brimir - SH-2 JIT compile worker tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"

#include <brimir/jit/compile_worker.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace brimir::jit;

namespace {

// Blocks Finalize until opened. Only the main test thread makes Catch2 assertions.
struct Latch {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    std::atomic<bool> entered{false};

    void Open() {
        {
            std::lock_guard lock{m};
            open = true;
        }
        cv.notify_all();
    }
    void Wait() {
        entered = true;
        std::unique_lock lock{m};
        cv.wait(lock, [&] { return open; });
    }
};

std::atomic<int> g_live{0};

class TestCompile final : public PreparedCompile {
public:
    TestCompile(bool result, std::shared_ptr<Latch> latch = nullptr, std::atomic<int> *finalized = nullptr)
        : m_result(result)
        , m_latch(std::move(latch))
        , m_finalized(finalized) {
        ++g_live;
    }
    ~TestCompile() override {
        --g_live;
    }

    bool Finalize() noexcept override {
        m_thread = std::this_thread::get_id();
        if (m_latch) {
            m_latch->Wait();
        }
        if (m_finalized) {
            ++*m_finalized;
        }
        m_done = true;
        return m_result;
    }

    std::thread::id m_thread{};
    bool m_done = false;

private:
    bool m_result;
    std::shared_ptr<Latch> m_latch;
    std::atomic<int> *m_finalized;
};

template <typename Pred>
bool WaitUntil(Pred pred, std::chrono::milliseconds limit = std::chrono::milliseconds(10000)) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

CompileJob Job(const void *owner, uint64_t ticket, bool result = true, std::shared_ptr<Latch> latch = nullptr,
               std::atomic<int> *finalized = nullptr) {
    return CompileJob{owner, ticket, std::make_unique<TestCompile>(result, std::move(latch), finalized)};
}

} // namespace

TEST_CASE("Worker: finalizes on another thread and returns results to their owner", "[jit][worker]") {
    REQUIRE(g_live == 0);
    {
        CompileWorker worker;
        int ownerA = 0, ownerB = 0;
        // Odd tickets fail Finalize.
        for (uint64_t i = 0; i < 3; i++) {
            worker.Submit(Job(&ownerA, 10 + i, (i % 2) == 0));
            worker.Submit(Job(&ownerB, 20 + i, (i % 2) == 0));
        }

        std::vector<CompileResult> a, b;
        REQUIRE(WaitUntil([&] {
            worker.TakeResults(&ownerA, a);
            worker.TakeResults(&ownerB, b);
            return a.size() >= 3 && b.size() >= 3;
        }));
        REQUIRE(a.size() == 3);
        REQUIRE(b.size() == 3);
        for (uint64_t i = 0; i < 3; i++) {
            CHECK(a[i].ticket == 10 + i);
            CHECK(a[i].ok == ((i % 2) == 0));
            CHECK(b[i].ticket == 20 + i);
            CHECK(b[i].ok == ((i % 2) == 0));
            auto *ca = static_cast<TestCompile *>(a[i].prepared.get());
            auto *cb = static_cast<TestCompile *>(b[i].prepared.get());
            REQUIRE(ca != nullptr);
            REQUIRE(cb != nullptr);
            CHECK(ca->m_done);
            CHECK(cb->m_done);
            CHECK(ca->m_thread != std::this_thread::get_id());
            CHECK(cb->m_thread != std::this_thread::get_id());
        }
        CHECK(worker.Pending(&ownerA) == 0);
        CHECK(worker.Pending(&ownerB) == 0);
        CHECK_FALSE(worker.HasResults(&ownerA));
        CHECK_FALSE(worker.HasResults(&ownerB));
    }
    CHECK(g_live == 0);
}

TEST_CASE("Worker: Cancel drops queued jobs and waits for the job in flight", "[jit][worker]") {
    REQUIRE(g_live == 0);
    {
        CompileWorker worker;
        int ownerA = 0, ownerB = 0;
        auto latch = std::make_shared<Latch>();
        std::atomic<int> finalizedA{0};

        worker.Submit(Job(&ownerA, 1, true, latch, &finalizedA));
        REQUIRE(WaitUntil([&] { return latch->entered.load(); }));
        worker.Submit(Job(&ownerA, 2, true, nullptr, &finalizedA));
        worker.Submit(Job(&ownerA, 3, true, nullptr, &finalizedA));
        CHECK(worker.Pending(&ownerA) == 3);

        std::atomic<bool> cancelled{false};
        std::thread canceller([&] {
            worker.Cancel(&ownerA);
            cancelled = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_FALSE(cancelled.load());

        // Another owner is not held up by A's pending Cancel.
        worker.Submit(Job(&ownerB, 100));
        latch->Open();
        canceller.join();
        CHECK(cancelled.load());

        std::vector<CompileResult> a;
        worker.TakeResults(&ownerA, a);
        CHECK(a.empty());
        CHECK_FALSE(worker.HasResults(&ownerA));
        CHECK(worker.Pending(&ownerA) == 0);
        CHECK(finalizedA == 1); // the queued jobs never ran

        std::vector<CompileResult> b;
        REQUIRE(WaitUntil([&] {
            worker.TakeResults(&ownerB, b);
            return !b.empty();
        }));
        REQUIRE(b.size() == 1);
        CHECK(b[0].ticket == 100);
        CHECK(b[0].ok);

        // A can submit again after a Cancel.
        worker.Submit(Job(&ownerA, 4));
        REQUIRE(WaitUntil([&] {
            worker.TakeResults(&ownerA, a);
            return !a.empty();
        }));
        CHECK(a.size() == 1);
        CHECK(a[0].ticket == 4);

        // Cancel with nothing queued is a no-op, as is a Cancel of an unknown owner.
        worker.Cancel(&ownerA);
        worker.Cancel(&ownerB);
        int unknown = 0;
        worker.Cancel(&unknown);
    }
    CHECK(g_live == 0);
}

TEST_CASE("Worker: Cancel drops the result of a job finished before it", "[jit][worker]") {
    REQUIRE(g_live == 0);
    {
        CompileWorker worker;
        int owner = 0;
        worker.Submit(Job(&owner, 1));
        REQUIRE(WaitUntil([&] { return worker.HasResults(&owner); }));
        worker.Cancel(&owner);
        CHECK_FALSE(worker.HasResults(&owner));
        std::vector<CompileResult> out;
        worker.TakeResults(&owner, out);
        CHECK(out.empty());
        CHECK(g_live == 0);
    }
}

TEST_CASE("Worker: destruction with jobs queued and in flight neither hangs nor leaks", "[jit][worker]") {
    REQUIRE(g_live == 0);
    {
        auto latch = std::make_shared<Latch>();
        std::atomic<int> finalized{0};
        std::thread releaser;
        {
            CompileWorker worker;
            int owner = 0;
            worker.Submit(Job(&owner, 0, true, latch, &finalized));
            for (uint64_t i = 1; i < 100; i++) {
                worker.Submit(Job(&owner, i, true, nullptr, &finalized));
            }
            REQUIRE(WaitUntil([&] { return latch->entered.load(); }));
            releaser = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                latch->Open();
            });
            // Destroyed while the first job is blocked, then released while the destructor waits.
        }
        releaser.join();
        CHECK(finalized >= 1);
    }
    CHECK(g_live == 0);
}

TEST_CASE("Worker: destruction with undelivered results and jobs queued after release", "[jit][worker]") {
    REQUIRE(g_live == 0);
    {
        auto latch = std::make_shared<Latch>();
        CompileWorker worker;
        int owner = 0;
        worker.Submit(Job(&owner, 0, true, latch));
        for (uint64_t i = 1; i < 100; i++) {
            worker.Submit(Job(&owner, i));
        }
        REQUIRE(WaitUntil([&] { return latch->entered.load(); }));
        latch->Open();
        REQUIRE(WaitUntil([&] { return worker.HasResults(&owner); }));
    }
    CHECK(g_live == 0);
}

TEST_CASE("Worker: HasResults reports finished results", "[jit][worker]") {
    REQUIRE(g_live == 0);
    {
        CompileWorker worker;
        int owner = 0, other = 0;
        CHECK_FALSE(worker.HasResults(&owner));

        worker.Submit(Job(&owner, 7));
        REQUIRE(WaitUntil([&] { return worker.HasResults(&owner); }));
        CHECK_FALSE(worker.HasResults(&other));
        CHECK(worker.Pending(&owner) == 0);

        std::vector<CompileResult> out;
        worker.TakeResults(&owner, out);
        REQUIRE(out.size() == 1);
        CHECK(out[0].ticket == 7);
        CHECK_FALSE(worker.HasResults(&owner));

        out.clear();
        worker.TakeResults(&owner, out);
        CHECK(out.empty());
    }
    CHECK(g_live == 0);
}

TEST_CASE("Worker: BusyNs counts time spent in Finalize", "[jit][worker]") {
    CompileWorker worker;
    int owner = 0;
    CHECK(worker.BusyNs() == 0);
    auto latch = std::make_shared<Latch>();
    worker.Submit(Job(&owner, 1, true, latch));
    REQUIRE(WaitUntil([&] { return latch->entered.load(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    latch->Open();
    REQUIRE(WaitUntil([&] { return worker.HasResults(&owner); }));
    CHECK(worker.BusyNs() >= 10'000'000);
}
