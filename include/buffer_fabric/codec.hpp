#pragma once

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/hash.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

// ---------------------------------------------------------------------------
// Canonical little-endian byte encoding
//
// All durable and wire encodings use this one codec so that there is exactly
// one definition of the byte layout. Integers are fixed-width little-endian.
// Strings and blobs are length-prefixed with a u32 count of bytes; the reader
// always applies an explicit maximum and reports a Status rather than
// allocating based on an unvalidated length.
// ---------------------------------------------------------------------------

class BF_API ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(usize reserve_bytes) { buffer_.reserve(reserve_bytes); }

    void put_u8(u8 v) { buffer_.push_back(v); }
    void put_bool(bool v) { buffer_.push_back(v ? u8{1} : u8{0}); }

    void put_u16(u16 v) {
        buffer_.push_back(static_cast<u8>(v & 0xFFu));
        buffer_.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    }
    void put_u32(u32 v) {
        for (int i = 0; i < 4; ++i) buffer_.push_back(static_cast<u8>((v >> (8 * i)) & 0xFFu));
    }
    void put_u64(u64 v) {
        for (int i = 0; i < 8; ++i) buffer_.push_back(static_cast<u8>((v >> (8 * i)) & 0xFFu));
    }
    void put_i64(i64 v) { put_u64(static_cast<u64>(v)); }

    void put_bytes(const void* data, usize size) {
        if (size == 0) return;
        const auto* p = static_cast<const u8*>(data);
        buffer_.insert(buffer_.end(), p, p + size);
    }

    void put_blob(const void* data, usize size, std::string_view what);
    void put_blob(const std::vector<u8>& blob, std::string_view what) {
        put_blob(blob.data(), blob.size(), what);
    }
    void put_str(std::string_view text, usize max_bytes, std::string_view what);

    void put_digest(const Digest& d) {
        put_u64(d.hi);
        put_u64(d.lo);
    }

    template <class Id>
    void put_id(Id id) {
        put_u64(id.raw());
    }

    [[nodiscard]] const std::vector<u8>& data() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<u8> take() { return std::move(buffer_); }
    [[nodiscard]] usize size() const noexcept { return buffer_.size(); }
    [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }
    /// Latched when an append was rejected because it could not be encoded
    /// (oversized blob or string). Callers must check this before trusting the
    /// buffer.
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    void clear() noexcept {
        buffer_.clear();
        failed_ = false;
    }
    void assign(const void* data, usize size) {
        const auto* p = static_cast<const u8*>(data);
        buffer_.assign(p, p + size);
        failed_ = false;
    }

private:
    std::vector<u8> buffer_{};
    bool failed_{false};
};

class BF_API ByteReader {
public:
    ByteReader(const u8* data, usize size) noexcept : data_(data), size_(size) {}
    explicit ByteReader(const std::vector<u8>& buffer) noexcept
        : data_(buffer.data()), size_(buffer.size()) {}

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] bool empty() const noexcept { return !failed_ && offset_ == size_; }
    [[nodiscard]] usize remaining() const noexcept {
        return failed_ ? 0 : (size_ - offset_);
    }
    [[nodiscard]] usize offset() const noexcept { return offset_; }

    bool get_u8(u8* out) {
        if (!require(1)) return false;
        *out = data_[offset_++];
        return true;
    }
    bool get_bool(bool* out) {
        u8 v = 0;
        if (!get_u8(&v)) return false;
        if (v > 1) {
            fail();
            return false;
        }
        *out = (v != 0);
        return true;
    }
    bool get_u16(u16* out) {
        if (!require(2)) return false;
        *out = static_cast<u16>(static_cast<u16>(data_[offset_]) |
                                static_cast<u16>(static_cast<u16>(data_[offset_ + 1]) << 8));
        offset_ += 2;
        return true;
    }
    bool get_u32(u32* out) {
        if (!require(4)) return false;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(data_[offset_ + static_cast<usize>(i)]) << (8 * i);
        offset_ += 4;
        *out = v;
        return true;
    }
    bool get_u64(u64* out) {
        if (!require(8)) return false;
        u64 v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<u64>(data_[offset_ + static_cast<usize>(i)]) << (8 * i);
        offset_ += 8;
        *out = v;
        return true;
    }
    bool get_i64(i64* out) {
        u64 v = 0;
        if (!get_u64(&v)) return false;
        *out = static_cast<i64>(v);
        return true;
    }
    bool get_bytes(const u8** out, usize count) {
        if (!require(count)) return false;
        *out = data_ + offset_;
        offset_ += count;
        return true;
    }
    bool get_blob(std::vector<u8>* out, usize max_bytes);
    bool get_str(std::string* out, usize max_bytes);
    bool get_digest(Digest* out) {
        return get_u64(&out->hi) && get_u64(&out->lo);
    }
    template <class Id>
    bool get_id(Id* out) {
        u64 raw = 0;
        if (!get_u64(&raw)) return false;
        *out = Id::from_raw(raw);
        return true;
    }
    bool skip(usize count) { return require(count) ? (offset_ += count, true) : false; }

private:
    bool require(usize count) {
        if (failed_) return false;
        if (count > size_ - offset_) {
            fail();
            return false;
        }
        return true;
    }
    void fail() noexcept { failed_ = true; }

    const u8* data_{};
    usize size_{};
    usize offset_{};
    bool failed_{false};
};

/// Maximum string length accepted for each kind of encoded string, enforced on
/// both the write and the read side.
inline constexpr u32 kMaxEncodedStringBytes = 4096;

// ---------------------------------------------------------------------------
// Record framing
//
// A frame is: u32 magic, u32 version, u32 payload_size, u32 crc32c(payload),
// payload bytes. The framing is shared by the journal and the wire protocol so
// that a single hardened implementation covers both.
// ---------------------------------------------------------------------------
inline constexpr usize kFrameHeaderBytes = 16;

struct Frame {
    std::vector<u8> payload{};
};

[[nodiscard]] BF_API std::vector<u8> encode_frame(const void* payload, usize size, u32 magic,
                                                  u32 version);
[[nodiscard]] BF_API Status encode_frame_into(ByteWriter* writer, const void* payload, usize size,
                                              u32 magic, u32 version);

/// Decode one frame from \p data. On success \p consumed is set to the number
/// of bytes read. Reports CorruptRecord / IntegrityFailure / OversizedMessage
/// / TruncatedMessage. A header that does not start with \p magic is reported
/// as CorruptRecord so that a caller scanning for a resynchronisation point can
/// distinguish "not a frame" from "damaged frame".
[[nodiscard]] BF_API Status decode_frame(const u8* data, usize size, u32 magic, u32 version,
                                         usize max_payload, Frame* out, usize* consumed);

}  // namespace buffer_fabric
