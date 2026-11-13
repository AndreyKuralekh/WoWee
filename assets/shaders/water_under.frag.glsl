#version 450

// The world seen from under the water, in the headset (TASK_12, step 2).
// Drawn over an eye's finished scene and water, full screen, from the eye's
// copy of its scene: where both the eye and the point behind a pixel are under
// the water, the point wavers - by slow noise at the point itself, so the bed,
// the walls, the hands and the people sway the same in both eyes - and sinks
// into the water's body with distance (absorption, then haze). Where the eye
// is above the water (a meniscus across the eyes) or the water's surface lies
// between, the pixel is left to the water's own shader.

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
};

layout(set = 2, binding = 0) uniform sampler2D SceneColor;
layout(set = 2, binding = 1) uniform sampler2D SceneDepth;

layout(push_constant) uniform Push {
    vec4 waterColor;   // rgb the liquid's colour; a = 1 for the sea
    vec4 params;       // x = water level, y = seconds, z = sway strength, w = clarity
    vec4 params2;      // x = near, y = far
} push;

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outColor;

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float noise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash13(i), hash13(i + vec3(1, 0, 0)), f.x),
                   mix(hash13(i + vec3(0, 1, 0)), hash13(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash13(i + vec3(0, 0, 1)), hash13(i + vec3(1, 0, 1)), f.x),
                   mix(hash13(i + vec3(0, 1, 1)), hash13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

float linearizeDepth(float d, float n, float f) {
    return n * f / (f - d * (f - n));
}

void main() {
    float level = push.params.x;
    float seconds = push.params.y;
    float sway = push.params.z;
    float clarity = max(push.params.w, 0.1);

    // This pixel's ray, from the eye's own projection (its frustum is not
    // centred) into the world.
    vec2 ndc = TexCoord * 2.0 - 1.0;
    vec3 dirView = vec3((ndc.x + projection[2][0]) / projection[0][0],
                        (ndc.y + projection[2][1]) / projection[1][1], -1.0);
    mat3 toWorld = transpose(mat3(view));
    vec3 dir = normalize(toWorld * dirView);
    vec3 eye = viewPos.xyz;
    float nearPlane = push.params2.x;

    // The eye at this pixel, on the near plane: above the water, the pixel is
    // the dry world - the boundary is the meniscus across the eyes.
    vec3 nearPoint = eye + dir * (nearPlane / max(-dirView.z / length(dirView), 0.05));
    if (nearPoint.z > level) discard;

    vec3 forward = toWorld * vec3(0.0, 0.0, -1.0);
    float depth = texelFetch(SceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    float t = linearizeDepth(depth, nearPlane, push.params2.y) / max(dot(dir, forward), 0.05);
    // The surface between the eye and the point: the water's shader drew
    // that pixel - the world above as through the surface, or the dark of
    // its underside.
    if (dir.z > 0.0) {
        float tSurface = (level - eye.z) / dir.z;
        if (tSurface < t) discard;
    }
    t = min(t, 400.0);
    vec3 point = eye + dir * t;

    // The sway: two slow noises at the point, rising a little with time;
    // nothing at the eye, full a few yards out.
    float reach = smoothstep(0.3, 4.0, t);
    vec3 np = point * 0.9 + vec3(0.0, 0.0, seconds * 0.55);
    vec2 wobble = vec2(noise3(np), noise3(np + vec3(5.2, 1.3, 2.7))) - 0.5;
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(SceneColor, 0)) + wobble * (0.016 * sway * reach);
    vec3 scene = texture(SceneColor, clamp(uv, vec2(0.001), vec2(0.999))).rgb;

    // The water's body: red goes first, then green; and the haze it is lit by
    // is the liquid's colour under the sky's light, darker the deeper the eye.
    bool sea = push.waterColor.a > 0.5;
    vec3 absorption = (sea ? vec3(0.60, 0.15, 0.10) : vec3(0.46, 0.09, 0.06)) * 0.35 / clarity;
    float hazeDensity = (sea ? 0.045 : 0.035) / clarity;
    vec3 light = ambientColor.rgb * 1.2 + lightColor.rgb * 0.6;
    float eyeDepth = max(level - eye.z, 0.0);
    vec3 haze = push.waterColor.rgb * light * exp(-0.035 * eyeDepth);
    vec3 colour = scene * exp(-absorption * t);
    colour = mix(colour, haze, 1.0 - exp(-hazeDensity * t));
    // A touch of the water's colour on everything, the hands too.
    colour = mix(colour, haze, 0.12);
    outColor = vec4(colour, 1.0);
}
