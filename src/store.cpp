#include "power_capacity/store.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "detail/file_lock.hpp"
#include "detail/platform_io.hpp"
#include "detail/serialization.hpp"

namespace power_capacity {
namespace {

constexpr const char* kLockSuffix = ".lock";
constexpr const char* kStagingMarker = ".stg-";
constexpr std::size_t kMaxStagingSweep = 64;

std::uint64_t now_nanos() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

/// Normalizes a path for the creation-path binding check. Case is folded on
/// Windows because the platform's file system is case-insensitive.
std::string normalize_path(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
  if (error) {
    normalized = std::filesystem::absolute(path, error);
    if (error) {
      normalized = path;
    }
  }
  std::string text = detail::path_utf8(normalized);
#if defined(_WIN32)
  for (char& character : text) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
#endif
  return text;
}

std::filesystem::path lock_path_for(const std::filesystem::path& store_path) {
  std::filesystem::path lock = store_path;
  lock += kLockSuffix;
  return lock;
}

/// Removes leftover staging files for this store. Only regular files whose names
/// begin with the store file name followed by the staging marker are touched, and
/// the number of removals is bounded, so a sweep can never wander outside the
/// store's own directory or delete an unrelated file.
void sweep_staging_files(const std::filesystem::path& store_path) noexcept {
  const std::filesystem::path directory = store_path.parent_path().empty()
                                              ? std::filesystem::path(".")
                                              : store_path.parent_path();
  const std::string prefix = detail::path_utf8(store_path.filename()) + kStagingMarker;
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    return;
  }
  std::size_t removed = 0;
  for (const std::filesystem::directory_entry& entry : iterator) {
    if (removed >= kMaxStagingSweep) {
      return;
    }
    const std::string name = detail::path_utf8(entry.path().filename());
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    std::error_code status_error;
    const std::filesystem::file_status status = entry.symlink_status(status_error);
    if (status_error || !std::filesystem::is_regular_file(status)) {
      continue;
    }
    std::error_code remove_error;
    if (std::filesystem::remove(entry.path(), remove_error)) {
      ++removed;
    }
  }
}

}  // namespace

std::string path_to_utf8(const std::filesystem::path& path) { return detail::path_utf8(path); }

struct CapacityStore::Impl {
  StoreOpenOptions options;
  std::filesystem::path path;
  detail::ExclusiveFileLock lock;
  SessionId session;
  std::uint64_t attempt_counter = 0;
  std::uint64_t publish_count = 0;
  ResourceLimits limits;
};

CapacityStore::~CapacityStore() = default;
CapacityStore::CapacityStore(CapacityStore&&) noexcept = default;
CapacityStore& CapacityStore::operator=(CapacityStore&&) noexcept = default;

Result<std::shared_ptr<const CapacityState>> CapacityStore::read_file(
    const std::filesystem::path& path, const StoreReadOptions& options) {
  // The strict read path applies the same path validation as the open path, so an
  // inspection tool can never read through a link or a directory that the engine
  // itself would refuse.
  const Status validated = detail::validate_store_path(path);
  if (!validated.ok()) {
    return validated;
  }
  const Result<std::vector<std::byte>> bytes =
      detail::read_file_bounded(path, options.limits.max_store_bytes);
  if (!bytes.ok()) {
    return bytes.status();
  }
  const Result<detail::DecodedArtifact> decoded = detail::decode_artifact(
      std::span<const std::byte>(bytes.value().data(), bytes.value().size()), options.limits);
  if (!decoded.ok()) {
    return decoded.status();
  }

  CapacityContent content = decoded.value().content;
  if (options.expected_store_identity.has_value() &&
      content.store_identity != *options.expected_store_identity) {
    return Status::error(StatusCode::StaleAuthority,
                         "store '" + detail::path_utf8(path) + "' carries identity " +
                             content.store_identity.to_hex() + " but identity " +
                             options.expected_store_identity->to_hex() + " was expected");
  }
  if (options.minimum_generation.has_value() &&
      content.generation.value() < options.minimum_generation->value()) {
    return Status::error(StatusCode::StaleGeneration,
                         "store '" + detail::path_utf8(path) + "' is committed at generation " +
                             std::to_string(content.generation.value()) +
                             ", below the required floor of " +
                             std::to_string(options.minimum_generation->value()) +
                             "; an older copy of an otherwise valid store is a rollback, not a "
                             "recovery");
  }
  if (options.enforce_path_binding && !content.created_path.empty() &&
      content.created_path != normalize_path(path)) {
    return Status::error(StatusCode::Conflict,
                         "store '" + detail::path_utf8(path) + "' was created at '" +
                             content.created_path + "' and path binding is enforced");
  }
  return CapacityState::build(std::move(content), options.limits);
}

Status CapacityStore::verify_file(const std::filesystem::path& path,
                                  const StoreReadOptions& options) {
  const Result<std::shared_ptr<const CapacityState>> state = read_file(path, options);
  if (!state.ok()) {
    return state.status();
  }
  return Status::success();
}

Result<std::shared_ptr<CapacityStore>> CapacityStore::open(const StoreOpenOptions& options) {
  if (options.path.empty()) {
    return Status::error(StatusCode::InvalidArgument, "store path must not be empty");
  }
  const Status publishable = detail::validate_store_path(options.path);
  if (!publishable.ok()) {
    return publishable;
  }
  if (options.created_at.value() < 0) {
    return Status::error(StatusCode::InvalidArgument, "store creation instant must not be negative");
  }

  auto impl = std::make_shared<Impl>();
  impl->options = options;
  impl->path = options.path;
  impl->limits = options.limits;
  impl->session = SessionId::generate();

  auto store = std::shared_ptr<CapacityStore>(new CapacityStore());
  store->impl_ = impl;
  store->open_ = true;
  store->info_.path = options.path;
  store->info_.writable = options.access == StoreAccess::ReadWrite;
  store->info_.format_version = detail::kArtifactFormatVersion;

  const bool exists = std::filesystem::exists(options.path);
  const bool writable = options.access == StoreAccess::ReadWrite;

  if (!exists && !options.create_if_missing) {
    return Status::error(StatusCode::NotFound,
                         "store not found at '" + detail::path_utf8(options.path) + "'");
  }
  if (!exists && !writable) {
    return Status::error(StatusCode::NotFound,
                         "store not found at '" + detail::path_utf8(options.path) +
                             "' and this open is read-only");
  }

  if (writable) {
    Result<detail::ExclusiveFileLock> lock = detail::ExclusiveFileLock::acquire(lock_path_for(options.path));
    if (!lock.ok()) {
      return lock.status();
    }
    impl->lock = std::move(lock.value());
  }

  std::shared_ptr<const CapacityState> state;
  bool created = false;

  if (exists) {
    StoreReadOptions read_options;
    read_options.limits = options.limits;
    read_options.expected_store_identity = options.expected_store_identity;
    read_options.minimum_generation = options.minimum_generation;
    read_options.enforce_path_binding = options.enforce_path_binding;
    const Result<std::shared_ptr<const CapacityState>> read = read_file(options.path, read_options);
    if (!read.ok()) {
      return read.status();
    }
    state = read.value();
  } else {
    // A freshly created store starts at generation 1 with an empty model. The
    // empty model is written through the same commit protocol as any other
    // state, so the file at the store path is never a partial artifact.
    CapacityContent content;
    content.store_identity = StoreIdentity::generate();
    content.generation = Generation(1);
    content.epoch = options.epoch;
    content.incarnation = options.incarnation;
    content.created_at = options.created_at;
    content.updated_at = options.created_at;
    content.last_revalidated_at = options.created_at;
    content.created_path = normalize_path(options.path);
    if (options.expected_store_identity.has_value()) {
      content.store_identity = *options.expected_store_identity;
    }
    const Result<std::shared_ptr<const CapacityState>> built =
        CapacityState::build(std::move(content), options.limits);
    if (!built.ok()) {
      return built.status();
    }
    state = built.value();
    created = true;
    store->info_.store_identity = state->store_identity();
    store->info_.generation = Generation(0);
    store->info_.created_at = state->created_at();
    store->info_.updated_at = state->created_at();
    store->info_.last_revalidated_at = state->created_at();
    store->info_.recorded_path = state->created_path();

    const Result<CommitResult> committed = store->commit(*state);
    if (!committed.ok()) {
      return committed.status();
    }
  }

  if (writable) {
    sweep_staging_files(options.path);
  }

  store->opened_state_ = state;
  store->info_.path = options.path;
  store->info_.store_identity = state->store_identity();
  store->info_.generation = state->generation();
  store->info_.epoch = state->epoch();
  store->info_.incarnation = state->incarnation();
  store->info_.created_at = state->created_at();
  store->info_.updated_at = state->updated_at();
  store->info_.last_revalidated_at = state->last_revalidated_at();
  store->info_.recorded_path = state->created_path();
  store->info_.path_binding_matches =
      state->created_path().empty() || state->created_path() == normalize_path(options.path);
  store->info_.writable = writable;
  store->info_.created = created;
  store->info_.format_version = detail::kArtifactFormatVersion;
  store->info_.domain_count = state->domain_count();
  store->info_.load_count = state->load_count();
  store->info_.group_count = state->group_count();
  store->info_.source_count = state->source_count();
  store->info_.applied_operation_count = state->applied_operations().size();
  const Result<std::uint64_t> size = detail::file_size(options.path);
  store->info_.file_bytes = size.ok() ? size.value() : 0;
  store->open_ = true;
  return store;
}

Result<CommitResult> CapacityStore::commit(const CapacityState& state) {
  if (!open_ || !impl_) {
    return Status::error(StatusCode::Closed, "store is not open");
  }
  if (!info_.writable) {
    return Status::error(StatusCode::ReadOnly, "store was opened read-only");
  }
  if (state.store_identity() != info_.store_identity) {
    return Status::error(StatusCode::StaleAuthority,
                         "state carries store identity " + state.store_identity().to_hex() +
                             " but this store is " + info_.store_identity.to_hex());
  }
  const std::uint64_t expected_generation = info_.generation.value() + 1;
  if (state.generation().value() != expected_generation) {
    return Status::error(StatusCode::StaleGeneration,
                         "commit requires generation " + std::to_string(expected_generation) +
                             " but the state carries generation " +
                             std::to_string(state.generation().value()));
  }

  const std::uint64_t total_start = now_nanos();
  CommitMetrics metrics;

  const std::uint64_t encode_start = now_nanos();
  const Result<detail::EncodedArtifact> encoded =
      detail::encode_artifact(state.content(), impl_->limits);
  if (!encoded.ok()) {
    return encoded.status();
  }
  metrics.encode_nanos = now_nanos() - encode_start;
  metrics.bytes_written = static_cast<std::uint64_t>(encoded.value().bytes.size());
  if (metrics.bytes_written > impl_->limits.max_store_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "encoded artifact of " + std::to_string(metrics.bytes_written) +
                             " bytes exceeds the configured store limit");
  }

  ++impl_->attempt_counter;
  const std::uint64_t attempt = impl_->attempt_counter;
  const std::string session_tag = impl_->session.to_hex().substr(0, 8);
  std::filesystem::path staging = impl_->path;
  staging += std::string(kStagingMarker) + std::to_string(detail::current_process_id()) + "-" +
             session_tag + "-" + std::to_string(attempt);

  const std::uint64_t write_start = now_nanos();
  const Status written = detail::write_file_durable(
      staging,
      std::span<const std::byte>(encoded.value().bytes.data(), encoded.value().bytes.size()),
      impl_->limits.max_store_bytes);
  metrics.staging_write_nanos = now_nanos() - write_start;
  if (!written.ok()) {
    detail::remove_file(staging);
    return written;
  }

  // The staged bytes are re-read and fully re-decoded before the publish. The
  // verification uses the same strict reader as the open path, so an artifact
  // that could not be opened can never be published.
  const std::uint64_t verify_start = now_nanos();
  StoreReadOptions verified_options;
  verified_options.limits = impl_->limits;
  const Result<std::shared_ptr<const CapacityState>> verified =
      read_file(staging, verified_options);
  metrics.verify_nanos = now_nanos() - verify_start;
  if (!verified.ok()) {
    detail::remove_file(staging);
    return Status::error(StatusCode::Corruption,
                         "staged artifact failed verification before publish: " +
                             verified.status().to_string());
  }
  if (verified.value()->generation() != state.generation()) {
    detail::remove_file(staging);
    return Status::error(StatusCode::Corruption,
                         "staged artifact carries generation " +
                             std::to_string(verified.value()->generation().value()) +
                             " instead of " + std::to_string(state.generation().value()));
  }

  const std::uint64_t publish_start = now_nanos();
  const Status published = detail::atomic_replace(staging, impl_->path);
  metrics.publish_nanos = now_nanos() - publish_start;
  if (!published.ok()) {
    detail::remove_file(staging);
    return published;
  }

  const std::uint64_t cleanup_start = now_nanos();
  // Flushing the directory entry makes the rename itself durable. It is not
  // available on Windows without a backup-semantics directory handle, and the
  // status is deliberately ignored here rather than reported as success.
  const Status directory_flush = detail::sync_directory(impl_->path.parent_path());
  (void)directory_flush;
  metrics.cleanup_nanos = now_nanos() - cleanup_start;

  info_.generation = state.generation();
  info_.epoch = state.epoch();
  info_.incarnation = state.incarnation();
  info_.updated_at = state.updated_at();
  info_.last_revalidated_at = state.last_revalidated_at();
  info_.domain_count = state.domain_count();
  info_.load_count = state.load_count();
  info_.group_count = state.group_count();
  info_.source_count = state.source_count();
  info_.applied_operation_count = state.applied_operations().size();
  info_.path_binding_matches =
      state.created_path().empty() || state.created_path() == normalize_path(impl_->path);
  const Result<std::uint64_t> size = detail::file_size(impl_->path);
  if (size.ok()) {
    info_.file_bytes = size.value();
  }
  impl_->publish_count += 1;
  info_.publish_count = impl_->publish_count;
  info_.payload_crc32c = encoded.value().payload_crc32c;

  metrics.total_nanos = now_nanos() - total_start;

  CommitResult result;
  result.generation = state.generation();
  result.attempt = AttemptId(attempt);
  result.metrics = metrics;
  result.already_applied = false;
  return result;
}

Status CapacityStore::close() {
  if (!open_) {
    return Status::success();
  }
  open_ = false;
  if (impl_) {
    const Status released = impl_->lock.release();
    impl_.reset();
    opened_state_.reset();
    info_ = StoreInfo{};
    return released;
  }
  return Status::success();
}

}  // namespace power_capacity
