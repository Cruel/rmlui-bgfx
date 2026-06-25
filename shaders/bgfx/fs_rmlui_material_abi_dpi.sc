$input v_texcoord0, v_color0

#include "bgfx_shader.sh"

uniform vec4 u_rmluiMaterialParams0;
uniform vec4 u_rmluiMaterialParams1;

void main()
{
    float dpi = max(u_rmluiMaterialParams0.w, 0.01);
    float stripes = 0.5 + 0.5 * sin((v_texcoord0.x + v_texcoord0.y) * 32.0 * dpi);
    float warm = clamp((dpi - 1.0) / 2.0, 0.0, 1.0);
    vec3 low = vec3(0.05, 0.45, 0.85);
    vec3 high = vec3(1.0, 0.55, 0.05);
    vec3 color = mix(low, high, warm);
    color = mix(color * 0.45, color, stripes);
    gl_FragColor = vec4(color, v_color0.a);
}
