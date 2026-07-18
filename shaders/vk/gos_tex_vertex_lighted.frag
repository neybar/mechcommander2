#version 450
// Vulkan port of shaders/gos_tex_vertex_lighted.frag.
// pc mapping: v0=light_offset_; flags bit 0 = alpha test.

#include "common.glsl"
#include "lighting.glsl"

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec2 Texcoord;
layout(location = 2) in vec4 VertexColor;
layout(location = 3) in vec3 VertexLight;
layout(location = 4) in vec3 WorldPos;
layout(location = 5) in vec3 CameraPos;

layout(set = 0, binding = 0) uniform sampler2D tex1;

layout(location = 0) out vec4 FragColor;

void main(void)
{
    vec4 c = vec4(1,1,1,1);
    vec4 tex_color = texture(tex1, Texcoord);
    c *= tex_color;

    if((pc.flags & 1u) != 0u && tex_color.a == 0.0)
        discard;

#if ENABLE_VERTEX_LIGHTING
    vec3 lighting = VertexLight;
#else
    int lights_index = int(pc.v0.x);
    vec3 lighting = calc_light(lights_index, Normal, VertexLight);
#endif

    c.xyz = c.xyz * lighting;

    c.xyz = apply_fog(c.xyz, WorldPos.xyz, CameraPos);

    FragColor = vec4(c.xyz, c.a);
}
