$input v_texcoord0, v_color0

#include "bgfx_shader.sh"

uniform vec4 u_rmluiMaterialParams0;
uniform vec4 u_rmluiMaterialParams1;

void main()
{
    float t = u_rmluiMaterialParams0.x;
    float wave = 0.5 + 0.5 * sin(t * 3.0 + v_texcoord0.x * 10.0 + v_texcoord0.y * 4.0);
    float pulse = 0.5 + 0.5 * sin(t * 6.0);
    gl_FragColor = vec4(wave, pulse, 1.0 - wave * 0.65, v_color0.a);
}
