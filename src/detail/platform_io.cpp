#include "detail/platform_io.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace power_capacity::detail {
namespace {

Status error_from_errno(const std::filesystem::path& path, const char* what, int error) {
  StatusCode code = StatusCode::IoFailure;
  if (error == EACCES || error == EPERM) {
    code = StatusCode::PermissionDenied;
  } else if (error == ENOENT) {
    code = StatusCode::NotFound;
  }
  return Status::error(code, std::string(what) + " '" + path_utf8(path) + "' failed: " +
                                 std::generic_category().message(error));
}

}  // namespace

std::uint64_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string path_utf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return std::string(utf8.begin(), utf8.end());
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    if (error.value() == static_cast<int>(std::errc::no_such_file_or_directory)) {
      return Status::error(StatusCode::NotFound, "file not found: '" + path_utf8(path) + "'");
    }
    return Status::error(StatusCode::IoFailure,
                         "could not read the size of '" + path_utf8(path) + "': " + error.message());
  }
  return static_cast<std::uint64_t>(size);
}

Result<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path,
                                                 std::uint64_t max_bytes) {
  const Result<std::uint64_t> size = power_capacity::detail::file_size(path);
  if (!size.ok()) {
    return size.status();
  }
  if (size.value() > max_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "artifact '" + path_utf8(path) + "' is " + std::to_string(size.value()) +
                             " bytes, above the configured limit of " +
                             std::to_string(max_bytes) + " bytes");
  }

  std::vector<std::byte> buffer(static_cast<std::size_t>(size.value()));
#if defined(_WIN32)
  FILE* file = nullptr;
  const errno_t open_error = _wfopen_s(&file, path.wstring().c_str(), L"rb");
  if (open_error != 0 || file == nullptr) {
    return error_from_errno(path, "opening", open_error == 0 ? EIO : open_error);
  }
  std::size_t read_bytes = 0;
  if (!buffer.empty()) {
    read_bytes = std::fread(buffer.data(), 1, buffer.size(), file);
  }
  const bool short_read = read_bytes != buffer.size();
  const int read_error = short_read ? errno : 0;
  std::fclose(file);
  if (short_read) {
    return error_from_errno(path, "reading", read_error == 0 ? EIO : read_error);
  }
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return error_from_errno(path, "opening", errno);
  }
  std::size_t total = 0;
  while (total < buffer.size()) {
    const ssize_t read_bytes = ::read(descriptor, buffer.data() + total, buffer.size() - total);
    if (read_bytes < 0) {
      const int read_error = errno;
      ::close(descriptor);
      return error_from_errno(path, "reading", read_error);
    }
    if (read_bytes == 0) {
      ::close(descriptor);
      return Status::error(StatusCode::Corruption,
                           "artifact '" + path_utf8(path) + "' shrank while being read");
    }
    total += static_cast<std::size_t>(read_bytes);
  }
  ::close(descriptor);
#endif
  return buffer;
}

Status write_file_durable(const std::filesystem::path& path, std::span<const std::byte> data,
                          std::uint64_t max_bytes) {
  if (data.size() > max_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "refusing to write " + std::to_string(data.size()) +
                             " bytes, above the configured limit of " +
                             std::to_string(max_bytes) + " bytes");
  }
#if defined(_WIN32)
  FILE* file = nullptr;
  const errno_t open_error = _wfopen_s(&file, path.wstring().c_str(), L"wb");
  if (open_error != 0 || file == nullptr) {
    return error_from_errno(path, "creating", open_error == 0 ? EIO : open_error);
  }
  if (!data.empty() && std::fwrite(data.data(), 1, data.size(), file) != data.size()) {
    const int write_error = errno;
    std::fclose(file);
    remove_file(path);
    return error_from_errno(path, "writing", write_error == 0 ? EIO : write_error);
  }
  if (std::fflush(file) != 0) {
    const int flush_error = errno;
    std::fclose(file);
    remove_file(path);
    return error_from_errno(path, "flushing", flush_error == 0 ? EIO : flush_error);
  }
  if (_commit(_fileno(file)) != 0) {
    const int commit_error = errno;
    std::fclose(file);
    remove_file(path);
    return error_from_errno(path, "committing", commit_error == 0 ? EIO : commit_error);
  }
  if (std::fclose(file) != 0) {
    const int close_error = errno;
    remove_file(path);
    return error_from_errno(path, "closing", close_error == 0 ? EIO : close_error);
  }
#else
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  if (descriptor < 0) {
    return error_from_errno(path, "creating", errno);
  }
  std::size_t total = 0;
  while (total < data.size()) {
    const ssize_t written = ::write(descriptor, data.data() + total, data.size() - total);
    if (written < 0) {
      const int write_error = errno;
      ::close(descriptor);
      remove_file(path);
      return error_from_errno(path, "writing", write_error);
    }
    total += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    const int sync_error = errno;
    ::close(descriptor);
    remove_file(path);
    return error_from_errno(path, "flushing", sync_error);
  }
  if (::close(descriptor) != 0) {
    const int close_error = errno;
    remove_file(path);
    return error_from_errno(path, "closing", close_error);
  }
#endif
  return Status::success();
}

Status sync_directory(const std::filesystem::path& directory) {
#if defined(_WIN32)
  (void)directory;
  return Status::error(StatusCode::Unsupported,
                       "directory flush is not available on this platform without opening a "
                       "directory handle with backup semantics");
#else
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return error_from_errno(directory, "opening directory", errno);
  }
  const int result = ::fsync(descriptor);
  const int sync_error = errno;
  ::close(descriptor);
  if (result != 0) {
    return error_from_errno(directory, "flushing directory", sync_error);
  }
  return Status::success();
#endif
}

Status atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code error;
  std::filesystem::rename(from, to, error);
  if (error) {
    return Status::error(StatusCode::IoFailure,
                         "atomic publish of '" + path_utf8(to) + "' from '" + path_utf8(from) +
                             "' failed: " + error.message());
  }
  return Status::success();
}

Status remove_file(const std::filesystem::path& path) noexcept {
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  if (error) {
    return Status::error(StatusCode::IoFailure,
                         "could not remove '" + path_utf8(path) + "': " + error.message());
  }
  if (!removed) {
    return Status::error(StatusCode::NotFound, "nothing to remove at '" + path_utf8(path) + "'");
  }
  return Status::success();
}

Status validate_store_path(const std::filesystem::path& target) {
  if (target.empty() || !target.has_filename()) {
    return Status::error(StatusCode::InvalidArgument,
                         "store path must name a file, got '" + path_utf8(target) + "'");
  }
  const std::filesystem::path parent = target.parent_path();
  if (!parent.empty()) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(parent, error);
    if (error) {
      return Status::error(StatusCode::NotFound,
                           "store directory '" + path_utf8(parent) + "' is not accessible: " +
                               error.message());
    }
    if (!std::filesystem::is_directory(status)) {
      return Status::error(StatusCode::InvalidArgument,
                           "store directory '" + path_utf8(parent) + "' is not a directory");
    }
  }

  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::symlink_status(target, error);
  if (error) {
    if (error.value() == static_cast<int>(std::errc::no_such_file_or_directory)) {
      return Status::success();
    }
    return Status::error(StatusCode::IoFailure,
                         "could not inspect '" + path_utf8(target) + "': " + error.message());
  }
  if (status.type() == std::filesystem::file_type::not_found) {
    return Status::success();
  }
  if (std::filesystem::is_symlink(status)) {
    return Status::error(StatusCode::InvalidArgument,
                         "store path '" + path_utf8(target) +
                             "' is a symbolic link; refusing to publish through it");
  }
  if (std::filesystem::is_directory(status)) {
    return Status::error(StatusCode::InvalidArgument,
                         "store path '" + path_utf8(target) + "' is a directory");
  }
  if (!std::filesystem::is_regular_file(status)) {
    return Status::error(StatusCode::InvalidArgument,
                         "store path '" + path_utf8(target) + "' is not a regular file");
  }
  return Status::success();
}

}  // namespace power_capacity::detail
