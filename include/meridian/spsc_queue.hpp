#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace meridian {

// Bounded single-producer / single-consumer ring buffer. Capacity must be a power of two.
// tail_ is written only by the producer, head_ only by the consumer. Each side keeps a cached copy of
// the other's index so the shared cache line is read only when the ring looks full/empty.
template <typename T, size_t N>
class SpscQueue {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_nothrow_move_assignable_v<T>, "T must be nothrow move assignable");
    static constexpr size_t kLine = 64;

public:
    bool try_push(const T& v) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ == N) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ == N) return false;
        }
        buf_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;
        }
        out = std::move(buf_[h & (N - 1)]);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    static constexpr size_t capacity() noexcept { return N; }

private:
    alignas(kLine) std::atomic<size_t> tail_{0};   // producer-owned
    size_t head_cache_ = 0;                        // producer's copy of head_
    alignas(kLine) std::atomic<size_t> head_{0};   // consumer-owned
    size_t tail_cache_ = 0;                        // consumer's copy of tail_
    alignas(kLine) T buf_[N]{};
};

}  // namespace meridian
