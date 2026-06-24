$input v_texcoord0

#include "bgfx_shader.sh"

SAMPLER2D(s_texColor, 0);
uniform vec4 u_texCoordBounds;

void main()
{
    vec2 uv = mix(u_texCoordBounds.xy, u_texCoordBounds.zw, v_texcoord0);
    gl_FragColor = texture2D(s_texColor, uv);
}
