// Vulkan-backend implementation of the gos_* graphics API (M2).
// Immediate-mode path (quads/tris/lines/points + text) renders for real:
// SPIR-V pipelines keyed on render state, a per-frame vertex ring buffer,
// textures decoded via the shared Image code and uploaded on first use.
// Retained-buffer draws (mech meshes, FMV) are still no-ops — next stage.
// Frame lifecycle (swapchain/render pass) lives in rendervk/gos_render.cpp.

#include "gameos.hpp"
#include "gos_render.h"
#include "gos_font.h"
#include "vk_internal.h"

#include "utils/Image.h"
#include "utils/vec.h"

#include "platform_stdlib.h" // _splitpath

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static graphics::RenderWindowHandle g_win_h = NULL;

bool g_disable_quads = true;

////////////////////////////////////////////////////////////////////////////////
// render states (declared early — draw code reads them)

static const int NUM_RENDER_STATES = gos_MaxState;
static int g_render_states[NUM_RENDER_STATES] = {0};
static std::vector<std::vector<int> > g_state_stack;

static int g_width = 0;   // logical size (game coordinate space)
static int g_height = 0;
static bool g_pending_resize = false;
static int g_req_width = 0;
static int g_req_height = 0;
static bool g_req_fullscreen = false;

static float g_viewport[4] = {0, 0, 1, 1}; // top, left, bottom, right
static vec4 g_render_viewport(0, 0, 0, 0);
static mat4 g_projection = mat4::identity();

////////////////////////////////////////////////////////////////////////////////
// textures

struct VkStubTexture {
    std::string name_;
    gos_TextureFormat format_;
    uint32_t w_;
    uint32_t h_;
    std::vector<DWORD> pixels_; // 8888, memory order R,G,B,A (as GL path)
    bool alive_;
    bool dirty_;                // CPU pixels newer than GPU image

    VkImage image_;
    VkDeviceMemory memory_;
    VkImageView view_;
};

static std::vector<VkStubTexture> g_textures;

static DWORD addTexture(VkStubTexture&& t)
{
    // reuse dead slots so long sessions don't grow the table unboundedly
    for(size_t i = 0; i < g_textures.size(); ++i) {
        if(!g_textures[i].alive_ && g_textures[i].image_ == VK_NULL_HANDLE) {
            g_textures[i] = t;
            return (DWORD)(i + 1);
        }
    }
    g_textures.push_back(t);
    return (DWORD)g_textures.size(); // handle 0 = "no texture"
}

static VkStubTexture* getTexture(DWORD handle)
{
    if(handle == 0 || handle > g_textures.size())
        return NULL;
    return &g_textures[handle - 1];
}

// decode into the stub's 8888 buffer (RGB gets alpha 255)
static void fillPixels(VkStubTexture& t, const Image& img)
{
    t.w_ = img.getWidth();
    t.h_ = img.getHeight();
    t.pixels_.resize((size_t)t.w_ * t.h_);
    const uint8_t* src = img.getPixels();
    const size_t n = (size_t)t.w_ * t.h_;
    if(img.getFormat() == FORMAT_RGBA8) {
        memcpy(t.pixels_.data(), src, n * 4);
    } else { // FORMAT_RGB8
        uint8_t* dst = (uint8_t*)t.pixels_.data();
        for(size_t i = 0; i < n; ++i) {
            dst[4*i+0] = src[3*i+0];
            dst[4*i+1] = src[3*i+1];
            dst[4*i+2] = src[3*i+2];
            dst[4*i+3] = 0xff;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Vulkan draw machinery

namespace {

struct PushConstants {
    float mvp[16];
    float fog_color[4];
    float foreground[4];
    uint32_t flags;
    uint32_t pad_[3];
};

enum ShaderKind { SHADER_VERTEX = 0, SHADER_TEX_VERTEX = 1, SHADER_TEXT = 2, SHADER_KIND_COUNT };
enum TopoKind { TOPO_TRIS = 0, TOPO_LINES = 1, TOPO_POINTS = 2, TOPO_COUNT };

struct VkDrawEngine {
    bool initialized;
    bool init_failed;

    VkShaderModule vs_vertex, vs_text;
    VkShaderModule fs_vertex, fs_tex_vertex, fs_text;

    VkDescriptorSetLayout dset_layout;
    VkPipelineLayout pipe_layout;
    std::map<uint32_t, VkPipeline> pipelines;

    VkSampler samplers[4]; // [filter linear?][address clamp?]

    // per-frame vertex/staging ring (single frame in flight)
    VkBuffer ring;
    VkDeviceMemory ring_mem;
    uint8_t* ring_ptr;
    VkDeviceSize ring_size;
    VkDeviceSize ring_off;
    bool ring_overflowed;

    VkDescriptorPool dpool;
    std::map<uint64_t, VkDescriptorSet> dset_cache; // per-frame (view,sampler)->set

    VkPipeline bound_pipeline;
};

VkDrawEngine g_eng = {};

VkShaderModule loadShaderModule(VkDevice dev, const char* path)
{
    FILE* f = fopen(path, "rb");
    if(!f) {
        SPEW(("GRAPHICS", "VK: cannot open shader %s (cwd must be the game dir)\n", path));
        return VK_NULL_HANDLE;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint32_t> words((size + 3) / 4, 0);
    fread(words.data(), 1, size, f);
    fclose(f);

    VkShaderModuleCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = (size_t)size;
    ci.pCode = words.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if(vkCreateShaderModule(dev, &ci, NULL, &mod) != VK_SUCCESS) {
        SPEW(("GRAPHICS", "VK: vkCreateShaderModule failed for %s\n", path));
        return VK_NULL_HANDLE;
    }
    return mod;
}

bool engineInit()
{
    if(g_eng.initialized)
        return true;
    if(g_eng.init_failed)
        return false;

    graphics::VkFrame* fr = graphics::vk_frame();
    if(!fr)
        return false;
    VkDevice dev = fr->device;

    g_eng.vs_vertex = loadShaderModule(dev, "shaders/vk/gos_vertex.vert.spv");
    g_eng.fs_vertex = loadShaderModule(dev, "shaders/vk/gos_vertex.frag.spv");
    g_eng.fs_tex_vertex = loadShaderModule(dev, "shaders/vk/gos_tex_vertex.frag.spv");
    g_eng.vs_text = loadShaderModule(dev, "shaders/vk/gos_text.vert.spv");
    g_eng.fs_text = loadShaderModule(dev, "shaders/vk/gos_text.frag.spv");
    if(!g_eng.vs_vertex || !g_eng.fs_vertex || !g_eng.fs_tex_vertex || !g_eng.vs_text || !g_eng.fs_text) {
        g_eng.init_failed = true; // draws become no-ops; frame still clears
        return false;
    }

    VkDescriptorSetLayoutBinding b = {};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dlci = {};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 1;
    dlci.pBindings = &b;
    vkCreateDescriptorSetLayout(dev, &dlci, NULL, &g_eng.dset_layout);

    VkPushConstantRange pcr = {};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo plci = {};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &g_eng.dset_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(dev, &plci, NULL, &g_eng.pipe_layout);

    for(int filt = 0; filt < 2; ++filt) {
        for(int addr = 0; addr < 2; ++addr) {
            VkSamplerCreateInfo sci = {};
            sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sci.magFilter = filt ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            sci.minFilter = sci.magFilter;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sci.addressModeU = addr ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                    : VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sci.addressModeV = sci.addressModeU;
            sci.addressModeW = sci.addressModeU;
            vkCreateSampler(dev, &sci, NULL, &g_eng.samplers[filt * 2 + addr]);
        }
    }

    // 16 MB host-visible ring: vertex data + texture staging for one frame
    g_eng.ring_size = 16u * 1024 * 1024;
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = g_eng.ring_size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
              | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(dev, &bci, NULL, &g_eng.ring);
    VkMemoryRequirements mreq;
    vkGetBufferMemoryRequirements(dev, g_eng.ring, &mreq);
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mreq.size;
    mai.memoryTypeIndex = graphics::vk_find_memory_type(mreq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(dev, &mai, NULL, &g_eng.ring_mem);
    vkBindBufferMemory(dev, g_eng.ring, g_eng.ring_mem, 0);
    vkMapMemory(dev, g_eng.ring_mem, 0, VK_WHOLE_SIZE, 0, (void**)&g_eng.ring_ptr);

    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 };
    VkDescriptorPoolCreateInfo dpci = {};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 4096;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &dps;
    vkCreateDescriptorPool(dev, &dpci, NULL, &g_eng.dpool);

    g_eng.initialized = true;
    return true;
}

uint32_t pipelineKey(ShaderKind sh, TopoKind topo)
{
    uint32_t blend = g_render_states[gos_State_AlphaMode];      // 0..4
    uint32_t zcomp = g_render_states[gos_State_ZCompare];       // 0..2
    uint32_t zwrite = g_render_states[gos_State_ZWrite] ? 1 : 0;
    uint32_t cull = g_render_states[gos_State_Culling];         // 0..2 (gos_Cull_None=0? see below)
    // gos_Cull_None=1,CW=2,CCW=3 in gameos.hpp -> normalize to 0..2
    if(cull >= 1) cull -= 1;
    return (uint32_t)sh | ((uint32_t)topo << 3) | (blend << 6) | (zcomp << 10)
         | (zwrite << 13) | (cull << 14);
}

VkPipeline getPipeline(ShaderKind sh, TopoKind topo)
{
    uint32_t key = pipelineKey(sh, topo);
    std::map<uint32_t, VkPipeline>::iterator it = g_eng.pipelines.find(key);
    if(it != g_eng.pipelines.end())
        return it->second;

    graphics::VkFrame* fr = graphics::vk_frame();

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = (sh == SHADER_TEXT) ? g_eng.vs_text : g_eng.vs_vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = (sh == SHADER_TEXT) ? g_eng.fs_text
                     : (sh == SHADER_TEX_VERTEX) ? g_eng.fs_tex_vertex : g_eng.fs_vertex;
    stages[1].pName = "main";

    // gos_VERTEX: 4 floats pos, u8x4 argb, u8x4 frgb, 2 floats uv (32 bytes)
    VkVertexInputBindingDescription bind = { 0, sizeof(gos_VERTEX), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[4] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R8G8B8A8_UNORM, 16 },
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, 20 },
        { 3, 0, VK_FORMAT_R32G32_SFLOAT, 24 },
    };
    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = (topo == TOPO_LINES) ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                : (topo == TOPO_POINTS) ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    switch(g_render_states[gos_State_Culling]) {
        case gos_Cull_CW:  rs.cullMode = VK_CULL_MODE_BACK_BIT; break;
        case gos_Cull_CCW: rs.cullMode = VK_CULL_MODE_FRONT_BIT; break;
        default:           rs.cullMode = VK_CULL_MODE_NONE; break;
    }
    // negative-height viewport keeps GL's NDC orientation, so GL winding rules apply
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    const int zc = g_render_states[gos_State_ZCompare];
    ds.depthTestEnable = zc != 0;
    ds.depthWriteEnable = g_render_states[gos_State_ZWrite] ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = (zc == 1) ? VK_COMPARE_OP_LESS_OR_EQUAL
                      : (zc == 2) ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = 0xF;
    switch(g_render_states[gos_State_AlphaMode]) {
        default:
        case gos_Alpha_OneZero:
            cba.blendEnable = VK_FALSE;
            break;
        case gos_Alpha_OneOne:
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case gos_Alpha_AlphaInvAlpha:
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case gos_Alpha_OneInvAlpha:
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case gos_Alpha_AlphaOne:
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
    }
    cba.srcAlphaBlendFactor = cba.srcColorBlendFactor ? cba.srcColorBlendFactor : VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = cba.dstColorBlendFactor ? cba.dstColorBlendFactor : VK_BLEND_FACTOR_ZERO;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsci = {};
    dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dsci.dynamicStateCount = 2;
    dsci.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gpci = {};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dsci;
    gpci.layout = g_eng.pipe_layout;
    gpci.renderPass = fr->render_pass;

    VkPipeline pipe = VK_NULL_HANDLE;
    if(vkCreateGraphicsPipelines(fr->device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe) != VK_SUCCESS) {
        SPEW(("GRAPHICS", "VK: pipeline creation failed (key %x)\n", key));
    }
    g_eng.pipelines[key] = pipe;
    return pipe;
}

// allocate space in the per-frame ring; NULL on overflow (frame drops draws)
uint8_t* ringAlloc(VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize* out_off)
{
    VkDeviceSize off = (g_eng.ring_off + align - 1) & ~(align - 1);
    if(off + bytes > g_eng.ring_size) {
        if(!g_eng.ring_overflowed) {
            SPEW(("GRAPHICS", "VK: ring buffer overflow, draws dropped this frame\n"));
            g_eng.ring_overflowed = true;
        }
        return NULL;
    }
    g_eng.ring_off = off + bytes;
    *out_off = off;
    return g_eng.ring_ptr + off;
}

// ensure the texture has an up-to-date VkImage; records upload into upload_cb
bool textureToGpu(VkStubTexture* t)
{
    graphics::VkFrame* fr = graphics::vk_frame();
    VkDevice dev = fr->device;

    if(t->image_ == VK_NULL_HANDLE) {
        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent.width = t->w_;
        ici.extent.height = t->h_;
        ici.extent.depth = 1;
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(dev, &ici, NULL, &t->image_) != VK_SUCCESS)
            return false;

        VkMemoryRequirements mreq;
        vkGetImageMemoryRequirements(dev, t->image_, &mreq);
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mreq.size;
        mai.memoryTypeIndex = graphics::vk_find_memory_type(mreq.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev, &mai, NULL, &t->memory_);
        vkBindImageMemory(dev, t->image_, t->memory_, 0);

        VkImageViewCreateInfo vci = {};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = t->image_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        vkCreateImageView(dev, &vci, NULL, &t->view_);
        t->dirty_ = true;
    }

    if(t->dirty_) {
        const VkDeviceSize bytes = (VkDeviceSize)t->w_ * t->h_ * 4;
        VkDeviceSize off = 0;
        uint8_t* dst = ringAlloc(bytes, 16, &off);
        if(!dst)
            return true; // keep old contents this frame
        if(t->pixels_.size() * sizeof(DWORD) >= bytes)
            memcpy(dst, t->pixels_.data(), bytes);
        else
            memset(dst, 0, bytes);

        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;

        VkImageMemoryBarrier to_dst = {};
        to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // discard previous
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = t->image_;
        to_dst.subresourceRange = range;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(fr->upload_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);

        VkBufferImageCopy bic = {};
        bic.bufferOffset = off;
        bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bic.imageSubresource.layerCount = 1;
        bic.imageExtent.width = t->w_;
        bic.imageExtent.height = t->h_;
        bic.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(fr->upload_cb, g_eng.ring, t->image_,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

        VkImageMemoryBarrier to_read = to_dst;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(fr->upload_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &to_read);

        t->dirty_ = false;
    }
    return true;
}

VkSampler samplerFromStates()
{
    const int filt = (g_render_states[gos_State_Filter] != gos_FilterNone) ? 1 : 0;
    const int addr = (g_render_states[gos_State_TextureAddress] == gos_TextureClamp) ? 1 : 0;
    return g_eng.samplers[filt * 2 + addr];
}

VkDescriptorSet descriptorFor(VkImageView view, VkSampler sampler)
{
    const uint64_t key = (uint64_t)(uintptr_t)view ^ ((uint64_t)(uintptr_t)sampler << 1);
    std::map<uint64_t, VkDescriptorSet>::iterator it = g_eng.dset_cache.find(key);
    if(it != g_eng.dset_cache.end())
        return it->second;

    graphics::VkFrame* fr = graphics::vk_frame();
    VkDescriptorSetAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = g_eng.dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_eng.dset_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if(vkAllocateDescriptorSets(fr->device, &ai, &set) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkDescriptorImageInfo dii = {};
    dii.sampler = sampler;
    dii.imageView = view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w = {};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(fr->device, 1, &w, 0, NULL);

    g_eng.dset_cache[key] = set;
    return set;
}

void fillPushConstants(PushConstants* pc)
{
    // mat4 here is row-major storage with row_major-consumed mvp on GL via
    // setTransform -> glUniformMatrix4fv(transpose=TRUE)? The GL path passes
    // projection_ directly; vec.h mat4 is row-major and the GL shaders were
    // fed via setMat4 with transpose enabled upstream. SPIR-V std140 expects
    // column-major, so transpose here.
    const mat4& m = g_projection;
    for(int r = 0; r < 4; ++r)
        for(int c = 0; c < 4; ++c)
            pc->mvp[c * 4 + r] = m[r * 4 + c];

    const vec4 fog = uint32_to_vec4((uint32_t)g_render_states[gos_State_Fog]);
    pc->fog_color[0] = fog.x;
    pc->fog_color[1] = fog.y;
    pc->fog_color[2] = fog.z;
    pc->fog_color[3] = fog.w;

    pc->flags = g_render_states[gos_State_AlphaTest] ? 1u : 0u;
}

// shared tail of every immediate draw: pipeline, descriptors, push, draw
void emitDraw(ShaderKind sh, TopoKind topo, const gos_VERTEX* vertices, int count,
              const float* foreground /*text only*/)
{
    graphics::VkFrame* fr = graphics::vk_frame();
    if(!fr || !fr->frame_active || count <= 0)
        return;
    if(!engineInit())
        return;

    // texture (if the shader samples one)
    VkDescriptorSet dset = VK_NULL_HANDLE;
    if(sh != SHADER_VERTEX) {
        DWORD tex_handle = (DWORD)g_render_states[gos_State_Texture];
        VkStubTexture* t = getTexture(tex_handle);
        if(!t || !t->alive_) {
            // textured shader without a texture: fall back to untextured
            sh = SHADER_TEXT == sh ? sh : SHADER_VERTEX;
            if(sh != SHADER_VERTEX)
                return; // text without font texture — nothing sane to draw
        } else {
            if(!textureToGpu(t))
                return;
            dset = descriptorFor(t->view_, samplerFromStates());
            if(dset == VK_NULL_HANDLE)
                return;
        }
    }

    VkDeviceSize voff = 0;
    uint8_t* dst = ringAlloc((VkDeviceSize)count * sizeof(gos_VERTEX), 4, &voff);
    if(!dst)
        return;
    memcpy(dst, vertices, (size_t)count * sizeof(gos_VERTEX));

    VkPipeline pipe = getPipeline(sh, topo);
    if(pipe == VK_NULL_HANDLE)
        return;

    VkCommandBuffer cb = fr->draw_cb;
    if(g_eng.bound_pipeline != pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        g_eng.bound_pipeline = pipe;
    }

    if(dset != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_eng.pipe_layout,
                                0, 1, &dset, 0, NULL);

    PushConstants pc = {};
    fillPushConstants(&pc);
    if(foreground)
        memcpy(pc.foreground, foreground, sizeof(pc.foreground));
    vkCmdPushConstants(cb, g_eng.pipe_layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(pc), &pc);

    vkCmdBindVertexBuffers(cb, 0, 1, &g_eng.ring, &voff);
    vkCmdDraw(cb, (uint32_t)count, 1, 0, 0);
}

void engineBeginFrame()
{
    if(!g_eng.initialized)
        return;
    graphics::VkFrame* fr = graphics::vk_frame();
    g_eng.ring_off = 0;
    g_eng.ring_overflowed = false;
    g_eng.bound_pipeline = VK_NULL_HANDLE;
    g_eng.dset_cache.clear();
    vkResetDescriptorPool(fr->device, g_eng.dpool, 0);

    // negative-height viewport = GL clip-space orientation (core in Vk 1.1)
    VkViewport vp = {};
    vp.x = 0.0f;
    vp.y = (float)fr->extent.height;
    vp.width = (float)fr->extent.width;
    vp.height = -(float)fr->extent.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(fr->draw_cb, 0, 1, &vp);
    VkRect2D sc = {};
    sc.extent = fr->extent;
    vkCmdSetScissor(fr->draw_cb, 0, 1, &sc);
}

} // anonymous namespace

////////////////////////////////////////////////////////////////////////////////
// texture API

DWORD __stdcall gos_NewEmptyTexture(gos_TextureFormat Format, const char* Name,
        DWORD HeightWidth, DWORD Hints, gos_RebuildFunction, void*)
{
    uint32_t w = HeightWidth;
    uint32_t h = HeightWidth;
    if(HeightWidth & 0xffff0000) {
        h = HeightWidth >> 16;
        w = HeightWidth & 0xffff;
    }
    VkStubTexture t = {};
    t.name_ = Name ? Name : "";
    t.format_ = Format;
    t.w_ = w;
    t.h_ = h;
    t.pixels_.resize((size_t)w * h, 0);
    t.alive_ = true;
    t.dirty_ = true;
    return addTexture(std::move(t));
}

DWORD __stdcall gos_NewTextureFromMemory(gos_TextureFormat Format, const char* FileName,
        BYTE* pBitmap, DWORD Size, DWORD Hints, gos_RebuildFunction, void*)
{
    VkStubTexture t = {};
    t.name_ = FileName ? FileName : "";
    t.format_ = Format;
    Image img;
    if(pBitmap && Size && img.loadTGA(pBitmap, Size)) {
        fillPixels(t, img);
    } else {
        t.w_ = 32;
        t.h_ = 32;
        t.pixels_.resize(32 * 32, 0);
    }
    t.alive_ = true;
    t.dirty_ = true;
    return addTexture(std::move(t));
}

DWORD __stdcall gos_NewTextureFromFile(gos_TextureFormat Format, const char* FileName,
        DWORD Hints, gos_RebuildFunction, void*)
{
    VkStubTexture t = {};
    t.name_ = FileName ? FileName : "";
    t.format_ = Format;
    Image img;
    if(FileName && img.loadFromFile(FileName)) {
        fillPixels(t, img);
    } else {
        t.w_ = 32;
        t.h_ = 32;
        t.pixels_.resize(32 * 32, 0);
    }
    t.alive_ = true;
    t.dirty_ = true;
    return addTexture(std::move(t));
}

void __stdcall gos_DestroyTexture(DWORD Handle)
{
    VkStubTexture* t = getTexture(Handle);
    if(!t)
        return;
    t->alive_ = false;
    t->pixels_.clear();
    t->pixels_.shrink_to_fit();
    // GPU objects are destroyed lazily at shutdown (single frame in flight
    // means the image may still be referenced by the in-flight frame)
}

void __stdcall gos_LockTexture(DWORD Handle, DWORD /*MipMapSize*/, bool /*ReadOnly*/,
        TEXTUREPTR* TextureInfo)
{
    gosASSERT(TextureInfo);
    VkStubTexture* t = getTexture(Handle);
    gosASSERT(t);
    if(t->pixels_.empty())
        t->pixels_.resize((size_t)t->w_ * t->h_, 0);
    TextureInfo->pTexture = t->pixels_.data();
    TextureInfo->Width = t->w_;
    TextureInfo->Height = t->h_;
    TextureInfo->Pitch = t->w_; // in DWORDs
    TextureInfo->Type = t->format_;
}

void __stdcall gos_UnLockTexture(DWORD Handle)
{
    VkStubTexture* t = getTexture(Handle);
    if(t)
        t->dirty_ = true;
}

void __stdcall gos_UpdateTexture(DWORD Handle, unsigned char* data, DWORD size)
{
    VkStubTexture* t = getTexture(Handle);
    if(t && data && size <= t->pixels_.size() * sizeof(DWORD)) {
        memcpy(t->pixels_.data(), data, size);
        t->dirty_ = true;
    }
}

void __stdcall gos_ConvertTextureRect(DWORD, DWORD, DWORD, DWORD*, DWORD, DWORD, DWORD)
{
}

bool __stdcall gos_RecreateTextureHeaps() { return false; }
void __stdcall gos_PreloadTexture(DWORD) {}

void __stdcall gos_SetTextureName(DWORD Handle, const char* name)
{
    VkStubTexture* t = getTexture(Handle);
    if(t && name)
        t->name_ = name;
}

const char* __stdcall gos_GetTextureName(DWORD Handle)
{
    VkStubTexture* t = getTexture(Handle);
    return t ? t->name_.c_str() : "";
}

////////////////////////////////////////////////////////////////////////////////
// buffers / vertex declarations (retained path — still CPU bookkeeping only)

class gosBuffer {
public:
    gosBUFFER_TYPE type_;
    gosBUFFER_USAGE usage_;
    int element_size_;
    uint32_t count_;
    std::vector<uint8_t> data_;
};

class gosVertexDeclaration {
public:
    std::vector<gosVERTEX_FORMAT_RECORD> records_;
};

HGOSBUFFER __stdcall gos_CreateBuffer(gosBUFFER_TYPE type, gosBUFFER_USAGE usage,
        int element_size, uint32_t count, void* pdata)
{
    gosBuffer* b = new gosBuffer();
    b->type_ = type;
    b->usage_ = usage;
    b->element_size_ = element_size;
    b->count_ = count;
    b->data_.resize((size_t)element_size * count, 0);
    if(pdata)
        memcpy(b->data_.data(), pdata, b->data_.size());
    return b;
}

void __stdcall gos_DestroyBuffer(HGOSBUFFER buffer)
{
    delete buffer;
}

void __stdcall gos_UpdateBuffer(HGOSBUFFER buffer, void* data, size_t offset, size_t num_bytes)
{
    gosASSERT(buffer);
    if(offset + num_bytes <= buffer->data_.size() && data)
        memcpy(buffer->data_.data() + offset, data, num_bytes);
}

void __stdcall gos_BindBufferBase(HGOSBUFFER, uint32_t) {}

uint32_t gos_GetBufferSizeBytes(HGOSBUFFER buffer)
{
    gosASSERT(buffer);
    return (uint32_t)buffer->data_.size();
}

HGOSVERTEXDECLARATION __stdcall gos_CreateVertexDeclaration(gosVERTEX_FORMAT_RECORD* records, int count)
{
    gosVertexDeclaration* vd = new gosVertexDeclaration();
    vd->records_.assign(records, records + count);
    return vd;
}

void __stdcall gos_DestroyVertexDeclaration(HGOSVERTEXDECLARATION vdecl)
{
    delete vdecl;
}

////////////////////////////////////////////////////////////////////////////////
// render materials (retained mech path — no-ops for now)

class gosRenderMaterial {
public:
    std::string name_;
};

HGOSRENDERMATERIAL __stdcall gos_getRenderMaterial(const char* material)
{
    static std::vector<gosRenderMaterial*> mats;
    for(size_t i = 0; i < mats.size(); ++i)
        if(mats[i]->name_ == material)
            return mats[i];
    gosRenderMaterial* m = new gosRenderMaterial();
    m->name_ = material ? material : "";
    mats.push_back(m);
    return m;
}

void __stdcall gos_ApplyRenderMaterial(HGOSRENDERMATERIAL) {}
void __stdcall gos_SetRenderMaterialParameterFloat4(HGOSRENDERMATERIAL, const char*, const float*) {}
void __stdcall gos_SetRenderMaterialParameterMat4(HGOSRENDERMATERIAL, const char*, const float*) {}
void __stdcall gos_SetRenderMaterialUniformBlockBindingPoint(HGOSRENDERMATERIAL, const char*, uint32_t) {}

////////////////////////////////////////////////////////////////////////////////
// fonts / text — real metrics and real rendering (port of the GL drawText)

class gosFont {
public:
    gosGlyphInfo gi_;
    std::string name_;
    int ref_count_;
    DWORD tex_id_;

    static gosFont* load(const char* fontFile);

    int getMaxCharWidth() const { return gi_.max_advance_; }
    int getMaxCharHeight() const { return gi_.font_line_skip_; }
    int getFontAscent() const { return gi_.font_ascent_; }
    DWORD getTextureId() const { return tex_id_; }

    const gosGlyphMetrics& getGlyphMetrics(int c) const {
        static gosGlyphMetrics dummy = {0, 8, 0, 12, 8, 0, 0, 0};
        if(!gi_.glyphs_)
            return dummy;
        uint32_t idx = (uint32_t)c - gi_.start_glyph_;
        if((uint32_t)c < gi_.start_glyph_ || idx >= gi_.num_glyphs_)
            return dummy;
        return gi_.glyphs_[idx];
    }

    int getCharAdvance(int c) const {
        if(!gi_.glyphs_ || (uint32_t)c < gi_.start_glyph_)
            return gi_.max_advance_;
        uint32_t idx = (uint32_t)c - gi_.start_glyph_;
        if(idx >= gi_.num_glyphs_ || !gi_.glyphs_[idx].valid)
            return gi_.max_advance_;
        return gi_.glyphs_[idx].advance;
    }
};

gosFont* gosFont::load(const char* fontFile)
{
    char fname[256] = {0};
    char dir[256] = {0};
    _splitpath(fontFile, NULL, dir, fname, NULL);

    char glyphName[512] = {0};
    snprintf(glyphName, sizeof(glyphName) - 1, "%s/%s.glyph", dir, fname);
    char textureName[512] = {0};
    snprintf(textureName, sizeof(textureName) - 1, "%s/%s.bmp", dir, fname);

    gosFont* font = new gosFont();
    font->name_ = fontFile ? fontFile : "";
    font->ref_count_ = 1;
    if(!gos_load_glyphs(glyphName, font->gi_)) {
        SPEW(("GRAPHICS", "VK: failed to load glyphs: %s\n", glyphName));
        font->gi_.max_advance_ = 8;
        font->gi_.font_line_skip_ = 12;
        font->gi_.font_ascent_ = 10;
        font->gi_.num_glyphs_ = 0;
        font->gi_.glyphs_ = NULL;
    }
    font->tex_id_ = gos_NewTextureFromFile(gos_Texture_Alpha, textureName, 0, NULL, NULL);
    return font;
}

static std::vector<gosFont*> g_fonts;

struct gosTextAttribs {
    gosFont* FontHandle;
    DWORD Foreground;
    float Size;
    bool WordWrap;
    bool Proportional;
    bool Bold;
    bool Italic;
    DWORD WrapType;
    bool DisableEmbeddedCodes;
};

static gosTextAttribs g_text_attribs = {0};
static int g_text_pos_x = 0;
static int g_text_pos_y = 0;
static int g_text_region[4] = {0}; // left, top, right, bottom

HGOSFONT3D __stdcall gos_LoadFont(const char* FontFile, DWORD /*StartLine*/,
        int /*CharCount*/, DWORD /*TextureHandle*/)
{
    for(size_t i = 0; i < g_fonts.size(); ++i) {
        if(g_fonts[i]->name_ == FontFile) {
            g_fonts[i]->ref_count_++;
            return g_fonts[i];
        }
    }
    gosFont* font = gosFont::load(FontFile);
    g_fonts.push_back(font);
    return font;
}

void __stdcall gos_DeleteFont(HGOSFONT3D FontHandle)
{
    gosFont* font = FontHandle;
    if(!font)
        return;
    font->ref_count_--;
    if(font->ref_count_ <= 0) {
        for(size_t i = 0; i < g_fonts.size(); ++i) {
            if(g_fonts[i] == font) {
                g_fonts.erase(g_fonts.begin() + i);
                break;
            }
        }
        gos_DestroyTexture(font->tex_id_);
        delete[] font->gi_.glyphs_;
        delete font;
    }
}

void __stdcall gos_TextSetAttributes(HGOSFONT3D FontHandle, DWORD Foreground, float Size,
        bool WordWrap, bool Proportional, bool Bold, bool Italic,
        DWORD WrapType, bool DisableEmbeddedCodes)
{
    g_text_attribs.FontHandle = FontHandle;
    g_text_attribs.Foreground = Foreground;
    g_text_attribs.Size = Size;
    g_text_attribs.WordWrap = WordWrap;
    g_text_attribs.Proportional = Proportional;
    g_text_attribs.Bold = Bold;
    g_text_attribs.Italic = Italic;
    g_text_attribs.WrapType = WrapType;
    g_text_attribs.DisableEmbeddedCodes = DisableEmbeddedCodes;
}

void __stdcall gos_TextSetPosition(int XPosition, int YPosition)
{
    g_text_pos_x = XPosition;
    g_text_pos_y = YPosition;
}

void __stdcall gos_TextGetPrintPosition(int* XPosition, int* YPosition)
{
    if(XPosition) *XPosition = g_text_pos_x;
    if(YPosition) *YPosition = g_text_pos_y;
}

void __stdcall gos_TextSetRegion(int Left, int Top, int Right, int Bottom)
{
    g_text_region[0] = Left;
    g_text_region[1] = Top;
    g_text_region[2] = Right;
    g_text_region[3] = Bottom;
}

static int get_next_break(const char* text)
{
    const char* start = text;
    do {
        char c = *text;
        if(c == ' ' || c == '\n')
            return (int32_t)(text - start);
    } while(*text++);
    return (int32_t)(text - start - 1);
}

static int findTextBreak(const char* text, const gosFont* font, const int region_width,
                         int* out_str_width)
{
    int width = 0;
    int pos = 0;
    int space_adv = font->getCharAdvance(' ');

    while(text[pos]) {
        int break_pos = get_next_break(text + pos);
        int cur_width = 0;
        for(int j = 0; j < break_pos; ++j)
            cur_width += font->getCharAdvance(text[pos + j]);

        if(width + cur_width >= region_width) {
            if(pos == 0) {
                width = cur_width;
                pos = break_pos;
            }
            break;
        } else {
            width += cur_width;
            pos += break_pos;
            if(text[pos] == '\n') {
                pos++;
                break;
            }
            if(text[pos] == ' ') {
                width += space_adv;
                pos++;
            }
        }
    }

    if(out_str_width)
        *out_str_width = width;
    return pos;
}

static int calcNumTextLines(const char* text, int count, const gosFont* font, int region_width)
{
    int pos = 0;
    int num_lines = 0;
    while(pos < count) {
        int num_chars = findTextBreak(text + pos, font, region_width, NULL);
        if(num_chars == 0)
            break;
        pos += num_chars;
        num_lines++;
    }
    return num_lines;
}

void __stdcall gos_TextStringLength(DWORD* Width, DWORD* Height, const char* fmt, ...)
{
    gosASSERT(Width && Height);
    *Width = 1;
    *Height = 1;
    if(!fmt)
        return;

    const int MAX_TEXT_LEN = 4096;
    char text[MAX_TEXT_LEN] = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, MAX_TEXT_LEN - 1, fmt, ap);
    va_end(ap);

    const gosFont* font = g_text_attribs.FontHandle;
    const int line_h = font ? font->getMaxCharHeight() : 12;
    int max_width = 0;
    int cur_width = 0;
    int num_lines = 1;
    for(const char* p = text; *p; ++p) {
        if(*p == '\n') {
            num_lines++;
            cur_width = 0;
            continue;
        }
        cur_width += font ? font->getCharAdvance((unsigned char)*p) : 8;
        if(cur_width > max_width)
            max_width = cur_width;
    }
    *Width = max_width > 0 ? max_width : 1;
    *Height = num_lines * line_h;
}

static void addCharQuad(std::vector<gos_VERTEX>& out, float u, float v, float u2, float v2,
                        float x, float y, float x2, float y2)
{
    gos_VERTEX tl = {}, tr = {}, bl = {}, br = {};
    tl.x = x;  tl.y = y;  tl.u = u;  tl.v = v;
    tr.x = x2; tr.y = y;  tr.u = u2; tr.v = v;
    bl.x = x;  bl.y = y2; bl.u = u;  bl.v = v2;
    br.x = x2; br.y = y2; br.u = u2; br.v = v2;
    tl.rhw = tr.rhw = bl.rhw = br.rhw = 1.0f;
    tl.argb = tr.argb = bl.argb = br.argb = 0xffffffff;
    tl.frgb = tr.frgb = bl.frgb = br.frgb = 0xff000000;
    out.push_back(tl); out.push_back(tr); out.push_back(bl);
    out.push_back(tr); out.push_back(br); out.push_back(bl);
}

static void drawTextInternal(const char* text)
{
    const gosFont* font = g_text_attribs.FontHandle;
    if(!font || !text || !*text)
        return;
    VkStubTexture* tex = getTexture(font->getTextureId());
    if(!tex || !tex->alive_)
        return;

    const int count = (int)strlen(text);
    const float oo_tex_w = 1.0f / (float)tex->w_;
    const float oo_tex_h = 1.0f / (float)tex->h_;
    const int font_height = font->getMaxCharHeight();
    const int font_ascent = font->getFontAscent();
    const int region_width = g_text_region[2] - g_text_region[0];
    const int region_height = g_text_region[3] - g_text_region[1];

    float x = (float)g_text_pos_x;
    float y = (float)g_text_pos_y;
    const float start_x = x;

    const int num_lines = calcNumTextLines(text, count, font, region_width);
    if(g_text_attribs.WrapType == 3)
        y += (region_height - num_lines * font_height) / 2;

    std::vector<gos_VERTEX> verts;
    verts.reserve(count * 6);

    int pos = 0;
    int str_width = 0;
    while(pos < count) {
        x = start_x;
        int num_chars = findTextBreak(text + pos, font, region_width, &str_width);
        if(num_chars == 0)
            break;

        switch(g_text_attribs.WrapType) {
            case 1: x += region_width - str_width; break;
            case 2:
            case 3: x += (region_width - str_width) / 2; break;
            default: break;
        }

        for(int i = 0; i < num_chars; ++i) {
            const char c = text[i + pos];
            const gosGlyphMetrics& gm = font->getGlyphMetrics(c);
            int char_off_x = gm.minx;
            int char_off_y = font_ascent - gm.maxy;
            int char_w = gm.maxx - gm.minx;
            int char_h = gm.maxy - gm.miny;

            uint32_t iu0 = gm.u + char_off_x;
            uint32_t iv0 = gm.v + char_off_y;

            float u0 = (float)iu0 * oo_tex_w;
            float v0 = (float)iv0 * oo_tex_h;
            float u1 = (float)(iu0 + char_w) * oo_tex_w;
            float v1 = (float)(iv0 + char_h) * oo_tex_h;

            addCharQuad(verts, u0, v0, u1, v1,
                        x + char_off_x, y + char_off_y,
                        x + char_off_x + char_w, y + char_off_y + char_h);
            x += font->getCharAdvance(c);
        }
        y += font_height;
        pos += num_chars;
    }

    if(verts.empty())
        return;

    // like GL: temporarily force the font texture + nearest filtering
    const int prev_texture = g_render_states[gos_State_Texture];
    const int prev_filter = g_render_states[gos_State_Filter];
    g_render_states[gos_State_Texture] = (int)font->getTextureId();
    g_render_states[gos_State_Filter] = gos_FilterNone;

    float fg[4];
    fg[0] = (float)((g_text_attribs.Foreground & 0xFF0000) >> 16) / 255.0f;
    fg[1] = (float)((g_text_attribs.Foreground & 0xFF00) >> 8) / 255.0f;
    fg[2] = (float)(g_text_attribs.Foreground & 0xFF) / 255.0f;
    fg[3] = 1.0f;

    emitDraw(SHADER_TEXT, TOPO_TRIS, verts.data(), (int)verts.size(), fg);

    g_render_states[gos_State_Texture] = prev_texture;
    g_render_states[gos_State_Filter] = prev_filter;
}

void __stdcall gos_TextDraw(const char* Message, ...)
{
    if(!Message)
        return;
    const int MAX_TEXT_LEN = 4096;
    char text[MAX_TEXT_LEN] = {0};
    va_list ap;
    va_start(ap, Message);
    vsnprintf(text, MAX_TEXT_LEN - 1, Message, ap);
    va_end(ap);
    drawTextInternal(text);
}

void __stdcall gos_TextDrawBackground(int Left, int Top, int Right, int Bottom, DWORD Color)
{
    gos_VERTEX v[6] = {};
    const float x0 = (float)Left, y0 = (float)Top;
    const float x1 = (float)Right, y1 = (float)Bottom;
    for(int i = 0; i < 6; ++i) {
        v[i].z = 0.0f;
        v[i].rhw = 1.0f;
        v[i].argb = Color;
        v[i].frgb = 0xff000000;
    }
    v[0].x = x0; v[0].y = y0;
    v[1].x = x1; v[1].y = y0;
    v[2].x = x0; v[2].y = y1;
    v[3].x = x1; v[3].y = y0;
    v[4].x = x1; v[4].y = y1;
    v[5].x = x0; v[5].y = y1;
    emitDraw(SHADER_VERTEX, TOPO_TRIS, v, 6, NULL);
}

////////////////////////////////////////////////////////////////////////////////
// render states / viewport / screen mode API

void __stdcall gos_SetRenderState(gos_RenderState RenderState, int Value)
{
    gosASSERT(RenderState < NUM_RENDER_STATES);
    g_render_states[RenderState] = Value;
}

void __stdcall gos_PushRenderStates()
{
    g_state_stack.push_back(std::vector<int>(g_render_states, g_render_states + NUM_RENDER_STATES));
}

void __stdcall gos_PopRenderStates()
{
    if(!g_state_stack.empty()) {
        memcpy(g_render_states, g_state_stack.back().data(), sizeof(g_render_states));
        g_state_stack.pop_back();
    }
}

void __stdcall gos_SetScreenMode(DWORD Width, DWORD Height, DWORD /*bitDepth*/,
        DWORD /*Device*/, bool /*disableZBuffer*/, bool /*AntiAlias*/, bool /*RenderToVram*/,
        bool GotoFullScreen, int /*DirtyRectangle*/, bool GotoWindowMode,
        bool /*EnableStencil*/, DWORD /*Renderer*/)
{
    g_req_width = Width;
    g_req_height = Height;
    g_req_fullscreen = GotoFullScreen || (Environment.fullScreen && !GotoWindowMode);
    g_pending_resize = true;
}

void __stdcall gos_SetupViewport(bool /*FillZ*/, float /*ZBuffer*/, bool /*FillBG*/,
        DWORD /*BGColor*/, float top, float left, float bottom, float right,
        bool /*ClearStencil*/, DWORD /*StencilValue*/)
{
    g_viewport[0] = top;
    g_viewport[1] = left;
    g_viewport[2] = bottom;
    g_viewport[3] = right;
}

void __stdcall gos_GetViewport(float* pViewportMulX, float* pViewportMulY,
        float* pViewportAddX, float* pViewportAddY)
{
    gosASSERT(pViewportMulX && pViewportMulY && pViewportAddX && pViewportAddY);
    *pViewportMulX = (g_viewport[3] - g_viewport[1]) * g_width;
    *pViewportMulY = (g_viewport[2] - g_viewport[0]) * g_height;
    *pViewportAddX = g_viewport[1] * g_width;
    *pViewportAddY = g_viewport[0] * g_height;
}

void __stdcall gos_SetRenderViewport(float x, float y, float w, float h)
{
    g_render_viewport = vec4(x, y, w, h);
}

vec4 __stdcall gos_GetRenderViewport()
{
    return g_render_viewport;
}

const mat4& __stdcall gos_GetProjection()
{
    return g_projection;
}

////////////////////////////////////////////////////////////////////////////////
// immediate-mode draw API

void _stdcall gos_DrawPoints(gos_VERTEX* Vertices, int NumVertices)
{
    emitDraw(g_render_states[gos_State_Texture] ? SHADER_TEX_VERTEX : SHADER_VERTEX,
             TOPO_POINTS, Vertices, NumVertices, NULL);
}

void _stdcall gos_DrawLines(gos_VERTEX* Vertices, int NumVertices)
{
    emitDraw(g_render_states[gos_State_Texture] ? SHADER_TEX_VERTEX : SHADER_VERTEX,
             TOPO_LINES, Vertices, NumVertices, NULL);
}

void _stdcall gos_DrawQuads(gos_VERTEX* Vertices, int NumVertices)
{
    if(g_disable_quads || !Vertices)
        return;
    // expand quads to triangle lists, same as the GL path
    const int num_quads = NumVertices / 4;
    if(num_quads <= 0)
        return;
    std::vector<gos_VERTEX> tris;
    tris.reserve((size_t)num_quads * 6);
    for(int q = 0; q < num_quads; ++q) {
        const gos_VERTEX* v = Vertices + q * 4;
        tris.push_back(v[0]); tris.push_back(v[1]); tris.push_back(v[2]);
        tris.push_back(v[0]); tris.push_back(v[2]); tris.push_back(v[3]);
    }
    emitDraw(g_render_states[gos_State_Texture] ? SHADER_TEX_VERTEX : SHADER_VERTEX,
             TOPO_TRIS, tris.data(), (int)tris.size(), NULL);
}

void _stdcall gos_DrawTriangles(gos_VERTEX* Vertices, int NumVertices)
{
    emitDraw(g_render_states[gos_State_Texture] ? SHADER_TEX_VERTEX : SHADER_VERTEX,
             TOPO_TRIS, Vertices, NumVertices, NULL);
}

void __stdcall gos_DrawStrips(gos_VERTEX*, int) {}
void __stdcall gos_DrawFans(gos_VERTEX*, int) {}

void __stdcall gos_RenderIndexedArray(gos_VERTEX* pVertexArray, DWORD NumberVertices,
        WORD* lpwIndices, DWORD NumberIndices)
{
    if(!pVertexArray || !lpwIndices || NumberIndices == 0)
        return;
    // expand indices CPU-side (scaffold; index draws come with retained path)
    std::vector<gos_VERTEX> tris;
    tris.reserve(NumberIndices);
    for(DWORD i = 0; i < NumberIndices; ++i) {
        if(lpwIndices[i] < NumberVertices)
            tris.push_back(pVertexArray[lpwIndices[i]]);
    }
    emitDraw(g_render_states[gos_State_Texture] ? SHADER_TEX_VERTEX : SHADER_VERTEX,
             TOPO_TRIS, tris.data(), (int)tris.size(), NULL);
}

void __stdcall gos_RenderIndexedArray(gos_VERTEX_2UV*, DWORD, WORD*, DWORD) {}
void __stdcall gos_RenderIndexedArray(HGOSBUFFER, HGOSBUFFER, HGOSVERTEXDECLARATION, const float*) {}
void __stdcall gos_RenderIndexedArray(HGOSBUFFER, HGOSBUFFER, HGOSVERTEXDECLARATION) {}

////////////////////////////////////////////////////////////////////////////////
// renderer lifecycle — mirrors the GL path's structure

void gos_DestroyWindow()
{
    graphics::destroy_window(g_win_h);
    g_win_h = NULL;
}

HGOSWINDOW gos_CreateWindow(int w, int h, int bpp, int display)
{
    g_win_h = graphics::create_window("mc2", w, h, bpp, display);
    if(g_win_h) {
        Environment.displayIndex = display;
        graphics::get_window_size(g_win_h, &Environment.screenWidth, &Environment.screenHeight);
    }
    return g_win_h;
}

HGOSWINDOW gos_GetWindow()
{
    return g_win_h;
}

static graphics::RenderContextHandle g_render_ctx = NULL;

static void updateProjection()
{
    g_projection = mat4(2.0f / (float)g_width, 0, 0.0f, -1.0f,
            0, -2.0f / (float)g_height, 0.0f, 1.0f,
            0, 0, 1.0f, 0.0f,
            0, 0, 0.0f, 1.0f);
}

// non-null token so `if(gos_GetRenderer())` guards in game code behave the
// same as on the GL path; nothing dereferences it across the API boundary
gosRenderer* gos_GetRenderer()
{
    return (gosRenderer*)(g_render_ctx ? (void*)1 : NULL);
}

bool gos_CreateRenderer(HGOSWINDOW win, int w, int h)
{
    gosASSERT(win);
    g_render_ctx = graphics::init_render_context(win);
    if(!g_render_ctx)
        return false;
    g_width = w;
    g_height = h;
    updateProjection();
    return true;
}

graphics::RenderContextHandle gos_GetRenderContext()
{
    return g_render_ctx;
}

void gos_DestroyRenderer()
{
    if(g_render_ctx) {
        graphics::destroy_render_context(g_render_ctx);
        g_render_ctx = NULL;
    }
}

void gos_RendererBeginFrame()
{
    g_disable_quads = false;
    if(graphics::vk_begin_frame()) {
        engineInit();
        engineBeginFrame();
    }
}

void gos_RendererEndFrame()
{
}

void gos_RendererHandleEvents()
{
    if(g_pending_resize) {
        g_width = g_req_width;
        g_height = g_req_height;
        updateProjection();
        if(graphics::resize_window(g_win_h, g_width, g_height)) {
            graphics::set_window_fullscreen(g_win_h, g_req_fullscreen);
            Environment.screenWidth = g_width;
            Environment.screenHeight = g_height;
            graphics::get_drawable_size(g_win_h, &Environment.drawableWidth,
                                        &Environment.drawableHeight);
        }
        g_pending_resize = false;
    }
}

void gos_RenderUpdateDebugInput() {}
void gos_RenderEnableDebugDrawCalls() {}
bool gos_RenderGetEnableDebugDrawCalls() { return false; }

////////////////////////////////////////////////////////////////////////////////
// misc queries

size_t __stdcall gos_GetMachineInformation(MachineInfo mi, int /*Param1*/,
        int Param2, int Param3, int Param4)
{
    if(mi == gos_Info_GetDeviceLocalMemory)
        return 1024 * 1024 * 1024;
    if(mi == gos_Info_GetDeviceAGPMemory)
        return 512 * 1024 * 1024;
    if(mi == gos_Info_CanMultitextureDetail)
        return true;
    if(mi == gos_Info_NumberDevices)
        return 1;
    if(mi == gos_Info_GetDeviceName)
        return (size_t)"Vulkan (MoltenVK)";
    if(mi == gos_Info_ValidMode)
        return graphics::is_mode_supported(Param2, Param3, Param4) ? 1 : 0;
    if(mi == gos_Info_GetIMECaretStatus)
        return 1;
    return 0;
}

int gos_GetWindowDisplayIndex()
{
    return graphics::get_window_display_index(g_render_ctx);
}

int gos_GetNumDisplayModes(int DisplayIndex)
{
    return graphics::get_num_display_modes(DisplayIndex);
}

bool gos_GetDisplayModeByIndex(int DisplayIndex, int ModeIndex, int* XRes, int* YRes, int* BitDepth)
{
    return graphics::get_display_mode_by_index(DisplayIndex, ModeIndex, XRes, YRes, BitDepth);
}

void __stdcall gos_StartRenderToTexture(DWORD /*Handle*/) {}
void __stdcall gos_EndRenderToTexture(bool /*ClearBorder*/) {}
bool __stdcall gos_VertexBuffersLost(DWORD /*VertexBufferHandle*/) { return false; }
