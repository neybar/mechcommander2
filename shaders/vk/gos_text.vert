#version 450
// Vulkan port of shaders/gos_text.vert (no divide by pos.w, unlike gos_vertex)

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
    gl_Position = pc.m0 * vec4(pos.xyz, 1);
    Color = color;
    FogValue = fog.w;
    Texcoord = texcoord;
}
