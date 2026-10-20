// Brimir - SH-2 JIT compile worker tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "jit_test_backend.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/jit/block_cache.hpp>
#include <brimir/jit/compile_worker.hpp>
#include <brimir/jit/executor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
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

        std::atomic<bool> cancelling{false};
        std::atomic<bool> cancelled{false};
        std::thread canceller([&] {
            cancelling = true;
            worker.Cancel(&ownerA);
            cancelled = true;
        });
        // Once the canceller is about to call Cancel, give it time to block in it.
        CHECK(WaitUntil([&] { return cancelling.load(); })); // CHECK: the thread must be joined below
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

TEST_CASE("Worker: HasResults is per owner", "[jit][worker]") {
    REQUIRE(g_live == 0);
    {
        CompileWorker worker;
        int ownerA = 0, ownerB = 0;
        worker.Submit(Job(&ownerA, 1));
        worker.Submit(Job(&ownerA, 2));
        REQUIRE(WaitUntil([&] { return worker.Pending(&ownerA) == 0; }));
        CHECK(worker.HasResults(&ownerA));
        CHECK_FALSE(worker.HasResults(&ownerB)); // A's results are not B's
        std::vector<CompileResult> out;
        worker.TakeResults(&ownerB, out);
        CHECK(out.empty());
        CHECK(worker.HasResults(&ownerA));

        worker.TakeResults(&ownerA, out);
        CHECK(out.size() == 2);
        CHECK_FALSE(worker.HasResults(&ownerA));
        CHECK_FALSE(worker.HasResults(&ownerB));
    }
    CHECK(g_live == 0);
}

TEST_CASE("Worker: HasResults with more owners than counted ones", "[jit][worker]") {
    REQUIRE(g_live == 0);
    {
        CompileWorker worker;
        std::array<int, 8> owners{};
        int none = 0;
        for (size_t i = 0; i < owners.size(); i++) {
            for (uint64_t t = 0; t <= i; t++) {
                worker.Submit(Job(&owners[i], i * 100 + t));
            }
        }
        REQUIRE(WaitUntil([&] {
            for (const int &o : owners) {
                if (worker.Pending(&o) != 0) {
                    return false;
                }
            }
            return true;
        }));
        for (const int &o : owners) {
            CHECK(worker.HasResults(&o));
        }
        CHECK_FALSE(worker.HasResults(&none));

        // Taking or cancelling some owners' results leaves the others' reported.
        for (size_t i = 0; i < owners.size(); i += 2) {
            std::vector<CompileResult> out;
            worker.TakeResults(&owners[i], out);
            CHECK(out.size() == i + 1);
        }
        worker.Cancel(&owners[1]);
        for (size_t i = 0; i < owners.size(); i++) {
            INFO("owner " << i);
            CHECK(worker.HasResults(&owners[i]) == (i % 2 == 1 && i != 1));
        }
        CHECK_FALSE(worker.HasResults(&none));

        // New results after the counts changed.
        worker.Submit(Job(&owners[0], 1000));
        REQUIRE(WaitUntil([&] { return worker.HasResults(&owners[0]); }));
        for (size_t i = 1; i < owners.size(); i += 2) {
            std::vector<CompileResult> out;
            worker.TakeResults(&owners[i], out);
            CHECK(out.size() == (i == 1 ? 0 : i + 1));
        }
        for (size_t i = 1; i < owners.size(); i++) {
            CHECK_FALSE(worker.HasResults(&owners[i]));
        }
        std::vector<CompileResult> out;
        worker.TakeResults(&owners[0], out);
        REQUIRE(out.size() == 1);
        CHECK(out[0].ticket == 1000);
        CHECK_FALSE(worker.HasResults(&owners[0]));
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

// --- Background compiles in the block cache ---------------------------------------------------

namespace {

using sh2test::Rig;

constexpr uint32_t kCode = 0x06001000;
constexpr uint32_t kThreshold = 2;
constexpr uint16_t kSleepOp = static_cast<uint16_t>(sh2test::kSleep);

uint16_t AddI(uint32_t n, uint32_t imm) {
    return static_cast<uint16_t>(0x7000 | (n << 8) | (imm & 0xFF));
}
uint16_t MovLL(uint32_t n, uint32_t m) {
    return static_cast<uint16_t>(0x6002 | (n << 8) | (m << 4));
}

// Two 3-instruction blocks at the same PC (SLEEP ends them).
const std::vector<uint16_t> kProgramP{AddI(3, 1), AddI(3, 2), AddI(3, 3), kSleepOp};
const std::vector<uint16_t> kProgramQ{AddI(3, 5), AddI(4, 1), AddI(3, 7), kSleepOp};

bool WaitForResults(CompileWorker &worker, const void *owner) {
    return WaitUntil([&] { return worker.Pending(owner) == 0 && worker.HasResults(owner); },
                     std::chrono::milliseconds(5000));
}

// Keeps the worker busy with another owner's job until opened (at the latest when destroyed, so a
// failed REQUIRE never leaves the worker's destructor waiting): the cache's jobs queue behind it.
// Declare it after the worker.
struct WorkerBlocker {
    std::shared_ptr<Latch> latch = std::make_shared<Latch>();
    int owner = 0;

    explicit WorkerBlocker(CompileWorker &worker) {
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

struct LatchOpener {
    std::shared_ptr<Latch> latch;
    ~LatchOpener() {
        latch->Open();
    }
};

// An x64 executor with a worker and threshold 2 next to a reference interpreter rig.
struct AsyncPair {
    std::unique_ptr<Rig> ref = std::make_unique<Rig>();
    std::unique_ptr<Rig> jit = std::make_unique<Rig>();
    Executor exec;

    explicit AsyncPair(CompileWorker &worker)
        : exec{BackendKind::X64, kThreshold, &worker} {}

    const void *Owner() const {
        return &exec.Cache();
    }
    const BlockCache &Cache() const {
        return exec.Cache();
    }
    const CachedBlock *Entry(uint32_t pc = kCode) const {
        return exec.Cache().Find(pc);
    }
    void WriteCode(uint32_t address, const std::vector<uint16_t> &words) {
        ref->WriteCode(address, words);
        jit->WriteCode(address, words);
    }
    void Write32(uint32_t address, uint32_t value) {
        ref->Write32(address, value);
        jit->Write32(address, value);
    }
    void Load(const ymir::savestate::SH2SaveState &state) {
        ref->Load(state);
        jit->Load(state);
    }

    // One executor step on the JIT rig and the same instructions on the reference rig; they must match.
    ExitInfo Step() {
        const ExitInfo info = exec.Step(jit->sh2->GetJitContext());
        uint64_t refCycles = 0;
        for (uint32_t i = 0; i < info.retired; ++i) {
            refCycles += ref->sh2->Step<false, false>();
        }
        if (info.busWait) {
            refCycles += ref->sh2->Step<false, false>();
        }
        CHECK(info.cycles == refCycles);
        const std::string diff = sh2test::DiffRigs(*ref, *jit);
        INFO(diff);
        CHECK(diff.empty());
        return info;
    }
    // Step from pc (the registers are kept).
    ExitInfo StepAt(uint32_t pc) {
        Load(ref->BaseState(pc));
        return Step();
    }
};

// Wraps the x64 backend; its prepared compiles block in Finalize until the latch opens.
class LatchedBackend final : public INativeBackend {
public:
    static inline std::atomic<int> live{0}; // its prepared compiles alive

    explicit LatchedBackend(std::shared_ptr<Latch> latch)
        : m_inner(MakeNativeBackend(BackendKind::X64))
        , m_latch(std::move(latch)) {}

    BackendKind Kind() const override {
        return m_inner->Kind();
    }
    std::unique_ptr<PreparedCompile> Prepare(const Block &block, const ymir::sh2::SH2JitContext &ctx) override {
        auto inner = m_inner->Prepare(block, ctx);
        if (inner == nullptr) {
            return nullptr;
        }
        return std::make_unique<Latched>(std::move(inner), m_latch);
    }
    bool Commit(PreparedCompile &prepared, NativeCode &out) override {
        return m_inner->Commit(*static_cast<Latched &>(prepared).inner, out);
    }
    ExitInfo Run(const NativeCode &code, ymir::sh2::SH2JitContext &ctx, uint64_t target, const bool *abortRequested,
                 bool allowChain) override {
        return m_inner->Run(code, ctx, target, abortRequested, allowChain);
    }
    void Publish(uint32_t pc, const NativeCode &code) override {
        m_inner->Publish(pc, code);
    }
    void Unpublish(uint32_t pc) override {
        m_inner->Unpublish(pc);
    }
    void Release(uint32_t pc, const NativeCode &code) override {
        m_inner->Release(pc, code);
    }
    void Reset() override {
        m_inner->Reset();
    }
    size_t CodeBytes() const override {
        return m_inner->CodeBytes();
    }

private:
    struct Latched final : PreparedCompile {
        Latched(std::unique_ptr<PreparedCompile> p, std::shared_ptr<Latch> l)
            : inner(std::move(p))
            , latch(std::move(l)) {
            ++live;
        }
        ~Latched() override {
            --live;
        }
        bool Finalize() noexcept override {
            latch->Wait();
            return inner->Finalize();
        }
        std::unique_ptr<PreparedCompile> inner;
        std::shared_ptr<Latch> latch;
    };

    std::unique_ptr<INativeBackend> m_inner;
    std::shared_ptr<Latch> m_latch;
};

} // namespace

TEST_CASE("Background compile: the block runs on IR until its native code is installed", "[jit][worker]") {
    if (!IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend in this build");
    }
    CompileWorker worker;
    AsyncPair p{worker};
    p.WriteCode(kCode, kProgramP);

    REQUIRE(p.StepAt(kCode).retired == 3); // built, runs on IR
    REQUIRE(p.Entry() != nullptr);
    CHECK(p.Entry()->code.entry == nullptr);
    CHECK(p.Cache().PendingCompiles() == 0);

    REQUIRE(p.StepAt(kCode).retired == 3); // the threshold run submits and still runs on IR
    CHECK(p.Cache().PendingCompiles() == 1);
    CHECK(p.Entry()->code.entry == nullptr);
    CHECK(p.Entry()->pendingTicket != 0);
    CHECK(p.exec.GetStats().nativeBlocksRun == 0);

    REQUIRE(WaitForResults(worker, p.Owner()));
    REQUIRE(p.StepAt(kCode).retired == 3); // installed at the start of Get, runs native
    CHECK(p.Entry()->code.entry != nullptr);
    CHECK(p.Entry()->pendingTicket == 0);
    CHECK(p.Entry()->block.code.empty());
    CHECK(p.exec.GetStats().nativeBlocksRun == 1);
    CHECK(p.Cache().AsyncCompiles() == 1);
    CHECK(p.Cache().AsyncDropped() == 0);
    CHECK(p.Cache().NativeCompiles() == 1);
    CHECK(p.Cache().CompileFallbacks() == 0);
    CHECK(p.Cache().PendingCompiles() == 0);
    CHECK(p.Cache().CachedInsts() == 0);
    CHECK(p.Cache().NativeCompileNs() > 0);

    REQUIRE(p.StepAt(kCode).retired == 3);
    CHECK(p.exec.GetStats().nativeBlocksRun == 2);
    CHECK(p.Cache().Compiles() == 1);
}

TEST_CASE("Background compile: a result from before a flush is dropped", "[jit][worker]") {
    if (!IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend in this build");
    }
    CompileWorker worker;
    AsyncPair p{worker};
    p.WriteCode(kCode, kProgramP);
    REQUIRE(p.StepAt(kCode).retired == 3);
    REQUIRE(p.StepAt(kCode).retired == 3); // submits
    REQUIRE(p.Cache().PendingCompiles() == 1);
    const uint64_t oldTicket = p.Entry()->pendingTicket;

    p.exec.Flush();
    CHECK(p.Cache().PendingCompiles() == 0);
    CHECK(p.Cache().AsyncDropped() == 1);
    CHECK(worker.Pending(p.Owner()) == 0);
    CHECK_FALSE(worker.HasResults(p.Owner()));

    REQUIRE(p.StepAt(kCode).retired == 3); // rebuilt at the same PC
    CHECK(p.Cache().Compiles() == 2);
    REQUIRE(p.Entry() != nullptr);
    CHECK(p.Entry()->code.entry == nullptr);
    CHECK(p.Entry()->pendingTicket == 0);
    REQUIRE(WaitUntil([&] { return worker.Pending(p.Owner()) == 0; }));

    REQUIRE(p.StepAt(kCode).retired == 3); // nothing of the old generation is installed
    CHECK(p.Entry()->code.entry == nullptr);
    CHECK(p.Cache().AsyncCompiles() == 0);
    CHECK(p.Cache().AsyncDropped() == 1);
    CHECK(p.exec.GetStats().nativeBlocksRun == 0);
    CHECK(p.Entry()->pendingTicket != 0); // its own compile (threshold run)
    CHECK(p.Entry()->pendingTicket != oldTicket);

    REQUIRE(WaitForResults(worker, p.Owner()));
    REQUIRE(p.StepAt(kCode).retired == 3);
    CHECK(p.Entry()->code.entry != nullptr);
    CHECK(p.Cache().AsyncCompiles() == 1);
    CHECK(p.Cache().AsyncDropped() == 1);
}

TEST_CASE("Background compile: a result for a dropped block is not installed into its successor", "[jit][worker]") {
    if (!IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend in this build");
    }
    CompileWorker worker;
    WorkerBlocker blocker{worker};
    REQUIRE(blocker.Entered());
    AsyncPair p{worker};
    p.WriteCode(kCode, kProgramP);
    REQUIRE(p.StepAt(kCode).retired == 3);
    REQUIRE(p.StepAt(kCode).retired == 3); // submits P; queued behind the blocker
    REQUIRE(p.Cache().PendingCompiles() == 1);
    const uint64_t ticketP = p.Entry()->pendingTicket;
    CHECK(worker.Pending(p.Owner()) == 1);

    p.WriteCode(kCode, kProgramQ);
    REQUIRE(p.StepAt(kCode).retired == 3); // P is dropped, Q built at the same PC
    CHECK(p.Cache().Invalidations() == 1);
    CHECK(p.Cache().Compiles() == 2);
    REQUIRE(p.Entry() != nullptr);
    CHECK(p.Entry()->block.guestOpcodes.front() == kProgramQ.front());
    CHECK(p.Entry()->code.entry == nullptr);
    CHECK(p.Entry()->pendingTicket == 0);

    blocker.Open();
    REQUIRE(WaitForResults(worker, p.Owner()));
    REQUIRE(p.StepAt(kCode).retired == 3); // P's result arrives and is dropped
    CHECK(p.Entry()->code.entry == nullptr);
    CHECK(p.Cache().AsyncDropped() == 1);
    CHECK(p.Cache().AsyncCompiles() == 0);
    CHECK(p.exec.GetStats().nativeBlocksRun == 0);
    CHECK(p.Entry()->pendingTicket != 0); // Q's threshold run submitted Q
    CHECK(p.Entry()->pendingTicket != ticketP);

    REQUIRE(WaitForResults(worker, p.Owner()));
    REQUIRE(p.StepAt(kCode).retired == 3); // Q's native code runs Q
    CHECK(p.Entry()->code.entry != nullptr);
    CHECK(p.Cache().AsyncCompiles() == 1);
    CHECK(p.Cache().AsyncDropped() == 1);
    CHECK(p.exec.GetStats().nativeBlocksRun == 1);
    CHECK(p.exec.GetStats().staleEntries == 0);
}

// A pending IR block retired to the IR pool gives up its ticket: taken back before its result
// arrives, it is the same entry object again, but the result is still dropped.
TEST_CASE("Background compile: a result for a retired IR block is not installed after its reuse", "[jit][worker]") {
    if (!IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend in this build");
    }
    CompileWorker worker;
    WorkerBlocker blocker{worker};
    REQUIRE(blocker.Entered());
    AsyncPair p{worker};
    p.WriteCode(kCode, kProgramP);
    REQUIRE(p.StepAt(kCode).retired == 3);
    REQUIRE(p.StepAt(kCode).retired == 3); // submits P; queued behind the blocker
    REQUIRE(p.Cache().PendingCompiles() == 1);
    const CachedBlock *entryP = p.Entry();
    const uint64_t ticketP = entryP->pendingTicket;
    REQUIRE(ticketP != 0);

    p.WriteCode(kCode, kProgramQ);
    REQUIRE(p.StepAt(kCode).retired == 3); // P retired to the IR pool, Q built
    CHECK(p.Cache().RetiredIrInsts() > 0);
    p.WriteCode(kCode, kProgramP);
    REQUIRE(p.StepAt(kCode).retired == 3); // Q retired, P taken back before its result
    CHECK(p.Cache().RetiredIrReuses() == 1);
    CHECK(p.Cache().Compiles() == 2);
    REQUIRE(p.Entry() == entryP);
    CHECK(p.Entry()->pendingTicket == 0);
    CHECK(p.Entry()->code.entry == nullptr);
    CHECK(p.Entry()->runsUntilNative == kThreshold - 1);

    blocker.Open();
    REQUIRE(WaitForResults(worker, p.Owner()));
    REQUIRE(p.StepAt(kCode).retired == 3); // P's old result arrives and is dropped
    CHECK(p.Cache().AsyncDropped() == 1);
    CHECK(p.Cache().AsyncCompiles() == 0);
    CHECK(p.exec.GetStats().nativeBlocksRun == 0);
    REQUIRE(p.Entry() == entryP);
    CHECK(p.Entry()->code.entry == nullptr);
    CHECK(p.Entry()->pendingTicket != 0); // this was its threshold run: submitted again
    CHECK(p.Entry()->pendingTicket != ticketP);

    REQUIRE(WaitForResults(worker, p.Owner()));
    REQUIRE(p.StepAt(kCode).retired == 3); // compiled again, runs natively
    CHECK(p.Entry()->code.entry != nullptr);
    CHECK(p.Cache().AsyncCompiles() == 1);
    CHECK(p.Cache().AsyncDropped() == 1);
    CHECK(p.exec.GetStats().nativeBlocksRun == 1);
    CHECK(p.exec.GetStats().staleEntries == 0);
    CHECK(p.Cache().Compiles() == 2);
}

TEST_CASE("Background compile: a page remapped before the commit is caught at entry", "[jit][worker]") {
    if (!IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend in this build");
    }
    constexpr uint32_t kData = 0x06002000; // same 64 KiB page as the code
    CompileWorker worker;
    // Declared before the rigs: their buses point at these pages until the rigs are gone.
    std::array<std::unique_ptr<std::array<uint8_t, 0x10000>>, 2> pages;
    AsyncPair p{worker};
    // add #1,R3 ; mov.l @R9,R2 ; add #1,R3 ; sleep
    p.WriteCode(kCode, {AddI(3, 1), MovLL(2, 9), AddI(3, 1), kSleepOp});
    p.Write32(kData, 0x11111111);
    auto state = p.ref->BaseState(kCode);
    state.R[3] = 0;
    state.R[9] = kData;
    p.Load(state);
    REQUIRE(p.Step().retired == 3);
    REQUIRE(p.StepAt(kCode).retired == 3); // Prepare sees the RAM arrays
    REQUIRE(p.Cache().PendingCompiles() == 1);

    // Remap bus 0x6000000-0x600FFFF (SH-2 0x06000000) on both rigs to a copy with new data.
    Rig *rigs[2] = {p.ref.get(), p.jit.get()};
    for (int i = 0; i < 2; ++i) {
        pages[i] = std::make_unique<std::array<uint8_t, 0x10000>>();
        std::copy_n(rigs[i]->ram->begin(), 0x10000, pages[i]->begin());
        std::fill_n(pages[i]->begin() + 0x2000, 4, uint8_t{0x22}); // kData: 0x22222222
        rigs[i]->bus.MapArray(0x6000000, 0x600FFFF, *pages[i], true);
    }

    REQUIRE(WaitForResults(worker, p.Owner()));
    REQUIRE(p.StepAt(kCode).retired == 3); // committed, stale at entry, rebuilt and run on IR
    CHECK(p.jit->State().R[2] == 0x22222222u);
    CHECK(p.Cache().AsyncCompiles() == 1);
    CHECK(p.exec.GetStats().staleEntries == 1);
    CHECK(p.Cache().Invalidations() == 1);
    CHECK(p.Cache().Compiles() == 2);
    CHECK(p.Cache().RetiredReuses() == 0);
    REQUIRE(p.Entry() != nullptr);
    CHECK(p.Entry()->code.entry == nullptr);

    REQUIRE(p.StepAt(kCode).retired == 3); // the new block's threshold run
    REQUIRE(WaitForResults(worker, p.Owner()));
    REQUIRE(p.StepAt(kCode).retired == 3);
    CHECK(p.Entry()->code.entry != nullptr);
    CHECK(p.Cache().AsyncCompiles() == 2);
    CHECK(p.exec.GetStats().staleEntries == 1);
    CHECK(p.exec.GetStats().nativeBlocksRun == 1);
}

TEST_CASE("Background compile: destroying an executor with a compile in flight", "[jit][worker]") {
    if (!IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend in this build");
    }
    REQUIRE(LatchedBackend::live == 0);
    CompileWorker worker;
    auto latch = std::make_shared<Latch>();
    Rig rig;
    rig.WriteCode(kCode, kProgramP);
    rig.Load(rig.BaseState(kCode));
    // What an executor holds, destroyed in its order (the cache, then the backend).
    auto backend = std::make_unique<LatchedBackend>(latch);
    auto cache = std::make_unique<BlockCache>(backend.get(), kMaxNativeCodeBytes, kThreshold, kMaxCachedInsts,
                                              kMaxRetiredCodeBytes, kMaxStaleDampShift, &worker);
    const LatchOpener opener{latch}; // destroyed before the cache, so a failed REQUIRE cannot hang
    const void *owner = cache.get();
    auto &ctx = rig.sh2->GetJitContext();
    cache->Get(ctx, kCode);
    cache->Get(ctx, kCode); // the threshold run submits
    REQUIRE(cache->PendingCompiles() == 1);
    REQUIRE(WaitUntil([&] { return latch->entered.load(); })); // its Finalize is running

    // The other CPU's job, queued behind it.
    AsyncPair other{worker};
    other.WriteCode(kCode, kProgramP);
    REQUIRE(other.StepAt(kCode).retired == 3);
    REQUIRE(other.StepAt(kCode).retired == 3);
    REQUIRE(worker.Pending(other.Owner()) == 1);

    std::atomic<bool> destroying{false};
    std::atomic<bool> destroyed{false};
    std::thread destroyer([&] {
        destroying = true;
        cache.reset();
        backend.reset();
        destroyed = true;
    });
    // Only CHECKs until the join: the thread must be joined.
    CHECK(WaitUntil([&] { return destroying.load(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(destroyed.load()); // waits for its job in flight
    latch->Open();
    destroyer.join();

    CHECK(destroyed.load());
    CHECK(worker.Pending(owner) == 0);
    CHECK_FALSE(worker.HasResults(owner));
    CHECK(LatchedBackend::live == 0);

    REQUIRE(WaitForResults(worker, other.Owner()));
    REQUIRE(other.StepAt(kCode).retired == 3);
    CHECK(other.Entry()->code.entry != nullptr);
    CHECK(other.Cache().AsyncCompiles() == 1);
    CHECK(other.Cache().AsyncDropped() == 0);
}

TEST_CASE("Background compile: the pending bound defers compiles", "[jit][worker]") {
    if (!IsBackendAvailable(BackendKind::X64)) {
        SKIP("no x64 backend in this build");
    }
    CompileWorker worker;
    WorkerBlocker blocker{worker};
    REQUIRE(blocker.Entered());
    AsyncPair p{worker};
    // kMaxPendingCompiles + 1 one-instruction blocks: add #1,R3 ; sleep
    std::vector<uint16_t> code;
    for (size_t i = 0; i <= kMaxPendingCompiles; i++) {
        code.push_back(AddI(3, 1));
        code.push_back(kSleepOp);
    }
    p.WriteCode(kCode, code);
    const auto pcOf = [](size_t i) { return kCode + static_cast<uint32_t>(4 * i); };
    for (size_t i = 0; i < kMaxPendingCompiles; i++) {
        REQUIRE(p.StepAt(pcOf(i)).retired == 1);
        REQUIRE(p.StepAt(pcOf(i)).retired == 1);
    }
    CHECK(p.Cache().PendingCompiles() == kMaxPendingCompiles);
    CHECK(worker.Pending(p.Owner()) == kMaxPendingCompiles);

    const uint32_t last = pcOf(kMaxPendingCompiles);
    REQUIRE(p.StepAt(last).retired == 1);
    REQUIRE(p.StepAt(last).retired == 1); // its threshold run: deferred
    CHECK(p.Cache().PendingCompiles() == kMaxPendingCompiles);
    CHECK(worker.Pending(p.Owner()) == kMaxPendingCompiles);
    const CachedBlock *deferred = p.Entry(last);
    REQUIRE(deferred != nullptr);
    CHECK(deferred->code.entry == nullptr);
    CHECK(deferred->pendingTicket == 0);
    CHECK(deferred->runsUntilNative == p.Cache().NativeCompileThreshold());
    CHECK(p.Cache().CompileFallbacks() == 0);

    // Once the queue has drained, it compiles a threshold's runs later.
    blocker.Open();
    REQUIRE(WaitForResults(worker, p.Owner()));
    REQUIRE(p.StepAt(last).retired == 1); // installs the others
    CHECK(p.Cache().AsyncCompiles() == kMaxPendingCompiles);
    CHECK(p.Cache().PendingCompiles() == 0);
    REQUIRE(p.StepAt(last).retired == 1); // submits
    CHECK(p.Cache().PendingCompiles() == 1);
    REQUIRE(WaitForResults(worker, p.Owner()));
    REQUIRE(p.StepAt(last).retired == 1);
    CHECK(p.Entry(last)->code.entry != nullptr);
    CHECK(p.Cache().AsyncCompiles() == kMaxPendingCompiles + 1);
}