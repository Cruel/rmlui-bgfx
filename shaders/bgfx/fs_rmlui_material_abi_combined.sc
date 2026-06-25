$input v_texcoord0, v_color0

#include "bgfx_shader.sh"

uniform vec4 u_rmluiMaterialParams0;
uniform vec4 u_rmluiMaterialParams1;

void main()
{
    float time = u_rmluiMaterialParams0.x;
    vec2 dimensions = max(u_rmluiMaterialParams0.yz, vec2(1.0, 1.0));
    float dpi = max(u_rmluiMaterialParams0.w, 0.01);
    vec2 mouse = u_rmluiMaterialParams1.xy;
    float mouse_valid = u_rmluiMaterialParams1.z;

    vec2 scaled = v_texcoord0 * dimensions / max(24.0 * dpi, 1.0);
    float grid = step(0.90, max(abs(fract(scaled.x) - 0.5), abs(fract(scaled.y) - 0.5)) * 2.0);
    vec2 mouse_norm = clamp(mouse / vec2(1024.0, 768.0), vec2(0.0, 0.0), vec2(1.0, 1.0));
    float wave = 0.5 + 0.5 * sin(time * 4.0 + v_texcoord0.x * 8.0);
    vec3 base = vec3(wave, dimensions.x / 900.0, dimensions.y / 600.0);
    vec3 mouse_color = vec3(mouse_norm.x, mouse_norm.y, 1.0 - mouse_norm.x);
    vec3 color = mix(base, mouse_color, mouse_valid * 0.55);
    color = mix(color, vec3(1.0), grid * 0.28);
    gl_FragColor = vec4(color, v_color0.a);
}
