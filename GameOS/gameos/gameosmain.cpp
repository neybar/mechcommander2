#include "gameos.hpp"
#include "gos_render.h"
#include "platform_io.h" // chdir, getcwd, stat
#include <stdio.h>
#include <time.h>

#include <SDL2/SDL.h>
#include "gos_input.h"

#include "utils/camera.h"
#include "utils/timing.h"

#include <signal.h>

extern graphics::RenderContextHandle gos_GetRenderContext();
extern void gos_DestroyRenderer();
extern void gos_RendererBeginFrame();
extern void gos_RendererEndFrame();
extern void gos_RendererHandleEvents();
extern void gos_RenderUpdateDebugInput();
extern void gos_RenderEnableDebugDrawCalls();
extern bool gos_RenderGetEnableDebugDrawCalls();

extern bool gosExitGameOS();

extern bool gos_CreateAudio();
extern void gos_UpdateAudio();
extern void gos_DestroyAudio();

static bool g_exit = false;
static bool g_focus_lost = false;
extern bool g_gos_focus_lost_dbg; // MC2_DEBUG_INPUT dump in gameos_input.cpp
#if 0
static camera g_camera;
#endif

static void handle_key_down( SDL_Keysym* keysym ) {
    switch( keysym->sym ) {
        case SDLK_ESCAPE:
            if(keysym->mod & KMOD_RALT)
                g_exit = true;
            break;
        case 'd':
            if(keysym->mod & KMOD_RALT)
                gos_RenderEnableDebugDrawCalls();
            break;
    }
}

static void set_mouse_capture(bool enabled)
{
    graphics::RenderWindowHandle win = gos_GetWindow();
    bool fullscreen = graphics::is_window_fullscreen(win);
    // only confine the OS cursor in fullscreen: in windowed mode a grab
    // makes the window chrome unreachable, and SDL's confinement rect
    // goes stale when the window is resized (800x600 menus -> mission
    // resolution), caging the cursor in a menu-sized box
    SDL_bool capture = (enabled && fullscreen) ? SDL_TRUE : SDL_FALSE;
    graphics::grab_window(win, (bool)capture);

    SDL_CaptureMouse(capture);
    SDL_ShowCursor(enabled ? SDL_DISABLE : SDL_ENABLE);
}

static void process_events( void ) {

    input::beginUpdateMouseState();

    SDL_Event event;
    while( SDL_PollEvent( &event ) ) {

        // quit requests (Cmd-Q, SIGINT from ctrl-c, window close button)
        // must work even while the window doesn't have focus
        if(event.type == SDL_QUIT ||
           (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
            g_exit = true;
            continue;
        }

        if(g_focus_lost) {
            if(event.type != SDL_WINDOWEVENT || event.window.event != SDL_WINDOWEVENT_FOCUS_GAINED) {
                continue;
            } else {
                g_focus_lost = false;
            }
        }

        switch( event.type ) {
        case SDL_KEYDOWN:
            handle_key_down( &event.key.keysym );
            // fallthrough
        case SDL_KEYUP:
            input::handleKeyEvent(&event);
            break;
        case SDL_QUIT:
            g_exit = true;
            break;
        case SDL_WINDOWEVENT:
            switch(event.window.event) {
                case SDL_WINDOWEVENT_RESIZED:
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                {
                    float w = (float)event.window.data1;(void)w;
                    float h = (float)event.window.data2;(void)h;
                    SPEW(("INPUT", "resize event: w: %f h:%f\n", w, h));

                    // the window's real size can diverge from what
                    // gos_SetScreenMode requested (macOS clamps oversized
                    // windows, Cocoa resizes asynchronously, the user can
                    // drag-resize) — refresh the drawable size here or mouse
                    // normalization and the viewport keep using the stale one
                    graphics::get_drawable_size(gos_GetWindow(),
                        &Environment.drawableWidth, &Environment.drawableHeight);

                    // re-apply the mouse capture so a fullscreen grab's
                    // confinement rect tracks the new window size
                    set_mouse_capture(!g_focus_lost);
                    break;
                }
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    if (getenv("MC2_DEBUG_INPUT")) {
                        printf("[FOCUSDBG] t=%u FOCUS_LOST\n", SDL_GetTicks()); fflush(stdout);
                    }
                    g_focus_lost = true;
                    g_gos_focus_lost_dbg = true;
                    set_mouse_capture(false);
                    break;
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    if (getenv("MC2_DEBUG_INPUT")) {
                        printf("[FOCUSDBG] t=%u FOCUS_GAINED\n", SDL_GetTicks()); fflush(stdout);
                    }
                    g_focus_lost = false;
                    g_gos_focus_lost_dbg = false;
                    set_mouse_capture(true);
                    break;
            }
            break;
        case SDL_MOUSEMOTION:
            input::handleMouseMotion(&event); 
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            //input::handleMouseButton(&event);
            break;
        case SDL_MOUSEWHEEL:
            input::handleMouseWheel(&event);
            break;
        }
    }

    input::updateMouseState();
    input::updateKeyboardState();
}

static void draw_screen( void )
{
    // per-frame graphics-API setup (clear, viewport, state reset) lives in
    // the renderer's begin/end frame — no GL calls in the main loop
    gos_RendererBeginFrame();
    Environment.UpdateRenderers();
    gos_RendererEndFrame();
}

extern float frameRate;

// AD-4: -assetdir <path>, parsed in ParseCommandLine (code/mechcmd2.cpp).
extern char assetDirOverride[];

// AD-4: point the engine at a user-supplied asset install instead of
// requiring launch-from-game-dir. Every asset path in the codebase
// (mclib/paths.cpp et al.) is relative to CWD, so resolving down to a
// chdir() here is enough to cover all of them with no further changes.
// Runs before InitializeGameEngine() so we fail with a clear message
// instead of falling into mclib/file.cpp's missing-file retry loop,
// which hangs forever on this port (see ENGINEERING_LOG).
static void resolveAssetDirectory()
{
    const char* dir = NULL;
    const char* source = NULL;
    if (assetDirOverride[0] != '\0') {
        dir = assetDirOverride;
        source = "-assetdir";
    } else if (const char* env = getenv("MC2_ASSET_DIR")) {
        dir = env;
        source = "MC2_ASSET_DIR";
    }

    if (dir) {
        if (chdir(dir) != 0) {
            fprintf(stderr,
                "MC2: can't switch to asset directory '%s' (from %s): %s\n"
                "Pass a valid MechCommander 2 install directory via -assetdir <path> "
                "or the MC2_ASSET_DIR environment variable.\n",
                dir, source, strerror(errno));
            exit(1);
        }
    }

    struct stat st;
    if (stat("data", &st) != 0 || !S_ISDIR(st.st_mode)) {
        char cwd[1024] = {0};
        getcwd(cwd, sizeof(cwd));
        fprintf(stderr,
            "MC2: no 'data' directory found in '%s'.\n"
            "MC2 needs a MechCommander 2 asset install (retail or the Microsoft\n"
            "shared-source asset set). Point it at one with -assetdir <path>, the\n"
            "MC2_ASSET_DIR environment variable, or launch mc2 from inside the\n"
            "install directory.\n",
            cwd);
        exit(1);
    }
}

#ifndef DISABLE_GAMEOS_MAIN
int main(int argc, char** argv)
{
    //signal(SIGTRAP, SIG_IGN);

    // gather command line
	size_t cmdline_len = 0;
    for(int i=0;i<argc;++i) {
        cmdline_len += strlen(argv[i]);
        cmdline_len += 1; // ' '
    }
    char* cmdline = new char[cmdline_len + 1];
    size_t offset = 0;
    for(int i=0;i<argc;++i) {
        size_t arglen = strlen(argv[i]);
        memcpy(cmdline + offset, argv[i], arglen);
        cmdline[offset + arglen] = ' ';
        offset += arglen + 1;
    }
    cmdline[cmdline_len] = '\0';

    // fills in Environment structure
    GetGameOSEnvironment(cmdline);

    delete[] cmdline;
    cmdline = NULL;

    resolveAssetDirectory();

    Environment.InitializeGameEngine();

	timing::init();

    graphics::RenderWindowHandle win = gos_GetWindow();
    graphics::RenderContextHandle ctx = gos_GetRenderContext();

    // dev/smoke-test hook: MC2_AUTOQUIT_SECS=N drives the normal quit path
    // (same as closing the window) after N seconds
    double autoquit_secs = 0.0;
    if (const char* aq = getenv("MC2_AUTOQUIT_SECS"))
        autoquit_secs = atof(aq);
    const uint64_t loop_start_tick = timing::gettickcount();

    while( !g_exit ) {

        if (autoquit_secs > 0.0 &&
            timing::ticks2sec(timing::gettickcount() - loop_start_tick) > autoquit_secs) {
            g_exit = true;
        }

		uint64_t start_tick = timing::gettickcount();

        process_events();

        if(gos_RenderGetEnableDebugDrawCalls()) {
            gos_RenderUpdateDebugInput();
        } else {
            Environment.DoGameLogic();
        }

        win = gos_GetWindow();
        ctx = gos_GetRenderContext();

		gos_RendererHandleEvents();
        gos_UpdateAudio();

        graphics::make_current_context(ctx);
        draw_screen();
        graphics::swap_window(win);

        g_exit |= gosExitGameOS();

		uint64_t end_tick = timing::gettickcount();
		double dt_sec = timing::ticks2sec(end_tick - start_tick);
		frameRate = (float)(1.0 / dt_sec);
    }
    
    Environment.TerminateGameEngine();

    return 0;
}
#endif // DISABLE_GAMEOS_MAIN
