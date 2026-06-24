$input a_position, a_texcoord0
$output v_texcoord0, v_blur0, v_blur1, v_blur2, v_blur3

#include "bgfx_shader.sh"

uniform vec4 u_blurParams;

void main()
{
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_texcoord0 = a_texcoord0;
    vec2 offset = u_blurParams.xy;
    v_blur0 = a_texcoord0 - offset * 3.0;
    v_blur1 = a_texcoord0 - offset * 2.0;
    v_blur2 = a_texcoord0 - offset;
    v_blur3 = a_texcoord0 + offset;
}
