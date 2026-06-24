$input v_texcoord0

#include "bgfx_shader.sh"

SAMPLER2D(s_texColor, 0);
uniform mat4 u_colorMatrix;

void main()
{
    vec4 texel = texture2D(s_texColor, v_texcoord0);
    vec3 transformed = mul(texel, u_colorMatrix).rgb;
    gl_FragColor = vec4(transformed, texel.a);
}
