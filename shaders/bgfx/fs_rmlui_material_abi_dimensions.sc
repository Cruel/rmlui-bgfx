$input v_texcoord0, v_color0

#include "bgfx_shader.sh"

uniform vec4 u_rmluiMaterialParams0;
uniform vec4 u_rmluiMaterialParams1;

void main()
{
    vec2 dimensions = max(u_rmluiMaterialParams0.yz, vec2(1.0, 1.0));
    vec2 cell = abs(fract(v_texcoord0 * dimensions / 32.0) - 0.5);
    float grid = step(0.46, max(cell.x, cell.y));
    float width_signal = clamp(dimensions.x / 800.0, 0.0, 1.0);
    float height_signal = clamp(dimensions.y / 500.0, 0.0, 1.0);
    vec3 color = mix(vec3(width_signal, height_signal, 0.25), vec3(1.0, 1.0, 1.0), grid * 0.35);
    gl_FragColor = vec4(color, v_color0.a);
}
