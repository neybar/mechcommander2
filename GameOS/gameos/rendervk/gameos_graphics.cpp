// Vulkan-backend implementation of the gos_* graphics API (M2).
// Renders the immediate-mode path (quads/tris/lines/points + text) and the
// retained path (mech meshes via the lighted materials + UBOs, FMV YCbCr).
// SPIR-V pipelines are keyed on render state + vertex layout; a per-frame
// host-visible ring holds immediate vertices and texture staging; retained
// vertex/index/uniform buffers are host-visible VkBuffers owned by gosBuffer.
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

static char g_last_draw_desc[256]; // MC2_VK_DEBUG: last quad draw of the frame
static const bool g_vk_debug = getenv("MC2_VK_DEBUG") != NULL;

////////////////////////////////////////////////////////////////////////////////
// textures

struct VkStubTexture {
    std::string name_;
    gos_TextureFormat format_;
    uint32_t w_;
    uint32_t h_;
    std::vector<DWORD> pixels_; // 8888 (or w*h bytes for Luminance)
    bool alive_;
    bool dirty_;                // CPU pixels newer than GPU image
    bool lock_read_only_;

    VkImage image_;
    VkDeviceMemory memory_;
    VkImageView view_;

    bool isLuminance() const { return format_ == gos_Texture_Luminance; }
};

static std::vector<VkStubTexture> g_textures;

// GPU objects that may still be referenced by the in-flight frame are
// destroyed at the next vk_begin_frame (after the frame fence has been waited)
struct DeferredImage { VkImage image; VkDeviceMemory memory; VkImageView view; };
struct DeferredBuffer { VkBuffer buffer; VkDeviceMemory memory; };
static std::vector<DeferredImage> g_deferred_images;
static std::vector<DeferredBuffer> g_deferred_buffers;

static void deferDestroyTextureGpu(VkStubTexture* t)
{
    if(t->image_ != VK_NULL_HANDLE) {
        DeferredImage d = { t->image_, t->memory_, t->view_ };
        g_deferred_images.push_back(d);
        t->image_ = VK_NULL_HANDLE;
        t->memory_ = VK_NULL_HANDLE;
        t->view_ = VK_NULL_HANDLE;
    }
}

static DWORD addTexture(VkStubTexture&& t)
{
    // NEVER reuse slots: the game's texture cache (txmmgr) keeps stale
    // handles around after cache-out, and the GL path's handles are
    // append-only — reusing a slot aliases two logically distinct textures
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
// buffers / vertex declarations (retained path)

class gosBuffer {
public:
    gosBUFFER_TYPE type_;
    gosBUFFER_USAGE usage_;
    int element_size_;
    uint32_t count_;
    std::vector<uint8_t> data_;

    VkBuffer vk_buffer_;
    VkDeviceMemory vk_memory_;
    uint8_t* mapped_;
    VkDeviceSize vk_size_;

    void ensureGpu();
    void writeGpu(size_t offset, const void* src, size_t bytes);
};

class gosVertexDeclaration {
public:
    std::vector<gosVERTEX_FORMAT_RECORD> records_;
};

////////////////////////////////////////////////////////////////////////////////
// render materials

enum MaterialKind {
    MAT_UNKNOWN = 0,
    MAT_VERTEX_LIGHTED,      // "gos_vertex_lighted" (plain mvp shader in GL too)
    MAT_TEX_VERTEX_LIGHTED,  // "gos_tex_vertex_lighted" (mech/object path)
    MAT_YCBCR,               // "gos_YCbCr" (FMV)
};

class gosRenderMaterial {
public:
    std::string name_;
    MaterialKind kind_;
    // named parameters, mirroring the GL loose uniforms we care about
    mat4 wvp_;
    mat4 world_;
    mat4 projection_;
    vec4 light_offset_;
    vec4 vp_;
    vec4 texture_crop_size_;
    vec4 scale_offset_;
};

static gosRenderMaterial* g_current_material = NULL;
static gosBuffer* g_ubo_slots[2] = { NULL, NULL }; // 0=LightsData, 1=SceneData

////////////////////////////////////////////////////////////////////////////////
// Vulkan draw machinery

namespace {

struct PushConstants {
    float m0[16];
    float m1[16];
    float m2[16];
    float v0[4];
    float v1[4];
    uint32_t flags;
    uint32_t pad_[3];
};

enum ShaderKind {
    SHADER_VERTEX = 0,
    SHADER_TEX_VERTEX,
    SHADER_TEXT,
    SHADER_VERTEX_LIGHTED,
    SHADER_TEX_VERTEX_LIGHTED,
    SHADER_YCBCR,
    SHADER_KIND_COUNT
};
enum TopoKind { TOPO_TRIS = 0, TOPO_LINES = 1, TOPO_POINTS = 2, TOPO_COUNT };

struct ShaderPair { VkShaderModule vs, fs; };

struct PipelineKey {
    uint32_t state_bits;
    const gosVertexDeclaration* vdecl; // NULL = gos_VERTEX layout
    bool operator<(const PipelineKey& o) const {
        if(state_bits != o.state_bits) return state_bits < o.state_bits;
        return vdecl < o.vdecl;
    }
};

struct VkDrawEngine {
    bool initialized;
    bool init_failed;

    ShaderPair shaders[SHADER_KIND_COUNT];

    VkDescriptorSetLayout dset_layout;
    VkPipelineLayout pipe_layout;
    std::map<PipelineKey, VkPipeline> pipelines;

    VkSampler samplers[4]; // [filter linear?][address clamp?]

    // per-frame vertex/staging ring (single frame in flight)
    VkBuffer ring;
    VkDeviceMemory ring_mem;
    uint8_t* ring_ptr;
    VkDeviceSize ring_size;
    VkDeviceSize ring_off;
    bool ring_overflowed;

    VkDescriptorPool dpool;
    // keyed on the exact binding tuple — a folded hash collided under
    // texture churn and silently bound the wrong texture (see ENGINEERING_LOG)
    struct DsetKey {
        VkSampler sampler;
        VkImageView views[3];
        VkBuffer ubos[2];
        bool operator<(const DsetKey& o) const {
            return memcmp(this, &o, sizeof(DsetKey)) < 0;
        }
    };
    std::map<DsetKey, VkDescriptorSet> dset_cache;

    VkPipeline bound_pipeline;

    // fallbacks for descriptor bindings a draw doesn't use
    VkImage dummy_image;
    VkDeviceMemory dummy_image_mem;
    VkImageView dummy_view;
    bool dummy_uploaded;
    VkBuffer dummy_ubo;
    VkDeviceMemory dummy_ubo_mem;
};

VkDrawEngine g_eng = {};

// dummy UBO must cover the largest std140 block the shaders declare
// (ObjectLights light[32] — see shaders/vk/lighting.glsl)
static const VkDeviceSize DUMMY_UBO_SIZE = 64 * 1024;

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

bool loadShaderPair(VkDevice dev, ShaderPair* out, const char* vs_name, const char* fs_name)
{
    char path[512];
    snprintf(path, sizeof(path), "shaders/vk/%s.spv", vs_name);
    out->vs = loadShaderModule(dev, path);
    snprintf(path, sizeof(path), "shaders/vk/%s.spv", fs_name);
    out->fs = loadShaderModule(dev, path);
    return out->vs && out->fs;
}

void createHostBuffer(VkDevice dev, VkDeviceSize size, VkBufferUsageFlags usage,
                      VkBuffer* buf, VkDeviceMemory* mem, uint8_t** mapped)
{
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(dev, &bci, NULL, buf);
    VkMemoryRequirements mreq;
    vkGetBufferMemoryRequirements(dev, *buf, &mreq);
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mreq.size;
    mai.memoryTypeIndex = graphics::vk_find_memory_type(mreq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(dev, &mai, NULL, mem);
    vkBindBufferMemory(dev, *buf, *mem, 0);
    if(mapped)
        vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, (void**)mapped);
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

    bool ok = true;
    ok &= loadShaderPair(dev, &g_eng.shaders[SHADER_VERTEX], "gos_vertex.vert", "gos_vertex.frag");
    ok &= loadShaderPair(dev, &g_eng.shaders[SHADER_TEX_VERTEX], "gos_vertex.vert", "gos_tex_vertex.frag");
    ok &= loadShaderPair(dev, &g_eng.shaders[SHADER_TEXT], "gos_text.vert", "gos_text.frag");
    ok &= loadShaderPair(dev, &g_eng.shaders[SHADER_VERTEX_LIGHTED], "gos_vertex_lighted.vert", "gos_vertex_lighted.frag");
    ok &= loadShaderPair(dev, &g_eng.shaders[SHADER_TEX_VERTEX_LIGHTED], "gos_tex_vertex_lighted.vert", "gos_tex_vertex_lighted.frag");
    ok &= loadShaderPair(dev, &g_eng.shaders[SHADER_YCBCR], "gos_YCbCr.vert", "gos_YCbCr.frag");
    if(!ok) {
        g_eng.init_failed = true; // draws become no-ops; frame still clears
        return false;
    }

    // one layout for everything: 3 samplers + lights UBO + scene UBO
    VkDescriptorSetLayoutBinding binds[5] = {};
    for(int i = 0; i < 3; ++i) {
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for(int i = 3; i < 5; ++i) {
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci = {};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 5;
    dlci.pBindings = binds;
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

    // 16 MB host-visible ring: immediate vertex data + texture staging
    g_eng.ring_size = 16u * 1024 * 1024;
    createHostBuffer(dev, g_eng.ring_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
          | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            &g_eng.ring, &g_eng.ring_mem, &g_eng.ring_ptr);

    VkDescriptorPoolSize dps[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * 4096 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * 4096 },
    };
    VkDescriptorPoolCreateInfo dpci = {};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 4096;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = dps;
    vkCreateDescriptorPool(dev, &dpci, NULL, &g_eng.dpool);

    // dummy resources for unused bindings
    {
        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent.width = 1;
        ici.extent.height = 1;
        ici.extent.depth = 1;
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(dev, &ici, NULL, &g_eng.dummy_image);
        VkMemoryRequirements mreq;
        vkGetImageMemoryRequirements(dev, g_eng.dummy_image, &mreq);
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mreq.size;
        mai.memoryTypeIndex = graphics::vk_find_memory_type(mreq.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev, &mai, NULL, &g_eng.dummy_image_mem);
        vkBindImageMemory(dev, g_eng.dummy_image, g_eng.dummy_image_mem, 0);
        VkImageViewCreateInfo vci = {};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = g_eng.dummy_image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        vkCreateImageView(dev, &vci, NULL, &g_eng.dummy_view);
        g_eng.dummy_uploaded = false;
    }
    createHostBuffer(dev, DUMMY_UBO_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            &g_eng.dummy_ubo, &g_eng.dummy_ubo_mem, NULL);

    g_eng.initialized = true;
    return true;
}

// state bits shared by every pipeline: blend, depth, cull (+ shader/topology)
uint32_t stateBits(ShaderKind sh, TopoKind topo)
{
    uint32_t blend = g_render_states[gos_State_AlphaMode];      // 0..4
    uint32_t zcomp = g_render_states[gos_State_ZCompare];       // 0..2
    uint32_t zwrite = g_render_states[gos_State_ZWrite] ? 1 : 0;
    uint32_t cull = g_render_states[gos_State_Culling];
    if(cull >= 1) cull -= 1;                                    // gos_Cull_None=1
    return (uint32_t)sh | ((uint32_t)topo << 4) | (blend << 7) | (zcomp << 11)
         | (zwrite << 14) | (cull << 15);
}

VkFormat attribFormat(const gosVERTEX_FORMAT_RECORD& r)
{
    if(r.type == gosVERTEX_ATTRIB_TYPE::FLOAT) {
        switch(r.num_components) {
            case 1: return VK_FORMAT_R32_SFLOAT;
            case 2: return VK_FORMAT_R32G32_SFLOAT;
            case 3: return VK_FORMAT_R32G32B32_SFLOAT;
            case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
        }
    }
    if(r.type == gosVERTEX_ATTRIB_TYPE::UNSIGNED_BYTE) {
        if(r.num_components == 4)
            return r.normalized ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_UINT;
    }
    if(r.type == gosVERTEX_ATTRIB_TYPE::UNSIGNED_SHORT) {
        if(r.num_components == 2)
            return r.normalized ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R16G16_UINT;
        if(r.num_components == 4)
            return r.normalized ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R16G16B16A16_UINT;
    }
    SPEW(("GRAPHICS", "VK: unsupported vertex attrib (type %d x%d)\n",
          (int)r.type, r.num_components));
    return VK_FORMAT_R32_SFLOAT;
}

VkPipeline getPipeline(ShaderKind sh, TopoKind topo, const gosVertexDeclaration* vdecl)
{
    PipelineKey key = { stateBits(sh, topo), vdecl };
    std::map<PipelineKey, VkPipeline>::iterator it = g_eng.pipelines.find(key);
    if(it != g_eng.pipelines.end())
        return it->second;

    graphics::VkFrame* fr = graphics::vk_frame();

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g_eng.shaders[sh].vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g_eng.shaders[sh].fs;
    stages[1].pName = "main";

    // vertex input: gos_VERTEX for the immediate path, else from the vdecl
    VkVertexInputBindingDescription bind = { 0, sizeof(gos_VERTEX), VK_VERTEX_INPUT_RATE_VERTEX };
    std::vector<VkVertexInputAttributeDescription> attrs;
    if(!vdecl) {
        VkVertexInputAttributeDescription a[4] = {
            { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
            { 1, 0, VK_FORMAT_R8G8B8A8_UNORM, 16 },
            { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, 20 },
            { 3, 0, VK_FORMAT_R32G32_SFLOAT, 24 },
        };
        attrs.assign(a, a + 4);
    } else {
        bind.stride = vdecl->records_.empty() ? 0 : (uint32_t)vdecl->records_[0].stride;
        for(size_t i = 0; i < vdecl->records_.size(); ++i) {
            const gosVERTEX_FORMAT_RECORD& r = vdecl->records_[i];
            VkVertexInputAttributeDescription a = {};
            a.location = (uint32_t)r.index;
            a.binding = 0;
            a.format = attribFormat(r);
            a.offset = (uint32_t)r.offset;
            attrs.push_back(a);
        }
    }
    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size();
    vi.pVertexAttributeDescriptions = attrs.data();

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
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
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
    cba.srcAlphaBlendFactor = cba.srcColorBlendFactor;
    cba.dstAlphaBlendFactor = cba.dstColorBlendFactor;
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
        SPEW(("GRAPHICS", "VK: pipeline creation failed (bits %x)\n", key.state_bits));
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
            if(g_vk_debug)
                printf("[VKDBG] ring overflow at %llu bytes, draws dropped this frame\n",
                       (unsigned long long)off);
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

    const VkFormat fmt = t->isLuminance() ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize bytes = (VkDeviceSize)t->w_ * t->h_ * (t->isLuminance() ? 1 : 4);

    if(t->image_ == VK_NULL_HANDLE) {
        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
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
        vci.format = fmt;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        vkCreateImageView(dev, &vci, NULL, &t->view_);
        t->dirty_ = true;
    }

    if(t->dirty_) {
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

void ensureDummyUploaded()
{
    if(g_eng.dummy_uploaded)
        return;
    graphics::VkFrame* fr = graphics::vk_frame();
    VkDeviceSize off = 0;
    uint8_t* dst = ringAlloc(4, 16, &off);
    if(!dst)
        return;
    dst[0] = dst[1] = dst[2] = dst[3] = 0xff; // white

    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    VkImageMemoryBarrier to_dst = {};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = g_eng.dummy_image;
    to_dst.subresourceRange = range;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(fr->upload_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
    VkBufferImageCopy bic = {};
    bic.bufferOffset = off;
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = 1;
    bic.imageExtent.height = 1;
    bic.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(fr->upload_cb, g_eng.ring, g_eng.dummy_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    VkImageMemoryBarrier to_read = to_dst;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(fr->upload_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &to_read);
    g_eng.dummy_uploaded = true;
}

VkSampler samplerFromStates()
{
    const int filt = (g_render_states[gos_State_Filter] != gos_FilterNone) ? 1 : 0;
    const int addr = (g_render_states[gos_State_TextureAddress] == gos_TextureClamp) ? 1 : 0;
    return g_eng.samplers[filt * 2 + addr];
}

// full descriptor set: 3 texture views (dummy where absent) + 2 UBOs
VkDescriptorSet descriptorFor(VkImageView views[3], VkSampler sampler)
{
    ensureDummyUploaded();

    VkBuffer ubo0 = (g_ubo_slots[0] && g_ubo_slots[0]->vk_buffer_) ? g_ubo_slots[0]->vk_buffer_ : g_eng.dummy_ubo;
    VkBuffer ubo1 = (g_ubo_slots[1] && g_ubo_slots[1]->vk_buffer_) ? g_ubo_slots[1]->vk_buffer_ : g_eng.dummy_ubo;

    VkDrawEngine::DsetKey key = {};
    key.sampler = sampler;
    for(int i = 0; i < 3; ++i)
        key.views[i] = views[i] ? views[i] : g_eng.dummy_view;
    key.ubos[0] = ubo0;
    key.ubos[1] = ubo1;

    static const bool no_cache = getenv("MC2_VK_NO_DSET_CACHE") != NULL;
    std::map<VkDrawEngine::DsetKey, VkDescriptorSet>::iterator it = g_eng.dset_cache.find(key);
    if(!no_cache && it != g_eng.dset_cache.end())
        return it->second;

    graphics::VkFrame* fr = graphics::vk_frame();
    VkDescriptorSetAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = g_eng.dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_eng.dset_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if(vkAllocateDescriptorSets(fr->device, &ai, &set) != VK_SUCCESS) {
        static int spewed = 0;
        if(g_vk_debug && spewed < 40) {
            printf("[VKDBG] descriptor pool exhausted (%zu sets cached), draw dropped\n",
                   g_eng.dset_cache.size());
            spewed++;
        }
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo dii[3];
    VkWriteDescriptorSet w[5] = {};
    for(int i = 0; i < 3; ++i) {
        dii[i].sampler = sampler;
        dii[i].imageView = views[i] ? views[i] : g_eng.dummy_view;
        dii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].descriptorCount = 1;
        w[i].pImageInfo = &dii[i];
    }
    VkDescriptorBufferInfo dbi[2];
    dbi[0].buffer = ubo0;
    dbi[0].offset = 0;
    dbi[0].range = VK_WHOLE_SIZE;
    dbi[1].buffer = ubo1;
    dbi[1].offset = 0;
    dbi[1].range = VK_WHOLE_SIZE;
    for(int i = 0; i < 2; ++i) {
        w[3 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[3 + i].dstSet = set;
        w[3 + i].dstBinding = 3 + i;
        w[3 + i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[3 + i].descriptorCount = 1;
        w[3 + i].pBufferInfo = &dbi[i];
    }
    vkUpdateDescriptorSets(fr->device, 5, w, 0, NULL);

    g_eng.dset_cache[key] = set;
    return set;
}

void matToFloats(const mat4& m, float* out /*column-major*/)
{
    const float* p = (const float*)m; // row-major storage
    for(int r = 0; r < 4; ++r)
        for(int c = 0; c < 4; ++c)
            out[c * 4 + r] = p[r * 4 + c];
}

// binds pipeline + descriptors + pushes constants; returns cb or NULL
VkCommandBuffer setupDraw(ShaderKind sh, TopoKind topo, const gosVertexDeclaration* vdecl,
                          VkImageView views[3], const PushConstants* pc)
{
    graphics::VkFrame* fr = graphics::vk_frame();
    if(!fr || !fr->frame_active)
        return NULL;
    if(!engineInit())
        return NULL;

    VkDescriptorSet dset = descriptorFor(views, samplerFromStates());
    if(dset == VK_NULL_HANDLE)
        return NULL;

    VkPipeline pipe = getPipeline(sh, topo, vdecl);
    if(pipe == VK_NULL_HANDLE)
        return NULL;

    VkCommandBuffer cb = fr->draw_cb;
    if(g_eng.bound_pipeline != pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        g_eng.bound_pipeline = pipe;
    }
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_eng.pipe_layout,
                            0, 1, &dset, 0, NULL);
    vkCmdPushConstants(cb, g_eng.pipe_layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(PushConstants), pc);
    return cb;
}

// immediate-mode draw: vertices copied into the ring
void emitDraw(ShaderKind sh, TopoKind topo, const gos_VERTEX* vertices, int count,
              const float* foreground /*text only*/)
{
    if(count <= 0)
        return;

    graphics::VkFrame* fr = graphics::vk_frame();
    if(!fr || !fr->frame_active || !engineInit())
        return;

    VkImageView views[3] = { NULL, NULL, NULL };
    if(sh != SHADER_VERTEX) {
        DWORD tex_handle = (DWORD)g_render_states[gos_State_Texture];
        VkStubTexture* t = getTexture(tex_handle);
        if(!t || !t->alive_) {
            static int spewed = 0;
            if(g_vk_debug && spewed < 40) {
                printf("[VKDBG] textured draw with bad handle %u (t=%p alive=%d) count=%d\n",
                       tex_handle, (void*)t, t ? (int)t->alive_ : -1, count);
                spewed++;
            }
            if(sh == SHADER_TEXT)
                return; // text without its font texture — nothing sane to draw
            sh = SHADER_VERTEX;
        } else {
            if(!textureToGpu(t))
                return;
            views[0] = t->view_;
        }
    }

    PushConstants pc = {};
    matToFloats(g_projection, pc.m0);
    const vec4 fog = uint32_to_vec4((uint32_t)g_render_states[gos_State_Fog]);
    pc.v0[0] = fog.x; pc.v0[1] = fog.y; pc.v0[2] = fog.z; pc.v0[3] = fog.w;
    if(foreground)
        memcpy(pc.v1, foreground, sizeof(pc.v1));
    pc.flags = g_render_states[gos_State_AlphaTest] ? 1u : 0u;

    VkDeviceSize voff = 0;
    uint8_t* dst = ringAlloc((VkDeviceSize)count * sizeof(gos_VERTEX), 4, &voff);
    if(!dst)
        return;
    memcpy(dst, vertices, (size_t)count * sizeof(gos_VERTEX));

    VkCommandBuffer cb = setupDraw(sh, topo, NULL, views, &pc);
    if(!cb)
        return;
    vkCmdBindVertexBuffers(cb, 0, 1, &g_eng.ring, &voff);
    vkCmdDraw(cb, (uint32_t)count, 1, 0, 0);

    // MC2_VK_DEBUG: the mouse cursor is the last quad drawn each frame —
    // remember what this draw used so engineBeginFrame can report it
    if(g_vk_debug && topo == TOPO_TRIS) {
        DWORD th = (DWORD)g_render_states[gos_State_Texture];
        VkStubTexture* t = getTexture(th);
        snprintf(g_last_draw_desc, sizeof(g_last_draw_desc),
                "sh=%d count=%d tex=%u '%s' %ux%u alpha=%d atest=%d uv0=%.3f,%.3f argb=%08x",
                (int)sh, count, th, t ? t->name_.c_str() : "-",
                t ? t->w_ : 0, t ? t->h_ : 0,
                g_render_states[gos_State_AlphaMode], g_render_states[gos_State_AlphaTest],
                vertices[0].u, vertices[0].v, vertices[0].argb);
    }
}

void engineBeginFrame()
{
    if(g_vk_debug) {
        static uint32_t frames = 0;
        static time_t last = 0;
        frames++;
        time_t now = time(NULL);
        if(now != last && g_last_draw_desc[0]) {
            printf("[VKDBG] last draw of frame %u: %s\n", frames, g_last_draw_desc);
            fflush(stdout);
            last = now;
        }
    }
    // previous frame's fence has been waited — deferred GPU objects are safe
    if(g_eng.initialized || !g_deferred_images.empty() || !g_deferred_buffers.empty()) {
        graphics::VkFrame* fr = graphics::vk_frame();
        for(size_t i = 0; i < g_deferred_images.size(); ++i) {
            vkDestroyImageView(fr->device, g_deferred_images[i].view, NULL);
            vkDestroyImage(fr->device, g_deferred_images[i].image, NULL);
            vkFreeMemory(fr->device, g_deferred_images[i].memory, NULL);
        }
        g_deferred_images.clear();
        for(size_t i = 0; i < g_deferred_buffers.size(); ++i) {
            vkDestroyBuffer(fr->device, g_deferred_buffers[i].buffer, NULL);
            vkFreeMemory(fr->device, g_deferred_buffers[i].memory, NULL);
        }
        g_deferred_buffers.clear();
    }

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
// gosBuffer GPU backing

void gosBuffer::ensureGpu()
{
    if(vk_buffer_ != VK_NULL_HANDLE || data_.empty())
        return;
    graphics::VkFrame* fr = graphics::vk_frame();
    if(!fr)
        return;
    VkBufferUsageFlags usage = 0;
    switch(type_) {
        case gosBUFFER_TYPE::VERTEX:  usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; break;
        case gosBUFFER_TYPE::INDEX:   usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT; break;
        case gosBUFFER_TYPE::UNIFORM: usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; break;
        default: return;
    }
    // uniform buffers get slack so std140 blocks larger than the CPU data
    // (fixed-size arrays in the shader) stay within the bound range
    vk_size_ = data_.size();
    if(type_ == gosBUFFER_TYPE::UNIFORM && vk_size_ < DUMMY_UBO_SIZE)
        vk_size_ = DUMMY_UBO_SIZE;
    createHostBuffer(fr->device, vk_size_, usage, &vk_buffer_, &vk_memory_, &mapped_);
    memcpy(mapped_, data_.data(), data_.size());
}

void gosBuffer::writeGpu(size_t offset, const void* src, size_t bytes)
{
    if(vk_buffer_ != VK_NULL_HANDLE && mapped_ && offset + bytes <= vk_size_)
        memcpy(mapped_ + offset, src, bytes);
}

HGOSBUFFER __stdcall gos_CreateBuffer(gosBUFFER_TYPE type, gosBUFFER_USAGE usage,
        int element_size, uint32_t count, void* pdata)
{
    gosBuffer* b = new gosBuffer();
    b->type_ = type;
    b->usage_ = usage;
    b->element_size_ = element_size;
    b->count_ = count;
    b->vk_buffer_ = VK_NULL_HANDLE;
    b->vk_memory_ = VK_NULL_HANDLE;
    b->mapped_ = NULL;
    b->vk_size_ = 0;
    b->data_.resize((size_t)element_size * count, 0);
    if(pdata)
        memcpy(b->data_.data(), pdata, b->data_.size());
    return b;
}

void __stdcall gos_DestroyBuffer(HGOSBUFFER buffer)
{
    if(!buffer)
        return;
    if(buffer->vk_buffer_ != VK_NULL_HANDLE) {
        DeferredBuffer d = { buffer->vk_buffer_, buffer->vk_memory_ };
        g_deferred_buffers.push_back(d);
    }
    for(int i = 0; i < 2; ++i)
        if(g_ubo_slots[i] == buffer)
            g_ubo_slots[i] = NULL;
    delete buffer;
}

void __stdcall gos_UpdateBuffer(HGOSBUFFER buffer, void* data, size_t offset, size_t num_bytes)
{
    gosASSERT(buffer);
    if(offset + num_bytes <= buffer->data_.size() && data) {
        memcpy(buffer->data_.data() + offset, data, num_bytes);
        buffer->writeGpu(offset, data, num_bytes);
    }
}

void __stdcall gos_BindBufferBase(HGOSBUFFER buffer, uint32_t slot)
{
    if(slot < 2)
        g_ubo_slots[slot] = buffer;
}

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
    delete vdecl; // pipelines keyed on this pointer stay in the cache; they
                  // are only ever selected again if the same address recurs,
                  // which at worst reuses a compatible layout
}

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
        if(g_vk_debug)
            printf("[VKDBG] loadTGA-from-memory FAILED: '%s' size=%u\n", FileName ? FileName : "?", Size);
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
        if(g_vk_debug)
            printf("[VKDBG] load-from-file FAILED: '%s'\n", FileName ? FileName : "?");
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
    deferDestroyTextureGpu(t);
}

// the game writes D3D-convention BGRA DWORDs into locked textures; the GL
// path converts RGBA->BGRA on Lock and back on Unlock — do the same in place
static void swizzleRB(VkStubTexture* t)
{
    DWORD* p = t->pixels_.data();
    const size_t n = t->pixels_.size();
    for(size_t i = 0; i < n; ++i) {
        DWORD c = p[i];
        p[i] = (c & 0xff00ff00) | ((c & 0xff) << 16) | ((c >> 16) & 0xff);
    }
}

void __stdcall gos_LockTexture(DWORD Handle, DWORD /*MipMapSize*/, bool ReadOnly,
        TEXTUREPTR* TextureInfo)
{
    gosASSERT(TextureInfo);
    VkStubTexture* t = getTexture(Handle);
    gosASSERT(t);
    if(t->pixels_.empty())
        t->pixels_.resize((size_t)t->w_ * t->h_, 0);
    if(!t->isLuminance())
        swizzleRB(t); // present as BGRA to the caller
    t->lock_read_only_ = ReadOnly;
    TextureInfo->pTexture = t->pixels_.data();
    TextureInfo->Width = t->w_;
    TextureInfo->Height = t->h_;
    TextureInfo->Pitch = t->w_; // in DWORDs
    TextureInfo->Type = t->format_;
}

void __stdcall gos_UnLockTexture(DWORD Handle)
{
    VkStubTexture* t = getTexture(Handle);
    if(!t)
        return;
    if(!t->isLuminance())
        swizzleRB(t); // back to RGBA for upload
    if(!t->lock_read_only_)
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
// render materials

static MaterialKind materialKindFromName(const char* name)
{
    if(0 == strcmp(name, "gos_vertex_lighted")) return MAT_VERTEX_LIGHTED;
    if(0 == strcmp(name, "gos_tex_vertex_lighted")) return MAT_TEX_VERTEX_LIGHTED;
    if(0 == strcmp(name, "gos_YCbCr")) return MAT_YCBCR;
    return MAT_UNKNOWN;
}

HGOSRENDERMATERIAL __stdcall gos_getRenderMaterial(const char* material)
{
    static std::vector<gosRenderMaterial*> mats;
    for(size_t i = 0; i < mats.size(); ++i)
        if(mats[i]->name_ == material)
            return mats[i];
    gosRenderMaterial* m = new gosRenderMaterial();
    m->name_ = material ? material : "";
    m->kind_ = materialKindFromName(m->name_.c_str());
    m->wvp_ = mat4::identity();
    m->world_ = mat4::identity();
    m->projection_ = mat4::identity();
    if(m->kind_ == MAT_UNKNOWN)
        SPEW(("GRAPHICS", "VK: unknown render material '%s' — draws with it are dropped\n",
              m->name_.c_str()));
    mats.push_back(m);
    return m;
}

void __stdcall gos_ApplyRenderMaterial(HGOSRENDERMATERIAL material)
{
    g_current_material = material;
}

void __stdcall gos_SetRenderMaterialParameterFloat4(HGOSRENDERMATERIAL material, const char* name, const float* v)
{
    gosASSERT(material && name && v);
    if(0 == strcmp(name, "light_offset_"))
        material->light_offset_ = vec4(v[0], v[1], v[2], v[3]);
    else if(0 == strcmp(name, "vp"))
        material->vp_ = vec4(v[0], v[1], v[2], v[3]);
    else if(0 == strcmp(name, "texture_crop_size_"))
        material->texture_crop_size_ = vec4(v[0], v[1], v[2], v[3]);
    else if(0 == strcmp(name, "scale_offset"))
        material->scale_offset_ = vec4(v[0], v[1], v[2], v[3]);
    // others (mirroring GL: unknown uniforms are silently ignored)
}

void __stdcall gos_SetRenderMaterialParameterMat4(HGOSRENDERMATERIAL material, const char* name, const float* m)
{
    gosASSERT(material && name && m);
    mat4 mm(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
            m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    if(0 == strcmp(name, "wvp_"))
        material->wvp_ = mm;
    else if(0 == strcmp(name, "world_"))
        material->world_ = mm;
    else if(0 == strcmp(name, "projection_"))
        material->projection_ = mm;
    // "view_", "mvp" etc: silently ignored, same as GL's missing-uniform path
}

void __stdcall gos_SetRenderMaterialUniformBlockBindingPoint(HGOSRENDERMATERIAL, const char*, uint32_t)
{
    // bindings are fixed in the vk shaders (lights=3, scene=4)
}

////////////////////////////////////////////////////////////////////////////////
// retained-path draws

static void emitRetainedDraw(gosRenderMaterial* mat, HGOSBUFFER ib, HGOSBUFFER vb,
                             HGOSVERTEXDECLARATION vdecl)
{
    if(!mat || mat->kind_ == MAT_UNKNOWN || !ib || !vb || ib->count_ == 0)
        return;
    graphics::VkFrame* fr = graphics::vk_frame();
    if(!fr || !fr->frame_active || !engineInit())
        return;

    ib->ensureGpu();
    vb->ensureGpu();
    if(ib->vk_buffer_ == VK_NULL_HANDLE || vb->vk_buffer_ == VK_NULL_HANDLE)
        return;

    ShaderKind sh;
    PushConstants pc = {};
    VkImageView views[3] = { NULL, NULL, NULL };

    switch(mat->kind_) {
        case MAT_VERTEX_LIGHTED:
            sh = SHADER_VERTEX_LIGHTED;
            matToFloats(g_projection, pc.m0); // GL's "mvp" for this shader
            break;
        case MAT_TEX_VERTEX_LIGHTED: {
            sh = SHADER_TEX_VERTEX_LIGHTED;
            matToFloats(mat->wvp_, pc.m0);
            matToFloats(mat->world_, pc.m1);
            matToFloats(mat->projection_, pc.m2);
            memcpy(pc.v0, (const float*)mat->light_offset_, sizeof(pc.v0));
            memcpy(pc.v1, (const float*)mat->vp_, sizeof(pc.v1));
            break;
        }
        case MAT_YCBCR:
            sh = SHADER_YCBCR;
            matToFloats(mat->projection_, pc.m0);
            memcpy(pc.v0, (const float*)mat->texture_crop_size_, sizeof(pc.v0));
            memcpy(pc.v1, (const float*)mat->scale_offset_, sizeof(pc.v1));
            break;
        default:
            return;
    }
    pc.flags = g_render_states[gos_State_AlphaTest] ? 1u : 0u;

    // textures from render states (tex1..tex3, YCbCr uses all three)
    const int tex_states[3] = { gos_State_Texture, gos_State_Texture2, gos_State_Texture3 };
    for(int i = 0; i < 3; ++i) {
        VkStubTexture* t = getTexture((DWORD)g_render_states[tex_states[i]]);
        if(t && t->alive_) {
            if(textureToGpu(t))
                views[i] = t->view_;
        }
    }

    VkCommandBuffer cb = setupDraw(sh, TOPO_TRIS, vdecl, views, &pc);
    if(!cb)
        return;

    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vb->vk_buffer_, &zero);
    vkCmdBindIndexBuffer(cb, ib->vk_buffer_, 0,
            ib->element_size_ == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, ib->count_, 1, 0, 0, 0);
}

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
    // expand indices CPU-side (simple and fine at these sizes)
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

void __stdcall gos_RenderIndexedArray(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, const float* mvp)
{
    // GL: selects the lighted material by texture state and sets its "mvp"
    // uniform — which the tex-lighted shader doesn't declare, so only vp and
    // projection_ actually land there. Mirror that behavior exactly.
    gosRenderMaterial* mat = gos_getRenderMaterial(
            g_render_states[gos_State_Texture] ? "gos_tex_vertex_lighted" : "gos_vertex_lighted");
    mat->vp_ = g_render_viewport;
    mat->projection_ = g_projection;
    (void)mvp; // GL's setMat4("mvp") finds no such uniform on these shaders
    emitRetainedDraw(mat, ib, vb, vdecl);
}

void __stdcall gos_RenderIndexedArray(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl)
{
    emitRetainedDraw(g_current_material, ib, vb, vdecl);
}

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

bool gos_GetDesktopDisplayMode(int DisplayIndex, int* XRes, int* YRes, int* BitDepth)
{
    return graphics::get_desktop_display_mode(DisplayIndex, XRes, YRes, BitDepth);
}

void __stdcall gos_StartRenderToTexture(DWORD /*Handle*/) {}
void __stdcall gos_EndRenderToTexture(bool /*ClearBorder*/) {}
bool __stdcall gos_VertexBuffersLost(DWORD /*VertexBufferHandle*/) { return false; }
