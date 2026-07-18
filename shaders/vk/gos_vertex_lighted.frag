#version 450
// Vulkan port of shaders/gos_vertex_lighted.frag (untextured variant — the
// GL file's ENABLE_TEXTURE1 branch is not compiled for this material).
// pc mapping: v0=fog_color.

#include "common.glsl"

layout(location = 0) in vec4 Color;
layout(location = 1) in float FogValue;
layout(location = 2) in vec2 Texcoord;

layout(location = 0) out vec4 FragColor;

void main(void)
{
    vec4 c = Color;
    if(pc.v0.x > 0.0 || pc.v0.y > 0.0 || pc.v0.z > 0.0 || pc.v0.w > 0.0)
        c.rgb = mix(pc.v0.rgb, c.rgb, FogValue);
    FragColor = c;
}
