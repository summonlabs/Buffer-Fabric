#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

// ---------------------------------------------------------------------------
// CRC-32C (Castagnoli), reflected, polynomial 0x1EDC6F41 / reflected 0x82F63B78
//
// Used for record integrity in the journal, snapshot container, and framed
// transport. This is an integrity check against accidental corruption and
// truncation, not a cryptographic authenticator.
// ---------------------------------------------------------------------------
namespace detail {

struct Crc32cTable {
    u32 entries[256]{};

    constexpr Crc32cTable() noexcept {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) != 0u ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
            }
            entries[i] = c;
        }
    }
};

inline constexpr Crc32cTable kCrc32cTable{};

}  // namespace detail

[[nodiscard]] inline u32 crc32c_update(u32 crc, const u8* data, usize size) noexcept {
    u32 c = crc;
    for (usize i = 0; i < size; ++i) {
        c = detail::kCrc32cTable.entries[(c ^ static_cast<u32>(data[i])) & 0xFFu] ^ (c >> 8);
    }
    return c;
}

[[nodiscard]] inline u32 crc32c(const void* data, usize size, u32 seed = 0) noexcept {
    return ~crc32c_update(~seed, static_cast<const u8*>(data), size);
}

inline void crc32c_mix(u32& crc, const void* data, usize size) noexcept {
    crc = crc32c_update(crc, static_cast<const u8*>(data), size);
}

// ---------------------------------------------------------------------------
// FNV-1a/128
//
// Stable content digest over a canonical byte encoding. Deterministic across
// platforms, compilers and process restarts. Not a cryptographic hash.
// ---------------------------------------------------------------------------
namespace detail {

inline void mul128(u64 alo, u64 ahi, u64 blo, u64 bhi, u64& rlo, u64& rhi) noexcept {
    const auto split = [](u64 x, u64 y, u64& hi, u64& lo) noexcept {
        const u64 x0 = x & 0xFFFFFFFFull;
        const u64 x1 = x >> 32;
        const u64 y0 = y & 0xFFFFFFFFull;
        const u64 y1 = y >> 32;
        const u64 p00 = x0 * y0;
        const u64 p01 = x0 * y1;
        const u64 p10 = x1 * y0;
        const u64 p11 = x1 * y1;
        const u64 mid = (p00 >> 32) + (p01 & 0xFFFFFFFFull) + (p10 & 0xFFFFFFFFull);
        lo = (p00 & 0xFFFFFFFFull) | (mid << 32);
        hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    };
    u64 p1hi = 0;
    u64 p1lo = 0;
    split(alo, blo, p1hi, p1lo);
    rlo = p1lo;
    rhi = p1hi + alo * bhi + ahi * blo;
}

inline constexpr u64 kFnv128OffsetHi = 0x6c62272e07bb0142ull;
inline constexpr u64 kFnv128OffsetLo = 0x62b821756295c58dull;
inline constexpr u64 kFnv128PrimeHi = 0x0000000001000000ull;
inline constexpr u64 kFnv128PrimeLo = 0x000000000000013Bull;

}  // namespace detail

class BF_API Fnv128 {
public:
    Fnv128() noexcept : hi_(detail::kFnv128OffsetHi), lo_(detail::kFnv128OffsetLo) {}

    void reset() noexcept {
        hi_ = detail::kFnv128OffsetHi;
        lo_ = detail::kFnv128OffsetLo;
    }

    void update(const void* data, usize size) noexcept {
        const auto* p = static_cast<const u8*>(data);
        for (usize i = 0; i < size; ++i) {
            lo_ ^= static_cast<u64>(p[i]);
            u64 nlo = 0;
            u64 nhi = 0;
            detail::mul128(lo_, hi_, detail::kFnv128PrimeLo, detail::kFnv128PrimeHi, nlo, nhi);
            lo_ = nlo;
            hi_ = nhi;
        }
    }

    void update_u64(u64 value) noexcept {
        u8 buf[8];
        for (int i = 0; i < 8; ++i) {
            buf[i] = static_cast<u8>((value >> (8 * i)) & 0xFFu);
        }
        update(buf, sizeof(buf));
    }

    [[nodiscard]] Digest digest() const noexcept {
        Digest d;
        d.hi = hi_;
        d.lo = lo_;
        return d;
    }

private:
    u64 hi_{};
    u64 lo_{};
};

[[nodiscard]] BF_API Digest digest_bytes(const void* data, usize size) noexcept;

}  // namespace buffer_fabric
