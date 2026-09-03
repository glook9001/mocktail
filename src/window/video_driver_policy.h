#ifndef MOCKTAIL_WINDOW_VIDEO_DRIVER_POLICY_H_
#define MOCKTAIL_WINDOW_VIDEO_DRIVER_POLICY_H_

#include <string_view>
#include <vector>

namespace mocktail {
namespace window {

enum class VideoDriverChoice {
  kSdlDefault,
  kWayland,
  kX11,
};

struct VideoDriverPolicyInput {
  bool has_explicit_sdl_driver = false;
  bool force_wayland = false;
  bool force_x11 = false;
  bool prefer_wayland = true;
  bool has_wayland_session = false;
  bool has_x11_display = false;
};

// Resolves the SDL video backend before SDL_Init. An explicit SDL driver is
// always authoritative. A Wayland session always resolves to native Wayland -
// XWayland is not supported. Otherwise falls back to SDL default (native X11
// on an X11-only session, etc.). If Vulkan WSI is unavailable the graphics
// backend falls back to OpenGL, not to a different video driver.
VideoDriverChoice ResolveVideoDriverChoice(const VideoDriverPolicyInput& input);

const char* VideoDriverChoiceName(VideoDriverChoice choice);

// SDL accepts a comma-separated priority list. An inherited override is only
// useful when at least one requested driver exists in the linked SDL build.
bool HasAvailableVideoDriverCandidate(
    std::string_view requested,
    const std::vector<std::string_view>& available_drivers);

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_VIDEO_DRIVER_POLICY_H_
