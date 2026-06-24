$input v_texcoord0

#include "bgfx_shader.sh"

SAMPLER2D(s_texColor, 0);
uniform vec4 u_shadowColor;
uniform vec4 u_shadowOffset;

void main()
{
    vec2 uv = v_texcoord0 - u_shadowOffset.xy;
    float alpha = texture2D(s_texColor, uv).a;
    gl_FragColor = u_shadowColor * alpha;
}
