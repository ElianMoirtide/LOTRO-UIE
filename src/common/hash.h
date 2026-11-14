#pragma once

#include <cstddef>
#include <cstdint>

namespace lotro_uie {

inline uint64_t HashBytes(const void* data, size_t size, uint64_t seed = 14695981039346656037ULL) noexcept {
    if (!data || size == 0) {
        return 0;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(bytes[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace lotro_uie
