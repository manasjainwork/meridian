#pragma once
// Log-linear histogram for latencies (sequencer design M4), in the style of HdrHistogram: values below 2^(k+1) are counted exactly,
// larger values fall into buckets whose width is at most 2^-k of the value (k = 7: under 0.8 percent). Fixed size, no allocation after
// construction, record() is a few instructions. A percentile is reported as the upper edge of its bucket (never below the true value),
// capped at the largest value seen. Open-loop measurement is done by the caller (latency from the intended send time), not here.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace meridian {

template <unsigned K = 7>
class Histogram {
public:
    Histogram() : counts_(size_t(66 - K) << K, 0) {}

    void record(uint64_t v) noexcept {
        ++counts_[index_of(v)];
        ++n_;
        sum_ += static_cast<long double>(v);
        if (v < min_) min_ = v;
        if (v > max_) max_ = v;
    }
    void add(const Histogram& o) noexcept {
        for (size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
        n_ += o.n_; sum_ += o.sum_;
        if (o.n_ && o.min_ < min_) min_ = o.min_;
        if (o.n_ && o.max_ > max_) max_ = o.max_;
    }

    uint64_t count() const noexcept { return n_; }
    uint64_t min() const noexcept { return n_ ? min_ : 0; }
    uint64_t max() const noexcept { return n_ ? max_ : 0; }
    double mean() const noexcept { return n_ ? static_cast<double>(sum_ / n_) : 0.0; }

    // The rank that percentile p (0 to 100) refers to among n sorted values: 1 to n.
    static uint64_t rank_of(uint64_t n, double p) noexcept {
        if (n == 0) return 0;
        const double r = std::ceil(p / 100.0 * static_cast<double>(n));
        return r < 1.0 ? 1 : (r > static_cast<double>(n) ? n : static_cast<uint64_t>(r));
    }
    uint64_t percentile(double p) const noexcept {
        if (n_ == 0) return 0;
        const uint64_t rank = rank_of(n_, p);
        uint64_t seen = 0;
        for (size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= rank) return std::min(high_of(i), max_);
        }
        return max_;
    }

private:
    static size_t index_of(uint64_t v) noexcept {
        const unsigned bw = static_cast<unsigned>(std::bit_width(v));
        if (bw <= K + 1) return static_cast<size_t>(v);
        const unsigned s = bw - 1 - K;
        return (size_t(s) << K) + static_cast<size_t>(v >> s);
    }
    static uint64_t high_of(size_t i) noexcept {
        if (i < (size_t(2) << K)) return i;
        const unsigned s = static_cast<unsigned>(i >> K) - 1;
        const uint64_t m = i - (size_t(s) << K);
        return (m << s) + ((uint64_t(1) << s) - 1);
    }

    std::vector<uint64_t> counts_;
    uint64_t n_ = 0, min_ = ~uint64_t(0), max_ = 0;
    long double sum_ = 0;
};

}  // namespace meridian
