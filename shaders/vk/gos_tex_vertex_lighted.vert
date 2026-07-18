#version 450
// Vulkan port of shaders/gos_tex_vertex_lighted.vert (mech/object path).
// Keep quirks verbatim: the D3D-era viewport math with the +100.0 x offset,
// abs(rhw) w, and the projection divide. pc mapping: m0=wvp_, m1=world_,
// m2=projection_, v0=light_offset_, v1=vp. (GL's mesh_data UBO is unused
// by the shader body and is dropped here.)

#include "common.glsl"
#include "lighting.glsl"

layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 aRGBLight;
layout(location = 3) in vec2 texcoord;

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec2 Texcoord;
layout(location = 2) out vec4 VertexColor;
layout(location = 3) out vec3 VertexLight;
layout(location = 4) out vec3 WorldPos;
layout(location = 5) out vec3 CameraPos;

void main(void)
{
    vec4 p = pc.m0 * vec4(pos.xyz, 1);
    float rhw = 1 / p.w;

    p.x = (p.x * rhw) * pc.v1.z + pc.v1.x + 100.0;
    p.y = (p.y * rhw) * pc.v1.w + pc.v1.y;
    p.z = p.z * rhw;
    p.w = abs(rhw);

    WorldPos = (pc.m1 * vec4(pos.xyz, 1.0)).xyz;
    CameraPos = g_scene.cameraPos.xyz;

    vec4 p2 = pc.m2 * vec4(p.xyz, 1);

    gl_Position = p2 / p.w;
    Normal = (pc.m1 * vec4(normal, 0)).xyz;
    Texcoord = texcoord;

    vec3 base_light = get_base_light(aRGBLight.bgra, false, 0.0, false, false,
            vec3(0.0), vec3(0.0), vec3(0.0));

#if ENABLE_VERTEX_LIGHTING
    int lights_index = int(pc.v0.x);
    VertexLight = calc_light(lights_index, Normal, base_light);
#else
    VertexLight = base_light;
#endif

    VertexColor = aRGBLight.bgra;
}
