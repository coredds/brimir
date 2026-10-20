#pragma once
// Background compile worker (design/sh2-jit-m2e.md). One thread runs PreparedCompile::Finalize for
// jobs submitted by any number of owners (the block caches of both CPUs) and parks the results
// until the owner takes them on its own thread.
//
// The worker never calls into an owner, a backend or a block cache: it only runs Finalize and
// moves objects between its queues. The owner pointer is an opaque key.
//
// Synchronization: the queue, the results and the in-flight record are guarded by one mutex. A
// job's Finalize runs with the mutex released, on a PreparedCompile no one else touches. The worker
// then takes the mutex to park the result, and TakeResults takes the same mutex to hand it over, so
// everything Finalize wrote happens before the owner's reads of it (Commit); likewise everything
// the owner did to the object before Submit happens before Finalize. HasResults is only a hint:
// use TakeResults for the data.
//
// Which thread destroys a PreparedCompile: a job cancelled while in flight is destroyed on the
// worker thread (before Cancel returns); every other job or result the worker still holds is
// destroyed on the thread calling Cancel or the destructor, and taken results by their taker. So a
// PreparedCompile destructor must touch only the object's own state.
#include <brimir/jit/backend.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace brimir::jit {

struct CompileJob {
    const void *owner;
    uint64_t ticket;
    std::unique_ptr<PreparedCompile> prepared;
};

struct CompileResult {
    uint64_t ticket;
    std::unique_ptr<PreparedCompile> prepared;
    bool ok; // Finalize's return value
};

class CompileWorker {
public:
    // Starts the worker thread.
    CompileWorker();
    // Drops the queued jobs and the undelivered results, waits for the job in flight and joins.
    ~CompileWorker();

    CompileWorker(const CompileWorker &) = delete;
    CompileWorker &operator=(const CompileWorker &) = delete;

    // Queues a job; jobs are finalized in submission order.
    void Submit(CompileJob job);

    // Appends the owner's finished results to out, in completion order, and forgets them.
    void TakeResults(const void *owner, std::vector<CompileResult> &out);

    // True if the owner has results to take. Takes no lock while at most kCountedOwners owners have
    // results (production has two, the caches of both CPUs): it reads the owner's count. Beyond
    // that, the extra owners' results are counted together, and while any are parked, a call for an
    // owner with no count takes the lock to look.
    bool HasResults(const void *owner) const;

    static constexpr size_t kCountedOwners = 4;

    // Drops the owner's queued jobs and undelivered results and returns only after the owner's job
    // in flight, if any, has finished; its result is dropped too. Afterwards nothing of the owner
    // is queued, running or waiting, until it submits again, and the worker holds no reference to
    // any of its PreparedCompile objects. Safe to call with nothing queued, and while the worker
    // runs another owner's job. The owner must not Submit concurrently with its own Cancel.
    void Cancel(const void *owner);

    // Total time the worker spent in Finalize since construction.
    uint64_t BusyNs() const { return m_busyNs.load(std::memory_order_relaxed); }

    // The owner's queued jobs plus its job in flight.
    size_t Pending(const void *owner) const;

private:
    struct Parked {
        const void *owner;
        CompileResult result;
    };

    // A parked-result count for one owner. Written only under the mutex (owner, then count when
    // claimed; count, then owner when released); read without it by HasResults.
    struct OwnerCount {
        std::atomic<const void *> owner{nullptr}; // nullptr: free
        std::atomic<size_t> count{0};
    };

    void Run();

    // Counts one more parked result of owner (mutex held).
    void CountParkedLocked(const void *owner);
    // Recomputes every count from m_results after results were removed (mutex held).
    void RecountLocked();
    // HasResults from the counts alone; nullopt if only the lock can tell (uncounted results).
    std::optional<bool> HasResultsUnlocked(const void *owner) const;

    mutable std::mutex m_mutex;
    std::condition_variable m_workCv; // a job was queued, or stop
    std::condition_variable m_doneCv; // the job in flight finished
    std::deque<CompileJob> m_queue;
    std::vector<Parked> m_results;
    bool m_stop = false;
    bool m_running = false; // a job is in flight
    const void *m_runningOwner = nullptr;
    bool m_runningCancelled = false; // Cancel hit the job in flight: drop its result
    std::array<OwnerCount, kCountedOwners> m_counts;
    // Parked results of owners without an OwnerCount, written under the mutex.
    std::atomic<size_t> m_uncounted{0};
    std::atomic<uint64_t> m_busyNs{0};
    std::thread m_thread; // last: starts once everything above is constructed
};

} // namespace brimir::jit
