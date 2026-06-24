$input v_texcoord0

#include "bgfx_shader.sh"

SAMPLER2D(s_texColor, 0);
uniform vec4 u_opacity;

void main()
{
    gl_FragColor = texture2D(s_texColor, v_texcoord0) * u_opacity.x;
}
