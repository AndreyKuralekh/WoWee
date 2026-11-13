#version 450

// Heat haze over a near fire (TASK_10, step 4): in a box over the flames the
// scene behind wavers - its copy, taken before the particles, is read back a
// little off where each pixel is, by noise that rises with time. Drawn with
// the fire volume's box vertex shader; every face is drawn and the far one
// shades, as there. What stands in front of the box (a mantel) is left as it
// is, and so is everything drawn after it - the flames and the particles.

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
layout(set = 2, binding = 0) uniform sampler2D uSceneColor;

layout(push_constant) uniform Push {
    vec4 boxMin;     // xyz; w = seconds
    vec4 boxMax;     // xyz; w = strength
    vec4 params;     // y = seed, z = a pool's level: nothing under it wavers (the water drawn
                     // after the copy would be lost from the shifted copy)
} push;

layout(location = 0) in vec3 vWorldPos;
layout(location = 0) out vec4 outColor;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float noise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), f.x),
               mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), f.x), f.y);
}

float viewDistance(float depth) {
    return projection[3][2] / (depth + projection[2][2]);
}

void main() {
    vec3 ro = viewPos.xyz;
    vec3 rd = normalize(vWorldPos - ro);
    vec3 bmin = push.boxMin.xyz;
    vec3 bmax = push.boxMax.xyz;
    vec3 inv = 1.0 / rd;
    vec3 t0 = (bmin - ro) * inv;
    vec3 t1 = (bmax - ro) * inv;
    vec3 tmin = min(t0, t1);
    vec3 tmax = max(t0, t1);
    float tNear = max(max(max(tmin.x, tmin.y), tmin.z), 0.0);
    float tFar = min(min(tmax.x, tmax.y), tmax.z);
    float tHere = length(vWorldPos - ro);
    if (abs(tHere - tFar) > 0.002 * tFar + 0.002) discard;

    // Something in front of the hot air is not seen through it.
    float sceneDepth = texelFetch(uSceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    vec3 forward = -vec3(view[0][2], view[1][2], view[2][2]);
    float tScene = viewDistance(sceneDepth) / max(dot(rd, forward), 1e-3);
    if (tScene <= tNear) discard;
    // Only the air over a waterfall's pool, not the water in it.
    if ((ro + rd * min(tScene, 1.0e4)).z < push.params.z) discard;

    // How strong the shimmer is where the ray crosses the box's middle:
    // strongest over the middle of the grate and low down, gone at the edges.
    vec3 mid = ro + rd * (0.5 * (tNear + min(tFar, tScene)));
    vec3 u = clamp((mid - bmin) / (bmax - bmin), 0.0, 1.0);
    vec2 q = (u.xy - 0.5) * 2.0;
    float edge = (1.0 - smoothstep(0.4, 1.0, abs(q.x))) * (1.0 - smoothstep(0.4, 1.0, abs(q.y)));
    float rise = smoothstep(0.0, 0.2, u.z) * (1.0 - smoothstep(0.5, 1.0, u.z));
    float strength = push.boxMax.w * edge * rise;
    // Untouched where there is no shimmer: the copy has no water in it.
    if (strength < 0.02) discard;

    // Rising ripples: noise in the box's own space, scrolled up.
    float t = push.boxMin.w;
    vec2 np = vec2((u.x + u.y) * 6.0 + push.params.y, u.z * 5.0 - t * 2.4);
    vec2 offset = vec2(noise2(np) - 0.5, noise2(np + vec2(17.3, 5.1)) - 0.5);

    vec2 size = vec2(textureSize(uSceneColor, 0));
    vec2 uv = gl_FragCoord.xy / size + offset * strength * 0.012;
    outColor = vec4(texture(uSceneColor, uv).rgb, 1.0);
}
