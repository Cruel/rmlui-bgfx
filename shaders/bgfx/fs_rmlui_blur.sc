$input v_texcoord0, v_blur0, v_blur1, v_blur2, v_blur3

#include "bgfx_shader.sh"

SAMPLER2D(s_texColor, 0);
uniform vec4 u_blurParams;
uniform vec4 u_blurWeights;
uniform vec4 u_texCoordBounds;

vec4 sample_bounded(vec2 uv)
{
    vec2 in_region = step(u_texCoordBounds.xy, uv) * step(uv, u_texCoordBounds.zw);
    return texture2D(s_texColor, uv) * in_region.x * in_region.y;
}

void main()
{
    vec2 offset = u_blurParams.xy;
    float sigma = max(u_blurParams.z, 0.0001);
    float sample_spacing = max(u_blurParams.w, 1.0);
    float two_sigma_sq = 2.0 * sigma * sigma;

    vec4 color = vec4(0.0, 0.0, 0.0, 0.0);
    float total_weight = 0.0;

    for (int tap = 0; tap < 61; ++tap) {
        float centered_tap = float(tap - 30);
        float sample_distance = centered_tap * sample_spacing;
        float weight = exp(-(sample_distance * sample_distance) / two_sigma_sq);
        color += sample_bounded(v_texcoord0 + offset * centered_tap) * weight;
        total_weight += weight;
    }

    gl_FragColor = color / max(total_weight, 0.000001);
}
