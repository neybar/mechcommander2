#version 450
// Vulkan port of shaders/gos_vertex.vert — keep quirks identical (the
// divide by pos.w mirrors the GL shader exactly).

#include "common.glsl"

layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 color;
layout(location = 2) in vec4 fog;
layout(location = 3) in vec2 texcoord;

layout(location = 0) out vec4 Color;
layout(location = 1) out float FogValue;
layout(location = 2) out vec2 Texcoord;

void main(void)
{
    vec4 p = pc.mvp * vec4(pos.xyz, 1);
    gl_Position = p / pos.w;
    Color = color;
    FogValue = fog.w;
    Texcoord = texcoord;
}
