#version 450
// Vulkan port of shaders/gos_YCbCr.vert (FMV quad).
// pc mapping: m0=projection_, v0=texture_crop_size_, v1=scale_offset.

#include "common.glsl"

layout(location = 0) in vec2 pos;

layout(location = 0) out vec2 Texcoord;

void main(void)
{
    gl_Position = pc.m0 * vec4(pos.xy * pc.v1.zw + pc.v1.xy, 0.0, 1.0);
    Texcoord = pos.xy * pc.v0.xy;
}
