$input v_texcoord0

#include "bgfx_shader.sh"

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_mask, 1);
uniform vec4 u_texCoordBounds;
uniform vec4 u_maskTexCoordTransform;

void main()
{
    vec2 source_uv = mix(u_texCoordBounds.xy, u_texCoordBounds.zw, v_texcoord0);
    vec4 texel = texture2D(s_texColor, source_uv);
    vec2 mask_uv = v_texcoord0 * u_maskTexCoordTransform.xy + u_maskTexCoordTransform.zw;
    vec2 mask_min = step(vec2_splat(0.0), mask_uv);
    vec2 mask_max = step(mask_uv, vec2_splat(1.0));
    float mask_inside = mask_min.x * mask_min.y * mask_max.x * mask_max.y;
    float mask_alpha = texture2D(s_mask, mask_uv).a * mask_inside;
    gl_FragColor = texel * mask_alpha;
}