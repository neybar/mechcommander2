cmake_minimum_required (VERSION 3.10)
project(gameos)

# Backend-independent GameOS code. Everything that talks to a graphics API
# lives in rendergl/ or rendervk/, selected by MC2_RENDERER (see top-level
# CMakeLists and docs/RENDERER_AUDIT.md).
set(SOURCES ${SOURCES}
    gameos.cpp
    gameos_res.cpp
    gameos_fileio.cpp
    gameos_input.cpp
    gameos_debugging.cpp
    gameos_sound.cpp
    gos_font.cpp
    gos_input.cpp

    utils/stream.cpp
    utils/camera.cpp
    utils/Image.cpp
    utils/logging.cpp
    utils/matrix.cpp
    utils/vec.cpp
    utils/timing.cpp
    utils/string_utils.cpp
    utils/file_utils.cpp
    )

if(MC2_RENDERER STREQUAL "VULKAN")
    set(RENDER_SOURCES
        rendervk/gameos_graphics.cpp
        rendervk/gos_render.cpp
        )
else()
    set(RENDER_SOURCES
        rendergl/gameos_graphics.cpp
        rendergl/gos_render.cpp
        rendergl/gl_utils.cpp
        rendergl/gl_render_constants.cpp
        rendergl/shader_builder.cpp
        )
endif()

set(MAIN_SRC gameosmain.cpp)

include_directories("../include"  ".")
include_directories(${THIRDPARTY_INCLUDE_DIRS})

add_library(gameos ${SOURCES} ${RENDER_SOURCES})
add_library(gameos_main ${MAIN_SRC})

if(MC2_RENDERER STREQUAL "VULKAN")
    target_link_libraries(gameos Vulkan::Vulkan)
endif()
