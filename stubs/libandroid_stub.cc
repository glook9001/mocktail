// Android NDK compatibility symbols used by libroblox.so on Linux. Asset calls
// can read from an extracted directory or directly from an APK.
//
// Symbols sourced from Android NDK r28 android/native_activity.h,
// android/asset_manager.h, android/looper.h, android/configuration.h.

#include "libc_shim/vulkan_etc1_sky_transcoder.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <minizip/unzip.h>
#if defined(MOCKTAIL_MINIZIP_HAS_STREAM_TELL)
#include <minizip/mz_strm.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <vector>

// Read-only asset access

struct AAssetManager {};
struct AAsset {
  std::string path;
  std::string apk_entry;
  std::vector<unsigned char> data;
  size_t offset = 0;
};
struct AAssetDir {};

namespace {

AAssetManager g_asset_manager;

const char* GetEnvNonEmpty(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? value : nullptr;
}

bool TraceEnabled() {
  return GetEnvNonEmpty("MOCKTAIL_ANDROID_STUB_TRACE") != nullptr ||
         GetEnvNonEmpty("MOCKTAIL_WINDOW_TRACE") != nullptr ||
         GetEnvNonEmpty("MOCKTAIL_FULL_TRACE") != nullptr;
}

bool AssetTraceEnabled() {
  return GetEnvNonEmpty("MOCKTAIL_ASSET_TRACE") != nullptr ||
         GetEnvNonEmpty("MOCKTAIL_TRACE_ALL") != nullptr ||
         GetEnvNonEmpty("MOCKTAIL_FULL_TRACE") != nullptr;
}

std::string StripAssetUriPrefix(const char* filename) {
  std::string path(filename ? filename : "");
  static const char* kPrefixes[] = {
      "file:///android_asset/",
      "rbxasset://",
  };
  for (const char* prefix : kPrefixes) {
    const size_t prefix_len = std::strlen(prefix);
    if (path.compare(0, prefix_len, prefix) == 0) {
      return path.substr(prefix_len);
    }
  }
  return path;
}

bool HasUnsafePathSegment(const std::string& path) {
  if (path.empty() || path[0] == '/') {
    return true;
  }
  return path == "." || path == ".." ||
         path.find("/../") != std::string::npos || path.find("../") == 0 ||
         path.rfind("/..") == path.size() - 3 ||
         path.find("//") != std::string::npos;
}

std::string StripAndroidAssetPrefix(const std::string& path) {
  static const char* kPrefixes[] = {
      "assets/",
      "content/",
  };
  for (const char* prefix : kPrefixes) {
    const size_t prefix_len = std::strlen(prefix);
    if (path.compare(0, prefix_len, prefix) == 0) {
      return path.substr(prefix_len);
    }
  }
  return path;
}

bool FileExists(const std::string& path) {
  struct stat st;
  return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string ResolveAssetPath(const char* filename) {
  std::string requested = StripAssetUriPrefix(filename);
  if (HasUnsafePathSegment(requested)) {
    return {};
  }
  const char* root_env = GetEnvNonEmpty("MOCKTAIL_ASSET_ROOT");
  std::string root = root_env != nullptr ? root_env : "rbx_bin/assets";
  std::string stripped = StripAndroidAssetPrefix(requested);

  const std::vector<std::string> candidates = {
      root + "/" + requested,
      root + "/" + stripped,
      root + "/ExtraContent/" + requested,
      root + "/ExtraContent/" + stripped,
  };
  for (const std::string& candidate : candidates) {
    if (FileExists(candidate)) {
      return candidate;
    }
  }

  return {};
}

bool LoadFile(const std::string& path, std::vector<unsigned char>* data) {
  if (data == nullptr) {
    return false;
  }
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return false;
  }
  long size = std::ftell(file);
  if (size < 0) {
    std::fclose(file);
    return false;
  }
  if (std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return false;
  }
  data->resize(static_cast<size_t>(size));
  if (size > 0) {
    size_t read = std::fread(data->data(), 1, static_cast<size_t>(size), file);
    if (read != static_cast<size_t>(size)) {
      std::fclose(file);
      return false;
    }
  }
  std::fclose(file);
  return true;
}

std::string ApkEntryName(const char* filename) {
  std::string requested = StripAssetUriPrefix(filename);
  constexpr const char* kAssetsPrefix = "assets/";
  if (requested.compare(0, std::strlen(kAssetsPrefix), kAssetsPrefix) == 0) {
    requested.erase(0, std::strlen(kAssetsPrefix));
  }
  return "assets/" + requested;
}

bool OpenStoredApkEntryDescriptor(const std::string& entry_name, int* fd,
                                  off_t* start, off_t* length) {
  const char* apk_path = GetEnvNonEmpty("MOCKTAIL_ASSET_APK_PATH");
  if (apk_path == nullptr || fd == nullptr || start == nullptr ||
      length == nullptr) {
    return false;
  }
  unzFile archive = unzOpen64(apk_path);
  if (archive == nullptr ||
      unzLocateFile(archive, entry_name.c_str(), 1) != UNZ_OK) {
    if (archive != nullptr) {
      unzClose(archive);
    }
    return false;
  }
  unz_file_info64 info{};
  if (unzGetCurrentFileInfo64(archive, &info, nullptr, 0, nullptr, 0, nullptr,
                              0) != UNZ_OK ||
      info.compression_method != 0 || unzOpenCurrentFile(archive) != UNZ_OK) {
    unzClose(archive);
    return false;
  }
  ZPOS64_T data_offset = 0;
#if defined(MOCKTAIL_MINIZIP_HAS_ZSTREAM_POS)
  data_offset = unzGetCurrentFileZStreamPos64(archive);
#elif defined(MOCKTAIL_MINIZIP_HAS_STREAM_TELL)
  const int64_t stream_offset = mz_stream_tell(unzGetStream(archive));
  if (stream_offset < 0) {
    unzCloseCurrentFile(archive);
    unzClose(archive);
    return false;
  }
  data_offset = static_cast<ZPOS64_T>(stream_offset);
#endif
  unzCloseCurrentFile(archive);
  unzClose(archive);
  if (data_offset > static_cast<ZPOS64_T>(INT64_MAX) ||
      info.uncompressed_size > static_cast<ZPOS64_T>(INT64_MAX)) {
    return false;
  }
  const int apk_fd = ::open(apk_path, O_RDONLY | O_CLOEXEC);
  if (apk_fd < 0) {
    return false;
  }
  *fd = apk_fd;
  *start = static_cast<off_t>(data_offset);
  *length = static_cast<off_t>(info.uncompressed_size);
  return true;
}

template <typename Fn>
Fn ResolveCached(Fn* slot, const char* name) {
  Fn fn = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (__builtin_expect(fn == nullptr, 0)) {
    fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, name));
    if (fn != nullptr) {
      __atomic_store_n(slot, fn, __ATOMIC_RELEASE);
    }
  }
  return fn;
}

void* MocktailNativeWindow() {
  using Fn = void* (*)();
  static Fn cached_fn = nullptr;
  Fn fn = ResolveCached(&cached_fn, "mocktail_native_window");
  return fn != nullptr ? fn() : nullptr;
}

int MocktailWindowWidth() {
  using Fn = int (*)();
  static Fn cached_fn = nullptr;
  Fn fn = ResolveCached(&cached_fn, "mocktail_window_width");
  return fn != nullptr ? fn() : 1280;
}

int MocktailWindowHeight() {
  using Fn = int (*)();
  static Fn cached_fn = nullptr;
  Fn fn = ResolveCached(&cached_fn, "mocktail_window_height");
  return fn != nullptr ? fn() : 720;
}

bool MocktailUsesDirectVulkan() {
  using Fn = bool (*)();
  static Fn cached_fn = nullptr;
  Fn fn = ResolveCached(&cached_fn, "mocktail_window_uses_direct_vulkan");
  if (fn != nullptr) {
    return fn();
  }
  const char* backend = GetEnvNonEmpty("MOCKTAIL_GRAPHICS_BACKEND");
  return backend != nullptr && std::strcmp(backend, "direct-vulkan") == 0;
}

}  // namespace

extern "C" {

AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename,
                           int) {
  if (mgr == nullptr) {
    return nullptr;
  }
  std::string path = ResolveAssetPath(filename);
  if (path.empty()) {
    return nullptr;
  }
  auto* asset = new AAsset;
  asset->path = path;
  asset->apk_entry = ApkEntryName(filename);
  if (!LoadFile(path, &asset->data)) {
    delete asset;
    return nullptr;
  }
  if (MocktailUsesDirectVulkan()) {
    libc_shim::TranscodeEtc1SkyTextureForVulkan(path.c_str(), &asset->data);
  }
  if (AssetTraceEnabled()) {
    std::fprintf(stderr, "[asset] open %s -> %s (%zu bytes)\n", filename,
                 path.c_str(), asset->data.size());
  }
  return asset;
}

void AAsset_close(AAsset* asset) {
  if (asset != nullptr && AssetTraceEnabled() &&
      asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr, "[asset] close %s buffer=%p size=%zu\n",
                 asset->path.c_str(), asset->data.data(), asset->data.size());
  }
  delete asset;
}

int AAsset_read(AAsset* asset, void* buf, size_t count) {
  if (asset == nullptr || buf == nullptr) {
    return -1;
  }
  const size_t remaining = asset->offset <= asset->data.size()
                               ? asset->data.size() - asset->offset
                               : 0;
  const size_t to_read = std::min(count, remaining);
  if (to_read > 0) {
    std::memcpy(buf, asset->data.data() + asset->offset, to_read);
    asset->offset += to_read;
  }
  if (AssetTraceEnabled() && asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr, "[asset] read %s count=%zu -> %zu offset=%zu\n",
                 asset->path.c_str(), count, to_read, asset->offset);
  }
  return static_cast<int>(to_read);
}

const void* AAsset_getBuffer(AAsset* asset) {
  if (asset == nullptr) {
    return nullptr;
  }
  if (AssetTraceEnabled() && asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr, "[asset] getBuffer %s -> %zu bytes\n",
                 asset->path.c_str(), asset->data.size());
  }
  return asset->data.empty() ? static_cast<const void*>("")
                             : static_cast<const void*>(asset->data.data());
}

off_t AAsset_getLength(AAsset* asset) {
  if (asset != nullptr && AssetTraceEnabled() &&
      asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr, "[asset] getLength %s -> %zu bytes\n",
                 asset->path.c_str(), asset->data.size());
  }
  return asset == nullptr ? 0 : static_cast<off_t>(asset->data.size());
}

off_t AAsset_getLength64(AAsset* asset) { return AAsset_getLength(asset); }

off_t AAsset_getRemainingLength(AAsset* asset) {
  if (asset == nullptr || asset->offset > asset->data.size()) {
    return 0;
  }
  if (AssetTraceEnabled() && asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr, "[asset] getRemainingLength %s -> %zu bytes\n",
                 asset->path.c_str(), asset->data.size() - asset->offset);
  }
  return static_cast<off_t>(asset->data.size() - asset->offset);
}

off_t AAsset_getRemainingLength64(AAsset* asset) {
  return AAsset_getRemainingLength(asset);
}

off_t AAsset_seek(AAsset* asset, off_t offset, int whence) {
  if (asset == nullptr) {
    return -1;
  }
  off_t base = 0;
  if (whence == SEEK_SET) {
    base = 0;
  } else if (whence == SEEK_CUR) {
    base = static_cast<off_t>(asset->offset);
  } else if (whence == SEEK_END) {
    base = static_cast<off_t>(asset->data.size());
  } else {
    errno = EINVAL;
    return -1;
  }
  off_t next = base + offset;
  if (next < 0) {
    errno = EINVAL;
    return -1;
  }
  const auto clamped = std::min(static_cast<size_t>(next), asset->data.size());
  asset->offset = clamped;
  if (AssetTraceEnabled() && asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr, "[asset] seek %s offset=%lld whence=%d -> %zu\n",
                 asset->path.c_str(), static_cast<long long>(offset), whence,
                 asset->offset);
  }
  return static_cast<off_t>(asset->offset);
}

off_t AAsset_seek64(AAsset* asset, off_t offset, int whence) {
  return AAsset_seek(asset, offset, whence);
}

int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart,
                              off_t* outLength) {
  if (asset == nullptr || asset->path.empty()) {
    return -1;
  }
  int fd = -1;
  off_t start = 0;
  off_t descriptor_length = static_cast<off_t>(asset->data.size());
  if (!OpenStoredApkEntryDescriptor(asset->apk_entry, &fd, &start,
                                    &descriptor_length)) {
    fd = ::open(asset->path.c_str(), O_RDONLY | O_CLOEXEC);
  }
  if (fd < 0) {
    return -1;
  }
  if (outStart != nullptr) {
    *outStart = start;
  }
  if (outLength != nullptr) {
    *outLength = descriptor_length;
  }
  if (AssetTraceEnabled() && asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr,
                 "[asset] openFileDescriptor %s -> fd=%d start=%lld "
                 "length=%zu\n",
                 asset->path.c_str(), fd, static_cast<long long>(start),
                 static_cast<size_t>(descriptor_length));
  }
  return fd;
}

int AAsset_openFileDescriptor64(AAsset* asset, off_t* outStart,
                                off_t* outLength) {
  return AAsset_openFileDescriptor(asset, outStart, outLength);
}

AAssetManager* AAssetManager_fromJava(void*, void*) {
  return &g_asset_manager;
}

// AConfiguration — device configuration query

struct AConfiguration {};

AConfiguration* AConfiguration_new() {
  return static_cast<AConfiguration*>(calloc(1, sizeof(AConfiguration)));
}

void AConfiguration_delete(AConfiguration* config) { free(config); }

void AConfiguration_fromAssetManager(AConfiguration*,
                                     AAssetManager*) {}

void AConfiguration_getLanguage(AConfiguration*, char* out) {
  if (out) {
    out[0] = 'e';
    out[1] = 'n';
  }
}

void AConfiguration_getCountry(AConfiguration*, char* out) {
  if (out) {
    out[0] = 'U';
    out[1] = 'S';
  }
}

int32_t AConfiguration_getScreenSize(AConfiguration*) {
  return 2;  // ACONFIGURATION_SCREENSIZE_NORMAL
}

int32_t AConfiguration_getScreenWidthDp(AConfiguration*) {
  return 360;
}

int32_t AConfiguration_getScreenHeightDp(AConfiguration*) {
  return 640;
}

int32_t AConfiguration_getNavHidden(AConfiguration*) {
  return 1;  // ACONFIGURATION_NAVHIDDEN_YES
}

}  // extern "C"

// ALooper — high-performance Linux epoll + eventfd event loop

namespace {

constexpr int kPollWake = -1;
constexpr int kPollCallback = -2;
constexpr int kPollTimeout = -3;
constexpr int kPollError = -4;
constexpr std::size_t kMaxRegistrations = 32;

using LooperCallback = int (*)(int, int, void*);

struct LooperRegistration {
  int fd = -1;
  int ident = 0;
  int events = 0;
  LooperCallback callback = nullptr;
  void* data = nullptr;
};

struct ALooper {
  ALooper()
      : epoll_fd(::epoll_create1(EPOLL_CLOEXEC)),
        wake_fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (epoll_fd < 0 || wake_fd < 0) {
      if (epoll_fd >= 0) ::close(epoll_fd);
      if (wake_fd >= 0) ::close(wake_fd);
      epoll_fd = -1;
      wake_fd = -1;
      return;
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = wake_fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &event) != 0) {
      ::close(epoll_fd);
      ::close(wake_fd);
      epoll_fd = -1;
      wake_fd = -1;
    }
  }

  ~ALooper() {
    if (epoll_fd >= 0) ::close(epoll_fd);
    if (wake_fd >= 0) ::close(wake_fd);
  }

  ALooper(const ALooper&) = delete;
  ALooper& operator=(const ALooper&) = delete;

  int epoll_fd = -1;
  int wake_fd = -1;
  std::atomic<int> refs{1};
  std::atomic<bool> polling{false};
  std::mutex mutex;
  std::array<LooperRegistration, kMaxRegistrations> registrations{};
  std::size_t registration_count = 0;
};

thread_local ALooper g_thread_looper;

uint32_t EpollEventsFromAndroid(int events) {
  uint32_t result = 0;
  if ((events & (1 << 0)) != 0) result |= EPOLLIN;
  if ((events & (1 << 1)) != 0) result |= EPOLLOUT;
  return result;
}

int AndroidEventsFromEpoll(uint32_t events) {
  int result = 0;
  if ((events & EPOLLIN) != 0) result |= 1 << 0;
  if ((events & EPOLLOUT) != 0) result |= 1 << 1;
  if ((events & EPOLLERR) != 0) result |= 1 << 2;
  if ((events & EPOLLHUP) != 0) result |= 1 << 3;
  if ((events & EPOLLPRI) != 0) result |= 1 << 4;
  return result;
}

LooperRegistration FindLooperRegistration(ALooper* looper, int fd, bool* found) {
  std::scoped_lock lock(looper->mutex);
  for (std::size_t i = 0; i < looper->registration_count; ++i) {
    if (looper->registrations[i].fd == fd) {
      if (found) *found = true;
      return looper->registrations[i];
    }
  }
  if (found) *found = false;
  return {};
}

}  // namespace

extern "C" {

ALooper* ALooper_forThread() { return &g_thread_looper; }
ALooper* ALooper_prepare(int) { return ALooper_forThread(); }

void ALooper_acquire(ALooper* looper) {
  if (looper != nullptr) {
    looper->refs.fetch_add(1, std::memory_order_relaxed);
  }
}

void ALooper_release(ALooper* looper) {
  if (looper != nullptr) {
    looper->refs.fetch_sub(1, std::memory_order_relaxed);
  }
}

int ALooper_addFd(ALooper* looper, int fd, int ident, int events,
                  int (*callback)(int, int, void*), void* data) {
  if (looper == nullptr || looper->epoll_fd < 0 || fd < 0 ||
      (callback == nullptr && ident < 0)) {
    errno = EINVAL;
    return 0;
  }

  epoll_event event{};
  event.events = EpollEventsFromAndroid(events);
  event.data.fd = fd;
  std::scoped_lock lock(looper->mutex);
  std::size_t index = looper->registration_count;
  for (std::size_t i = 0; i < looper->registration_count; ++i) {
    if (looper->registrations[i].fd == fd) {
      index = i;
      break;
    }
  }
  const bool update = index < looper->registration_count;
  if (!update && looper->registration_count == kMaxRegistrations) {
    errno = ENOSPC;
    return 0;
  }
  const int operation = update ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  if (::epoll_ctl(looper->epoll_fd, operation, fd, &event) != 0) return 0;
  looper->registrations[index] = {fd, ident, events, callback, data};
  if (!update) ++looper->registration_count;
  return 1;
}

int ALooper_removeFd(ALooper* looper, int fd) {
  if (looper == nullptr || looper->epoll_fd < 0) return 0;
  std::scoped_lock lock(looper->mutex);
  for (std::size_t i = 0; i < looper->registration_count; ++i) {
    if (looper->registrations[i].fd != fd) continue;
    (void)::epoll_ctl(looper->epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    looper->registrations[i] =
        looper->registrations[--looper->registration_count];
    return 1;
  }
  return 0;
}

void ALooper_wake(ALooper* looper) {
  if (looper == nullptr || looper->wake_fd < 0) return;
  const uint64_t value = 1;
  (void)::write(looper->wake_fd, &value, sizeof(value));
}

bool ALooper_isPolling(ALooper* looper) {
  return looper != nullptr &&
         looper->polling.load(std::memory_order_relaxed);
}

int ALooper_pollOnce(int timeout_ms, int* out_fd, int* out_events,
                     void** out_data) {
  auto* looper = static_cast<ALooper*>(ALooper_forThread());
  if (looper == nullptr || looper->epoll_fd < 0) return kPollError;

  std::array<epoll_event, kMaxRegistrations + 1> events{};
  looper->polling.store(true, std::memory_order_relaxed);
  const int ready = ::epoll_wait(looper->epoll_fd, events.data(),
                                 static_cast<int>(events.size()), timeout_ms);
  looper->polling.store(false, std::memory_order_relaxed);

  if (ready == 0) return kPollTimeout;
  if (ready < 0) return errno == EINTR ? kPollTimeout : kPollError;

  bool had_callback = false;
  bool had_wake = false;

  for (int i = 0; i < ready; ++i) {
    const int fd = events[static_cast<std::size_t>(i)].data.fd;
    if (fd == looper->wake_fd) {
      uint64_t value = 0;
      (void)::read(looper->wake_fd, &value, sizeof(value));
      had_wake = true;
      continue;
    }
    bool found = false;
    const LooperRegistration registration =
        FindLooperRegistration(looper, fd, &found);
    if (!found) continue;
    const int android_events =
        AndroidEventsFromEpoll(events[static_cast<std::size_t>(i)].events);
    if (registration.callback != nullptr) {
      const int keep = registration.callback(registration.fd, android_events,
                                             registration.data);
      if (!keep) (void)ALooper_removeFd(looper, registration.fd);
      had_callback = true;
    } else {
      if (out_fd) *out_fd = registration.fd;
      if (out_events) *out_events = android_events;
      if (out_data) *out_data = registration.data;
      return registration.ident;
    }
  }

  if (had_callback) return kPollCallback;
  if (had_wake) return kPollWake;
  return kPollTimeout;
}

int ALooper_pollAll(int timeout_ms, int* out_fd, int* out_events,
                    void** out_data) {
  int result = ALooper_pollOnce(timeout_ms, out_fd, out_events, out_data);
  while (result == kPollCallback || result == kPollWake) {
    result = ALooper_pollOnce(0, out_fd, out_events, out_data);
  }
  return result;
}

// ANativeWindow — surface / window handle

struct ANativeWindow {};

void ANativeWindow_acquire(ANativeWindow*) {}
void ANativeWindow_release(ANativeWindow*) {}

ANativeWindow* ANativeWindow_fromSurface(void* env, void* surface) {
  void* native_window = MocktailNativeWindow();
  // The direct-Vulkan WSI adapter ignores the Android window
  // member and creates VkSurfaceKHR from SDL_Window. Preserve the Java Surface
  // token as its opaque ANativeWindow identity so a recreated Surface is not
  // collapsed back onto the same SDL_Window pointer inside Roblox.
  if (surface != nullptr && MocktailUsesDirectVulkan()) {
    if (TraceEnabled()) {
      std::fprintf(stderr,
                   "[androidstub] ANativeWindow_fromSurface env=%p surface=%p "
                   "-> surface_identity=%p backend_window=%p\n",
                   env, surface, surface, native_window);
    }
    return reinterpret_cast<ANativeWindow*>(surface);
  }
  if (native_window != nullptr) {
    if (TraceEnabled()) {
      std::fprintf(stderr,
                   "[androidstub] ANativeWindow_fromSurface env=%p surface=%p "
                   "-> native_window=%p\n",
                   env, surface, native_window);
    }
    return reinterpret_cast<ANativeWindow*>(native_window);
  }
  static ANativeWindow fallback_window;
  if (TraceEnabled()) {
    std::fprintf(stderr,
                 "[androidstub] ANativeWindow_fromSurface env=%p surface=%p "
                 "-> fallback_window=%p\n",
                 env, surface, static_cast<void*>(&fallback_window));
  }
  return &fallback_window;
}

int32_t ANativeWindow_getWidth(ANativeWindow* window) {
  int width = MocktailWindowWidth();
  if (TraceEnabled()) {
    std::fprintf(stderr,
                 "[androidstub] ANativeWindow_getWidth window=%p -> %d\n",
                 static_cast<void*>(window), width);
  }
  return width;
}

int32_t ANativeWindow_getHeight(ANativeWindow* window) {
  int height = MocktailWindowHeight();
  if (TraceEnabled()) {
    std::fprintf(stderr,
                 "[androidstub] ANativeWindow_getHeight window=%p -> %d\n",
                 static_cast<void*>(window), height);
  }
  return height;
}

}  // extern "C"
