#pragma once
// Seeded one-way delay models for the virtual-clock sequencer (design M5, hypothesis H4). Everything here is a pure function of the seed:
// std::mt19937_64 is fully specified by the C++ standard, and the draw uses plain modulo (not std::uniform_int_distribution, whose output
// differs between standard libraries). Each agent gets its own generator derived from (seed, agent id), so the delays an agent sees do not
// depend on how many other agents exist or in which order their messages are pushed.
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace meridian::vseq {

inline uint64_t splitmix64(uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

class DelayModel {
public:
    static DelayModel fixed(uint64_t d) { DelayModel m; m.lo_ = m.hi_ = d; return m; }
    static DelayModel uniform(uint64_t lo, uint64_t hi) { DelayModel m; m.lo_ = lo; m.hi_ = hi < lo ? lo : hi; return m; }
    // Draws uniformly from a list of measured delays (for example the ack-interval samples of a real run). An empty list means zero delay.
    static DelayModel empirical(std::vector<uint64_t> samples) { DelayModel m; m.samples_ = std::move(samples); m.emp_ = true; return m; }

    uint64_t draw(std::mt19937_64& g) const {
        if (emp_) return samples_.empty() ? 0 : samples_[g() % samples_.size()];
        return lo_ == hi_ ? lo_ : lo_ + g() % (hi_ - lo_ + 1);
    }

private:
    uint64_t lo_ = 0, hi_ = 0;
    std::vector<uint64_t> samples_;
    bool emp_ = false;
};

// One generator per agent: the same (seed, agent) always gives the same stream.
inline std::mt19937_64 agent_rng(uint64_t seed, uint32_t agent) { return std::mt19937_64(splitmix64(seed ^ splitmix64(agent))); }

}  // namespace meridian::vseq
