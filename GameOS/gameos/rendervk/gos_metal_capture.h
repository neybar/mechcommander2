// Metal frame-capture hook for the Vulkan backend (macOS/MoltenVK only).
//
// mc2-vk runs Vulkan on Metal through MoltenVK, so Xcode's Metal frame debugger
// can see the real draws: per-pixel draw history, per-draw blend/depth state,
// bound textures, fragment shader debugging. That answers "which draw wrote
// this pixel", which printf-style pixel tracing cannot -- see the black-quad
// hunt in docs/ENGINEERING_LOG.md for what this was built for.
//
// Off unless MC2_VK_CAPTURE_AT_SECS=<seconds> is set: the first frame drawn
// after that much process uptime is captured to MC2_VK_CAPTURE_FILE (default
// /tmp/mc2-vk.gputrace), one capture per run. The game must also be launched
// with METAL_CAPTURE_ENABLED=1 or Metal refuses to hand out a trace.
//
// Off Apple every entry point compiles to nothing.
#ifndef GOS_METAL_CAPTURE_H
#define GOS_METAL_CAPTURE_H

#include <vulkan/vulkan.h>

namespace graphics {

#ifdef __APPLE__

// True when the env asks for a capture. Checked before device creation so the
// VK_EXT_metal_objects plumbing only happens on a capture run -- an ordinary
// run creates exactly the device it always did.
bool metal_capture_requested();

// Grab the MTLDevice backing this VkDevice. metal_objects_enabled says whether
// VK_EXT_metal_objects was both advertised and enabled at device creation;
// without it there is no way to reach the Metal object and capture stays off.
void metal_capture_init(VkDevice device, bool metal_objects_enabled);

// Frame boundaries. Call frame_begin after the swapchain image is acquired but
// before the frame's command buffers open, and frame_end after the present has
// been submitted. They must pair: a frame that skips drawing must call neither.
void metal_capture_frame_begin();
void metal_capture_frame_end();

#else

inline bool metal_capture_requested() { return false; }
inline void metal_capture_init(VkDevice, bool) {}
inline void metal_capture_frame_begin() {}
inline void metal_capture_frame_end() {}

#endif // __APPLE__

} // namespace graphics

#endif // GOS_METAL_CAPTURE_H
