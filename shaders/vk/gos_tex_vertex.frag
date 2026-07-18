#version 450
// Vulkan port of shaders/gos_tex_vertex.frag — note the deliberate
// Color.bgra swizzle (matches GL) and the alpha-test-on-texture-alpha
// variation, here a push-constant flag instead of a compiled variant.

#include "common.glsl"

layout(location = 0) in vec4 Color;
layout(location = 1) in float FogValue;
layout(location = 2) in vec2 Texcoord;

layout(set = 0, binding = 0) uniform sampler2D tex1;

layout(location = 0) out vec4 FragColor;

void main(void)
{
    vec4 c = Color.bgra;
    vec4 tex_color = texture(tex1, Texcoord);
    c *= tex_color;

    if((pc.flags & 1u) != 0u && tex_color.a == 0.0)
        discard;

    if(pc.v0.x > 0.0 || pc.v0.y > 0.0 || pc.v0.z > 0.0 || pc.v0.w > 0.0)
        c.rgb = mix(pc.v0.rgb, c.rgb, FogValue);
    FragColor = c;
}
