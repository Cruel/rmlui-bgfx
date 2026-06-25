$input v_texcoord0, v_color0

#include "bgfx_shader.sh"

uniform vec4 u_rmluiMaterialParams0;
uniform vec4 u_rmluiMaterialParams1;

void main()
{
    vec2 dimensions = max(u_rmluiMaterialParams0.yz, vec2(1.0, 1.0));
    vec2 mouse = u_rmluiMaterialParams1.xy;
    float valid = u_rmluiMaterialParams1.z;
    vec2 normalized_mouse = clamp(mouse / vec2(1024.0, 768.0), vec2(0.0, 0.0), vec2(1.0, 1.0));
    vec2 local = v_texcoord0 * dimensions;
    float local_pattern = 0.5 + 0.5 * sin((local.x + local.y) * 0.04);
    vec3 color = vec3(normalized_mouse.x, normalized_mouse.y, local_pattern);
    color = mix(vec3(0.08, 0.08, 0.10), color, valid);
    gl_FragColor = vec4(color, v_color0.a);
}
