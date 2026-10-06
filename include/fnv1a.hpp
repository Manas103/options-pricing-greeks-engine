// FNV-1a 64-bit running hash, folded incrementally over raw bytes. Same
// technique xor-signal-codec (github.com/Manas103/xor-signal-codec) uses to
// prove a 40,000,000-point round trip is bit-exact: an exact digest match
// is equivalent evidence to a byte-for-byte diff and is far cheaper to
// carry around as a single number.
#pragma once
#include <cstddef>
#include <cstdint>

namespace quote {

constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

inline void fnv1a_fold(uint64_t& h, const void* data, std::size_t n) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= bytes[i];
        h *= FNV_PRIME;
    }
}

} // namespace quote
