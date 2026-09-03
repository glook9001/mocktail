#include "libc_shim/libc_shim.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <limits.h>
#include <string_view>
#include <sys/syscall.h>
#if defined(__linux__)
#include <linux/openat2.h>
#endif
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace libc_shim {

namespace {

struct PathMappingEntry {
  std::string android_prefix;
  std::string host_prefix;
};

std::unordered_map<std::string, std::string> g_path_mappings;
std::vector<PathMappingEntry> g_sorted_mappings;

void RebuildSortedMappingsLocked() {
  g_sorted_mappings.clear();
  g_sorted_mappings.reserve(g_path_mappings.size());
  for (const auto& [android_prefix, host_prefix] : g_path_mappings) {
    g_sorted_mappings.push_back({android_prefix, host_prefix});
  }
  // Sort longest prefix first so the first match is always the longest match.
  std::sort(g_sorted_mappings.begin(), g_sorted_mappings.end(),
            [](const PathMappingEntry& a, const PathMappingEntry& b) {
              return a.android_prefix.size() > b.android_prefix.size();
            });
}

bool g_installed = false;
std::atomic<GuestAllocator> g_guest_allocator{nullptr};

constexpr char kCaBundleOverride[] = "MOCKTAIL_CA_BUNDLE";
constexpr std::array<const char*, 2> kDefaultCaBundles = {
    "/etc/ssl/cert.pem",
    "/etc/ssl/certs/ca-certificates.crt",
};
constexpr std::array<const char*, 2> kAndroidCaBundlePaths = {
    "/data/user/0/com.roblox.client/files/exe/cacert.pem",
    "/data/data/com.roblox.client/files/exe/cacert.pem",
};

bool IsEnabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

const char* GetEnvNonEmpty(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? value : nullptr;
}

std::string HomePath(const char* suffix) {
  const char* home = std::getenv("HOME");
  return std::string(home && home[0] != '\0' ? home : "/root") +
         (suffix ? suffix : "");
}

std::string RuntimeRoot() {
  const char* runtime_root = std::getenv("MOCKTAIL_RUNTIME_ROOT");
  if (runtime_root != nullptr && runtime_root[0] != '\0') {
    return runtime_root;
  }
  return "runtime/sober";
}

std::string DefaultDataRoot() {
  if (IsEnabled("MOCKTAIL_USE_REAL_SOBER_PATHS")) {
    return HomePath("/.var/app/org.vinegarhq.Sober/data/sober");
  }
  return RuntimeRoot() + "/data";
}

bool PrefixMatchesPath(std::string_view path, std::string_view prefix) {
  if (path.size() < prefix.size()) {
    return false;
  }
  if (path.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  return path.size() == prefix.size() || path[prefix.size()] == '/';
}

bool ValidateHostCaBundle(const char* path, std::string* canonical_path) {
  if (path == nullptr || path[0] != '/' || canonical_path == nullptr) {
    return false;
  }

  char resolved_path[PATH_MAX];
  if (::realpath(path, resolved_path) == nullptr || resolved_path[0] != '/') {
    return false;
  }

  const int fd = ::open(resolved_path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  struct stat file_status {};
  const bool status_available = ::fstat(fd, &file_status) == 0;
  const int close_result = ::close(fd);
  const bool has_read_permission =
      status_available &&
      (file_status.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) != 0;
  if (!status_available || close_result != 0 || !S_ISREG(file_status.st_mode) ||
      file_status.st_size <= 0 || !has_read_permission) {
    return false;
  }

  *canonical_path = resolved_path;
  return true;
}

bool EnsureDirectory(const std::string& path) {
  if (path.empty()) {
    return false;
  }

  std::string partial;
  size_t index = 0;
  if (path[0] == '/') {
    partial = "/";
    index = 1;
  }

  while (index <= path.size()) {
    const size_t next = path.find('/', index);
    std::string component =
        path.substr(index, next == std::string::npos ? next : next - index);
    if (!component.empty()) {
      if (partial.size() > 1 && partial.back() != '/') {
        partial.push_back('/');
      }
      partial += component;
      if (::mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
      }
    }
    if (next == std::string::npos) {
      break;
    }
    index = next + 1;
  }

  return true;
}

void PruneStaleStorageAndOrphanedFolders(const std::string& data_root) {
  std::error_code ec;
  if (!std::filesystem::exists(data_root, ec)) {
    return;
  }
  // Remove orphaned ContentProvider_* session directories left behind from previous runs.
  for (const auto& entry : std::filesystem::directory_iterator(data_root, ec)) {
    if (ec) break;
    if (entry.is_directory(ec)) {
      const std::string filename = entry.path().filename().string();
      if (filename.rfind("ContentProvider_", 0) == 0 &&
          filename != "ContentProvider_2") {
        std::filesystem::remove_all(entry.path(), ec);
      }
    }
  }
}

void EnsureDefaultDataLayout(const std::string& data_root) {
  PruneStaleStorageAndOrphanedFolders(data_root);
  EnsureDirectory(data_root);
  EnsureDirectory(data_root + "/files");
  EnsureDirectory(data_root + "/cache");
  EnsureDirectory(data_root + "/shared_prefs");
  EnsureDirectory(data_root + "/rbx-storage");
  EnsureDirectory(data_root + "/appData");
  EnsureDirectory(data_root + "/appData/LocalStorage");
  EnsureDirectory(data_root + "/appData/rbx-storage");
  EnsureDirectory(data_root + "/files/appData");
  EnsureDirectory(data_root + "/files/appData/LocalStorage");
  EnsureDirectory(data_root + "/files/appData/OTAPatchBackups");
  EnsureDirectory(data_root + "/files/appData/rbx-storage");
  EnsureDirectory(data_root + "/cache/ContentProvider_2");
  EnsureDirectory(data_root + "/cache/rbx-storage");
  EnsureDirectory(data_root + "/cache/sounds");
  EnsureDirectory(data_root + "/sdcard/Android/data/com.roblox.client");
  EnsureDirectory(data_root + "/sdcard/Android/data/com.roblox.client/files");
  EnsureDirectory(data_root + "/sdcard/Android/data/com.roblox.client/cache");
}

}  // namespace

void ConfigureGuestAllocator(GuestAllocator allocator) noexcept {
  g_guest_allocator.store(allocator, std::memory_order_release);
}

GuestAllocator GuestAllocatorForCalls() noexcept {
  return g_guest_allocator.load(std::memory_order_acquire);
}

HostCaBundleResolution ResolveHostCaBundle() {
  const char* override_path = std::getenv(kCaBundleOverride);
  if (override_path != nullptr) {
    HostCaBundleResolution resolution;
    resolution.from_override = true;
    if (!ValidateHostCaBundle(override_path, &resolution.host_path)) {
      resolution.status = HostCaBundleStatus::kInvalidOverride;
      resolution.host_path.clear();
      return resolution;
    }
    resolution.status = HostCaBundleStatus::kReady;
    return resolution;
  }

  for (const char* candidate : kDefaultCaBundles) {
    HostCaBundleResolution resolution;
    if (ValidateHostCaBundle(candidate, &resolution.host_path)) {
      resolution.status = HostCaBundleStatus::kReady;
      return resolution;
    }
  }
  return {};
}

HostCaBundleResolution ConfigureHostCaBundlePathMappings() {
  for (const char* android_path : kAndroidCaBundlePaths) {
    g_path_mappings.erase(android_path);
  }

  HostCaBundleResolution resolution = ResolveHostCaBundle();
  if (!resolution.ok()) {
    return resolution;
  }
  for (const char* android_path : kAndroidCaBundlePaths) {
    RegisterPathMapping(android_path, resolution.host_path);
  }
  return resolution;
}

const char* HostCaBundleStatusName(HostCaBundleStatus status) {
  switch (status) {
    case HostCaBundleStatus::kReady:
      return "Ready";
    case HostCaBundleStatus::kInvalidOverride:
      return "InvalidOverride";
    case HostCaBundleStatus::kUnavailable:
      return "Unavailable";
  }
  return "Unknown";
}

void Install() {
  if (g_installed) {
    return;
  }
  g_installed = true;

  const std::string data_root = DefaultDataRoot();

  RegisterPathMapping("/data/user/0/com.roblox.client", data_root);
  RegisterPathMapping("/data/data/com.roblox.client", data_root);
  const HostCaBundleResolution ca_bundle =
      ConfigureHostCaBundlePathMappings();
  if (ca_bundle.ok()) {
    std::cout << "  [libc_shim] Host CA bundle mapped from "
              << (ca_bundle.from_override ? "validated override"
                                          : "system trust store")
              << '\n';
  } else {
    std::cerr << "  [libc_shim] Host CA bundle unavailable: "
              << HostCaBundleStatusName(ca_bundle.status)
              << "; TLS verification remains fail-closed\n";
  }
  const char* cache_root = GetEnvNonEmpty("MOCKTAIL_ANDROID_CACHE_HOST_ROOT");
  if (cache_root != nullptr) {
    RegisterPathMapping("/data/user/0/com.roblox.client/cache", cache_root);
    RegisterPathMapping("/data/data/com.roblox.client/cache", cache_root);
    EnsureDirectory(cache_root);
  }
  const char* shader_cache_path =
      GetEnvNonEmpty("MOCKTAIL_VULKAN_SHADER_CACHE_HOST_PATH");
  if (shader_cache_path != nullptr) {
    RegisterPathMapping(
        "/data/user/0/com.roblox.client/shadercachevk.bin",
        shader_cache_path);
    RegisterPathMapping(
        "/data/data/com.roblox.client/shadercachevk.bin",
        shader_cache_path);
  }
  const char* asset_content_root = GetEnvNonEmpty("MOCKTAIL_ASSET_PATH");
  if (asset_content_root != nullptr) {
    std::string content_root(asset_content_root);
    while (content_root.size() > 1 && content_root.back() == '/') {
      content_root.pop_back();
    }
    const std::string content_suffix = "/content";
    if (content_root.size() > content_suffix.size() &&
        content_root.compare(content_root.size() - content_suffix.size(),
                             content_suffix.size(), content_suffix) == 0) {
      const std::string assets_root = content_root.substr(
          0, content_root.size() - content_suffix.size());
      RegisterPathMapping("content", content_root);
      RegisterPathMapping("rbx_bin/assets/content", content_root);
      RegisterPathMapping("rbx_bin/assets", assets_root);
    }
  }
  RegisterPathMapping("/sdcard/Android/data/com.roblox.client",
                      data_root + "/sdcard/Android/data/com.roblox.client");
  RegisterPathMapping("/storage/emulated/0/Android/data/com.roblox.client",
                      data_root + "/sdcard/Android/data/com.roblox.client");
  EnsureDefaultDataLayout(data_root);

  std::cout << "  [libc_shim] Path shims installed ("
            << g_path_mappings.size() << " prefix mappings)\n";
}

std::string TranslatePath(std::string_view android_path) {
  if (android_path.empty()) {
    return std::string();
  }

  // Linear scan through sorted list (longest prefixes first).
  for (const auto& entry : g_sorted_mappings) {
    if (PrefixMatchesPath(android_path, entry.android_prefix)) {
      std::string result;
      result.reserve(entry.host_prefix.size() + (android_path.size() - entry.android_prefix.size()));
      result.append(entry.host_prefix);
      result.append(android_path.data() + entry.android_prefix.size(),
                    android_path.size() - entry.android_prefix.size());
      return result;
    }
  }

  return std::string(android_path);
}

std::string TranslatePath(const std::string& android_path) {
  return TranslatePath(std::string_view(android_path));
}

void RegisterPathMapping(const std::string& android_prefix,
                         const std::string& host_prefix) {
  g_path_mappings[android_prefix] = host_prefix;
  RebuildSortedMappingsLocked();
}

void ClearPathMappings() {
  g_path_mappings.clear();
  g_sorted_mappings.clear();
}

}  // namespace libc_shim

namespace {

// Fast-path check: returns true if path definitely refers to an unmapped host system path
// that cannot match any registered Android prefix or relative asset directory.
inline bool StartsWith(std::string_view str, std::string_view prefix) noexcept {
  return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

inline bool IsHostSystemPath(std::string_view path) noexcept {
  if (path.empty() || path[0] != '/') {
    return false;
  }
  // Android guest paths typically start with /data/, /sdcard/, /storage/, /system/, etc.
  // Native Linux host subsystems never contain mocktail guest mappings.
  return StartsWith(path, "/proc/") ||
         StartsWith(path, "/sys/") ||
         StartsWith(path, "/dev/") ||
         StartsWith(path, "/etc/") ||
         StartsWith(path, "/usr/") ||
         StartsWith(path, "/lib") ||
         StartsWith(path, "/var/") ||
         StartsWith(path, "/home/") ||
         StartsWith(path, "/tmp/");
}

const char* HostPath(const char* path, std::string* storage) {
  if (path == nullptr) {
    return nullptr;
  }
  if (path[0] == '\0') {
    return path;
  }

  const std::string_view view(path);

  // Fast path 1: Standard host system paths (/proc, /sys, /dev, etc.)
  if (IsHostSystemPath(view)) {
    return path;
  }

  // Fast path 2: Scan sorted mappings. If no prefix matches, zero allocation return.
  for (const auto& entry : libc_shim::g_sorted_mappings) {
    if (libc_shim::PrefixMatchesPath(view, entry.android_prefix)) {
      storage->clear();
      storage->reserve(entry.host_prefix.size() + (view.size() - entry.android_prefix.size()));
      storage->append(entry.host_prefix);
      storage->append(path + entry.android_prefix.size(),
                      view.size() - entry.android_prefix.size());
      return storage->c_str();
    }
  }

  // No mapping matched; return original pointer without heap allocation.
  return path;
}

bool PathTraceEnabled() {
  const char* value = std::getenv("MOCKTAIL_PATH_TRACE");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void TracePathCall(const char* name, const char* path, const char* host_path,
                   int result) {
  if (!PathTraceEnabled()) {
    return;
  }
  std::fprintf(stderr, "  [path] %s %s", name, path ? path : "(null)");
  if (path != nullptr && host_path != nullptr &&
      std::strcmp(path, host_path) != 0) {
    std::fprintf(stderr, " -> %s", host_path);
  }
  std::fprintf(stderr, " result=%d errno=%d\n", result, result < 0 ? errno : 0);
}

void TracePathCallPtr(const char* name, const char* path,
                      const char* host_path, const void* result) {
  if (!PathTraceEnabled()) {
    return;
  }
  std::fprintf(stderr, "  [path] %s %s", name, path ? path : "(null)");
  if (path != nullptr && host_path != nullptr &&
      std::strcmp(path, host_path) != 0) {
    std::fprintf(stderr, " -> %s", host_path);
  }
  std::fprintf(stderr, " result=%p errno=%d\n", result, result ? 0 : errno);
}

}  // namespace

// Modern Linux open helper: attempts openat2 with RESOLVE_NO_MAGICLINKS
// to prevent symlink traversal trickery in unprivileged user namespaces.
inline int ModernLinuxOpen(const char* host_path, int flags, mode_t mode) {
  if (host_path == nullptr) {
    errno = EFAULT;
    return -1;
  }

#if defined(__linux__) && defined(SYS_openat2) && defined(RESOLVE_NO_MAGICLINKS)
  struct open_how how;
  std::memset(&how, 0, sizeof(how));
  how.flags = static_cast<__u64>(flags);
  how.mode = static_cast<__u64>((flags & (O_CREAT | O_TMPFILE)) != 0 ? mode : 0);
  how.resolve = RESOLVE_NO_MAGICLINKS;

  const long res = ::syscall(SYS_openat2, AT_FDCWD, host_path, &how, sizeof(how));
  if (res >= 0) {
    return static_cast<int>(res);
  }
  if (errno != ENOSYS && errno != EPERM && errno != EINVAL) {
    return -1;
  }
#endif

  if ((flags & O_CREAT) != 0) {
    return ::open(host_path, flags, mode);
  }
  return ::open(host_path, flags);
}

extern "C" int mocktail_open(const char* path, int flags, ...) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  mode_t mode = 0;
  if ((flags & O_CREAT) != 0) {
    va_list args;
    va_start(args, flags);
    mode = static_cast<mode_t>(va_arg(args, int));
    va_end(args);
  }
  const int result = ModernLinuxOpen(host_path, flags, mode);
  TracePathCall("open", path, host_path, result);
  return result;
}

extern "C" int mocktail___open_2(const char* path, int flags) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  const mode_t mode = (flags & O_CREAT) != 0 ? 0600 : 0;
  const int result = ModernLinuxOpen(host_path, flags, mode);
  TracePathCall("__open_2", path, host_path, result);
  return result;
}

extern "C" FILE* mocktail_fopen(const char* path, const char* mode) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  FILE* result = ::fopen(host_path, mode);
  TracePathCallPtr("fopen", path, host_path, result);
  return result;
}

extern "C" int mocktail_access(const char* path, int mode) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  int result = ::access(host_path, mode);
  TracePathCall("access", path, host_path, result);
  return result;
}

// Modern Linux stat helper: queries kernel VFS with statx using AT_STATX_DONT_SYNC
// to avoid forced filesystem/disk barrier flushes, reading directly from the page cache.
inline int ModernLinuxStat(const char* host_path, struct stat* statbuf, int flags) {
  if (host_path == nullptr || statbuf == nullptr) {
    errno = EFAULT;
    return -1;
  }

#if defined(__linux__) && defined(STATX_BASIC_STATS)
  struct statx stx;
  // Use AT_STATX_DONT_SYNC to read directly from kernel dcache/page cache
  const int res = ::statx(AT_FDCWD, host_path, flags | AT_STATX_DONT_SYNC,
                          STATX_BASIC_STATS, &stx);
  if (res == 0) {
    std::memset(statbuf, 0, sizeof(*statbuf));
    statbuf->st_dev = ((static_cast<dev_t>(stx.stx_dev_major) << 8) |
                       (stx.stx_dev_minor & 0xff));
    statbuf->st_ino = static_cast<ino_t>(stx.stx_ino);
    statbuf->st_mode = static_cast<mode_t>(stx.stx_mode);
    statbuf->st_nlink = static_cast<nlink_t>(stx.stx_nlink);
    statbuf->st_uid = static_cast<uid_t>(stx.stx_uid);
    statbuf->st_gid = static_cast<gid_t>(stx.stx_gid);
    statbuf->st_rdev = ((static_cast<dev_t>(stx.stx_rdev_major) << 8) |
                        (stx.stx_rdev_minor & 0xff));
    statbuf->st_size = static_cast<off_t>(stx.stx_size);
    statbuf->st_blksize = static_cast<blksize_t>(stx.stx_blksize);
    statbuf->st_blocks = static_cast<blkcnt_t>(stx.stx_blocks);
    statbuf->st_atim.tv_sec = stx.stx_atime.tv_sec;
    statbuf->st_atim.tv_nsec = stx.stx_atime.tv_nsec;
    statbuf->st_mtim.tv_sec = stx.stx_mtime.tv_sec;
    statbuf->st_mtim.tv_nsec = stx.stx_mtime.tv_nsec;
    statbuf->st_ctim.tv_sec = stx.stx_ctime.tv_sec;
    statbuf->st_ctim.tv_nsec = stx.stx_ctime.tv_nsec;
    return 0;
  }
  // If statx is unsupported or rejected by older kernel/seccomp filter, fallback to legacy
  if (errno != ENOSYS && errno != EPERM) {
    return -1;
  }
#endif

  if ((flags & AT_SYMLINK_NOFOLLOW) != 0) {
    return ::lstat(host_path, statbuf);
  }
  return ::stat(host_path, statbuf);
}

extern "C" int mocktail_stat(const char* path, struct stat* statbuf) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  int result = ModernLinuxStat(host_path, statbuf, 0);
  TracePathCall("stat", path, host_path, result);
  return result;
}

extern "C" int mocktail_lstat(const char* path, struct stat* statbuf) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  int result = ModernLinuxStat(host_path, statbuf, AT_SYMLINK_NOFOLLOW);
  TracePathCall("lstat", path, host_path, result);
  return result;
}

extern "C" int mocktail_statvfs(const char* path, struct statvfs* statbuf) {
  std::string translated;
  return ::statvfs(HostPath(path, &translated), statbuf);
}

extern "C" int mocktail_statfs(const char* path, struct statfs* statbuf) {
  std::string translated;
  return ::statfs(HostPath(path, &translated), statbuf);
}

extern "C" int mocktail_mkdir(const char* path, mode_t mode) {
  std::string translated;
  return ::mkdir(HostPath(path, &translated), mode);
}

extern "C" DIR* mocktail_opendir(const char* path) {
  std::string translated;
  return ::opendir(HostPath(path, &translated));
}

extern "C" int mocktail_rename(const char* old_path, const char* new_path) {
  std::string translated_old;
  std::string translated_new;
  return ::rename(HostPath(old_path, &translated_old),
                  HostPath(new_path, &translated_new));
}

extern "C" int mocktail_unlink(const char* path) {
  std::string translated;
  return ::unlink(HostPath(path, &translated));
}

extern "C" int mocktail_rmdir(const char* path) {
  std::string translated;
  return ::rmdir(HostPath(path, &translated));
}

extern "C" char* mocktail_realpath(const char* path, char* resolved_path) {
  std::string translated;
  const char* host_path = HostPath(path, &translated);
  if (resolved_path != nullptr) {
    return ::realpath(host_path, resolved_path);
  }

  char canonical_path[PATH_MAX];
  if (::realpath(host_path, canonical_path) == nullptr) {
    return nullptr;
  }
  const size_t bytes = std::strlen(canonical_path) + 1;
  libc_shim::GuestAllocator allocator = libc_shim::GuestAllocatorForCalls();
  if (allocator == nullptr) {
    return ::strdup(canonical_path);
  }
  auto* result = static_cast<char*>(allocator(bytes));
  if (result == nullptr) {
    errno = ENOMEM;
    return nullptr;
  }
  std::memcpy(result, canonical_path, bytes);
  return result;
}

extern "C" ssize_t mocktail_readlink(const char* path, char* buf,
                                      size_t bufsiz) {
  std::string translated;
  return ::readlink(HostPath(path, &translated), buf, bufsiz);
}

extern "C" ssize_t mocktail___readlink_chk(const char* path, char* buf,
                                            size_t len, size_t buf_len) {
  // Bionic treats impossible ssize_t results and proven overruns as fatal;
  // returning EINVAL would continue after a detected buffer overflow.
  if (len > static_cast<size_t>(SSIZE_MAX)) {
    std::fprintf(stderr,
                 "FORTIFY: readlink: size %zu > SSIZE_MAX\n", len);
    std::abort();
  }
  if (len > buf_len) {
    std::fprintf(stderr,
                 "FORTIFY: readlink: prevented %zu-byte write into "
                 "%zu-byte buffer\n",
                 len, buf_len);
    std::abort();
  }
  return mocktail_readlink(path, buf, len);
}
