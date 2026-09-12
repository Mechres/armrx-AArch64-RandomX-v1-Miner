#include "armrx/partial_dataset.hpp"
#include "armrx/dataset.hpp"
#include "armrx/log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace armrx {

namespace {

/// Size of each chunk per thread when filling. 64 MiB chunks give good
/// parallelism without excessive thread creation overhead.
constexpr std::uint64_t kFillChunkBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kFillChunkItems = kFillChunkBytes / kRandomXDatasetItemBytes;

} // namespace

PartialDataset::PartialDataset(std::size_t item_count)
    : allocated_items_(item_count)
{
    if (item_count == 0) {
        fill_complete_.store(true, std::memory_order_release);
        return;
    }

    // Audit P5 (partial-dataset size validation): a partial dataset is a
    // prefix of the real RandomX dataset, so it cannot legitimately exceed
    // that extent. MinerApp::run() already rejects an oversized --dataset-mb
    // request via validate_partial_dataset_request() before ever reaching
    // here, but that is an application-layer convenience, not an invariant
    // of this class -- any other caller (tests, future embedders) gets the
    // same protection directly, and gets it BEFORE the mmap/prefault below
    // rather than discovering it later via a range exception inside a
    // background fill thread (initialize_dataset()'s own dataset_range_is_valid
    // check, which fires only once fill_worker() actually reaches an
    // out-of-range chunk).
    if (item_count > randomx_dataset_item_count()) {
        throw std::invalid_argument(
            "PartialDataset: item_count (" + std::to_string(item_count) +
            ") exceeds the full RandomX dataset extent (" +
            std::to_string(randomx_dataset_item_count()) + " items)");
    }
    if (item_count > std::numeric_limits<std::size_t>::max() / kRandomXDatasetItemBytes) {
        throw std::invalid_argument("PartialDataset: item_count overflows when converted to bytes");
    }

    const std::size_t total_bytes = item_count * kRandomXDatasetItemBytes;

    // Simple mmap with transparent hugepage hint
    data_ = static_cast<std::byte*>(
        ::mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (data_ == MAP_FAILED) {
        throw std::bad_alloc();
    }

    // Hugepage hint (best-effort; succeeds silently if THP is available)
    ::madvise(data_, total_bytes, MADV_HUGEPAGE);

    // Pre-fault the buffer so THP promotion happens NOW, at init, instead of
    // lazily on first random write-touch during hashing. Without this, a 256 MiB
    // (light) / 512 MiB buffer is promoted to 2 MB huge pages page-by-page under
    // mining load -- a soft-fault storm that drags the hashrate up gradually over
    // ~20 minutes (observed) before reaching steady state. XMRig pre-faults its
    // dataset and is at full speed immediately. Mirrors the pre-fault already done
    // for the scratchpad (vm.cpp) and Argon2d cache (argon2.cpp).
    // MADV_POPULATE_WRITE (Linux 5.14+) prefaults writable pages without memset.
#if defined(MADV_POPULATE_WRITE)
    ::madvise(data_, total_bytes, MADV_POPULATE_WRITE);
#else
    // Fallback: touch every page to fault it in (mmap/MADV_HUGEPAGE anon is
    // already zero, so writing zeros only forces the fault + THP promotion).
    constexpr std::size_t kPage = 4096;
    volatile std::byte* p = data_;
    for (std::size_t off = 0; off < total_bytes; off += kPage)
        p[off];  // read-touch faults the page; combined with MADV_HUGEPAGE the
                  // kernel promotes to a huge page on the write that follows.
    // Force write fault (promotes THP): write a zero to each page.
    for (std::size_t off = 0; off < total_bytes; off += kPage)
        p[off] = std::byte{0};
#endif

    ARMRX_LOG_INFO << "PartialDataset: allocated " << item_count
                   << " items (" << (total_bytes / (1024ULL * 1024ULL))
                   << " MiB)";
}

PartialDataset::~PartialDataset() {
    const bool fill_finished =
        item_count_.load(std::memory_order_acquire) >= allocated_items_;
    if (!fill_finished) {
        // See cancel(): wakes any wait_for_fill()/wait_until_published()
        // waiter and lets any still-running fill thread wind down within one
        // bounded sub-chunk, so the join below cannot block for the whole
        // (potentially minutes-long) remaining fill.
        cancel();
    }

    for (auto& t : fill_threads_) {
        if (t.joinable()) t.join();
    }

    if (data_) {
        ::munmap(data_, allocated_items_ * kRandomXDatasetItemBytes);
        data_ = nullptr;
    }
}

void PartialDataset::cancel() {
    // Set the PERSISTENT flag first (round 2 audit fix). This is what
    // actually guarantees the cancellation cannot be lost: it is completely
    // independent of the stop_ shared_ptr below, so a concurrent start_fill()
    // that swaps stop_ for a fresh token AFTER this line (but possibly before
    // or after the token-signalling below) cannot un-cancel anything -- every
    // wait/cancellation check in this file also checks this flag.
    shutdown_requested_.store(true, std::memory_order_release);

    // Also signal the CURRENT generation's token, best-effort, so a waiter
    // blocked on it wakes immediately rather than at its next poll interval.
    // stop_ is a plain (non-atomic) shared_ptr member that start_fill() can
    // reassign concurrently (a live seed rotation running on a different
    // thread than the one calling cancel(), e.g. MiningEngine::stop() racing
    // a job-callback thread). fill_cv_mutex_ is the lock both that
    // reassignment and this read now use, matching the lock already held by
    // wait_for_fill()/wait_until_published() below when they dereference
    // stop_ -- without a shared lock, this would be a data race on the
    // shared_ptr object itself (independent of the atomic<bool> it points to).
    // If this reads a token that a racing start_fill() immediately replaces,
    // the store below lands on an orphaned token nobody is looking at
    // anymore -- harmless now that shutdown_requested_ is the actual
    // correctness guarantee; this is purely a latency optimization for
    // whichever generation's token we happen to catch.
    std::shared_ptr<std::atomic<bool>> stop;
    {
        std::lock_guard<std::mutex> lk(fill_cv_mutex_);
        stop = stop_;
    }
    if (stop) stop->store(true, std::memory_order_release);
    // Wake any wait_for_fill()/wait_until_published() waiter -- including one
    // that started waiting before any start_fill() call ever ran (a mining
    // engine's workers call wait_for_fill() unconditionally at startup; audit
    // P1, "shutdown hangs before the first partial-dataset fill").
    fill_cv_.notify_all();
}

void PartialDataset::start_fill(std::shared_ptr<const Argon2dCache> cache_holder,
                                 const std::vector<unsigned>& core_order,
                                 const std::vector<unsigned>& exclude_cores,
                                 const std::vector<unsigned>& chunk_delays_ms)
{
    if (allocated_items_ == 0) return;

    // Audit follow-up (P1, round 2): once shutdown has been requested, this
    // object is not expected to serve another rotation (see cancel()'s doc
    // comment). Refusing here is not what makes cancellation correct (every
    // wait/cancellation check below also checks shutdown_requested_
    // independently, which is what actually prevents the signal from being
    // lost to a token swap), but it avoids uselessly spinning up threads and
    // touching the buffer after the engine has already decided to stop.
    if (shutdown_requested_.load(std::memory_order_acquire)) return;

    // Audit P2 (empty fill-affinity list): reject invalid input up front,
    // before any thread is started, rather than reaching i % avail_cores.size()
    // with size() == 0 (division by zero) or null-dereferencing the cache
    // inside a fill thread. See the header doc comment for the contract.
    if (!cache_holder) {
        throw std::invalid_argument("PartialDataset::start_fill: cache_holder must not be null");
    }
    if (core_order.empty()) {
        throw std::invalid_argument(
            "PartialDataset::start_fill: core_order must not be empty "
            "(pass the caller's real core list; there is no unpinned mode)");
    }

    // Serialize with any still-running prior fill: join and drop its threads
    // (they would otherwise keep writing this same buffer concurrently with
    // the new fill, corrupting it). This is what makes start_fill() safe to
    // call repeatedly across seed rotations. The mutex guards against a
    // concurrent wait_for_fill() also joining these threads.
    {
        std::lock_guard<std::mutex> lock(fill_join_mutex_);
        for (auto& t : fill_threads_) {
            if (t.joinable()) t.join();
        }
        fill_threads_.clear();
    }

    // Reader-quiescence barrier (audit P1, seed-rotation race): publish the
    // reset ("not ready") state only while holding the EXCLUSIVE side of
    // rotation_mutex_, which cannot be acquired while any ReadGuard (an
    // in-flight hash reading data()/item_count_atomic()) is held. This closes
    // the race where an old-seed hash, already past its bound snapshot,
    // could keep reading data_ while this call started overwriting it. Once
    // this section releases the lock, any reader that acquires the shared
    // side next observes item_count() == 0 (the JIT hybrid path takes the
    // always-safe derive path for a zero bound) until the fill below
    // republishes real bytes via the existing atomic contiguous-publish
    // protocol -- so the exclusive section only needs to span the reset
    // itself, not the whole (re)fill.
    {
        std::unique_lock<PartialDataset::RotationLock> lock(rotation_mutex_);
        item_count_.store(0, std::memory_order_relaxed);
        contiguous_done_.store(0, std::memory_order_relaxed);
        fill_complete_.store(false, std::memory_order_relaxed);
        fill_failed_.store(false, std::memory_order_relaxed);
        // Audit follow-up (seed-rotation race, still open after round 2):
        // publish the NEW generation HERE, inside the same exclusive section
        // as the reset above, instead of letting the caller (MiningEngine)
        // bump its own, separately-timed counter after start_fill() returns.
        // The old design had a real gap: this exclusive section could
        // release (admitting new ReadGuard holders) and fill threads could
        // already be spawned and publishing new-seed bytes, all before the
        // caller's own counter incremented -- a reader in that window would
        // see its stale cache-generation snapshot still "match" the
        // caller's not-yet-bumped counter, and proceed to hash with an old
        // cache against an already-rotating prefix. release ordering here
        // pairs with generation()'s acquire load, so any ReadGuard acquired
        // after this section unlocks is guaranteed to observe the new
        // generation together with the reset state, never one without the
        // other.
        generation_.fetch_add(1, std::memory_order_release);
        // fill_cv_mutex_ also guards this reassignment (see cancel() and
        // wait_for_fill()): stop_ is a plain shared_ptr member that those
        // functions read concurrently from other threads.
        std::lock_guard<std::mutex> cv_lock(fill_cv_mutex_);
        stop_ = std::make_shared<std::atomic<bool>>(false);
    }

    // Keep the cache alive while fill threads are running
    cache_holder_ = cache_holder;

    const auto total_items = static_cast<std::uint64_t>(allocated_items_);

    // Build the list of available cores excluding mining cores
    std::vector<unsigned> avail_cores;
    for (auto c : core_order) {
        if (std::find(exclude_cores.begin(), exclude_cores.end(), c) == exclude_cores.end()) {
            avail_cores.push_back(c);
        }
    }
    if (avail_cores.empty()) {
        // Fallback: allow sharing if all cores excluded
        avail_cores = core_order;
    }

    const unsigned num_workers = static_cast<unsigned>(std::min<std::size_t>(
        (total_items + kFillChunkItems - 1) / kFillChunkItems,
        std::max<std::size_t>(1, avail_cores.size())));

    const auto items_per_worker = (total_items + num_workers - 1) / num_workers;

    // Contiguous-publish bookkeeping: one done-flag per fill chunk (chunk_id
    // indexes this vector). items_per_chunk_ maps a chunk index to its item span.
    chunk_done_ = std::make_unique<std::vector<std::atomic<bool>>>(num_workers);
    for (auto& f : *chunk_done_) f.store(false, std::memory_order_relaxed);
    items_per_chunk_ = items_per_worker;

    // Audit follow-up (P1, round 2): this loop mutates fill_threads_ (via
    // emplace_back) without a lock in the original version of this fix.
    // wait_for_fill() can now return EARLY via cancel() (not just once
    // fill_complete_ becomes true naturally, which -- before cancel() existed
    // -- could only happen long after this loop had already finished
    // spawning). cancel() can be called from another thread (MiningEngine::
    // stop()) at any time, including while this loop is still running, so a
    // mining worker's wait_for_fill() could reach its join+clear section
    // (guarded by fill_join_mutex_) CONCURRENTLY with this loop's unguarded
    // writes -- a real data race (not just a logical one) on the vector
    // itself. Guarded by the same fill_join_mutex_ used by every other
    // fill_threads_ access (the initial join+clear above, and
    // wait_for_fill()'s final join+clear) so the two can never interleave.
    {
        std::lock_guard<std::mutex> lock(fill_join_mutex_);
        fill_threads_.reserve(num_workers);
        for (unsigned i = 0; i < num_workers; ++i) {
            const auto start = static_cast<std::uint64_t>(i) * items_per_worker;
            const auto end = std::min(start + items_per_worker, total_items);
            if (start >= end) break;

            const unsigned cpu_id = avail_cores[i % avail_cores.size()];
            const unsigned delay_ms = (i < chunk_delays_ms.size()) ? chunk_delays_ms[i] : 0;
            fill_threads_.emplace_back(&PartialDataset::fill_worker, this,
                                       cache_holder_, stop_, start, end, cpu_id, i,
                                       delay_ms);
        }
    }

    ARMRX_LOG_INFO << "PartialDataset: started fill with " << num_workers
                   << " workers (" << items_per_worker << " items each)";
}

void PartialDataset::fill_worker(std::shared_ptr<const Argon2dCache> cache_holder,
                                 std::shared_ptr<std::atomic<bool>> stop,
                                 std::uint64_t start_item,
                                  std::uint64_t end_item,
                                  unsigned cpu_id,
                                  std::size_t chunk_id,
                                  unsigned delay_ms)
{
    // Keep the cache alive for the duration of this fill worker.
    // The shared_ptr is passed by value into the thread, so each worker
    // holds its own reference independently.

    // Test-only artificial lag so a test can force a lagging chunk (verifies
    // contiguous-publish safety). Production passes delay_ms == 0.
    // Also checks the persistent shutdown_requested_ flag (round 2 audit
    // fix), not just this worker's own per-generation `stop` token: if
    // cancel() raced a concurrent start_fill() and ended up signalling a
    // token that was about to be replaced, `stop` alone could stay false for
    // this (new) generation's workers even though shutdown was requested.
    if (stop->load(std::memory_order_acquire) || shutdown_requested_.load(std::memory_order_acquire)) return;
    if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

    // Explicit CPU pinning (mirrors worker_loop()'s AffinityMode::All pattern).
    // sched_setaffinity(0, ...) is portable across glibc, musl, and Android/bionic
    // (which lacks pthread_setaffinity_np).
    cpu_set_t cpus{};
    CPU_ZERO(&cpus);
    CPU_SET(static_cast<int>(cpu_id), &cpus);
    if (sched_setaffinity(0, sizeof(cpus), &cpus) != 0) {
        ARMRX_LOG_WARN << "PartialDataset fill worker: CPU pinning to cpu " << cpu_id
                       << " failed (" << std::strerror(errno)
                       << ") — topology-aware fill NOT enforced for this worker";
    }

    try {
        // Cancellation-bounded fill (audit P2, shutdown during an active
        // fill): split this worker's assigned [start_item, end_item) range
        // into sub-chunks of at most kFillChunkItems items, checking `stop`
        // between them, instead of one single initialize_dataset() call that
        // could run uninterrupted for the whole range (observed up to ~164s
        // with few available fill cores). initialize_dataset() derives each
        // item independently from (cache, item_number) -- see dataset.cpp --
        // so splitting one call into several sequential sub-range calls
        // produces byte-identical output; it only adds checkpoints.
        //
        // A cancelled fill returns WITHOUT marking this chunk done: bytes
        // already written by a completed sub-chunk are real RandomX dataset
        // items (never garbage), but chunk_done_/item_count_/contiguous_done_
        // are only advanced below, after the FULL chunk finishes -- so a
        // reader can never observe this chunk's span as part of the safe
        // published prefix if it was only partially filled.
        for (std::uint64_t sub_start = start_item; sub_start < end_item; ) {
            if (stop->load(std::memory_order_acquire) || shutdown_requested_.load(std::memory_order_acquire)) {
                ARMRX_LOG_DEBUG << "PartialDataset fill worker on cpu " << cpu_id
                                << ": cancelled at item " << sub_start
                                << " of chunk [" << start_item << ", " << end_item << ")";
                return;
            }
            const auto sub_end = std::min(sub_start + kFillChunkItems, end_item);
            const auto sub_items = sub_end - sub_start;
            const auto byte_offset = sub_start * kRandomXDatasetItemBytes;
            auto span = std::span<std::byte>(
                data_ + byte_offset,
                static_cast<std::size_t>(sub_items * kRandomXDatasetItemBytes));
            // The cache is valid because cache_holder keeps it alive.
            initialize_dataset(span, *cache_holder, sub_start, sub_items);
            sub_start = sub_end;
        }
    } catch (const std::exception& ex) {
        // Audit P5 (partial-dataset size validation): a caller-side check
        // (MinerApp::run() / validate_partial_dataset_request()) should
        // reject an out-of-range request before ever reaching here, but this
        // is the last line of defense against an uncaught exception escaping
        // a detached-like std::thread (which would call std::terminate() and
        // kill the whole process). Treat it like cancellation: wake every
        // waiter instead of letting them hang on a fill that can never
        // finish, and leave item_count_ at whatever prefix was already
        // safely published.
        ARMRX_LOG_ERROR << "PartialDataset fill worker on cpu " << cpu_id
                        << ": initialize_dataset failed (" << ex.what()
                        << ") -- aborting fill";
        fill_failed_.store(true, std::memory_order_release);
        stop->store(true, std::memory_order_release);
        fill_cv_.notify_all();
        return;
    }

    // Mark THIS chunk done (release: its bytes are now fully initialized and
    // visible to any reader that observes item_count_ >= end_item via acquire).
    (*chunk_done_)[chunk_id].store(true, std::memory_order_release);

    // Contiguous-publish: advance the published bound only over the prefix that
    // is NOW fully initialized. While the chunk immediately after the current
    // contiguous cursor is done, swallow it and keep going. This guarantees
    // item_count_ always names a fully-filled [0, item_count_) prefix, so a
    // hashing worker can safely read any item < item_count_ (no out-of-order
    // publish of a lagging chunk's uninitialized bytes -- closes audit C1).
    std::uint64_t cursor = contiguous_done_.load(std::memory_order_acquire);
    for (;;) {
        // Advance cursor over consecutive finished chunks. The chunk that
        // owns item `cursor` is chunk (cursor / items_per_chunk_); it is done
        // when its flag is set, and then the whole chunk's item span is safe.
        while (cursor < allocated_items_ && chunk_done_) {
            const std::size_t owning = static_cast<std::size_t>(cursor / items_per_chunk_);
            if (owning >= chunk_done_->size()) break;
            if (!(*chunk_done_)[owning].load(std::memory_order_acquire)) break;
            const std::uint64_t chunk_span = (owning + 1 < chunk_done_->size())
                ? items_per_chunk_
                : (allocated_items_ - owning * items_per_chunk_);
            cursor += chunk_span;
        }
        // Publish the new contiguous bound (release pairs with the JIT's acquire).
        const std::uint64_t prev = contiguous_done_.exchange(cursor, std::memory_order_release);
        // Also raise the legacy item_count_ to the contiguous bound so existing
        // readers (span(), wait_for_fill polling) see the safe prefix.
        std::uint64_t item_prev = item_count_.load(std::memory_order_relaxed);
        while (item_prev < cursor &&
               !item_count_.compare_exchange_weak(item_prev, cursor,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed)) {
            // another worker published further; loop
        }
        // Wake any wait_for_fill()/wait_until_published() waiter now that the
        // contiguous bound advanced (deterministic mid-fill observation).
        fill_cv_.notify_all();
        // If we made no progress, another worker will handle further advances
        // when its chunk completes. Terminate.
        if (cursor == prev) break;
        // Re-load and try again (a chunk after `prev` may now be done).
        cursor = contiguous_done_.load(std::memory_order_acquire);
    }

    // Mark the whole fill complete once the contiguous prefix reaches the end.
    if (!stop->load(std::memory_order_acquire) &&
        !shutdown_requested_.load(std::memory_order_acquire) &&
        contiguous_done_.load(std::memory_order_acquire) >= allocated_items_) {
        fill_complete_.store(true, std::memory_order_release);
        // Wake wait_for_fill() (and any wait_until_published) — full completion.
        fill_cv_.notify_all();
    }

    ARMRX_LOG_DEBUG << "PartialDataset worker on cpu " << cpu_id
                    << ": filled items [" << start_item << ", " << end_item << ")";
}

void PartialDataset::wait_for_fill() {
    // Wait for fill completion via condition variable (replaces the old 100 ms
    // poll loop, which added wakeup latency and test flakiness under TSAN/ASan
    // scheduling skew). Wakes on fill_complete_ OR cancellation (stop_) so a
    // cancelled fill never blocks forever.
    //
    // The fill workers raise the atomic item_count_/fill_complete_ and call
    // fill_cv_.notify_all() WITHOUT holding fill_cv_mutex_ (the hot publish
    // path is intentionally lock-free). A notify_all() that lands in the
    // window between the predicate check and the futex wait is therefore lost,
    // which can block this waiter forever (no subsequent notify, no guaranteed
    // spurious wakeup) -- the observed test_partial_dataset deadlock. Re-check
    // the predicate on a short timeout backstop so liveness never depends on a
    // single notify. Correctness is unchanged: the published counters are
    // atomics and the CV is only a wakeup hint.
    {
        std::unique_lock<std::mutex> lk(fill_cv_mutex_);
        static constexpr auto kPoll = std::chrono::milliseconds(20);
        // Also checks the persistent shutdown_requested_ flag (round 2 audit
        // fix): stop_ alone is not sufficient because a concurrent
        // start_fill() can replace it with a fresh (false) token for a new
        // generation immediately after cancel() signalled the OLD one --
        // shutdown_requested_ is never replaced, so it cannot be missed this
        // way.
        while (!fill_complete_.load(std::memory_order_acquire) &&
               !stop_->load(std::memory_order_acquire) &&
               !shutdown_requested_.load(std::memory_order_acquire)) {
            fill_cv_.wait_for(lk, kPoll);
        }
    }

    // Log fill completion once per process (multiple workers may observe it).
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        ARMRX_LOG_INFO << "PartialDataset: fill complete — workers resuming";
    }

    // Join all threads. Guarded so concurrent callers (one per mining worker)
    // don't race on join() of the same fill threads (undefined behavior).
    {
        std::lock_guard<std::mutex> lock(fill_join_mutex_);
        for (auto& t : fill_threads_) {
            if (t.joinable()) t.join();
        }
        // Drop the (now-joined) handles so a subsequent start_fill() doesn't
        // re-emplace onto a vector of stale, unjoinable threads and grow it
        // unboundedly across many seed rotations. The next start_fill() also
        // clears this under the same mutex; this clears the trailing ones.
        fill_threads_.clear();
    }
}

void PartialDataset::wait_until_published(std::size_t count) {
    std::unique_lock<std::mutex> lk(fill_cv_mutex_);
    // See wait_for_fill(): a lock-free notify_all() from a fill worker can be
    // lost in the predicate-check/futex-wait window, so re-check the published
    // count on a short timeout backstop instead of relying on a single wakeup.
    static constexpr auto kPoll = std::chrono::milliseconds(20);
    // See wait_for_fill()'s matching comment on shutdown_requested_.
    while (item_count_.load(std::memory_order_acquire) < count &&
           !stop_->load(std::memory_order_acquire) &&
           !shutdown_requested_.load(std::memory_order_acquire)) {
        fill_cv_.wait_for(lk, kPoll);
    }
}

} // namespace armrx
