#pragma once

// Compile-worker test helpers shared by the worker and executor tests: a latch that holds Finalize,
// a test compile job, a bounded wait, and a blocker that keeps the worker busy.

#include <brimir/jit/compile_worker.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace sh2test {

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

// TestCompile objects alive.
inline std::atomic<int> g_live{0};

class TestCompile final : public brimir::jit::PreparedCompile {
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

inline brimir::jit::CompileJob Job(const void *owner, uint64_t ticket, bool result = true,
                                   std::shared_ptr<Latch> latch = nullptr, std::atomic<int> *finalized = nullptr) {
    return brimir::jit::CompileJob{owner, ticket,
                                   std::make_unique<TestCompile>(result, std::move(latch), finalized)};
}

// Waits until `owner` has no job queued or in flight and a result is waiting for it.
inline bool WaitForResults(brimir::jit::CompileWorker &worker, const void *owner) {
    return WaitUntil([&] { return worker.Pending(owner) == 0 && worker.HasResults(owner); },
                     std::chrono::milliseconds(5000));
}

// Keeps the worker busy with another owner's job until opened (at the latest when destroyed, so a
// failed REQUIRE never leaves the worker's destructor waiting): the cache's jobs queue behind it.
// Declare it after the worker.
struct WorkerBlocker {
    std::shared_ptr<Latch> latch = std::make_shared<Latch>();
    int owner = 0;

    explicit WorkerBlocker(brimir::jit::CompileWorker &worker) {
        worker.Submit(Job(&owner, 0, true, latch));
    }
    ~WorkerBlocker() {
        latch->Open();
    }
    bool Entered() {
        return WaitUntil([&] { return latch->entered.load(); });
    }
    void Open() {
        latch->Open();
    }
};

} // namespace sh2test
