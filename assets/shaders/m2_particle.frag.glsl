#version 450

// The first two matrices of the per-frame block, for the depth's distance.
layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
};

layout(set = 1, binding = 0) uniform sampler2D uTexture;
// A copy of the scene's depth, for soft particles (flags & 1); a stand-in
// texture otherwise, never read.
layout(set = 2, binding = 0) uniform sampler2D uSceneDepth;

layout(push_constant) uniform Push {
    vec2 tileCount;
    int alphaKey;
    int flags;      // 1 soft: fade into the scene; 2 blend an atlas frame into the next
} push;

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vTile;
layout(location = 2) in float vFogVisibility;

layout(location = 0) out vec4 outColor;

vec2 tileUv(float tile, vec2 p) {
    float tx = mod(tile, push.tileCount.x);
    float ty = floor(tile / push.tileCount.x);
    return (vec2(tx, ty) + p) / push.tileCount;
}

// Distance from the eye of a depth-buffer value, for the perspective the
// matrices give (zero to one depth).
float viewDistance(float depth) {
    return projection[3][2] / (depth + projection[2][2]);
}

void main() {
    vec2 p = gl_PointCoord;
    float tile = floor(vTile);
    vec4 texColor = texture(uTexture, tileUv(tile, p));
    // A flame's atlas is a few frames of a flicker; stepped from one to the
    // next it jumps, blended by how far between them the particle is it rolls.
    if ((push.flags & 2) != 0) {
        float tiles = push.tileCount.x * push.tileCount.y;
        float phase = vTile - tile;
        if (tiles > 1.0 && phase > 0.001) {
            vec4 nextColor = texture(uTexture, tileUv(mod(tile + 1.0, tiles), p));
            texColor = mix(texColor, nextColor, phase);
        }
    }

    if (push.alphaKey != 0) {
        float lum = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        if (lum < 0.05) discard;
    }

    // Soft circular falloff for point-sprite edges.
    float edge = 1.0 - smoothstep(0.4, 0.5, length(p - 0.5));
    float alpha = texColor.a * vColor.a * edge * vFogVisibility;

    // Soft particles: a sprite fades over the last stretch before the surface
    // behind it, instead of being cut by it in a hard line - the logs and the
    // firebox of a hearth, the ground under smoke.
    if ((push.flags & 1) != 0) {
        float sceneDepth = texelFetch(uSceneDepth, ivec2(gl_FragCoord.xy), 0).r;
        float gap = viewDistance(sceneDepth) - viewDistance(gl_FragCoord.z);
        alpha *= clamp(gap / 0.25, 0.0, 1.0);
    }

    vec3 rgb = texColor.rgb * vColor.rgb * alpha;
    outColor = vec4(rgb, alpha);
}
