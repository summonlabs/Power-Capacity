#pragma once

#include <cstddef>
#include <cstdint>

namespace power_capacity {

/// Hard bounds applied to every external and persisted input before allocation.
///
/// Limits are part of the authority model: a declaration that exceeds a bound is
/// refused with `StatusCode::LimitExceeded` before any memory is reserved for it.
struct ResourceLimits {
  /// Largest accepted store file, checked against the on-disk size before reading.
  std::uint64_t max_store_bytes = 256ull * 1024ull * 1024ull;
  /// Largest accepted payload section, checked against the declared length.
  std::uint64_t max_payload_bytes = 256ull * 1024ull * 1024ull;
  std::size_t max_domains = 200000;
  std::size_t max_loads = 500000;
  std::size_t max_groups = 100000;
  std::size_t max_members_per_group = 256;
  std::uint32_t max_redundancy_tolerance = 64;
  std::size_t max_sources = 4096;
  std::uint32_t max_tree_depth = 64;
  std::size_t max_identifier_bytes = 128;
  std::size_t max_label_bytes = 256;
  std::size_t max_path_bytes = 4096;
  std::size_t max_applied_operations = 256;
  std::size_t max_idempotency_key_bytes = 64;
  /// Largest magnitude of a single capacity, load, or reserve declaration, 100 GW.
  std::int64_t max_component_milliwatts = 100'000'000'000'000;
  /// Largest magnitude of an aggregate rollup or sum, 1 PW.
  std::int64_t max_aggregate_milliwatts = 1'000'000'000'000'000;

  static ResourceLimits defaults() { return ResourceLimits{}; }
};

}  // namespace power_capacity
