//#version 300 es

#define PREC highp

in PREC vec2 Texcoord;

layout (location=0) out PREC vec4 FragColor;

uniform sampler2D tex1;
uniform sampler2D tex2;
uniform sampler2D tex3;

#define texture_y tex1
#define texture_cb tex2
#define texture_cr tex3

PREC mat4 rec601 = mat4(
        1.16438,  0.00000,  1.59603, -0.87079,
        1.16438, -0.39176, -0.81297,  0.52959,
        1.16438,  2.01723,  0.00000, -1.08139,
        0, 0, 0, 1
        );

void main(void)
{
    PREC float y = texture(texture_y, Texcoord).r;
    PREC float cb = texture(texture_cb, Texcoord).r;
    PREC float cr = texture(texture_cr, Texcoord).r;
    FragColor = vec4(y, cb, cr, 1.0) * rec601;
}
