#version 450

// A hearth's fire as a volume (TASK_10, step 3), after Fuller et al., "Real-
// time Procedural Volumetric Fire" (I3D 2007): a flame's shape - wide and hot
// at the base, narrowing and cooling as it rises - broken up by turbulent
// noise that scrolls upward with time, marched through along the view ray.
// Written for this client; nothing here is taken from another program.
//
// Emission only, added to what is behind (blend ONE, ONE): fire gives light
// and hides little. The ray stops at the scene's depth (the eye's copy, as
// for soft particles), so logs and the firebox stand in front of the flames
// where they are, and the volume is the same in both eyes but for parallax.

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
};

layout(set = 1, binding = 0) uniform sampler2D uSceneDepth;

layout(push_constant) uniform Push {
    vec4 boxMin;     // xyz; w = seconds
    vec4 boxMax;     // xyz; w = brightness (flicker and fade in)
    vec4 params;     // x = steps, y = seed
} push;

layout(location = 0) in vec3 vWorldPos;
layout(location = 0) out vec4 outColor;

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

// Value noise on a lattice, smoothly interpolated.
float noise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i);
    float n100 = hash13(i + vec3(1, 0, 0));
    float n010 = hash13(i + vec3(0, 1, 0));
    float n110 = hash13(i + vec3(1, 1, 0));
    float n001 = hash13(i + vec3(0, 0, 1));
    float n101 = hash13(i + vec3(1, 0, 1));
    float n011 = hash13(i + vec3(0, 1, 1));
    float n111 = hash13(i + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

// Turbulence: three octaves of the noise, each half the size and weight.
float turbulence(vec3 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 3; ++i) {
        sum += amp * noise3(p);
        p = p * 2.03 + vec3(1.7, 9.2, 3.1);
        amp *= 0.5;
    }
    return sum / 0.875;
}

// Distance from the eye of a depth-buffer value (zero to one depth).
float viewDistance(float depth) {
    return projection[3][2] / (depth + projection[2][2]);
}

// How much flame there is at a point, u its place in the box (0..1, z up),
// size the box's in yards.
float flameDensity(vec3 u, vec3 size, float t) {
    vec2 q = (u.xy - 0.5) * 2.0;               // -1..1 across the grate
    float h = u.z;
    // A grate is long one way: along it the fire burns as a row of tongues
    // a third of a yard apart, across it as one.
    bool xLong = size.x >= size.y;
    float along = xLong ? q.x : q.y;
    float across = xLong ? q.y : q.x;
    float longYards = max(size.x, size.y);
    // The tongues: noise rising with time - faster as it climbs, as hot air
    // does - finer along the grate, leaning with a slow sway.
    vec3 np = vec3(along * longYards / 0.33, across * 1.2, h * 2.6 - t * 1.6) + vec3(push.params.y);
    np.xy += vec2(sin(t * 0.7 + h * 3.0), cos(t * 0.5 + h * 2.0)) * 0.25 * h;
    float n = turbulence(np);
    // The shape: the whole grate burns at the bottom; up the flames narrow
    // across and thin, and the noise eats more of them the higher they go.
    float edgeAlong = 1.0 - smoothstep(0.75, 1.0, abs(along));
    float widthAcross = mix(0.95, 0.4, h);
    float body = (1.0 - abs(across) / widthAcross) * edgeAlong;
    float density = body * 1.3 + (n - 0.5) * (0.8 + h * 2.2) - h * 1.35;
    return clamp(density, 0.0, 1.0) * smoothstep(0.0, 0.06, h) * (1.0 - smoothstep(0.85, 1.0, h));
}

// Black-body-ish: deep red where the flame is thin and high, through orange
// to a pale yellow in the dense heart near the logs.
vec3 flameColor(float density, float h) {
    float heat = clamp(density * 1.25 - h * 0.35, 0.0, 1.0);
    vec3 red = vec3(0.55, 0.06, 0.01);
    vec3 orange = vec3(1.0, 0.38, 0.06);
    vec3 yellow = vec3(1.0, 0.86, 0.45);
    vec3 c = mix(red, orange, smoothstep(0.0, 0.5, heat));
    return mix(c, yellow, smoothstep(0.5, 1.0, heat));
}

void main() {
    vec3 ro = viewPos.xyz;
    vec3 rd = normalize(vWorldPos - ro);
    vec3 bmin = push.boxMin.xyz;
    vec3 bmax = push.boxMax.xyz;

    // Where the ray is in the box.
    vec3 inv = 1.0 / rd;
    vec3 t0 = (bmin - ro) * inv;
    vec3 t1 = (bmax - ro) * inv;
    vec3 tmin = min(t0, t1);
    vec3 tmax = max(t0, t1);
    float tNear = max(max(max(tmin.x, tmin.y), tmin.z), 0.0);
    float tFar = min(min(tmax.x, tmax.y), tmax.z);
    // Every face of the box is drawn; only the far one, where the ray leaves
    // it, shades - whichever way the triangles wind, once a pixel.
    float tHere = length(vWorldPos - ro);
    if (abs(tHere - tFar) > 0.002 * tFar + 0.002) discard;

    // And where the scene stops it: the depth under the pixel, as a distance
    // along the ray rather than along the view axis.
    float sceneDepth = texelFetch(uSceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    vec3 forward = -vec3(view[0][2], view[1][2], view[2][2]);
    float along = max(dot(rd, forward), 1e-3);
    float tScene = viewDistance(sceneDepth) / along;
    tFar = min(tFar, tScene);
    if (tFar <= tNear) discard;

    int steps = int(push.params.x);
    float dt = (tFar - tNear) / float(steps);
    // Jittered start, so the steps do not show as slices.
    float jitter = hash13(vec3(gl_FragCoord.xy, push.boxMin.w * 60.0));
    vec3 size = bmax - bmin;
    float t = push.boxMin.w;
    vec3 light = vec3(0.0);
    float transmit = 1.0;
    for (int i = 0; i < steps; ++i) {
        vec3 p = ro + rd * (tNear + (float(i) + jitter) * dt);
        vec3 u = (p - bmin) / size;
        float d = flameDensity(u, size, t);
        if (d > 0.001) {
            float a = 1.0 - exp(-d * dt * 6.0);
            light += transmit * a * flameColor(d, u.z) * (0.95 + 1.3 * d);
            transmit *= 1.0 - a * 0.35;
            if (transmit < 0.05) break;
        }
    }
    outColor = vec4(light * push.boxMax.w, 0.0);
}
