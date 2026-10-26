#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace meridian {

// Hierarchical bitset: bit i of level k+1 is set iff word i of level k is non-zero.
// next_ge / prev_le cost one word probe per level (3 levels cover 262,144 prices).
class PriceBitmap {
public:
    static constexpr size_t npos = ~size_t{0};

    explicit PriceBitmap(size_t n) {
        size_t bits = n ? n : 1;
        for (;;) {
            const size_t words = (bits + 63) / 64;
            lv_.emplace_back(words, 0);
            if (words == 1) break;
            bits = words;
        }
    }

    void set(size_t i) noexcept {
        for (auto& w : lv_) {
            w[i >> 6] |= uint64_t{1} << (i & 63);
            i >>= 6;
        }
    }

    void clear(size_t i) noexcept {
        for (auto& w : lv_) {
            w[i >> 6] &= ~(uint64_t{1} << (i & 63));
            if (w[i >> 6] != 0) break;
            i >>= 6;
        }
    }

    bool test(size_t i) const noexcept { return (lv_[0][i >> 6] >> (i & 63)) & 1; }

    // smallest set index >= i, or npos
    size_t next_ge(size_t i) const noexcept {
        for (size_t l = 0; l < lv_.size(); ++l) {
            const size_t w = i >> 6;
            if (w >= lv_[l].size()) return npos;
            const uint64_t m = lv_[l][w] & (~uint64_t{0} << (i & 63));
            if (m) {
                i = (w << 6) + static_cast<size_t>(std::countr_zero(m));
                while (l > 0) { --l; i = (i << 6) + static_cast<size_t>(std::countr_zero(lv_[l][i])); }
                return i;
            }
            i = w + 1;
        }
        return npos;
    }

    // largest set index <= i, or npos
    size_t prev_le(size_t i) const noexcept {
        const size_t top = lv_[0].size() * 64;
        if (i >= top) i = top - 1;
        for (size_t l = 0; l < lv_.size(); ++l) {
            const size_t w = i >> 6;
            const unsigned b = static_cast<unsigned>(i & 63);
            const uint64_t mask = (b == 63) ? ~uint64_t{0} : ((uint64_t{1} << (b + 1)) - 1);
            const uint64_t m = lv_[l][w] & mask;
            if (m) {
                i = (w << 6) + 63 - static_cast<size_t>(std::countl_zero(m));
                while (l > 0) { --l; i = (i << 6) + 63 - static_cast<size_t>(std::countl_zero(lv_[l][i])); }
                return i;
            }
            if (w == 0) return npos;
            i = w - 1;
        }
        return npos;
    }

private:
    std::vector<std::vector<uint64_t>> lv_;
};

}  // namespace meridian
