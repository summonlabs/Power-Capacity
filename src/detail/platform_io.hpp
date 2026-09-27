#pragma once

// Internal platform file operations. Not installed.
//
// The store needs four things the C++ standard library does not expose portably:
// a bounded read that refuses an oversized artifact before allocating for it, a
// write that flushes all the way to the storage device, an atomic replace, and a
// directory flush. This header isolates those operations.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "power_capacity/status.hpp"

namespace power_capacity::detail {

/// Reads at most `max_bytes`. The size is checked from the file metadata before
/// any buffer is allocated, so an oversized or maliciously large artifact is
/// refused without allocating for it.
Result<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path,
                                                 std::uint64_t max_bytes);

Result<std::uint64_t> file_size(const std::filesystem::path& path);

/// Creates or truncates `path`, writes every byte, flushes the stream, and then
/// asks the operating system to flush the file's data to the device.
Status write_file_durable(const std::filesystem::path& path, std::span<const std::byte> data,
                          std::uint64_t max_bytes);

/// Flushes a directory entry so that a rename survives an operating-system
/// crash. On Windows there is no portable equivalent that does not require a
/// directory handle opened with backup semantics; this returns a documented
/// `Unsupported` status there and the caller records the limitation rather than
/// claiming durability it did not obtain.
Status sync_directory(const std::filesystem::path& directory);

/// Atomically replaces `to` with `from`. On Windows this is
/// `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)` underneath
/// `std::filesystem::rename`; on POSIX it is `rename(2)`.
Status atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to);

Status remove_file(const std::filesystem::path& path) noexcept;

/// Validates a store path before it is created, read, or published through.
///
/// Rejects a path with no file name, a parent that is missing or is not a
/// directory, a target that is a symbolic link, a target that is a directory, and
/// a target that exists as anything other than a regular file. A path that does
/// not exist yet is accepted, so that a caller can create it.
///
/// This is applied by the open path and by the strict read path alike, so
/// inspection tooling cannot be more permissive than the engine.
Status validate_store_path(const std::filesystem::path& target);

std::uint64_t current_process_id() noexcept;

/// UTF-8 rendering of a path, safe for any path the platform accepts.
///
/// `std::filesystem::path::string()` converts through the active ANSI code page
/// on Windows and throws for a path containing a character that page cannot
/// represent, which would turn a legitimate Unicode store path into an
/// exception. Every diagnostic and every derived file name in this library goes
/// through this helper instead.
std::string path_utf8(const std::filesystem::path& path);

}  // namespace power_capacity::detail
