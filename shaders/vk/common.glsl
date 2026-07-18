// Shared push-constant block for ALL vk gos shaders (one pipeline layout).
// Field meaning depends on the material, mirroring the GL loose uniforms:
//   immediate (gos_vertex/tex_vertex/text):  m0=mvp     v0=fog_color        v1=Foreground
//   lighted (mech path):                     m0=wvp_    m1=world_  m2=projection_
//                                            v0=light_offset_     v1=vp (viewport)
//   YCbCr (FMV):                             m0=projection_
//                                            v0=texture_crop_size_ v1=scale_offset
// flags bit 0: alpha test (GL's ALPHA_TEST compile-time variant)
layout(push_constant) uniform PC {
    mat4 m0;
    mat4 m1;
    mat4 m2;
    vec4 v0;
    vec4 v1;
    uint flags;
} pc;
