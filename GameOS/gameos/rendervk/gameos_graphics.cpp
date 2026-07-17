// Vulkan-backend implementation of the gos_* graphics API. M2 scaffold:
// a "null renderer" — every entry point the game links is here with real
// CPU-side bookkeeping (textures own correctly-sized 8888 buffers so
// Lock/Unlock round-trips are safe, fonts load real glyph metrics so GUI
// layout math works), but nothing is drawn yet. gos_render.cpp clears and
// presents the swapchain each frame. See docs/RENDERER_AUDIT.md.

#include "gameos.hpp"
#include "gos_render.h"
#include "gos_font.h"

#include "utils/Image.h"
#include "utils/vec.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "platform_stdlib.h" // _splitpath

static graphics::RenderWindowHandle g_win_h = NULL;

bool g_disable_quads = true;

////////////////////////////////////////////////////////////////////////////////
// textures

struct VkStubTexture {
    std::string name_;
    gos_TextureFormat format_;
    uint32_t w_;
    uint32_t h_;
    std::vector<DWORD> pixels_; // always 8888, like the GL path's lock buffer
    bool alive_;
};

static std::vector<VkStubTexture> g_textures;

static DWORD addTexture(VkStubTexture&& t)
{
    // handle 0 means "no texture" everywhere in the game, so 1-based
    g_textures.push_back(t);
    return (DWORD)g_textures.size();
}

static VkStubTexture* getTexture(DWORD handle)
{
    if(handle == 0 || handle > g_textures.size())
        return NULL;
    return &g_textures[handle - 1];
}

DWORD __stdcall gos_NewEmptyTexture(gos_TextureFormat Format, const char* Name,
        DWORD HeightWidth, DWORD Hints, gos_RebuildFunction, void*)
{
    uint32_t w = HeightWidth;
    uint32_t h = HeightWidth;
    if(HeightWidth & 0xffff0000) {
        h = HeightWidth >> 16;
        w = HeightWidth & 0xffff;
    }
    VkStubTexture t;
    t.name_ = Name ? Name : "";
    t.format_ = Format;
    t.w_ = w;
    t.h_ = h;
    t.pixels_.resize((size_t)w * h, 0);
    t.alive_ = true;
    return addTexture(std::move(t));
}

DWORD __stdcall gos_NewTextureFromMemory(gos_TextureFormat Format, const char* FileName,
        BYTE* pBitmap, DWORD Size, DWORD Hints, gos_RebuildFunction, void*)
{
    Image img;
    uint32_t w = 32, h = 32;
    if(pBitmap && Size && img.loadTGA(pBitmap, Size)) {
        w = img.getWidth();
        h = img.getHeight();
    }
    VkStubTexture t;
    t.name_ = FileName ? FileName : "";
    t.format_ = Format;
    t.w_ = w;
    t.h_ = h;
    t.pixels_.resize((size_t)w * h, 0);
    t.alive_ = true;
    return addTexture(std::move(t));
}

DWORD __stdcall gos_NewTextureFromFile(gos_TextureFormat Format, const char* FileName,
        DWORD Hints, gos_RebuildFunction, void*)
{
    Image img;
    uint32_t w = 32, h = 32;
    if(FileName && img.loadFromFile(FileName)) {
        w = img.getWidth();
        h = img.getHeight();
    }
    VkStubTexture t;
    t.name_ = FileName ? FileName : "";
    t.format_ = Format;
    t.w_ = w;
    t.h_ = h;
    t.pixels_.resize((size_t)w * h, 0);
    t.alive_ = true;
    return addTexture(std::move(t));
}

void __stdcall gos_DestroyTexture(DWORD Handle)
{
    VkStubTexture* t = getTexture(Handle);
    if(t) {
        t->alive_ = false;
        t->pixels_.clear();
        t->pixels_.shrink_to_fit();
    }
}

void __stdcall gos_LockTexture(DWORD Handle, DWORD /*MipMapSize*/, bool /*ReadOnly*/,
        TEXTUREPTR* TextureInfo)
{
    gosASSERT(TextureInfo);
    VkStubTexture* t = getTexture(Handle);
    gosASSERT(t);
    if(t->pixels_.empty())
        t->pixels_.resize((size_t)t->w_ * t->h_, 0); // destroyed+relocked guard
    TextureInfo->pTexture = t->pixels_.data();
    TextureInfo->Width = t->w_;
    TextureInfo->Height = t->h_;
    TextureInfo->Pitch = t->w_; // in DWORDs
    TextureInfo->Type = t->format_;
}

void __stdcall gos_UnLockTexture(DWORD /*Handle*/)
{
    // pixels stay in the stub buffer; the real Vulkan path will upload here
}

void __stdcall gos_UpdateTexture(DWORD Handle, unsigned char* data, DWORD size)
{
    VkStubTexture* t = getTexture(Handle);
    if(t && data && size <= t->pixels_.size() * sizeof(DWORD))
        memcpy(t->pixels_.data(), data, size);
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
// buffers / vertex declarations (retained path: tgl.cpp, txmmgr.cpp, mc2movie)

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
// render materials (retained mech path)

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
// fonts / text — real metrics (gos_load_glyphs is backend-neutral), no drawing

class gosFont {
public:
    gosGlyphInfo gi_;
    std::string name_;
    int ref_count_;

    static gosFont* load(const char* fontFile);

    int getMaxCharWidth() const { return gi_.max_advance_; }
    int getMaxCharHeight() const { return gi_.font_line_skip_; }

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

    gosFont* font = new gosFont();
    font->name_ = fontFile ? fontFile : "";
    font->ref_count_ = 1;
    if(!gos_load_glyphs(glyphName, font->gi_)) {
        SPEW(("GRAPHICS", "VK stub: failed to load glyphs: %s\n", glyphName));
        // leave zeroed glyph info; metrics fall back to defaults below
        font->gi_.max_advance_ = 8;
        font->gi_.font_line_skip_ = 12;
        font->gi_.font_ascent_ = 10;
        font->gi_.num_glyphs_ = 0;
        font->gi_.glyphs_ = NULL;
    }
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
static int g_text_region[4] = {0};

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

void __stdcall gos_TextDraw(const char* /*Message*/, ...) {}
void __stdcall gos_TextDrawBackground(int, int, int, int, DWORD) {}

////////////////////////////////////////////////////////////////////////////////
// render states / viewport / screen mode

static const int NUM_RENDER_STATES = gos_MaxState;
static int g_render_states[NUM_RENDER_STATES] = {0};
static std::vector<std::vector<int> > g_state_stack;

static int g_width = 0;
static int g_height = 0;
static bool g_pending_resize = false;
static int g_req_width = 0;
static int g_req_height = 0;
static bool g_req_fullscreen = false;

static float g_viewport[4] = {0, 0, 1, 1}; // top, left, bottom, right
static vec4 g_render_viewport(0, 0, 0, 0);
static mat4 g_projection = mat4::identity();

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
// draw calls — null (gos_render.cpp presents a cleared frame)

void _stdcall gos_DrawPoints(gos_VERTEX*, int) {}
void _stdcall gos_DrawLines(gos_VERTEX*, int) {}
void _stdcall gos_DrawQuads(gos_VERTEX*, int) {}
void _stdcall gos_DrawTriangles(gos_VERTEX*, int) {}
void __stdcall gos_DrawStrips(gos_VERTEX*, int) {}
void __stdcall gos_DrawFans(gos_VERTEX*, int) {}
void __stdcall gos_RenderIndexedArray(gos_VERTEX*, DWORD, WORD*, DWORD) {}
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
    g_projection = mat4(2.0f / (float)g_width, 0, 0.0f, -1.0f,
            0, -2.0f / (float)g_height, 0.0f, 1.0f,
            0, 0, 1.0f, 0.0f,
            0, 0, 0.0f, 1.0f);
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
}

void gos_RendererEndFrame()
{
}

void gos_RendererHandleEvents()
{
    if(g_pending_resize) {
        g_width = g_req_width;
        g_height = g_req_height;
        g_projection = mat4(2.0f / (float)g_width, 0, 0.0f, -1.0f,
                0, -2.0f / (float)g_height, 0.0f, 1.0f,
                0, 0, 1.0f, 0.0f,
                0, 0, 0.0f, 1.0f);
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
