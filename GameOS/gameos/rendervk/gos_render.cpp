// Vulkan (MoltenVK on macOS) implementation of the gos_render window/context
// API. M2 scaffold: opens an SDL Vulkan window, builds a swapchain, and
// clears/presents each frame. Drawing comes later; see docs/RENDERER_AUDIT.md.

#include "gameos.hpp"
#include "gos_render.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

// common input code (gameos_input.cpp) reads this
SDL_Window* g_sdl_window = NULL;

static bool g_verbose = false;

#define VK_CHECK(x) do { VkResult _r = (x); \
    if(_r != VK_SUCCESS) { \
        fprintf(stderr, "[VK] %s failed: %d (%s:%d)\n", #x, (int)_r, __FILE__, __LINE__); \
        abort(); \
    } } while(0)

namespace graphics {

struct RenderWindow {
    SDL_Window* window_;
    int width_;
    int height_;
    int bpp_;
};

struct RenderContext {
    RenderWindow*    render_window_;

    VkInstance       instance_;
    VkSurfaceKHR     surface_;
    VkPhysicalDevice phys_device_;
    VkDevice         device_;
    uint32_t         queue_family_;
    VkQueue          queue_;

    VkSwapchainKHR   swapchain_;
    VkFormat         swapchain_format_;
    VkExtent2D       swapchain_extent_;
    std::vector<VkImage> swapchain_images_;

    VkCommandPool    cmd_pool_;
    VkCommandBuffer  cmd_buf_;
    VkSemaphore      sem_image_available_;
    VkSemaphore      sem_render_done_;
    VkFence          frame_fence_;

    bool             swapchain_dirty_;
};

static RenderContext* g_ctx = NULL; // single window/device, same as GL path

void set_verbose(bool is_verbose) { g_verbose = is_verbose; }

RenderWindowHandle create_window(const char* pwinname, int width, int height,
                                 int wanted_bpp, int display_index)
{
    if(SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "[VK] SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return NULL;
    }

    // SDL needs a Vulkan loader; try the default search first, then the
    // Homebrew path (dev machines — the shipped .app will bundle MoltenVK)
    if(SDL_Vulkan_LoadLibrary(NULL) != 0) {
        if(SDL_Vulkan_LoadLibrary("/opt/homebrew/lib/libvulkan.dylib") != 0) {
            fprintf(stderr, "[VK] no Vulkan loader found: %s\n", SDL_GetError());
            return NULL;
        }
    }

    SDL_Window* win = SDL_CreateWindow(pwinname,
            SDL_WINDOWPOS_CENTERED_DISPLAY(display_index),
            SDL_WINDOWPOS_CENTERED_DISPLAY(display_index),
            width, height,
            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if(!win) {
        fprintf(stderr, "[VK] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return NULL;
    }

    RenderWindow* rw = new RenderWindow();
    rw->window_ = win;
    rw->width_ = width;
    rw->height_ = height;
    rw->bpp_ = wanted_bpp;
    g_sdl_window = win;
    return rw;
}

bool resize_window(RenderWindowHandle rw_handle, int width, int height)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw);
    SDL_SetWindowSize(rw->window_, width, height);
    SDL_SetWindowPosition(rw->window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    rw->width_ = width;
    rw->height_ = height;
    if(g_ctx)
        g_ctx->swapchain_dirty_ = true;
    return true;
}

void get_window_size(RenderWindowHandle rw_handle, int* width, int* height)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw && width && height);
    SDL_GetWindowSize(rw->window_, width, height);
}

int get_window_bpp(RenderWindowHandle rw_handle)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw);
    return rw->bpp_;
}

void get_drawable_size(RenderWindowHandle rw_handle, int* width, int* height)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw && width && height);
    SDL_Vulkan_GetDrawableSize(rw->window_, width, height);
}

void grab_window(RenderWindowHandle h, bool b_grab)
{
    RenderWindow* rw = (RenderWindow*)h;
    assert(rw);
    SDL_SetWindowGrab(rw->window_, b_grab ? SDL_TRUE : SDL_FALSE);
}

bool set_window_fullscreen(RenderWindowHandle rw_handle, bool fullscreen)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw);
    int rv = SDL_SetWindowFullscreen(rw->window_,
            fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    if(g_ctx)
        g_ctx->swapchain_dirty_ = true;
    return rv == 0;
}

bool is_window_fullscreen(RenderWindowHandle rw_handle)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw);
    return (SDL_GetWindowFlags(rw->window_) & SDL_WINDOW_FULLSCREEN) != 0;
}

bool is_mode_supported(int width, int height, int bpp)
{
    const int di = g_ctx ? get_window_display_index(g_ctx) : 0;
    const int num_modes = SDL_GetNumDisplayModes(di);
    for(int i = 0; i < num_modes; ++i) {
        SDL_DisplayMode mode;
        if(SDL_GetDisplayMode(di, i, &mode) != 0)
            continue;
        if(mode.w == width && mode.h == height && (int)SDL_BITSPERPIXEL(mode.format) == bpp)
            return true;
    }
    return false;
}

int get_window_display_index(RenderContextHandle ctx_h)
{
    RenderContext* ctx = (RenderContext*)ctx_h;
    if(ctx && ctx->render_window_)
        return SDL_GetWindowDisplayIndex(ctx->render_window_->window_);
    return 0;
}

bool get_desktop_display_mode(int display_index, int* width, int* height, int* bpp)
{
    SDL_DisplayMode mode;
    if(SDL_GetDesktopDisplayMode(display_index, &mode) != 0)
        return false;
    *width = mode.w;
    *height = mode.h;
    *bpp = SDL_BITSPERPIXEL(mode.format);
    return true;
}

int get_num_display_modes(int display_index)
{
    return SDL_GetNumDisplayModes(display_index);
}

bool get_display_mode_by_index(int display_index, int mode_index, int* width, int* height, int* bpp)
{
    SDL_DisplayMode mode;
    if(SDL_GetDisplayMode(display_index, mode_index, &mode) != 0)
        return false;
    *width = mode.w;
    *height = mode.h;
    *bpp = SDL_BITSPERPIXEL(mode.format);
    return true;
}

////////////////////////////////////////////////////////////////////////////////

static void create_swapchain(RenderContext* ctx)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->phys_device_, ctx->surface_, &caps));

    int dw = 0, dh = 0;
    SDL_Vulkan_GetDrawableSize(ctx->render_window_->window_, &dw, &dh);
    VkExtent2D extent = caps.currentExtent;
    if(extent.width == 0xFFFFFFFFu) {
        extent.width = (uint32_t)dw;
        extent.height = (uint32_t)dh;
    }

    uint32_t nformats = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->phys_device_, ctx->surface_, &nformats, NULL);
    std::vector<VkSurfaceFormatKHR> formats(nformats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->phys_device_, ctx->surface_, &nformats, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for(uint32_t i = 0; i < nformats; ++i) {
        if(formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = formats[i];
            break;
        }
    }

    uint32_t image_count = caps.minImageCount + 1;
    if(caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    VkSwapchainKHR old_swapchain = ctx->swapchain_;

    VkSwapchainCreateInfoKHR sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = ctx->surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync; always available
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old_swapchain;

    VK_CHECK(vkCreateSwapchainKHR(ctx->device_, &sci, NULL, &ctx->swapchain_));
    if(old_swapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(ctx->device_, old_swapchain, NULL);

    ctx->swapchain_format_ = chosen.format;
    ctx->swapchain_extent_ = extent;

    uint32_t nimages = 0;
    vkGetSwapchainImagesKHR(ctx->device_, ctx->swapchain_, &nimages, NULL);
    ctx->swapchain_images_.resize(nimages);
    vkGetSwapchainImagesKHR(ctx->device_, ctx->swapchain_, &nimages, ctx->swapchain_images_.data());

    ctx->swapchain_dirty_ = false;

    if(g_verbose)
        printf("[VK] swapchain %ux%u, %u images, format %d\n",
               extent.width, extent.height, nimages, (int)chosen.format);
}

RenderContextHandle init_render_context(RenderWindowHandle render_window)
{
    RenderWindow* rw = (RenderWindow*)render_window;
    assert(rw);

    RenderContext* ctx = new RenderContext(); // value-init zeroes all handles
    ctx->render_window_ = rw;

    // instance extensions: what SDL needs for the surface, plus portability
    // enumeration so the loader exposes MoltenVK (a non-conformant device)
    unsigned int next = 0;
    SDL_Vulkan_GetInstanceExtensions(rw->window_, &next, NULL);
    std::vector<const char*> exts(next);
    SDL_Vulkan_GetInstanceExtensions(rw->window_, &next, exts.data());

    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MechCommander 2";
    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;

#ifdef VK_KHR_portability_enumeration
    exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();

    VK_CHECK(vkCreateInstance(&ici, NULL, &ctx->instance_));

    if(!SDL_Vulkan_CreateSurface(rw->window_, ctx->instance_, &ctx->surface_)) {
        fprintf(stderr, "[VK] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return NULL;
    }

    // physical device: first one with a graphics queue that can present
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(ctx->instance_, &ndev, NULL);
    if(ndev == 0) {
        fprintf(stderr, "[VK] no Vulkan devices found\n");
        return NULL;
    }
    std::vector<VkPhysicalDevice> devices(ndev);
    vkEnumeratePhysicalDevices(ctx->instance_, &ndev, devices.data());

    for(uint32_t d = 0; d < ndev && !ctx->phys_device_; ++d) {
        uint32_t nqf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &nqf, NULL);
        std::vector<VkQueueFamilyProperties> qf(nqf);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &nqf, qf.data());
        for(uint32_t i = 0; i < nqf; ++i) {
            VkBool32 can_present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devices[d], i, ctx->surface_, &can_present);
            if((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && can_present) {
                ctx->phys_device_ = devices[d];
                ctx->queue_family_ = i;
                break;
            }
        }
    }
    if(!ctx->phys_device_) {
        fprintf(stderr, "[VK] no device with graphics+present queue\n");
        return NULL;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(ctx->phys_device_, &props);
    printf("[VK] device: %s (Vulkan %u.%u.%u)\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion));

    // device extensions: swapchain, plus portability_subset if the
    // implementation advertises it (required to be enabled then — MoltenVK)
    std::vector<const char*> dev_exts;
    dev_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    uint32_t ndevext = 0;
    vkEnumerateDeviceExtensionProperties(ctx->phys_device_, NULL, &ndevext, NULL);
    std::vector<VkExtensionProperties> devext(ndevext);
    vkEnumerateDeviceExtensionProperties(ctx->phys_device_, NULL, &ndevext, devext.data());
    for(uint32_t i = 0; i < ndevext; ++i) {
        if(0 == strcmp(devext[i].extensionName, "VK_KHR_portability_subset")) {
            dev_exts.push_back("VK_KHR_portability_subset");
            break;
        }
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = ctx->queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)dev_exts.size();
    dci.ppEnabledExtensionNames = dev_exts.data();

    VK_CHECK(vkCreateDevice(ctx->phys_device_, &dci, NULL, &ctx->device_));
    vkGetDeviceQueue(ctx->device_, ctx->queue_family_, 0, &ctx->queue_);

    VkCommandPoolCreateInfo cpi = {};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = ctx->queue_family_;
    VK_CHECK(vkCreateCommandPool(ctx->device_, &cpi, NULL, &ctx->cmd_pool_));

    VkCommandBufferAllocateInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = ctx->cmd_pool_;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx->device_, &cbi, &ctx->cmd_buf_));

    VkSemaphoreCreateInfo semci = {};
    semci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_CHECK(vkCreateSemaphore(ctx->device_, &semci, NULL, &ctx->sem_image_available_));
    VK_CHECK(vkCreateSemaphore(ctx->device_, &semci, NULL, &ctx->sem_render_done_));

    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VK_CHECK(vkCreateFence(ctx->device_, &fci, NULL, &ctx->frame_fence_));

    create_swapchain(ctx);

    g_ctx = ctx;
    return ctx;
}

void make_current_context(RenderContextHandle /*ctx_h*/)
{
    // no-op: Vulkan has no notion of a thread-current context
}

// One frame: acquire an image, clear it, present. This is deliberately the
// simplest correct thing (single frame in flight, fence-synchronized); the
// real renderer will replace the body of the frame, not this structure.
void swap_window(RenderWindowHandle /*h*/)
{
    RenderContext* ctx = g_ctx;
    if(!ctx)
        return;

    if(ctx->swapchain_dirty_) {
        vkDeviceWaitIdle(ctx->device_);
        create_swapchain(ctx);
    }

    uint32_t image_index = 0;
    VkResult res = vkAcquireNextImageKHR(ctx->device_, ctx->swapchain_, UINT64_MAX,
                                         ctx->sem_image_available_, VK_NULL_HANDLE, &image_index);
    if(res == VK_ERROR_OUT_OF_DATE_KHR) {
        ctx->swapchain_dirty_ = true;
        return; // next frame recreates and draws
    }

    VkCommandBuffer cb = ctx->cmd_buf_;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));

    VkImage image = ctx->swapchain_images_[image_index];
    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;

    VkImageMemoryBarrier to_clear = {};
    to_clear.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_clear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_clear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_clear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_clear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_clear.image = image;
    to_clear.subresourceRange = range;
    to_clear.srcAccessMask = 0;
    to_clear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_clear);

    // distinctive teal so a Vulkan run is visually unmistakable vs. GL black
    VkClearColorValue clear_color = {{0.0f, 0.25f, 0.30f, 1.0f}};
    vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear_color, 1, &range);

    VkImageMemoryBarrier to_present = to_clear;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);

    VK_CHECK(vkEndCommandBuffer(cb));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &ctx->sem_image_available_;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &ctx->sem_render_done_;
    VK_CHECK(vkQueueSubmit(ctx->queue_, 1, &si, ctx->frame_fence_));

    VkPresentInfoKHR pi = {};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &ctx->sem_render_done_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &ctx->swapchain_;
    pi.pImageIndices = &image_index;
    res = vkQueuePresentKHR(ctx->queue_, &pi);
    if(res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
        ctx->swapchain_dirty_ = true;

    VK_CHECK(vkWaitForFences(ctx->device_, 1, &ctx->frame_fence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(ctx->device_, 1, &ctx->frame_fence_));
}

void destroy_render_context(RenderContextHandle rc_handle)
{
    RenderContext* ctx = (RenderContext*)rc_handle;
    if(!ctx)
        return;
    vkDeviceWaitIdle(ctx->device_);
    vkDestroyFence(ctx->device_, ctx->frame_fence_, NULL);
    vkDestroySemaphore(ctx->device_, ctx->sem_render_done_, NULL);
    vkDestroySemaphore(ctx->device_, ctx->sem_image_available_, NULL);
    vkDestroyCommandPool(ctx->device_, ctx->cmd_pool_, NULL);
    vkDestroySwapchainKHR(ctx->device_, ctx->swapchain_, NULL);
    vkDestroyDevice(ctx->device_, NULL);
    vkDestroySurfaceKHR(ctx->instance_, ctx->surface_, NULL);
    vkDestroyInstance(ctx->instance_, NULL);
    if(ctx == g_ctx)
        g_ctx = NULL;
    delete ctx;
}

void destroy_window(RenderWindowHandle rw_handle)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    if(!rw)
        return;
    SDL_DestroyWindow(rw->window_);
    g_sdl_window = NULL;
    delete rw;
}

} // namespace graphics
