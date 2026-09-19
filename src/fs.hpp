#pragma once

// Internal file-system helpers. Not part of the public API.
//
// Every helper reports failure as a Status instead of throwing, and every read
// enforces an explicit byte bound before allocating.

#include <string>
#include <vector>

#include "buffer_fabric/status.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric::detail {

[[nodiscard]] std::string join_path(const std::string& directory, const std::string& leaf);

[[nodiscard]] bool path_exists(const std::string& path);

[[nodiscard]] Status ensure_directory(const std::string& path);

[[nodiscard]] Status remove_file(const std::string& path);

[[nodiscard]] Result<u64> file_size(const std::string& path);

/// Read the whole file, refusing to allocate more than \p max_bytes.
/// \p existed is set to false when the file was absent (Ok, empty result).
[[nodiscard]] Result<std::vector<u8>> read_file_bounded(const std::string& path, u64 max_bytes,
                                                        bool* existed);

/// Durable append. When \p durable is true the bytes are flushed to stable
/// storage before the call returns.
[[nodiscard]] Status append_file(const std::string& path, const void* data, usize size, bool durable);

/// Durable whole-file write through a temporary sibling plus atomic replace.
[[nodiscard]] Status write_file_atomic(const std::string& path, const std::string& temp_path,
                                       const void* data, usize size, bool durable);

[[nodiscard]] Status truncate_file(const std::string& path, u64 size);

/// Flush a directory entry so that a rename or unlink is durable.
[[nodiscard]] Status sync_directory(const std::string& path);

/// Flush an already-open file descriptor held by the caller. Used by Journal.
[[nodiscard]] Status flush_handle(void* native_handle);

}  // namespace buffer_fabric::detail
