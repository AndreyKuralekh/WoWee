#version 450

// The hands lit as the world round them is: its sun and its ambient light,
// so they are dim at night and warm at dusk. Light wraps a little round the
// fingers, as it does through skin, and a faint rim keeps the outline of a
// hand readable where the world is dark.

layout(set = 0, binding = 0) uniform HandData {
    mat4 skin[50];
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 skinColor;
} hand;

layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec4 eye;
} push;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vWorld;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 n = normalize(vNormal);
    vec3 toEye = normalize(push.eye.xyz - vWorld);
    // Both sides are drawn; a face seen from behind is lit as its front.
    if (dot(n, toEye) < 0.0) n = -n;
    vec3 toLight = normalize(-hand.lightDir.xyz);

    const float kWrap = 0.35;
    float diffuse = max((dot(n, toLight) + kWrap) / (1.0 + kWrap), 0.0);
    vec3 skin = hand.skinColor.rgb;
    vec3 color = skin * (hand.ambientColor.rgb + hand.lightColor.rgb * diffuse);

    float rim = pow(1.0 - max(dot(n, toEye), 0.0), 3.0);
    color += skin * rim * 0.18;
    // Never quite black: a hand that vanishes in a dark room is worse than one
    // a shade too bright.
    color = max(color, skin * 0.07);
    outColor = vec4(color, 1.0);
}
