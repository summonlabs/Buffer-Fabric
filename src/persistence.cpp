#include "buffer_fabric/persistence.hpp"

#include <cstdio>

#include "buffer_fabric/json.hpp"
#include "buffer_fabric/version.hpp"
#include "fs.hpp"

#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#endif

namespace buffer_fabric {
namespace {

[[nodiscard]] Status flush_stdio(void* handle) {
    auto* file = static_cast<std::FILE*>(handle);
    if (file == nullptr) return Status(ErrorCode::PersistenceError, "invalid file handle");
    if (std::fflush(file) != 0) {
        return Status(ErrorCode::PersistenceError, "fflush failed");
    }
#if defined(_WIN32)
    if (::_commit(::_fileno(file)) != 0) {
        return Status(ErrorCode::PersistenceError, "commit failed");
    }
#else
    if (::fsync(::fileno(file)) != 0) {
        return Status(ErrorCode::PersistenceError, "fsync failed");
    }
#endif
    return Status::success();
}

[[nodiscard]] u64 stdio_size(std::FILE* file) {
    if (file == nullptr) return 0;
    const long position = std::ftell(file);
    if (position < 0) return 0;
    if (std::fseek(file, 0, SEEK_END) != 0) return 0;
    const long end = std::ftell(file);
    std::fseek(file, position, SEEK_SET);
    return end < 0 ? 0 : static_cast<u64>(end);
}

}  // namespace

std::string_view to_string(DurabilityMode mode) noexcept {
    switch (mode) {
        case DurabilityMode::None: return "none";
        case DurabilityMode::JournalSync: return "journal_sync";
    }
    return "unknown";
}

std::string_view to_string(JournalOp op) noexcept {
    switch (op) {
        case JournalOp::EpochAdvance: return "epoch_advance";
        case JournalOp::PoolUpsert: return "pool_upsert";
        case JournalOp::PoolRemoved: return "pool_removed";
        case JournalOp::PolicyUpsert: return "policy_upsert";
        case JournalOp::QueueUpsert: return "queue_upsert";
        case JournalOp::QueueRetired: return "queue_retired";
        case JournalOp::CapacitySet: return "capacity_set";
        case JournalOp::AllocationReserved: return "allocation_reserved";
        case JournalOp::AllocationCommitted: return "allocation_committed";
        case JournalOp::AllocationReleased: return "allocation_released";
        case JournalOp::AllocationReclaimed: return "allocation_reclaimed";
        case JournalOp::AllocationFenced: return "allocation_fenced";
        case JournalOp::AllocationRevalidated: return "allocation_revalidated";
        case JournalOp::AllocationExpired: return "allocation_expired";
        case JournalOp::PressureObserved: return "pressure_observed";
        case JournalOp::AttemptRecorded: return "attempt_recorded";
        case JournalOp::Checkpoint: return "checkpoint";
        case JournalOp::BackendRegistered: return "backend_registered";
    }
    return "unknown";
}

std::string_view to_string(SnapshotFault fault) noexcept {
    switch (fault) {
        case SnapshotFault::None: return "none";
        case SnapshotFault::BadMagic: return "bad_magic";
        case SnapshotFault::BadEndMagic: return "bad_end_magic";
        case SnapshotFault::VersionUnsupported: return "version_unsupported";
        case SnapshotFault::HeaderCrcMismatch: return "header_crc_mismatch";
        case SnapshotFault::PayloadCrcMismatch: return "payload_crc_mismatch";
        case SnapshotFault::Oversized: return "oversized";
        case SnapshotFault::Truncated: return "truncated";
        case SnapshotFault::HeaderTruncated: return "header_truncated";
    }
    return "unknown";
}

std::string_view to_string(RecoveryOutcome outcome) noexcept {
    switch (outcome) {
        case RecoveryOutcome::Created: return "created";
        case RecoveryOutcome::SnapshotOnly: return "snapshot_only";
        case RecoveryOutcome::JournalReplayed: return "journal_replayed";
        case RecoveryOutcome::TailTruncated: return "tail_truncated";
        case RecoveryOutcome::JournalOnly: return "journal_only";
    }
    return "unknown";
}

Status PersistenceConfig::validate() const {
    if (directory.empty()) {
        return Status(ErrorCode::InvalidArgument, "persistence directory must be set");
    }
    if (directory.size() > 1024) {
        return Status(ErrorCode::InvalidArgument, "persistence directory path is too long");
    }
    if (max_journal_bytes == 0 || max_journal_bytes > (4ull << 30)) {
        return Status(ErrorCode::InvalidArgument, "max_journal_bytes out of range");
    }
    if (compact_journal_bytes == 0 || compact_journal_bytes > max_journal_bytes) {
        return Status(ErrorCode::InvalidArgument,
                      "compact_journal_bytes must be non-zero and no larger than max_journal_bytes");
    }
    if (max_snapshot_bytes == 0 || max_snapshot_bytes > kMaxSnapshotBytes) {
        return Status(ErrorCode::InvalidArgument, "max_snapshot_bytes out of range");
    }
    return Status::success();
}

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------

Journal::~Journal() { close(); }

Status Journal::open_append(const std::string& path, u64 max_bytes) {
    close();
    max_bytes_ = max_bytes;
    std::FILE* file = std::fopen(path.c_str(), "ab");
    if (file == nullptr) {
        return Status(ErrorCode::PersistenceError, "cannot open journal for append: " + path);
    }
    write_handle_ = file;
    std::fseek(file, 0, SEEK_END);
    const long end = std::ftell(file);
    bytes_written_ = end < 0 ? 0 : static_cast<u64>(end);
    file_size_ = bytes_written_;
    return Status::success();
}

Status Journal::open_read(const std::string& path, u64 max_bytes) {
    close();
    max_bytes_ = max_bytes;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return Status(ErrorCode::PersistenceError, "cannot open journal for read: " + path);
    }
    read_handle_ = file;
    file_size_ = stdio_size(file);
    if (file_size_ > max_bytes) {
        std::fclose(file);
        read_handle_ = nullptr;
        return Status(ErrorCode::OversizedMessage, "journal exceeds the configured bound");
    }
    read_offset_ = 0;
    records_read_ = 0;
    return Status::success();
}

void Journal::close() noexcept {
    if (read_handle_ != nullptr) {
        std::fclose(static_cast<std::FILE*>(read_handle_));
        read_handle_ = nullptr;
    }
    if (write_handle_ != nullptr) {
        std::fclose(static_cast<std::FILE*>(write_handle_));
        write_handle_ = nullptr;
    }
    read_offset_ = 0;
    records_read_ = 0;
}

Status Journal::append(const void* payload, usize size) {
    if (write_handle_ == nullptr) {
        return Status(ErrorCode::LifecycleViolation, "journal is not open for append");
    }
    if (size > kMaxJournalRecordBytes) {
        return Status(ErrorCode::OversizedMessage, "journal record exceeds the record bound");
    }
    if (max_bytes_ != 0 && bytes_written_ + kFrameHeaderBytes + size > max_bytes_) {
        return Status(ErrorCode::BoundedResourceExhausted,
                      "journal would exceed the configured byte bound");
    }
    const std::vector<u8> frame =
        encode_frame(payload, size, kJournalFrameMagic, BUFFER_FABRIC_JOURNAL_FORMAT_VERSION);
    std::FILE* file = static_cast<std::FILE*>(write_handle_);
    const usize written = std::fwrite(frame.data(), 1, frame.size(), file);
    if (written != frame.size()) {
        return Status(ErrorCode::PersistenceError, "short journal write");
    }
    bytes_written_ += static_cast<u64>(frame.size());
    file_size_ = bytes_written_;
    records_written_ += 1;
    return Status::success();
}

Status Journal::sync() {
    if (write_handle_ == nullptr) {
        return Status(ErrorCode::LifecycleViolation, "journal is not open for append");
    }
    return flush_stdio(write_handle_);
}

Status Journal::truncate_to(u64 offset) {
    if (write_handle_ == nullptr) {
        return Status(ErrorCode::LifecycleViolation, "journal is not open for append");
    }
    std::FILE* file = static_cast<std::FILE*>(write_handle_);
    if (std::fflush(file) != 0) {
        return Status(ErrorCode::PersistenceError, "flush before truncate failed");
    }
#if defined(_WIN32)
    if (::_chsize_s(::_fileno(file), static_cast<__int64>(offset)) != 0) {
        return Status(ErrorCode::PersistenceError, "truncate failed");
    }
#else
    if (::ftruncate(::fileno(file), static_cast<off_t>(offset)) != 0) {
        return Status(ErrorCode::PersistenceError, "truncate failed");
    }
#endif
    const long position = std::ftell(file);
    if (position < 0 || static_cast<u64>(position) > offset) {
        std::fseek(file, static_cast<long>(offset), SEEK_SET);
    }
    bytes_written_ = offset;
    file_size_ = offset;
    return Status::success();
}

Result<Journal::ReadResult> Journal::read_next() {
    ReadResult result;
    if (read_handle_ == nullptr) {
        return Status(ErrorCode::LifecycleViolation, "journal is not open for read");
    }
    if (read_offset_ >= file_size_) {
        result.eof = true;
        return result;
    }
    std::FILE* file = static_cast<std::FILE*>(read_handle_);
    u8 header[kFrameHeaderBytes];
    if (std::fread(header, 1, kFrameHeaderBytes, file) != kFrameHeaderBytes) {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "journal record header truncated at byte %llu of %llu",
                      static_cast<unsigned long long>(read_offset_),
                      static_cast<unsigned long long>(file_size_));
        return Status(ErrorCode::JournalTruncated, detail);
    }
    ByteReader reader(header, kFrameHeaderBytes);
    u32 magic = 0;
    u32 version = 0;
    u32 payload_bytes = 0;
    u32 expected_crc = 0;
    if (!reader.get_u32(&magic) || !reader.get_u32(&version) || !reader.get_u32(&payload_bytes) ||
        !reader.get_u32(&expected_crc)) {
        return Status(ErrorCode::CorruptRecord, "journal record header is not decodable");
    }
    char context[192];
    std::snprintf(context, sizeof(context),
                  "journal record at byte %llu (magic=%08x version=%u payload=%u crc=%08x)",
                  static_cast<unsigned long long>(read_offset_), magic, version, payload_bytes,
                  expected_crc);
    if (magic != kJournalFrameMagic) {
        return Status(ErrorCode::CorruptRecord, std::string("journal record magic mismatch: ") + context);
    }
    if (version != BUFFER_FABRIC_JOURNAL_FORMAT_VERSION) {
        return Status(ErrorCode::VersionMismatch,
                      std::string("journal record version mismatch: ") + context);
    }
    if (static_cast<usize>(payload_bytes) > kMaxJournalRecordBytes) {
        return Status(ErrorCode::OversizedMessage,
                      std::string("journal record payload exceeds the bound: ") + context);
    }
    if (read_offset_ + kFrameHeaderBytes + payload_bytes > file_size_) {
        return Status(ErrorCode::JournalTruncated,
                      std::string("journal record payload is truncated: ") + context);
    }
    result.payload.resize(payload_bytes);
    if (payload_bytes != 0 &&
        std::fread(result.payload.data(), 1, payload_bytes, file) != payload_bytes) {
        return Status(ErrorCode::JournalTruncated, "journal record payload is truncated");
    }
    const u32 actual_crc = crc32c(result.payload.data(), result.payload.size());
    if (actual_crc != expected_crc) {
        return Status(ErrorCode::IntegrityFailure,
                      std::string("journal record checksum mismatch: ") + context);
    }
    result.record_offset = read_offset_;
    result.next_offset = read_offset_ + kFrameHeaderBytes + payload_bytes;
    read_offset_ = result.next_offset;
    records_read_ += 1;
    return result;
}

// ---------------------------------------------------------------------------
// Snapshot container
// ---------------------------------------------------------------------------

std::vector<u8> encode_snapshot(const SnapshotHeader& header, const void* payload,
                                usize payload_size) {
    ByteWriter writer(kSnapshotHeaderBytes + payload_size + 16);
    writer.put_u32(kSnapshotFrameMagic);
    writer.put_u32(header.format_version);
    writer.put_u32(header.flags);
    writer.put_u32(static_cast<u32>(payload_size));
    writer.put_u64(header.epoch.raw());
    writer.put_u64(header.boot.hi);
    writer.put_u64(header.boot.lo);
    writer.put_u64(header.record_count);
    const u32 header_crc = crc32c(writer.data().data(), writer.size());
    writer.put_bytes(payload, payload_size);
    writer.put_u32(crc32c(payload, payload_size));
    writer.put_u32(header_crc);
    writer.put_u32(kSnapshotEndMagic);
    return writer.take();
}

Result<SnapshotHeader> decode_snapshot(const u8* data, usize size, usize max_payload,
                                       std::vector<u8>* payload_out) {
    SnapshotHeader header;
    if (data == nullptr) {
        return Status(ErrorCode::InvalidArgument, "snapshot data is null");
    }
    if (size < kSnapshotHeaderBytes + 12) {
        return Status(ErrorCode::CorruptRecord, "snapshot container is truncated");
    }
    ByteReader reader(data, size);
    u32 magic = 0;
    u32 payload_bytes = 0;
    if (!reader.get_u32(&magic)) return Status(ErrorCode::CorruptRecord, "snapshot header");
    if (magic != kSnapshotFrameMagic) {
        return Status(ErrorCode::CorruptRecord, "snapshot magic mismatch");
    }
    if (!reader.get_u32(&header.format_version)) {
        return Status(ErrorCode::CorruptRecord, "snapshot version");
    }
    if (!reader.get_u32(&header.flags)) return Status(ErrorCode::CorruptRecord, "snapshot flags");
    if (!reader.get_u32(&payload_bytes)) return Status(ErrorCode::CorruptRecord, "snapshot length");
    u64 epoch_raw = 0;
    if (!reader.get_u64(&epoch_raw)) return Status(ErrorCode::CorruptRecord, "snapshot epoch");
    if (!reader.get_u64(&header.boot.hi)) return Status(ErrorCode::CorruptRecord, "snapshot boot");
    if (!reader.get_u64(&header.boot.lo)) return Status(ErrorCode::CorruptRecord, "snapshot boot");
    if (!reader.get_u64(&header.record_count)) {
        return Status(ErrorCode::CorruptRecord, "snapshot record count");
    }
    header.epoch = EpochId::from_raw(epoch_raw);
    header.payload_bytes = payload_bytes;

    const u32 stored_header_crc = crc32c(data, kSnapshotHeaderBytes);
    if (header.format_version != BUFFER_FABRIC_PERSISTENCE_FORMAT_VERSION) {
        return Status(ErrorCode::VersionMismatch, "snapshot format version is not supported");
    }
    if (static_cast<usize>(payload_bytes) > max_payload) {
        return Status(ErrorCode::SnapshotOversized, "snapshot payload exceeds the configured bound");
    }
    if (size < kSnapshotHeaderBytes + static_cast<usize>(payload_bytes) + 12) {
        return Status(ErrorCode::CorruptRecord, "snapshot payload is truncated");
    }
    const u8* payload = data + kSnapshotHeaderBytes;
    u32 expected_payload_crc = 0;
    u32 header_crc = 0;
    u32 end_magic = 0;
    {
        ByteReader tail(data + kSnapshotHeaderBytes + payload_bytes, 12);
        if (!tail.get_u32(&expected_payload_crc)) {
            return Status(ErrorCode::CorruptRecord, "snapshot trailer");
        }
        if (!tail.get_u32(&header_crc)) return Status(ErrorCode::CorruptRecord, "snapshot trailer");
        if (!tail.get_u32(&end_magic)) return Status(ErrorCode::CorruptRecord, "snapshot trailer");
    }
    if (end_magic != kSnapshotEndMagic) {
        return Status(ErrorCode::CorruptRecord, "snapshot end magic mismatch");
    }
    if (header_crc != stored_header_crc) {
        return Status(ErrorCode::IntegrityFailure, "snapshot header checksum mismatch");
    }
    const u32 actual_payload_crc = crc32c(payload, payload_bytes);
    if (actual_payload_crc != expected_payload_crc) {
        return Status(ErrorCode::IntegrityFailure, "snapshot payload checksum mismatch");
    }
    if (payload_out != nullptr) {
        payload_out->assign(payload, payload + payload_bytes);
    }
    return header;
}

// ---------------------------------------------------------------------------
// Recovery report
// ---------------------------------------------------------------------------

std::string RecoveryReport::to_json() const {
    JsonWriter writer(4096);
    writer.begin_object();
    writer.key("outcome");
    writer.value_string(to_string(outcome));
    writer.key("durable_state_present");
    writer.value_bool(durable_state_present);
    writer.key("snapshot_loaded");
    writer.value_bool(snapshot_loaded);
    writer.key("snapshot_integrity_verified");
    writer.value_bool(snapshot_integrity_verified);
    writer.key("journal_loaded");
    writer.value_bool(journal_loaded);
    writer.key("snapshot_format_version");
    writer.value_u64(snapshot_format_version);
    writer.key("journal_format_version");
    writer.value_u64(journal_format_version);
    writer.key("snapshot_payload_bytes");
    writer.value_u64(snapshot_payload_bytes);
    writer.key("journal_records_read");
    writer.value_u64(journal_records_read);
    writer.key("journal_records_applied");
    writer.value_u64(journal_records_applied);
    writer.key("journal_records_rejected");
    writer.value_u64(journal_records_rejected);
    writer.key("journal_bytes_discarded");
    writer.value_u64(journal_bytes_discarded);
    writer.key("journal_records_discarded");
    writer.value_u64(journal_records_discarded);
    writer.key("durable_epoch");
    writer.value_u64(durable_epoch.raw());
    writer.key("new_epoch");
    writer.value_u64(new_epoch.raw());
    writer.key("boot_hi");
    writer.value_u64(boot.hi);
    writer.key("boot_lo");
    writer.value_u64(boot.lo);
    writer.key("previous_boot_hi");
    writer.value_u64(previous_boot.hi);
    writer.key("previous_boot_lo");
    writer.value_u64(previous_boot.lo);
    writer.key("pools_restored");
    writer.value_u64(pools_restored);
    writer.key("policies_restored");
    writer.value_u64(policies_restored);
    writer.key("queues_restored");
    writer.value_u64(queues_restored);
    writer.key("allocations_restored");
    writer.value_u64(allocations_restored);
    writer.key("allocations_restored_as_uncommitted");
    writer.value_u64(allocations_restored_as_uncommitted);
    writer.key("pressure_snapshots_invalidated");
    writer.value_u64(pressure_snapshots_invalidated);
    writer.key("fences_restored");
    writer.value_u64(fences_restored);
    writer.key("unfinished_attempts");
    writer.begin_array();
    for (const auto& attempt : unfinished_attempts) {
        writer.begin_object();
        writer.key("attempt");
        writer.value_u64(attempt.attempt.raw());
        writer.key("kind");
        writer.value_string(to_string(attempt.kind));
        writer.key("allocation");
        writer.value_u64(attempt.allocation.raw());
        writer.key("units");
        writer.value_u64(attempt.units);
        writer.end_object();
    }
    writer.end_array();
    writer.key("warnings");
    writer.begin_array();
    for (const auto& warning : warnings) {
        writer.value_string(warning);
    }
    writer.end_array();
    writer.end_object();
    return writer.str();
}

// ---------------------------------------------------------------------------
// DurableStore
// ---------------------------------------------------------------------------

DurableStore::DurableStore() = default;
DurableStore::~DurableStore() { close(); }

Status DurableStore::configure(const PersistenceConfig& config) {
    BF_TRY(config.validate());
    config_ = config;
    journal_path_ = detail::join_path(config.directory, "fabric.bfj");
    snapshot_path_ = detail::join_path(config.directory, "fabric.bfs");
    configured_ = true;
    return Status::success();
}

void DurableStore::close() noexcept {
    journal_.close();
    configured_ = false;
}

Status DurableStore::recover(const SnapshotVisitor& snapshot_visitor,
                             const RecordVisitor& record_visitor, RecoveryReport* report) {
    if (!configured_) {
        return Status(ErrorCode::LifecycleViolation, "durable store is not configured");
    }
    RecoveryReport local;
    BF_TRY(detail::ensure_directory(config_.directory));

    bool snapshot_existed = false;
    auto snapshot_bytes =
        detail::read_file_bounded(snapshot_path_, config_.max_snapshot_bytes, &snapshot_existed);
    if (!snapshot_bytes.ok()) return snapshot_bytes.status();

    bool journal_existed = false;
    auto journal_bytes =
        detail::read_file_bounded(journal_path_, config_.max_journal_bytes, &journal_existed);
    if (!journal_bytes.ok()) return journal_bytes.status();

    local.durable_state_present = snapshot_existed || journal_existed;
    u64 recovered_journal_bytes =
        journal_existed ? static_cast<u64>(journal_bytes.value().size()) : 0;

    if (snapshot_existed && !snapshot_bytes.value().empty()) {
        std::vector<u8> payload;
        auto header = decode_snapshot(snapshot_bytes.value().data(), snapshot_bytes.value().size(),
                                      static_cast<usize>(config_.max_snapshot_bytes), &payload);
        if (!header.ok()) {
            return header.status();
        }
        local.snapshot_loaded = true;
        local.snapshot_integrity_verified = true;
        local.snapshot_format_version = header.value().format_version;
        local.snapshot_payload_bytes = payload.size();
        local.durable_epoch = header.value().epoch;
        local.previous_boot = header.value().boot;
        if (snapshot_visitor) {
            BF_TRY(snapshot_visitor(header.value(), payload.data(), payload.size()));
        }
    }

    if (journal_existed && !journal_bytes.value().empty()) {
        local.journal_loaded = true;
        local.journal_format_version = BUFFER_FABRIC_JOURNAL_FORMAT_VERSION;
        BF_TRY(journal_.open_read(journal_path_, config_.max_journal_bytes));
        u64 ordinal = 0;
        bool damaged = false;
        std::string damage_reason;
        for (;;) {
            auto record = journal_.read_next();
            if (!record.ok()) {
                if (record.code() == ErrorCode::JournalTruncated ||
                    record.code() == ErrorCode::CorruptRecord ||
                    record.code() == ErrorCode::IntegrityFailure ||
                    record.code() == ErrorCode::OversizedMessage ||
                    record.code() == ErrorCode::VersionMismatch) {
                    damaged = true;
                    damage_reason = record.status().to_string();
                    break;
                }
                return record.status();
            }
            if (record.value().eof) break;
            local.journal_records_read += 1;
            const Status applied = record_visitor
                                       ? record_visitor(record.value().payload.data(),
                                                        record.value().payload.size(), ordinal)
                                       : Status::success();
            if (!applied.ok()) {
                local.journal_records_rejected += 1;
                journal_.close();
                return applied;
            }
            local.journal_records_applied += 1;
            ++ordinal;
        }
        const u64 damage_offset = journal_.read_offset();
        const u64 damaged_bytes =
            journal_.file_size() > damage_offset ? journal_.file_size() - damage_offset : 0;
        const u64 damaged_records = damaged_bytes == 0 ? 0 : 1;
        journal_.close();

        if (damaged) {
            // Determine whether intact records follow the damaged region. A
            // damaged region that is followed by a valid frame is mid-file
            // corruption, not a torn tail, and is refused by default.
            bool intact_after = false;
            const auto& raw = journal_bytes.value();
            if (damage_offset + 1 < raw.size()) {
                for (usize probe = static_cast<usize>(damage_offset) + 1;
                     probe + kFrameHeaderBytes <= raw.size(); ++probe) {
                    ByteReader header(raw.data() + probe, kFrameHeaderBytes);
                    u32 magic = 0;
                    u32 version = 0;
                    u32 payload_bytes = 0;
                    u32 crc = 0;
                    if (!header.get_u32(&magic) || !header.get_u32(&version) ||
                        !header.get_u32(&payload_bytes) || !header.get_u32(&crc)) {
                        break;
                    }
                    if (magic != kJournalFrameMagic) continue;
                    if (version != BUFFER_FABRIC_JOURNAL_FORMAT_VERSION) continue;
                    if (probe + kFrameHeaderBytes + payload_bytes > raw.size()) continue;
                    if (crc32c(raw.data() + probe + kFrameHeaderBytes, payload_bytes) != crc) continue;
                    intact_after = true;
                    break;
                }
            }
            if (intact_after && config_.refuse_on_midfile_damage) {
                char detail[256];
                std::snprintf(detail, sizeof(detail),
                              "journal contains damaged records followed by intact records "
                              "(damage at byte %llu of %llu, %llu bytes follow; %s)",
                              static_cast<unsigned long long>(damage_offset),
                              static_cast<unsigned long long>(raw.size()),
                              static_cast<unsigned long long>(damaged_bytes),
                              damage_reason.c_str());
                return Status(ErrorCode::CorruptRecord, detail);
            }
            if (!config_.truncate_damaged_tail) {
                return Status(ErrorCode::IntegrityFailure,
                              "journal tail is damaged and tail truncation is disabled");
            }
            BF_TRY(journal_.open_append(journal_path_, config_.max_journal_bytes));
            BF_TRY(journal_.truncate_to(damage_offset));
            BF_TRY(journal_.sync());
            journal_.close();
            local.journal_bytes_discarded = damaged_bytes;
            local.journal_records_discarded = damaged_records;
            local.warnings.push_back("journal tail was damaged by an interrupted write and discarded: " +
                                     damage_reason);
            local.outcome = RecoveryOutcome::TailTruncated;
            // The repaired journal is exactly as long as the last intact
            // record. Recording the pre-repair length here would make the next
            // append re-extend the file and zero-fill the gap.
            recovered_journal_bytes = damage_offset;
        } else {
            local.outcome = local.snapshot_loaded ? RecoveryOutcome::JournalReplayed
                                                  : RecoveryOutcome::JournalOnly;
        }
    } else {
        local.outcome = local.snapshot_loaded ? RecoveryOutcome::SnapshotOnly
                                              : RecoveryOutcome::Created;
    }

    journal_bytes_ = recovered_journal_bytes;
    recovery_count_ += 1;
    if (report != nullptr) *report = local;
    return Status::success();
}

Status DurableStore::append_record(const void* payload, usize size) {
    if (!configured_) {
        return Status(ErrorCode::LifecycleViolation, "durable store is not configured");
    }
    if (!journal_.writable()) {
        BF_TRY(journal_.open_append(journal_path_, config_.max_journal_bytes));
        // Trust the bytes that are actually on disk. Recovery has already
        // removed any damaged tail; forcing the file back to a remembered
        // length here would extend it with zeros and corrupt the record that
        // follows the gap.
        journal_bytes_ = journal_.bytes_written();
    }
    BF_TRY(journal_.append(payload, size));
    if (config_.mode == DurabilityMode::JournalSync) {
        BF_TRY(journal_.sync());
    }
    journal_bytes_ = journal_.bytes_written();
    return Status::success();
}

Status DurableStore::write_snapshot(const SnapshotHeader& header, const void* payload, usize size) {
    if (!configured_) {
        return Status(ErrorCode::LifecycleViolation, "durable store is not configured");
    }
    if (size > config_.max_snapshot_bytes) {
        return Status(ErrorCode::SnapshotOversized, "snapshot payload exceeds the configured bound");
    }
    SnapshotHeader effective = header;
    effective.format_version = BUFFER_FABRIC_PERSISTENCE_FORMAT_VERSION;
    const std::vector<u8> bytes = encode_snapshot(effective, payload, size);
    const std::string temp = snapshot_path_ + ".tmp";
    const bool durable = config_.mode == DurabilityMode::JournalSync;
    BF_TRY(detail::write_file_atomic(snapshot_path_, temp, bytes.data(), bytes.size(), durable));
    BF_TRY(detail::sync_directory(config_.directory));

    // The journal is truncated only after the snapshot is durably in place.
    // Replaying a non-empty journal over a snapshot is harmless because every
    // record is an idempotent full-object upsert.
    journal_.close();
    BF_TRY(journal_.open_append(journal_path_, config_.max_journal_bytes));
    BF_TRY(journal_.truncate_to(0));
    BF_TRY(journal_.sync());
    journal_bytes_ = 0;
    return Status::success();
}

Status DurableStore::truncate_journal_to(u64 offset) {
    BF_TRY(journal_.truncate_to(offset));
    journal_bytes_ = offset;
    return Status::success();
}

}  // namespace buffer_fabric
