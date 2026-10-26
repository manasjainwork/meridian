#pragma once
#include <charconv>
#include <cstdint>
#include <string>
#include <type_traits>

namespace meridian {

// An exchange order id is an integer identity, never a floating-point number.
// Any non-uint64 argument (double, int, ...) is rejected at compile time.
struct ExchangeOrderId {
    uint64_t v;
    explicit constexpr ExchangeOrderId(uint64_t x) noexcept : v(x) {}
    template <class T> ExchangeOrderId(T) = delete;
};

static_assert(std::is_constructible_v<ExchangeOrderId, uint64_t>);
static_assert(!std::is_constructible_v<ExchangeOrderId, double>);
static_assert(!std::is_constructible_v<ExchangeOrderId, int>);

inline std::string to_decimal(uint64_t v) {
    char b[24];
    const auto r = std::to_chars(b, b + sizeof b, v);
    return std::string(b, r.ptr);
}

}  // namespace meridian
