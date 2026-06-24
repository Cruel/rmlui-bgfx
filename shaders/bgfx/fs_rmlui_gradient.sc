$input v_texcoord0, v_color0

#include "bgfx_shader.sh"

#define MAX_STOPS 16
#define GRADIENT_LINEAR 1.0
#define GRADIENT_REPEATING_LINEAR 2.0
#define GRADIENT_RADIAL 3.0
#define GRADIENT_REPEATING_RADIAL 4.0
#define GRADIENT_CONIC 5.0
#define GRADIENT_REPEATING_CONIC 6.0
#define PI 3.14159265

uniform vec4 u_gradientParams[2];
uniform vec4 u_gradientStops[MAX_STOPS];
uniform vec4 u_gradientStopMeta[4];

float stop_position(int index)
{
    int group = index / 4;
    int lane = index - group * 4;
    vec4 meta = u_gradientStopMeta[group];
    if (lane == 0) return meta.x;
    if (lane == 1) return meta.y;
    if (lane == 2) return meta.z;
    return meta.w;
}

vec4 stop_color(int index)
{
    return u_gradientStops[index];
}

vec4 mix_stops(float t, int count)
{
    vec4 color = stop_color(0);
    for (int i = 1; i < MAX_STOPS; ++i) {
        if (i < count) {
            float p0 = stop_position(i - 1);
            float p1 = stop_position(i);
            color = mix(color, stop_color(i), smoothstep(p0, p1, t));
        }
    }
    return color;
}

void main()
{
    float kind = u_gradientParams[0].x;
    int count = int(u_gradientParams[0].y + 0.5);
    vec2 p = u_gradientParams[0].zw;
    vec2 v = u_gradientParams[1].xy;
    float t = 0.0;

    if (kind == GRADIENT_LINEAR || kind == GRADIENT_REPEATING_LINEAR) {
        vec2 delta = v_texcoord0 - p;
        t = dot(v, delta) / max(dot(v, v), 0.000001);
    } else if (kind == GRADIENT_RADIAL || kind == GRADIENT_REPEATING_RADIAL) {
        vec2 delta = v_texcoord0 - p;
        t = length(v * delta);
    } else {
        mat2 rotation = mat2(v.x, -v.y, v.y, v.x);
        vec2 delta = rotation * (v_texcoord0 - p);
        t = 0.5 + atan(-delta.x, delta.y) / (2.0 * PI);
    }

    if (kind == GRADIENT_REPEATING_LINEAR || kind == GRADIENT_REPEATING_RADIAL || kind == GRADIENT_REPEATING_CONIC) {
        float first = stop_position(0);
        float last = stop_position(max(count - 1, 0));
        t = first + mod(t - first, max(last - first, 0.000001));
    }

    gl_FragColor = v_color0 * mix_stops(t, count);
}
