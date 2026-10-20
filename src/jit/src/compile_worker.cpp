#include <brimir/jit/compile_worker.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace brimir::jit {

CompileWorker::CompileWorker() {
    m_thread = std::thread([this] { Run(); });
}

CompileWorker::~CompileWorker() {
    // Objects are destroyed after the lock is released (a PreparedCompile may be expensive to free).
    std::deque<CompileJob> dropped;
    {
        std::lock_guard lock{m_mutex};
        m_stop = true;
        dropped.swap(m_queue);
    }
    m_workCv.notify_one();
    // The thread finishes the job in flight, sees m_stop and returns.
    m_thread.join();
    // m_results is destroyed with the members; nothing runs concurrently any more.
}

void CompileWorker::Submit(CompileJob job) {
    {
        std::lock_guard lock{m_mutex};
        m_queue.push_back(std::move(job));
    }
    m_workCv.notify_one();
}

void CompileWorker::TakeResults(const void *owner, std::vector<CompileResult> &out) {
    if (m_resultCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::lock_guard lock{m_mutex};
    auto keep = m_results.begin();
    for (auto it = m_results.begin(); it != m_results.end(); ++it) {
        if (it->owner == owner) {
            out.push_back(std::move(it->result));
        } else {
            if (keep != it) {
                *keep = std::move(*it);
            }
            ++keep;
        }
    }
    m_results.erase(keep, m_results.end());
    m_resultCount.store(m_results.size(), std::memory_order_release);
}

bool CompileWorker::HasResults(const void *owner) const {
    if (m_resultCount.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::lock_guard lock{m_mutex};
    return std::any_of(m_results.begin(), m_results.end(), [&](const Parked &p) { return p.owner == owner; });
}

void CompileWorker::Cancel(const void *owner) {
    std::deque<CompileJob> droppedJobs;
    std::vector<Parked> droppedResults;
    {
        std::unique_lock lock{m_mutex};
        // Queued jobs, keeping the others' order.
        for (auto &job : m_queue) {
            if (job.owner == owner) {
                droppedJobs.push_back(std::move(job));
            }
        }
        std::erase_if(m_queue, [&](const CompileJob &job) { return job.owner == owner; });
        // Undelivered results.
        for (auto &parked : m_results) {
            if (parked.owner == owner) {
                droppedResults.push_back(std::move(parked));
            }
        }
        std::erase_if(m_results, [&](const Parked &p) { return p.owner == owner; });
        m_resultCount.store(m_results.size(), std::memory_order_release);
        // The job in flight: the worker drops its result and destroys it before clearing m_running,
        // so once the wait ends no object of this owner is left in the worker.
        if (m_running && m_runningOwner == owner) {
            m_runningCancelled = true;
            m_doneCv.wait(lock, [&] { return !(m_running && m_runningOwner == owner); });
        }
    }
    // droppedJobs and droppedResults are freed here, outside the lock.
}

size_t CompileWorker::Pending(const void *owner) const {
    std::lock_guard lock{m_mutex};
    size_t n = std::count_if(m_queue.begin(), m_queue.end(), [&](const CompileJob &job) { return job.owner == owner; });
    if (m_running && m_runningOwner == owner) {
        n++;
    }
    return n;
}

void CompileWorker::Run() {
    std::unique_lock lock{m_mutex};
    for (;;) {
        m_workCv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
        if (m_stop) {
            return;
        }
        CompileJob job = std::move(m_queue.front());
        m_queue.pop_front();
        m_running = true;
        m_runningOwner = job.owner;
        m_runningCancelled = false;
        lock.unlock();

        const auto start = std::chrono::steady_clock::now();
        const bool ok = job.prepared->Finalize();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
        m_busyNs.fetch_add(static_cast<uint64_t>(ns.count()), std::memory_order_relaxed);

        lock.lock();
        if (!m_runningCancelled) {
            m_results.push_back(Parked{job.owner, CompileResult{job.ticket, std::move(job.prepared), ok}});
            m_resultCount.store(m_results.size(), std::memory_order_release);
        } else {
            // Destroy the object before reporting the job as finished (see Cancel).
            lock.unlock();
            job.prepared.reset();
            lock.lock();
        }
        m_running = false;
        m_runningOwner = nullptr;
        m_doneCv.notify_all();
    }
}

} // namespace brimir::jit
