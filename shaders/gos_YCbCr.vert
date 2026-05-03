//#version 300 es

layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 texcoord;

uniform vec4 texture_crop_size_;
uniform mat4 projection_;

out vec4 Color;
out vec2 Texcoord;

void main(void)
{
    //gl_Position = vec4((pos.xy * 2.0 - 1.0) * vec2(1, -1), 0.0, 1.0);
    gl_Position = projection_ * vec4(pos.xy, 0.0, 1.0);
    Texcoord = texcoord*texture_crop_size_.xy;
}

