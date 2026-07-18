#version 450
// Vulkan port of shaders/gos_text.frag: glyph mask in tex red channel,
// color from the Foreground push constant.

#include "common.glsl"

layout(location = 0) in vec4 Color;
layout(location = 1) in float FogValue;
layout(location = 2) in vec2 Texcoord;

layout(set = 0, binding = 0) uniform sampler2D tex1;

layout(location = 0) out vec4 FragColor;

void main(void)
{
    vec4 c = pc.v1;
    vec4 mask = texture(tex1, Texcoord);
    c *= mask.xxxx;
    if(pc.v0.x > 0.0 || pc.v0.y > 0.0 || pc.v0.z > 0.0 || pc.v0.w > 0.0)
        c.rgb = mix(pc.v0.rgb, c.rgb, FogValue);
    FragColor = c;
}
