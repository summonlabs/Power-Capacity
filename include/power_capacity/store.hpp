#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "power_capacity/ids.hpp"
#include "power_capacity/limits.hpp"
#include "power_capacity/state.hpp"
#include "power_capacity/status.hpp"

namespace power_capacity {

enum class StoreAccess : std::uint8_t { ReadOnly = 0, ReadWrite = 1 };

/// The complete set of preconditions for reading a store.
struct StoreReadOptions {
  ResourceLimits limits = ResourceLimits::defaults();
  /// When present, the store must carry exactly this identity or the read fails
  /// with `StatusCode::StaleAuthority`. This is the primary defense against
  /// silently adopting a swapped or unrelated store.
  std::optional<StoreIdentity> expected_store_identity;
  /// When present, the store must be committed at or above this generation or the
  /// read fails with `StatusCode::StaleGeneration`. A store file can be replaced
  /// by an older but perfectly valid copy of itself, which no checksum can
  /// detect; a durable floor recorded elsewhere is what detects it.
  std::optional<Generation> minimum_generation;
  /// When true, refuse to read a store whose recorded creation path differs from
  /// the path being read, with `StatusCode::Conflict`.
  bool enforce_path_binding = false;
};

struct StoreOpenOptions {
  std::filesystem::path path;
  StoreAccess access = StoreAccess::ReadWrite;
  /// Create the store when the file does not exist. With `ReadOnly` access a
  /// missing file is always `NotFound`. A missing parent directory is never
  /// created implicitly.
  bool create_if_missing = false;
  std::optional<StoreIdentity> expected_store_identity;
  std::optional<Generation> minimum_generation;
  bool enforce_path_binding = false;
  /// Recorded in the store when it is created. Ignored for an existing store.
  Tick created_at;
  /// Authority binding recorded in the store when it is created. Ignored for an
  /// existing store.
  Epoch epoch;
  Incarnation incarnation;
  ResourceLimits limits = ResourceLimits::defaults();
};

/// Inspection view of an open store.
struct StoreInfo {
  std::filesystem::path path;
  StoreIdentity store_identity;
  Generation generation;
  Epoch epoch;
  Incarnation incarnation;
  Tick created_at;
  Tick updated_at;
  Tick last_revalidated_at;
  std::string recorded_path;
  bool path_binding_matches = true;
  bool writable = false;
  bool created = false;
  std::uint32_t format_version = 0;
  std::uint64_t file_bytes = 0;
  std::uint64_t publish_count = 0;
  std::size_t domain_count = 0;
  std::size_t load_count = 0;
  std::size_t group_count = 0;
  std::size_t source_count = 0;
  std::size_t applied_operation_count = 0;
  std::uint32_t payload_crc32c = 0;
};

/// Timings of one durable commit, in nanoseconds, from `std::chrono::steady_clock`.
/// These are local measurements for the caller's own diagnostics and benchmarks.
/// Nothing here is transmitted anywhere.
struct CommitMetrics {
  std::uint64_t encode_nanos = 0;
  std::uint64_t staging_write_nanos = 0;
  std::uint64_t verify_nanos = 0;
  std::uint64_t publish_nanos = 0;
  std::uint64_t cleanup_nanos = 0;
  std::uint64_t total_nanos = 0;
  std::uint64_t bytes_written = 0;
};

struct CommitResult {
  Generation generation;
  AttemptId attempt;
  CommitMetrics metrics;
  /// True when the same idempotency key had already been applied and the commit
  /// was therefore skipped.
  bool already_applied = false;
};

/// UTF-8 rendering of a path, safe for any path the platform accepts.
///
/// `std::filesystem::path::string()` on Windows converts through the active ANSI
/// code page and throws for a path containing a character that page cannot
/// represent. Any consumer that prints or compares a store path should use this
/// instead, so that a legitimate non-ASCII path is never turned into an
/// exception.
std::string path_to_utf8(const std::filesystem::path& path);

/// Versioned, integrity-checked persistence for one capacity state.
///
/// Commit protocol, in order:
///   plan -> validate -> reserve generation and attempt -> write staging ->
///   flush -> verify -> atomic publish -> cleanup
///
/// Nothing is visible at the store path until the atomic publish, and no state is
/// published in memory unless the publish succeeded.
class CapacityStore {
 public:
  CapacityStore() = default;
  ~CapacityStore();
  CapacityStore(const CapacityStore&) = delete;
  CapacityStore& operator=(const CapacityStore&) = delete;
  CapacityStore(CapacityStore&&) noexcept;
  CapacityStore& operator=(CapacityStore&&) noexcept;

  /// Opens or creates a store. Read-write opens take an operating-system level
  /// exclusive lock, so a second read-write open of the same path fails with
  /// `StatusCode::LockConflict`. The lock is released by the operating system
  /// when the holding process exits, including abnormal exit.
  static Result<std::shared_ptr<CapacityStore>> open(const StoreOpenOptions& options);

  /// Strict read path used by `open`, `verify`, and every inspection tool.
  /// Rejects truncated, oversized, wrong-version, wrong-endian, corrupt, and
  /// structurally invalid artifacts, and enforces the caller's identity, path
  /// binding, and generation floor.
  static Result<std::shared_ptr<const CapacityState>> read_file(const std::filesystem::path& path,
                                                                const StoreReadOptions& options);

  /// Equivalent to `read_file`, with the same strictness. Provided so that audit
  /// and inspection commands cannot drift away from the normal read path.
  static Status verify_file(const std::filesystem::path& path,
                            const StoreReadOptions& options = StoreReadOptions{});

  const StoreInfo& info() const noexcept { return info_; }
  bool writable() const noexcept { return info_.writable; }
  bool is_open() const noexcept { return open_; }

  /// The state that was read when this store was opened. It is the exact snapshot
  /// the store's own metadata describes, so a caller that needs both does not have
  /// to read the file a second time and cannot observe a different generation in
  /// each. It is never updated by a later commit; the engine owns the published
  /// state.
  const std::shared_ptr<const CapacityState>& opened_state() const noexcept {
    return opened_state_;
  }

  /// Durably publishes a new state. The state's generation must be exactly one
  /// greater than the current committed generation, and its store identity must
  /// match. On any failure before the publish the previous committed state is
  /// untouched and the in-memory state is not published by the caller.
  Result<CommitResult> commit(const CapacityState& state);

  /// Releases the writer lock. Idempotent.
  Status close();

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  std::shared_ptr<const CapacityState> opened_state_;
  StoreInfo info_;
  bool open_ = false;
};

}  // namespace power_capacity
