$input v_texcoord0

#include "bgfx_shader.sh"

SAMPLER2D(s_texColor, 0);
uniform vec4 u_texCoordBounds;
uniform vec4 u_opacity;
uniform mat4 u_colorMatrix;

void main()
{
    vec2 uv = mix(u_texCoordBounds.xy, u_texCoordBounds.zw, v_texcoord0);
    vec4 texel = texture2D(s_texColor, uv) * u_opacity.x;
    vec3 transformed = mul(texel, u_colorMatrix).rgb;
    gl_FragColor = vec4(transformed, texel.a);
}
