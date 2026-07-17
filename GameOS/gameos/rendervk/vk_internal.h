// Internal contract between rendervk/gos_render.cpp (window, device,
// swapchain, frame lifecycle) and rendervk/gameos_graphics.cpp (pipelines,
// textures, draws). Not visible outside the Vulkan backend.
#ifndef VK_INTERNAL_H
#define VK_INTERNAL_H

#include <vulkan/vulkan.h>

namespace graphics {

struct VkFrame {
    VkDevice         device;
    VkPhysicalDevice phys_device;
    VkQueue          queue;
    uint32_t         queue_family;
    VkRenderPass     render_pass;   // 1 subpass: color + depth
    VkCommandBuffer  draw_cb;       // inside the render pass during a frame
    VkCommandBuffer  upload_cb;     // outside the render pass, submitted first
    VkExtent2D       extent;        // current swapchain extent (pixels)
    bool             frame_active;  // between vk_begin_frame and present
};

// valid after init_render_context
VkFrame* vk_frame();

// acquire image, begin command buffers, begin the clearing render pass.
// no-op (false) if the swapchain is unusable this frame.
bool vk_begin_frame();

uint32_t vk_find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props);

} // namespace graphics

#endif // VK_INTERNAL_H
