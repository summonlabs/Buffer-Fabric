#include "buffer_fabric/codec.hpp"

#include <cstdio>

namespace buffer_fabric {
namespace {

[[nodiscard]] Status framing_error(ErrorCode code, const char* what, u64 actual, u64 limit) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s (actual=%llu limit=%llu)", what,
                  static_cast<unsigned long long>(actual), static_cast<unsigned long long>(limit));
    return Status(code, buffer);
}

}  // namespace

void ByteWriter::put_blob(const void* data, usize size, std::string_view what) {
    if (failed_) return;
    if (size > 0xFFFFFFFFull) {
        failed_ = true;
        return;
    }
    (void)what;
    put_u32(static_cast<u32>(size));
    put_bytes(data, size);
}

void ByteWriter::put_str(std::string_view text, usize max_bytes, std::string_view what) {
    if (failed_) return;
    if (max_bytes > kMaxEncodedStringBytes) max_bytes = kMaxEncodedStringBytes;
    if (text.size() > max_bytes) {
        failed_ = true;
        return;
    }
    (void)what;
    put_u32(static_cast<u32>(text.size()));
    put_bytes(text.data(), text.size());
}

bool ByteReader::get_blob(std::vector<u8>* out, usize max_bytes) {
    u32 length = 0;
    if (!get_u32(&length)) return false;
    if (static_cast<usize>(length) > max_bytes) {
        fail();
        return false;
    }
    const u8* cursor = nullptr;
    if (!get_bytes(&cursor, static_cast<usize>(length))) return false;
    out->assign(cursor, cursor + static_cast<usize>(length));
    return true;
}

bool ByteReader::get_str(std::string* out, usize max_bytes) {
    u32 length = 0;
    if (!get_u32(&length)) return false;
    if (max_bytes > kMaxEncodedStringBytes) max_bytes = kMaxEncodedStringBytes;
    if (static_cast<usize>(length) > max_bytes) {
        fail();
        return false;
    }
    const u8* cursor = nullptr;
    if (!get_bytes(&cursor, static_cast<usize>(length))) return false;
    out->assign(reinterpret_cast<const char*>(cursor), static_cast<usize>(length));
    return true;
}

std::vector<u8> encode_frame(const void* payload, usize size, u32 magic, u32 version) {
    std::vector<u8> out;
    out.reserve(kFrameHeaderBytes + size);
    ByteWriter header;
    header.put_u32(magic);
    header.put_u32(version);
    header.put_u32(static_cast<u32>(size));
    header.put_u32(crc32c(payload, size));
    out.insert(out.end(), header.data().begin(), header.data().end());
    const auto* bytes = static_cast<const u8*>(payload);
    out.insert(out.end(), bytes, bytes + size);
    return out;
}

Status encode_frame_into(ByteWriter* writer, const void* payload, usize size, u32 magic,
                         u32 version) {
    if (writer == nullptr) return Status(ErrorCode::InvalidArgument, "writer is null");
    if (size > 0xFFFFFFFFull) {
        return framing_error(ErrorCode::OversizedMessage, "frame payload too large", size,
                             0xFFFFFFFFull);
    }
    writer->put_u32(magic);
    writer->put_u32(version);
    writer->put_u32(static_cast<u32>(size));
    writer->put_u32(crc32c(payload, size));
    writer->put_bytes(payload, size);
    if (writer->failed()) {
        return Status(ErrorCode::OversizedMessage, "frame encoding failed");
    }
    return Status::success();
}

Status decode_frame(const u8* data, usize size, u32 magic, u32 version, usize max_payload,
                    Frame* out, usize* consumed) {
    if (out == nullptr || consumed == nullptr) {
        return Status(ErrorCode::InvalidArgument, "output pointers are null");
    }
    *consumed = 0;
    if (data == nullptr) return Status(ErrorCode::InvalidArgument, "frame data is null");
    if (size < kFrameHeaderBytes) {
        return framing_error(ErrorCode::TruncatedMessage, "frame header truncated", size,
                             kFrameHeaderBytes);
    }
    ByteReader reader(data, kFrameHeaderBytes);
    u32 got_magic = 0;
    u32 got_version = 0;
    u32 payload_bytes = 0;
    u32 expected_crc = 0;
    if (!reader.get_u32(&got_magic) || !reader.get_u32(&got_version) ||
        !reader.get_u32(&payload_bytes) || !reader.get_u32(&expected_crc)) {
        return Status(ErrorCode::CorruptRecord, "frame header is not decodable");
    }
    if (got_magic != magic) {
        return framing_error(ErrorCode::CorruptRecord, "frame magic mismatch", got_magic, magic);
    }
    if (got_version != version) {
        return framing_error(ErrorCode::VersionMismatch, "frame version mismatch", got_version,
                             version);
    }
    if (static_cast<usize>(payload_bytes) > max_payload) {
        return framing_error(ErrorCode::OversizedMessage, "frame payload exceeds bound",
                             payload_bytes, max_payload);
    }
    if (size - kFrameHeaderBytes < static_cast<usize>(payload_bytes)) {
        return framing_error(ErrorCode::TruncatedMessage, "frame payload truncated",
                             size - kFrameHeaderBytes, payload_bytes);
    }
    const u8* payload = data + kFrameHeaderBytes;
    const u32 actual_crc = crc32c(payload, static_cast<usize>(payload_bytes));
    if (actual_crc != expected_crc) {
        return framing_error(ErrorCode::IntegrityFailure, "frame checksum mismatch", actual_crc,
                             expected_crc);
    }
    out->payload.assign(payload, payload + static_cast<usize>(payload_bytes));
    *consumed = kFrameHeaderBytes + static_cast<usize>(payload_bytes);
    return Status::success();
}

}  // namespace buffer_fabric
