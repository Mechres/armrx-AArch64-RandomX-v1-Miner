#pragma once

#include "armrx/argon2.hpp"
#include "armrx/dataset.hpp"
#include "armrx/randomx_config.hpp"

#include <memory>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <span>
#include <vector>
#include <thread>

namespace armrx {

/**
 * PartialDataset — a memory-mapped prefix of the full RandomX dataset.
 *
 * In hybrid mode (light mode with a cached prefix), the JIT checks
 * item_number < partial_dataset_items_ before choosing the fast path
 * (direct load from this buffer) vs. the derivation path (existing
 * light-mode on-the-fly derivation).
 *
 * The buffer is mmap'd with MADV_HUGEPAGE and filled incrementally in
 * background threads with explicit CPU affinity (mirroring worker_loop()'s
 * AffinityMode::All pattern) — critical under isolcpus deployments where
 * un-pinned fill threads silently serialize onto one core (measured ~4-5×
 * slower during Gate A validation).
 */
class PartialDataset {
public:
    /// Construct with N items. N=0 means disabled (no allocation). Throws
    /// std::invalid_argument (before any allocation) if `item_count` exceeds
    /// the real RandomX dataset extent (randomx_dataset_item_count()) or would
    /// overflow converting to bytes -- a partial dataset is a prefix of the
    /// full dataset and cannot legitimately be larger than it (audit P5).
    explicit PartialDataset(std::size_t item_count);

    ~PartialDataset();

    PartialDataset(const PartialDataset&) = delete;
    PartialDataset& operator=(const PartialDataset&) = delete;
    PartialDataset(PartialDataset&&) = delete;
    PartialDataset& operator=(PartialDataset&&) = delete;

    /// RAII reader-quiescence guard (audit P1: seed-rotation race). Hold one
    /// for the entire duration of any operation that reads data()/
    /// item_count_atomic() and may retain a snapshot of the bound across
    /// multiple internal memory accesses (i.e. one full randomx_calculate_hash
    /// call) -- NOT just one atomic load. start_fill() takes the writer side
    /// (an exclusive lock) around the moment it resets item_count_ to 0 and
    /// before it lets any fill thread begin overwriting the buffer, so it
    /// cannot proceed while a ReadGuard is held, and any hash that starts
    /// after start_fill()'s reset section observes item_count() == 0 (forcing
    /// the always-safe derive/miss path) rather than a torn buffer. This is
    /// the reader-quiescence barrier: it replaces the old "reset counters,
    /// then just start overwriting" sequence, which let an in-flight hash
    /// keep reading a bound/buffer that a concurrent refill was already
    /// mutating underneath it.
    class ReadGuard {
    public:
        explicit ReadGuard(PartialDataset& pd) noexcept : pd_(pd) { pd_.rotation_mutex_.lock_shared(); }
        ~ReadGuard() { pd_.rotation_mutex_.unlock_shared(); }
        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;
    private:
        PartialDataset& pd_;
    };

    /// Returns the number of cached items (0 = disabled).
    [[nodiscard]] std::size_t item_count() const { return item_count_.load(std::memory_order_acquire); }

    /// Monotonically-increasing rotation generation, bumped exactly once per
    /// start_fill() call, INSIDE the same exclusive rotation_mutex_ section
    /// that resets item_count_/contiguous_done_/fill_complete_ (audit
    /// follow-up: a previous design bumped an analogous counter owned by
    /// MiningEngine *after* start_fill() had already released that lock and
    /// begun the refill -- a caller could observe the old counter value
    /// while data() was already being concurrently overwritten for the new
    /// generation. Publishing this counter atomically with the reset closes
    /// that: any ReadGuard acquired after a rotation's exclusive section has
    /// released is guaranteed to observe the NEW generation together with
    /// the reset (not-yet-refilled, or partially-refilled-but-safe) state,
    /// never one without the other. Callers should snapshot this alongside
    /// whatever cache corresponds to it (while holding the same lock/mutex
    /// that publishes both, e.g. MiningEngine::job_mutex_), then re-compare
    /// against a fresh read taken INSIDE a ReadGuard immediately before
    /// using data()/item_count_atomic() -- a mismatch means the snapshot is
    /// stale and the cache/prefix pairing can no longer be trusted for this
    /// hash.
    [[nodiscard]] std::uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

    /// Returns a pointer to the atomic item count for live JIT bound checking.
    /// The JIT reads this pointer every hash via memory_order_acquire, paired
    /// with the fill worker's memory_order_release CAS — guaranteeing written
    /// item bytes are visible when item_count increases.
    [[nodiscard]] const std::atomic<std::size_t>* item_count_atomic() const { return &item_count_; }

    /// Returns a pointer to the start of the cached data (or nullptr if disabled).
    [[nodiscard]] const std::byte* data() const { return data_; }

    /// Returns a read-only span over the currently-filled portion.
    [[nodiscard]] std::span<const std::byte> span() const {
        const auto count = item_count_.load(std::memory_order_acquire);
        return {data_, count * kRandomXDatasetItemBytes};
    }

    /// Number of fill chunks from the most recent start_fill (for the contiguous
    /// publish cursor). 0 if no fill has started.
    [[nodiscard]] std::size_t chunk_count() const {
        return chunk_done_ ? chunk_done_->size() : 0;
    }

    /// Start background fill of the buffer using initialize_dataset.
    /// Fills from start_item to item_count, incrementally raising item_count_ as each
    /// chunk completes. Threads are explicitly pinned mirroring worker_loop()'s
    /// AffinityMode::All pattern, skipping any CPU IDs in `exclude_cores`
    /// (e.g. cores already occupied by mining workers).
    /// `chunk_delays_ms` (optional, test-only) injects a per-chunk startup delay so a
    /// test can force a lagging chunk and verify contiguous-publish safety.
    ///
    /// Precondition (audit P2, empty fill-affinity list): `core_order` must be
    /// non-empty and `cache_holder` must be non-null -- an empty `core_order`
    /// previously reached `i % avail_cores.size()` with size() == 0 (division
    /// by zero / SIGFPE), and a null cache would null-deref inside the fill
    /// threads. Both are rejected up front with std::invalid_argument instead,
    /// before any thread is started. The engine's own caller always supplies a
    /// non-empty, detected core order and a freshly-initialized cache, so this
    /// only affects direct library callers passing bad input on purpose.
    ///
    /// Safe to call again on a live PartialDataset to re-fill it for a new
    /// seed (a rotation): internally joins any still-running fill from the
    /// previous generation, then re-publishes the reset (not-yet-filled)
    /// state under an exclusive ReadGuard lock before starting new fill
    /// threads, so no hash that is currently reading the buffer (holding a
    /// ReadGuard) can observe a mix of old- and new-seed bytes.
    void start_fill(std::shared_ptr<const Argon2dCache> cache_holder,
                    const std::vector<unsigned>& core_order,
                    const std::vector<unsigned>& exclude_cores = {},
                    const std::vector<unsigned>& chunk_delays_ms = {});

    /// Returns true if the fill has completed (all items fully computed).
    [[nodiscard]] bool fill_complete() const {
        return fill_complete_.load(std::memory_order_acquire);
    }

    /// Returns true if the most recent fill was aborted by an exception from
    /// initialize_dataset() (e.g. an out-of-range item span reaching a fill
    /// thread despite the caller's own size validation). A failed fill still
    /// wakes wait_for_fill()/wait_until_published() waiters (via the same
    /// path as cancel()) rather than hanging them; item_count_ simply stops
    /// advancing past whatever prefix was safely completed.
    [[nodiscard]] bool fill_failed() const {
        return fill_failed_.load(std::memory_order_acquire);
    }

    /// Wait for fill to complete (blocking, used in tests and by the mining
    /// workers at startup). The fill-thread join is guarded by fill_join_mutex_
    /// so multiple concurrent callers (one per mining worker) cannot race on
    /// join() of the same threads.
    void wait_for_fill();

    /// Block until at least `count` items are published (the contiguous prefix
    /// [0, count) is fully initialized). Replaces timing-dependent test polling:
    /// a test can observe a deterministic mid-fill state (e.g. a lagging chunk
    /// has NOT been skipped by contiguous publish) without relying on the
    /// scheduler catching an intermediate item_count_ sample. Returns once
    /// item_count_ >= count or the fill is cancelled (stop_ set).
    void wait_until_published(std::size_t count);

    /// Cancel any in-progress or not-yet-started fill and wake every current
    /// or future wait_for_fill()/wait_until_published() waiter (audit P1,
    /// shutdown before first fill / during active fill). Safe to call even if
    /// start_fill() was never invoked -- a mining engine's worker threads call
    /// wait_for_fill() unconditionally at startup and would otherwise block
    /// forever if the owning pool connection never delivers a first job.
    /// Fill threads observe cancellation between bounded (<= 64 MiB) internal
    /// sub-chunks, so a cancelled in-progress fill winds down in at most one
    /// sub-chunk's time rather than the whole (potentially minutes-long) fill.
    ///
    /// Permanent and one-way (round 2 audit fix): sets a persistent flag that
    /// is never cleared, in addition to signalling the current fill's stop
    /// token. A per-generation stop_ token alone is not enough -- start_fill()
    /// can replace it with a fresh (false) token for a concurrently-starting
    /// rotation at any point after cancel() has already read the OLD token,
    /// which would silently strand the cancellation signal on an orphaned
    /// token nobody is looking at anymore, while the new generation's fill
    /// workers/waiters wait for a refill that will never be told to stop.
    /// The persistent flag is checked everywhere the per-generation token is,
    /// so it cannot be lost this way. Correct for this class's only caller
    /// (MiningEngine::stop() / the destructor): once an engine is shutting
    /// down, this PartialDataset is not expected to serve another rotation.
    void cancel();

private:
    std::byte* data_ = nullptr;
    std::size_t allocated_items_ = 0;
    std::atomic<std::size_t> item_count_{0};
    std::atomic<bool> fill_complete_{false};
    std::atomic<bool> fill_failed_{false};
    // Persistent, never-cleared cancellation flag (audit follow-up, round 2).
    // Set only by cancel(). See cancel()'s doc comment for why this is
    // needed in addition to (not instead of) the per-generation stop_ token:
    // a token can be silently replaced by a racing start_fill() between
    // cancel() reading it and signalling it, which would otherwise drop the
    // cancellation for the new generation. Checked alongside stop_ in every
    // wait/cancellation check below (wait_for_fill, wait_until_published,
    // fill_worker) and by start_fill() itself, which refuses to start once
    // this is set.
    std::atomic<bool> shutdown_requested_{false};
    // Rotation generation, bumped inside start_fill()'s exclusive
    // rotation_mutex_ section -- see generation()'s doc comment for why this
    // must be published atomically with the item_count_/contiguous_done_/
    // fill_complete_ reset rather than by a separate, independently-timed
    // counter (audit follow-up, seed-rotation race not fully closed by round
    // 2's ReadGuard re-validation alone).
    std::atomic<std::uint64_t> generation_{0};

    // Writer-preferring reader-writer lock for the reader-quiescence barrier
    // (audit P1). std::shared_mutex/pthread_rwlock make NO fairness guarantee
    // -- glibc's default rwlock policy is reader-preferring, so a writer
    // (start_fill()'s reset section) can be starved indefinitely by mining
    // workers that keep re-acquiring the shared side back-to-back with almost
    // no gap between hashes (observed directly: a live seed rotation hung
    // test_light_mode_seed_rotation_rebuilds_partial_dataset for 900+ s
    // during development of this fix, with two workers continuously hashing
    // against a tiny partial dataset). This hand-rolled lock instead makes a
    // pending writer block all NEW readers immediately, bounding the writer's
    // wait to however long the CURRENTLY-active readers take to finish (one
    // hash each) rather than however long the reader stream continues.
    // Deliberately not pthread_rwlock + PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP:
    // that attribute is a glibc extension musl (the aarch64 cross-compile
    // target, see AGENTS.md) does not implement.
    class RotationLock {
    public:
        void lock_shared() {
            std::unique_lock<std::mutex> lk(m_);
            // Block while a writer holds the lock OR one is waiting -- this
            // is the writer-priority rule: a NEW reader must not cut in front
            // of an already-queued writer, however many other readers keep
            // arriving.
            readers_cv_.wait(lk, [this] { return !writer_active_ && waiting_writers_ == 0; });
            ++active_readers_;
        }
        void unlock_shared() {
            std::unique_lock<std::mutex> lk(m_);
            if (--active_readers_ == 0) {
                writer_cv_.notify_all();
            }
        }
        void lock() {
            std::unique_lock<std::mutex> lk(m_);
            ++waiting_writers_; // blocks new readers immediately, see lock_shared()
            writer_cv_.wait(lk, [this] { return !writer_active_ && active_readers_ == 0; });
            --waiting_writers_;
            writer_active_ = true;
        }
        void unlock() {
            {
                std::unique_lock<std::mutex> lk(m_);
                writer_active_ = false;
            }
            // Wake both: a still-waiting writer (another queued start_fill(),
            // in practice serialized by MiningEngine's job_mutex_ already, but
            // handled correctly regardless) via writer_cv_, and any readers
            // parked behind this writer via readers_cv_ -- they re-check
            // waiting_writers_ == 0 themselves, so this is safe even if
            // another writer is still queued.
            writer_cv_.notify_all();
            readers_cv_.notify_all();
        }
    private:
        std::mutex m_;
        std::condition_variable readers_cv_;
        std::condition_variable writer_cv_;
        int active_readers_ = 0;
        int waiting_writers_ = 0;
        bool writer_active_ = false;
    };

    // Reader-quiescence barrier (audit P1). Fill workers hold no lock while
    // writing data_ (the hot publish path is deliberately lock-free); instead,
    // start_fill() takes the EXCLUSIVE side only around the brief moment it
    // resets item_count_/contiguous_done_/fill_complete_ to their not-ready
    // state, which cannot proceed while any ReadGuard (ordinary hash
    // execution) holds the shared side. Once that reset completes and the
    // lock is released, new readers observe item_count() == 0 and use the
    // always-safe derive path until the fill republishes real bytes via the
    // existing atomic contiguous-publish protocol below -- so the exclusive
    // section does not need to span the whole (re)fill, only the transition.
    mutable RotationLock rotation_mutex_;
    // Contiguous-publish cursor: the highest item such that EVERY item in
    // [0, contiguous_done_) is fully initialized. item_count_ is advanced only
    // up to this bound, so any item < item_count_ is safe to read (no
    // out-of-order publish of a lagging chunk's bytes). Set by whichever fill
    // worker closes the contiguous gap after its chunk finishes.
    std::atomic<std::uint64_t> contiguous_done_{0};
    // Items per fill chunk (set in start_fill; last chunk may be smaller). Used
    // by the contiguous-publish cursor to map a chunk index to its item span.
    std::uint64_t items_per_chunk_{0};
    // One done-flag per fill chunk (indexed by chunk id assigned in start_fill).
    // A chunk sets its flag (release) after initialize_dataset completes, then
    // attempts to advance contiguous_done_ over now-contiguous finished chunks.
    std::unique_ptr<std::vector<std::atomic<bool>>> chunk_done_;
    mutable std::vector<std::thread> fill_threads_;
    // Guards the fill-thread join in wait_for_fill() against concurrent callers.
    mutable std::mutex fill_join_mutex_;
    // Condition variable + mutex backing wait_for_fill() and wait_until_published():
    // replaces the old 100 ms poll loop (test flakiness / wakeup latency). Notified
    // by fill_worker when item_count_ advances or fill_complete_ is set, and by the
    // destructor on cancellation so a waiter never blocks forever.
    mutable std::mutex fill_cv_mutex_;
    std::condition_variable fill_cv_;
    // Keeps the Argon2dCache alive while fill threads are running
    std::shared_ptr<const Argon2dCache> cache_holder_;
    // Shared independently of PartialDataset so a worker never needs the object
    // to stay alive to observe cancellation.
    std::shared_ptr<std::atomic<bool>> stop_ =
        std::make_shared<std::atomic<bool>>(false);

    void fill_worker(std::shared_ptr<const Argon2dCache> cache_holder,
                     std::shared_ptr<std::atomic<bool>> stop,
                     std::uint64_t start_item,
                     std::uint64_t end_item,
                     unsigned cpu_id,
                     std::size_t chunk_id,
                     unsigned delay_ms);
};

} // namespace armrx
