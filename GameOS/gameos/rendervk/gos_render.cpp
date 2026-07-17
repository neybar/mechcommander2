// Vulkan (MoltenVK on macOS) implementation of the gos_render window/context
// API. Owns the device, swapchain, depth buffer, render pass and the frame
// lifecycle: vk_begin_frame() opens a clearing render pass on the draw
// command buffer; swap_window() closes it, submits (uploads first) and
// presents. Drawing itself lives in rendervk/gameos_graphics.cpp.

#include "gameos.hpp"
#include "gos_render.h"
#include "vk_internal.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

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
    std::vector<VkImage>       swapchain_images_;
    std::vector<VkImageView>   swapchain_views_;
    std::vector<VkFramebuffer> framebuffers_;

    VkImage          depth_image_;
    VkDeviceMemory   depth_memory_;
    VkImageView      depth_view_;

    VkRenderPass     render_pass_;

    VkCommandPool    cmd_pool_;
    VkCommandBuffer  draw_cb_;
    VkCommandBuffer  upload_cb_;
    VkSemaphore      sem_image_available_;
    VkSemaphore      sem_render_done_;
    VkFence          frame_fence_;

    uint32_t         cur_image_;
    bool             swapchain_dirty_;

    VkFrame          frame_; // view handed to gameos_graphics
};

static RenderContext* g_ctx = NULL; // single window/device, same as GL path

VkFrame* vk_frame()
{
    return g_ctx ? &g_ctx->frame_ : NULL;
}

uint32_t vk_find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_ctx->phys_device_, &mp);
    for(uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    fprintf(stderr, "[VK] no suitable memory type (bits 0x%x props 0x%x)\n", type_bits, props);
    abort();
}

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

static void destroy_swapchain_views(RenderContext* ctx)
{
    for(size_t i = 0; i < ctx->framebuffers_.size(); ++i)
        vkDestroyFramebuffer(ctx->device_, ctx->framebuffers_[i], NULL);
    ctx->framebuffers_.clear();
    for(size_t i = 0; i < ctx->swapchain_views_.size(); ++i)
        vkDestroyImageView(ctx->device_, ctx->swapchain_views_[i], NULL);
    ctx->swapchain_views_.clear();
    if(ctx->depth_view_) {
        vkDestroyImageView(ctx->device_, ctx->depth_view_, NULL);
        vkDestroyImage(ctx->device_, ctx->depth_image_, NULL);
        vkFreeMemory(ctx->device_, ctx->depth_memory_, NULL);
        ctx->depth_view_ = VK_NULL_HANDLE;
        ctx->depth_image_ = VK_NULL_HANDLE;
        ctx->depth_memory_ = VK_NULL_HANDLE;
    }
}

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

    destroy_swapchain_views(ctx);
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

    // depth buffer
    VkImageCreateInfo di = {};
    di.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    di.imageType = VK_IMAGE_TYPE_2D;
    di.format = VK_FORMAT_D32_SFLOAT;
    di.extent.width = extent.width;
    di.extent.height = extent.height;
    di.extent.depth = 1;
    di.mipLevels = 1;
    di.arrayLayers = 1;
    di.samples = VK_SAMPLE_COUNT_1_BIT;
    di.tiling = VK_IMAGE_TILING_OPTIMAL;
    di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    di.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx->device_, &di, NULL, &ctx->depth_image_));

    VkMemoryRequirements mreq;
    vkGetImageMemoryRequirements(ctx->device_, ctx->depth_image_, &mreq);
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mreq.size;
    mai.memoryTypeIndex = vk_find_memory_type(mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx->device_, &mai, NULL, &ctx->depth_memory_));
    VK_CHECK(vkBindImageMemory(ctx->device_, ctx->depth_image_, ctx->depth_memory_, 0));

    VkImageViewCreateInfo dvi = {};
    dvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    dvi.image = ctx->depth_image_;
    dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvi.format = VK_FORMAT_D32_SFLOAT;
    dvi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    dvi.subresourceRange.levelCount = 1;
    dvi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx->device_, &dvi, NULL, &ctx->depth_view_));

    // color views + framebuffers
    ctx->swapchain_views_.resize(nimages);
    ctx->framebuffers_.resize(nimages);
    for(uint32_t i = 0; i < nimages; ++i) {
        VkImageViewCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = ctx->swapchain_images_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ctx->swapchain_format_;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx->device_, &vi, NULL, &ctx->swapchain_views_[i]));

        VkImageView attachments[2] = { ctx->swapchain_views_[i], ctx->depth_view_ };
        VkFramebufferCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = ctx->render_pass_;
        fci.attachmentCount = 2;
        fci.pAttachments = attachments;
        fci.width = extent.width;
        fci.height = extent.height;
        fci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(ctx->device_, &fci, NULL, &ctx->framebuffers_[i]));
    }

    ctx->frame_.extent = extent;
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

    // render pass: color (clear -> present) + depth (clear, discard)
    VkAttachmentDescription atts[2] = {};
    atts[0].format = VK_FORMAT_B8G8R8A8_UNORM; // fixed choice below too
    atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    atts[1].format = VK_FORMAT_D32_SFLOAT;
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    sub.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci = {};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments = atts;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(ctx->device_, &rpci, NULL, &ctx->render_pass_));

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
    VK_CHECK(vkAllocateCommandBuffers(ctx->device_, &cbi, &ctx->draw_cb_));
    VK_CHECK(vkAllocateCommandBuffers(ctx->device_, &cbi, &ctx->upload_cb_));

    VkSemaphoreCreateInfo semci = {};
    semci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_CHECK(vkCreateSemaphore(ctx->device_, &semci, NULL, &ctx->sem_image_available_));
    VK_CHECK(vkCreateSemaphore(ctx->device_, &semci, NULL, &ctx->sem_render_done_));

    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VK_CHECK(vkCreateFence(ctx->device_, &fci, NULL, &ctx->frame_fence_));

    ctx->frame_.device = ctx->device_;
    ctx->frame_.phys_device = ctx->phys_device_;
    ctx->frame_.queue = ctx->queue_;
    ctx->frame_.queue_family = ctx->queue_family_;
    ctx->frame_.render_pass = ctx->render_pass_;
    ctx->frame_.draw_cb = ctx->draw_cb_;
    ctx->frame_.upload_cb = ctx->upload_cb_;
    ctx->frame_.frame_active = false;

    g_ctx = ctx;
    create_swapchain(ctx);
    return ctx;
}

void make_current_context(RenderContextHandle /*ctx_h*/)
{
    // no-op: Vulkan has no notion of a thread-current context
}

bool vk_begin_frame()
{
    RenderContext* ctx = g_ctx;
    if(!ctx || ctx->frame_.frame_active)
        return ctx ? ctx->frame_.frame_active : false;

    if(ctx->swapchain_dirty_) {
        vkDeviceWaitIdle(ctx->device_);
        create_swapchain(ctx);
    }

    VkResult res = vkAcquireNextImageKHR(ctx->device_, ctx->swapchain_, UINT64_MAX,
                                         ctx->sem_image_available_, VK_NULL_HANDLE, &ctx->cur_image_);
    if(res == VK_ERROR_OUT_OF_DATE_KHR) {
        ctx->swapchain_dirty_ = true;
        return false; // skip this frame's drawing
    }

    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(ctx->upload_cb_, 0);
    VK_CHECK(vkBeginCommandBuffer(ctx->upload_cb_, &bi));
    vkResetCommandBuffer(ctx->draw_cb_, 0);
    VK_CHECK(vkBeginCommandBuffer(ctx->draw_cb_, &bi));

    VkClearValue clears[2];
    clears[0].color.float32[0] = 0.0f;
    clears[0].color.float32[1] = 0.0f;
    clears[0].color.float32[2] = 0.0f;
    clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 1.0f;
    clears[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rbi = {};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = ctx->render_pass_;
    rbi.framebuffer = ctx->framebuffers_[ctx->cur_image_];
    rbi.renderArea.extent = ctx->swapchain_extent_;
    rbi.clearValueCount = 2;
    rbi.pClearValues = clears;
    vkCmdBeginRenderPass(ctx->draw_cb_, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    ctx->frame_.frame_active = true;
    return true;
}

// End the frame: close the render pass, submit uploads then draws, present.
void swap_window(RenderWindowHandle /*h*/)
{
    RenderContext* ctx = g_ctx;
    if(!ctx)
        return;
    if(!ctx->frame_.frame_active)
        return; // nothing was begun this frame (e.g. swapchain out of date)

    vkCmdEndRenderPass(ctx->draw_cb_);
    VK_CHECK(vkEndCommandBuffer(ctx->draw_cb_));
    VK_CHECK(vkEndCommandBuffer(ctx->upload_cb_));

    VkCommandBuffer cbs[2] = { ctx->upload_cb_, ctx->draw_cb_ };
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &ctx->sem_image_available_;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 2;
    si.pCommandBuffers = cbs;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &ctx->sem_render_done_;
    VK_CHECK(vkQueueSubmit(ctx->queue_, 1, &si, ctx->frame_fence_));

    VkPresentInfoKHR pi = {};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &ctx->sem_render_done_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &ctx->swapchain_;
    pi.pImageIndices = &ctx->cur_image_;
    VkResult res = vkQueuePresentKHR(ctx->queue_, &pi);
    if(res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
        ctx->swapchain_dirty_ = true;

    // single frame in flight: wait now so per-frame resources (ring buffer,
    // descriptor pool) are free to reset at the next vk_begin_frame
    VK_CHECK(vkWaitForFences(ctx->device_, 1, &ctx->frame_fence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(ctx->device_, 1, &ctx->frame_fence_));

    ctx->frame_.frame_active = false;
}

void destroy_render_context(RenderContextHandle rc_handle)
{
    RenderContext* ctx = (RenderContext*)rc_handle;
    if(!ctx)
        return;
    vkDeviceWaitIdle(ctx->device_);
    destroy_swapchain_views(ctx);
    vkDestroyFence(ctx->device_, ctx->frame_fence_, NULL);
    vkDestroySemaphore(ctx->device_, ctx->sem_render_done_, NULL);
    vkDestroySemaphore(ctx->device_, ctx->sem_image_available_, NULL);
    vkDestroyCommandPool(ctx->device_, ctx->cmd_pool_, NULL);
    vkDestroyRenderPass(ctx->device_, ctx->render_pass_, NULL);
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
