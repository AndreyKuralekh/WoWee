#version 450

// The glow over a headset eye: the original client's full-screen glow, which
// softens what is bright and lets it bleed into what is round it. Three uses
// of one shader, picked by mode:
//   0 - from the eye, down to a quarter: what is brighter than the threshold
//   1 - a Gaussian along dir, for the two blur passes
//   2 - the blurred result, scaled by strength, added onto the eye

layout(set = 0, binding = 0) uniform sampler2D uSource;

layout(push_constant) uniform Push {
    vec2 texel;      // one texel of the source
    vec2 dir;        // blur direction, (1,0) or (0,1)
    int mode;
    float threshold; // where glow starts, in luminance
    float strength;  // how much of it is added
    float pad;
} push;

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    if (push.mode == 0) {
        // Four bilinear taps cover a 4x4 block of the eye, so nothing between
        // the quarter-size texels is skipped and a small bright thing does
        // not flicker in and out as the head moves.
        vec3 c = texture(uSource, TexCoord + push.texel * vec2(-1.0, -1.0)).rgb;
        c += texture(uSource, TexCoord + push.texel * vec2( 1.0, -1.0)).rgb;
        c += texture(uSource, TexCoord + push.texel * vec2(-1.0,  1.0)).rgb;
        c += texture(uSource, TexCoord + push.texel * vec2( 1.0,  1.0)).rgb;
        c *= 0.25;
        float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
        // A soft knee rather than a cut, so the glow grows in as a surface
        // brightens instead of switching on.
        float k = smoothstep(push.threshold, 1.0, lum);
        outColor = vec4(c * k, 1.0);
    } else if (push.mode == 1) {
        // Nine taps of a Gaussian through five bilinear fetches.
        vec2 o1 = push.dir * push.texel * 1.3846153846;
        vec2 o2 = push.dir * push.texel * 3.2307692308;
        vec3 c = texture(uSource, TexCoord).rgb * 0.2270270270;
        c += texture(uSource, TexCoord + o1).rgb * 0.3162162162;
        c += texture(uSource, TexCoord - o1).rgb * 0.3162162162;
        c += texture(uSource, TexCoord + o2).rgb * 0.0702702703;
        c += texture(uSource, TexCoord - o2).rgb * 0.0702702703;
        outColor = vec4(c, 1.0);
    } else {
        outColor = vec4(texture(uSource, TexCoord).rgb * push.strength, 1.0);
    }
}
