#include "buffer_fabric/status.hpp"

#include <cstdio>

#include "buffer_fabric/hash.hpp"

namespace buffer_fabric {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::AlreadyExists: return "AlreadyExists";
        case ErrorCode::NoChange: return "NoChange";
        case ErrorCode::NameTooLong: return "NameTooLong";
        case ErrorCode::BoundedResourceExhausted: return "BoundedResourceExhausted";
        case ErrorCode::Overflow: return "Overflow";
        case ErrorCode::Underflow: return "Underflow";
        case ErrorCode::Unsupported: return "Unsupported";
        case ErrorCode::NotImplemented: return "NotImplemented";
        case ErrorCode::Cancelled: return "Cancelled";
        case ErrorCode::ShuttingDown: return "ShuttingDown";
        case ErrorCode::LifecycleViolation: return "LifecycleViolation";
        case ErrorCode::WrongState: return "WrongState";
        case ErrorCode::UnknownPool: return "UnknownPool";
        case ErrorCode::UnknownQueue: return "UnknownQueue";
        case ErrorCode::UnknownAllocation: return "UnknownAllocation";
        case ErrorCode::UnknownPolicy: return "UnknownPolicy";
        case ErrorCode::UnknownBackend: return "UnknownBackend";
        case ErrorCode::UnknownResource: return "UnknownResource";
        case ErrorCode::DuplicatePool: return "DuplicatePool";
        case ErrorCode::DuplicateAllocation: return "DuplicateAllocation";
        case ErrorCode::DuplicatePolicy: return "DuplicatePolicy";
        case ErrorCode::DuplicateQueue: return "DuplicateQueue";
        case ErrorCode::PoolCycle: return "PoolCycle";
        case ErrorCode::PoolDepthExceeded: return "PoolDepthExceeded";
        case ErrorCode::DuplicateName: return "DuplicateName";
        case ErrorCode::CapacityExceeded: return "CapacityExceeded";
        case ErrorCode::ProtectedHeadroomViolation: return "ProtectedHeadroomViolation";
        case ErrorCode::OvercommitLimitExceeded: return "OvercommitLimitExceeded";
        case ErrorCode::BorrowLimitExceeded: return "BorrowLimitExceeded";
        case ErrorCode::AccountingViolation: return "AccountingViolation";
        case ErrorCode::InvariantViolation: return "InvariantViolation";
        case ErrorCode::ShareExceeded: return "ShareExceeded";
        case ErrorCode::HardLimitExceeded: return "HardLimitExceeded";
        case ErrorCode::PolicyForbids: return "PolicyForbids";
        case ErrorCode::StaleGeneration: return "StaleGeneration";
        case ErrorCode::StaleEpoch: return "StaleEpoch";
        case ErrorCode::StaleAuthority: return "StaleAuthority";
        case ErrorCode::StaleEvidence: return "StaleEvidence";
        case ErrorCode::EvidenceUnknown: return "EvidenceUnknown";
        case ErrorCode::Fenced: return "Fenced";
        case ErrorCode::AttemptConflict: return "AttemptConflict";
        case ErrorCode::EpochRegression: return "EpochRegression";
        case ErrorCode::PersistenceError: return "PersistenceError";
        case ErrorCode::CorruptRecord: return "CorruptRecord";
        case ErrorCode::VersionMismatch: return "VersionMismatch";
        case ErrorCode::IntegrityFailure: return "IntegrityFailure";
        case ErrorCode::JournalTruncated: return "JournalTruncated";
        case ErrorCode::SnapshotOversized: return "SnapshotOversized";
        case ErrorCode::BackendError: return "BackendError";
        case ErrorCode::BackendNotReady: return "BackendNotReady";
        case ErrorCode::TransportError: return "TransportError";
        case ErrorCode::ProtocolViolation: return "ProtocolViolation";
        case ErrorCode::OversizedMessage: return "OversizedMessage";
        case ErrorCode::TruncatedMessage: return "TruncatedMessage";
        case ErrorCode::PeerClosed: return "PeerClosed";
        case ErrorCode::ProtocolVersionMismatch: return "ProtocolVersionMismatch";
    }
    return "UnknownErrorCode";
}

Status::Status(ErrorCode code, std::string_view message)
    : code_(code), message_(truncate_to(message, kMaxStatusMessageBytes)) {}

Status::Status(ErrorCode code) : code_(code) {
    if (code != ErrorCode::Ok) {
        message_.assign(buffer_fabric::to_string(code));
    }
}

std::string Status::to_string() const {
    if (code_ == ErrorCode::Ok) return "Ok";
    std::string out(buffer_fabric::to_string(code_));
    if (!message_.empty()) {
        out.append(": ");
        out.append(message_);
    }
    return out;
}

Status make_status(ErrorCode code, std::string_view text) { return Status(code, text); }

std::string truncate_to(std::string_view text, usize max_bytes) {
    if (text.size() <= max_bytes) return std::string(text);
    std::string out(text.substr(0, max_bytes));
    return out;
}

bool is_valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxNameBytes) return false;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

u64 fnv1a64(std::string_view text) noexcept {
    u64 hash = 0xcbf29ce484222325ull;
    for (const char c : text) {
        hash ^= static_cast<u64>(static_cast<unsigned char>(c));
        hash *= 0x100000001b3ull;
    }
    return hash;
}

Digest digest_bytes(const void* data, usize size) noexcept {
    Fnv128 hasher;
    hasher.update(data, size);
    return hasher.digest();
}

std::string Digest::to_hex() const {
    char buffer[33];
    std::snprintf(buffer, sizeof(buffer), "%016llx%016llx", static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buffer);
}

}  // namespace buffer_fabric
