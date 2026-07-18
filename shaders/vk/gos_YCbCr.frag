#version 450
// Vulkan port of shaders/gos_YCbCr.frag — rec601 YCbCr->RGB for FMV.

#include "common.glsl"

layout(location = 0) in vec2 Texcoord;

layout(set = 0, binding = 0) uniform sampler2D tex1; // Y
layout(set = 0, binding = 1) uniform sampler2D tex2; // Cb
layout(set = 0, binding = 2) uniform sampler2D tex3; // Cr

layout(location = 0) out vec4 FragColor;

mat4 rec601 = mat4(
        1.16438,  0.00000,  1.59603, -0.87079,
        1.16438, -0.39176, -0.81297,  0.52959,
        1.16438,  2.01723,  0.00000, -1.08139,
        0, 0, 0, 1
        );

void main(void)
{
    float y = texture(tex1, Texcoord).r;
    float cb = texture(tex2, Texcoord).r;
    float cr = texture(tex3, Texcoord).r;
    FragColor = vec4(y, cb, cr, 1.0) * rec601;
}
