//#version 300 es

layout(location = 0) in vec2 pos;

uniform vec4 texture_crop_size_;
uniform mat4 projection_;
uniform vec4 scale_offset;

out vec4 Color;
out vec2 Texcoord;

void main(void)
{
    //gl_Position = vec4((pos.xy * 2.0 - 1.0) * vec2(1, -1), 0.0, 1.0);
    gl_Position = projection_ * vec4(pos.xy*scale_offset.zw + scale_offset.xy, 0.0, 1.0);
    Texcoord = pos.xy*texture_crop_size_.xy;
}

