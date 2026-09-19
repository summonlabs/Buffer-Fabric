#pragma once

#include <functional>
#include <string>
#include <vector>

#include "buffer_fabric/allocation.hpp"
#include "buffer_fabric/codec.hpp"
#include "buffer_fabric/config.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/time.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

// ---------------------------------------------------------------------------
// Durability model
//
// Durable state exists so that pool definitions, policy, committed allocations,
// lineage, provenance, fences and the fabric epoch survive a restart. It
// deliberately does NOT preserve:
//
//   * liveness of any worker or client;
//   * freshness of any pressure evidence;
//   * authority bound to a previous boot incarnation or epoch;
//   * any physical device effect.
//
// All of those are re-established from scratch after a restart, and evidence
// that was fresh before the restart is restored as STALE, requiring explicit
// revalidation.
// ---------------------------------------------------------------------------
enum class DurabilityMode : u8 {
    /// In-memory only. Durable mutation boundaries are not claimed.
    None = 0,
    /// Every accepted mutation is appended and flushed to the journal before
    /// the mutation is acknowledged.
    JournalSync = 1,
};

[[nodiscard]] BF_API std::string_view to_string(DurabilityMode mode) noexcept;

struct BF_API PersistenceConfig {
    std::string directory{};
    DurabilityMode mode{DurabilityMode::None};
    /// Journal is compacted into a snapshot once it exceeds this size.
    u64 compact_journal_bytes{64ull << 20};
    /// Hard bound on journal growth between compactions.
    u64 max_journal_bytes{512ull << 20};
    /// Snapshot payload bound.
    u64 max_snapshot_bytes{kMaxSnapshotBytes};
    /// When true a damaged journal tail is discarded up to the last intact
    /// record; when false any damage refuses recovery.
    bool truncate_damaged_tail{true};
    /// When true, a damaged record followed by other intact records is
    /// reported as corruption and recovery fails.
    bool refuse_on_midfile_damage{true};

    [[nodiscard]] Status validate() const;
};

/// Payload opcodes. The journal is a sequence of framed records whose payload
/// begins with one of these bytes.
enum class JournalOp : u8 {
    EpochAdvance = 1,
    PoolUpsert = 2,
    PoolRemoved = 3,
    PolicyUpsert = 4,
    QueueUpsert = 5,
    QueueRetired = 6,
    CapacitySet = 7,
    AllocationReserved = 8,
    AllocationCommitted = 9,
    AllocationReleased = 10,
    AllocationReclaimed = 11,
    AllocationFenced = 12,
    AllocationRevalidated = 13,
    AllocationExpired = 14,
    PressureObserved = 15,
    AttemptRecorded = 16,
    Checkpoint = 17,
    BackendRegistered = 18,
};

[[nodiscard]] BF_API std::string_view to_string(JournalOp op) noexcept;

inline constexpr u32 kJournalFrameMagic = 0x42464A31u;  // "BFJ1"
inline constexpr u32 kSnapshotFrameMagic = 0x42465331u; // "BFS1"

// ---------------------------------------------------------------------------
// Journal
//
// An append-only file of length-prefixed, CRC-32C protected frames. A partial
// or damaged record ends replay; the store then determines whether the damage
// is confined to the tail (recoverable, truncate) or followed by intact
// records (corruption, refuse).
// ---------------------------------------------------------------------------
class BF_API Journal {
public:
    Journal() = default;
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    [[nodiscard]] Status open_append(const std::string& path, u64 max_bytes);
    [[nodiscard]] Status open_read(const std::string& path, u64 max_bytes);
    void close() noexcept;

    [[nodiscard]] bool readable() const noexcept { return read_handle_ != nullptr; }
    [[nodiscard]] bool writable() const noexcept { return write_handle_ != nullptr; }

    [[nodiscard]] Status append(const void* payload, usize size);
    [[nodiscard]] Status sync();
    [[nodiscard]] Status truncate_to(u64 offset);

    [[nodiscard]] u64 records_written() const noexcept { return records_written_; }
    [[nodiscard]] u64 bytes_written() const noexcept { return bytes_written_; }
    [[nodiscard]] u64 read_offset() const noexcept { return read_offset_; }
    [[nodiscard]] u64 file_size() const noexcept { return file_size_; }
    [[nodiscard]] u64 records_read() const noexcept { return records_read_; }

    struct ReadResult {
        bool eof{false};
        std::vector<u8> payload{};
        u64 record_offset{0};
        u64 next_offset{0};
    };

    /// Read the next record. Ok with eof == true at clean end of file.
    [[nodiscard]] Result<ReadResult> read_next();

private:
    void* read_handle_{nullptr};
    void* write_handle_{nullptr};
    u64 max_bytes_{0};
    u64 records_written_{0};
    u64 bytes_written_{0};
    u64 read_offset_{0};
    u64 file_size_{0};
    u64 records_read_{0};
};

// ---------------------------------------------------------------------------
// Snapshot container
//
// Layout:
//   magic u32 | format_version u32 | flags u32 | payload_bytes u32
//   epoch u64 | boot_hi u64 | boot_lo u64 | record_count u64
//   payload[payload_bytes]
//   payload_crc u32 | header_crc u32 | end_magic u32
//
// The header CRC covers the fixed header, so a torn or partially overwritten
// header is detected before any payload is trusted.
// ---------------------------------------------------------------------------
inline constexpr u32 kSnapshotEndMagic = 0x42465345u;  // "BFSE"
inline constexpr usize kSnapshotHeaderBytes = 4 + 4 + 4 + 4 + 8 + 8 + 8 + 8;

struct BF_API SnapshotHeader {
    u32 format_version{0};
    u32 flags{0};
    u32 payload_bytes{0};
    EpochId epoch{};
    BootIncarnation boot{};
    u64 record_count{0};
};

enum class SnapshotFault : u8 {
    None = 0,
    BadMagic = 1,
    BadEndMagic = 2,
    VersionUnsupported = 3,
    HeaderCrcMismatch = 4,
    PayloadCrcMismatch = 5,
    Oversized = 6,
    Truncated = 7,
    HeaderTruncated = 8,
};

[[nodiscard]] BF_API std::string_view to_string(SnapshotFault fault) noexcept;

[[nodiscard]] BF_API std::vector<u8> encode_snapshot(const SnapshotHeader& header,
                                                     const void* payload, usize payload_size);

[[nodiscard]] BF_API Result<SnapshotHeader> decode_snapshot(const u8* data, usize size,
                                                            usize max_payload,
                                                            std::vector<u8>* payload_out);

// ---------------------------------------------------------------------------
// Recovery reporting
// ---------------------------------------------------------------------------
enum class RecoveryOutcome : u8 {
    /// No durable state existed; a new fabric image was created.
    Created = 0,
    /// A snapshot was loaded and the journal was empty.
    SnapshotOnly = 1,
    /// A snapshot was loaded and the journal replayed cleanly.
    JournalReplayed = 2,
    /// The journal tail was damaged by an interrupted write and discarded.
    TailTruncated = 3,
    /// No snapshot; the journal was replayed from the beginning.
    JournalOnly = 4,
};

[[nodiscard]] BF_API std::string_view to_string(RecoveryOutcome outcome) noexcept;

/// A durable mutation whose intent exists but whose completion was never
/// recorded. It is reported, never silently promoted to committed state.
struct BF_API UnfinishedAttempt {
    AttemptId attempt{};
    AttemptKind kind{AttemptKind::Allocate};
    AllocationId allocation{};
    u64 units{0};
    Tick journaled_at{0};
    Digest request_digest{};
};

struct BF_API RecoveryReport {
    RecoveryOutcome outcome{RecoveryOutcome::Created};
    bool durable_state_present{false};
    bool snapshot_loaded{false};
    bool snapshot_integrity_verified{false};
    bool journal_loaded{false};
    u32 snapshot_format_version{0};
    u32 journal_format_version{0};
    u64 snapshot_payload_bytes{0};
    u64 journal_records_read{0};
    u64 journal_records_applied{0};
    u64 journal_records_rejected{0};
    u64 journal_bytes_discarded{0};
    u64 journal_records_discarded{0};
    EpochId durable_epoch{};
    EpochId new_epoch{};
    BootIncarnation previous_boot{};
    BootIncarnation boot{};
    u64 pools_restored{0};
    u64 policies_restored{0};
    u64 queues_restored{0};
    u64 allocations_restored{0};
    u64 allocations_restored_as_uncommitted{0};
    u64 pressure_snapshots_invalidated{0};
    u64 fences_restored{0};
    std::vector<UnfinishedAttempt> unfinished_attempts{};
    std::vector<std::string> warnings{};

    [[nodiscard]] std::string to_json() const;
};

// ---------------------------------------------------------------------------
// Durable store
// ---------------------------------------------------------------------------
class BF_API DurableStore {
public:
    using RecordVisitor = std::function<Status(const u8* payload, usize size, u64 ordinal)>;
    using SnapshotVisitor =
        std::function<Status(const SnapshotHeader& header, const u8* payload, usize size)>;

    DurableStore();
    ~DurableStore();
    DurableStore(const DurableStore&) = delete;
    DurableStore& operator=(const DurableStore&) = delete;

    [[nodiscard]] Status configure(const PersistenceConfig& config);
    [[nodiscard]] const PersistenceConfig& config() const noexcept { return config_; }
    [[nodiscard]] bool enabled() const noexcept { return config_.mode != DurabilityMode::None; }

    [[nodiscard]] const std::string& journal_path() const noexcept { return journal_path_; }
    [[nodiscard]] const std::string& snapshot_path() const noexcept { return snapshot_path_; }

    /// Load the snapshot (if any) and replay the journal. The snapshot visitor
    /// is called first (when a snapshot exists) and then the record visitor is
    /// called once per intact journal payload, in order.
    [[nodiscard]] Status recover(const SnapshotVisitor& snapshot_visitor,
                                 const RecordVisitor& record_visitor, RecoveryReport* report);

    /// Append one framed record and (in JournalSync mode) flush it.
    [[nodiscard]] Status append_record(const void* payload, usize size);

    /// Replace the snapshot atomically and truncate the journal.
    [[nodiscard]] Status write_snapshot(const SnapshotHeader& header, const void* payload,
                                        usize size);

    /// Discard the journal up to the given offset after a damage recovery.
    [[nodiscard]] Status truncate_journal_to(u64 offset);

    void close() noexcept;

    [[nodiscard]] u64 journal_bytes() const noexcept { return journal_bytes_; }
    [[nodiscard]] bool needs_compaction() const noexcept {
        return journal_bytes_ >= config_.compact_journal_bytes;
    }
    [[nodiscard]] u64 recovery_count() const noexcept { return recovery_count_; }

private:
    PersistenceConfig config_{};
    std::string journal_path_{};
    std::string snapshot_path_{};
    Journal journal_{};
    u64 journal_bytes_{0};
    u64 recovery_count_{0};
    bool configured_{false};
};

}  // namespace buffer_fabric
