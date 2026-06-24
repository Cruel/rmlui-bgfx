$input v_texcoord0, v_blur0, v_blur1, v_blur2, v_blur3

#include "bgfx_shader.sh"

SAMPLER2D(s_texColor, 0);
uniform vec4 u_blurWeights;
uniform vec4 u_texCoordBounds;

vec4 sample_bounded(vec2 uv)
{
    vec2 in_region = step(u_texCoordBounds.xy, uv) * step(uv, u_texCoordBounds.zw);
    return texture2D(s_texColor, uv) * in_region.x * in_region.y;
}

void main()
{
    vec2 offset = v_texcoord0 - v_blur2;
    vec4 color = sample_bounded(v_texcoord0) * u_blurWeights.x;
    color += sample_bounded(v_blur2) * u_blurWeights.y;
    color += sample_bounded(v_blur3) * u_blurWeights.y;
    color += sample_bounded(v_blur1) * u_blurWeights.z;
    color += sample_bounded(v_texcoord0 + offset * 2.0) * u_blurWeights.z;
    color += sample_bounded(v_blur0) * u_blurWeights.w;
    color += sample_bounded(v_texcoord0 + offset * 3.0) * u_blurWeights.w;
    gl_FragColor = color;
}
