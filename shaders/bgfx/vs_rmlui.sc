$input a_position, a_color0, a_texcoord0
$output v_texcoord0, v_color0

uniform vec4 u_translate;
uniform mat4 u_projection;
uniform mat4 u_transform;

#include "bgfx_shader.sh"

void main()
{
    mat4 real_transform = mul(u_projection, u_transform);
    vec4 translated_pos = vec4(a_position.xy + u_translate.xy, 0.0, 1.0);
    gl_Position = mul(real_transform, translated_pos);

    v_color0 = a_color0;
    v_texcoord0 = a_texcoord0;
}
