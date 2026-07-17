// Shared push-constant block for the immediate-mode gos shaders.
// Mirrors the GL path's loose uniforms (mvp, fog_color, Foreground) plus a
// flags word replacing the ALPHA_TEST compile-time variation.
layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 fog_color;
    vec4 foreground; // text only
    uint flags;      // bit 0: alpha test
} pc;
