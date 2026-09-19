#pragma once

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

// ---------------------------------------------------------------------------
// Error codes
//
// Every failure path in the runtime maps onto exactly one of these codes. The
// code is a stable part of the public contract; messages are diagnostic only.
// ---------------------------------------------------------------------------
enum class ErrorCode : u16 {
    Ok = 0,

    // Structural / caller errors
    InvalidArgument = 1,
    NotFound = 2,
    AlreadyExists = 3,
    NoChange = 4,
    NameTooLong = 5,
    BoundedResourceExhausted = 6,
    Overflow = 7,
    Underflow = 8,
    Unsupported = 9,
    NotImplemented = 10,
    Cancelled = 11,
    ShuttingDown = 12,
    LifecycleViolation = 13,
    WrongState = 14,

    // Topology / identity closure
    UnknownPool = 20,
    UnknownQueue = 21,
    UnknownAllocation = 22,
    UnknownPolicy = 23,
    UnknownBackend = 24,
    UnknownResource = 25,
    DuplicatePool = 26,
    DuplicateAllocation = 27,
    DuplicatePolicy = 28,
    DuplicateQueue = 29,
    PoolCycle = 30,
    PoolDepthExceeded = 31,
    DuplicateName = 32,

    // Capacity / accounting
    CapacityExceeded = 40,
    ProtectedHeadroomViolation = 41,
    OvercommitLimitExceeded = 42,
    BorrowLimitExceeded = 43,
    AccountingViolation = 44,
    InvariantViolation = 45,
    ShareExceeded = 46,
    HardLimitExceeded = 47,
    PolicyForbids = 48,

    // Generation / epoch / authority
    StaleGeneration = 60,
    StaleEpoch = 61,
    StaleAuthority = 62,
    StaleEvidence = 63,
    EvidenceUnknown = 64,
    Fenced = 65,
    AttemptConflict = 66,
    EpochRegression = 67,

    // Persistence
    PersistenceError = 80,
    CorruptRecord = 81,
    VersionMismatch = 82,
    IntegrityFailure = 83,
    JournalTruncated = 84,
    SnapshotOversized = 85,

    // Backend
    BackendError = 90,
    BackendNotReady = 91,

    // Transport / protocol
    TransportError = 100,
    ProtocolViolation = 101,
    OversizedMessage = 102,
    TruncatedMessage = 103,
    PeerClosed = 104,
    ProtocolVersionMismatch = 105,
};

[[nodiscard]] BF_API std::string_view to_string(ErrorCode code) noexcept;

/// True when the code denotes a successful (non-error) outcome.
[[nodiscard]] inline bool is_ok(ErrorCode code) noexcept { return code == ErrorCode::Ok; }

// ---------------------------------------------------------------------------
// Status
//
// A code plus a bounded diagnostic message. The message is truncated to
// kMaxStatusMessageBytes at construction so that no caller-supplied text can
// grow a Status without bound.
// ---------------------------------------------------------------------------
class BF_API Status {
public:
    Status() noexcept = default;
    Status(ErrorCode code, std::string_view message);
    explicit Status(ErrorCode code);

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] static Status success() noexcept { return Status{}; }

    /// Self accessor so that a Status can be used with the same propagation
    /// helpers as a Result. A Status is its own status.
    [[nodiscard]] const Status& status() const noexcept { return *this; }

private:
    ErrorCode code_{ErrorCode::Ok};
    std::string message_{};
};

/// Build a Status from a code and a list of " key=value" fragments, truncated
/// to the bound. Fragments are joined verbatim.
[[nodiscard]] BF_API Status make_status(ErrorCode code, std::string_view text);

// ---------------------------------------------------------------------------
// Result<T>
//
// A value or a Status. There is no implicit error swallowing: accessing the
// value of a failed Result is a programming error and terminates.
// ---------------------------------------------------------------------------
template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Status status) : storage_(std::in_place_index<1>, std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Status& status() const noexcept {
        return storage_.index() == 1 ? std::get<1>(storage_) : kSuccess;
    }
    [[nodiscard]] ErrorCode code() const noexcept { return status().code(); }

    [[nodiscard]] T& value() & {
        if (storage_.index() != 0) std::terminate();
        return std::get<0>(storage_);
    }
    [[nodiscard]] const T& value() const& {
        if (storage_.index() != 0) std::terminate();
        return std::get<0>(storage_);
    }
    [[nodiscard]] T&& value() && {
        if (storage_.index() != 0) std::terminate();
        return std::get<0>(std::move(storage_));
    }

    [[nodiscard]] T value_or(T fallback) const {
        return storage_.index() == 0 ? std::get<0>(storage_) : std::move(fallback);
    }

private:
    static inline const Status kSuccess{};
    std::variant<T, Status> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() noexcept = default;
    Result(Status status) : status_(std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }
    [[nodiscard]] ErrorCode code() const noexcept { return status_.code(); }

private:
    Status status_{};
};

using VoidResult = Result<void>;

/// Propagate a failed Result out of the current function.
#define BF_TRY(expr)                                     \
    do {                                                 \
        auto bf_try_status_ = (expr);                    \
        if (!bf_try_status_.ok()) {                      \
            return bf_try_status_.status();              \
        }                                                \
    } while (false)

/// Assign a Result value or propagate the failure.
#define BF_ASSIGN(dest, expr)                            \
    do {                                                 \
        auto bf_res_ = (expr);                           \
        if (!bf_res_.ok()) {                             \
            return bf_res_.status();                     \
        }                                                \
        (dest) = std::move(bf_res_).value();             \
    } while (false)

/// Return a Status-constructed failure with a literal message.
#define BF_FAIL(code, message) return ::buffer_fabric::Status((code), (message))

}  // namespace buffer_fabric
