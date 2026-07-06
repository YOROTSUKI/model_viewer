#version 450

layout(input_attachment_index = 0, binding = 0) uniform subpassInput opaqueColorInput;
layout(input_attachment_index = 1, binding = 1) uniform subpassInput accumInput;
layout(input_attachment_index = 2, binding = 2) uniform subpassInput revealInput;
layout(input_attachment_index = 3, binding = 3) uniform subpassInput additiveInput;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 opaqueColor = subpassLoad(opaqueColorInput);
    vec4 accum = subpassLoad(accumInput);
    float revealage = clamp(subpassLoad(revealInput).r, 0.0, 1.0);
    vec3 additive = subpassLoad(additiveInput).rgb;

    float transparentAlpha = 1.0 - revealage;
    vec3 transparentColor = accum.a > 0.00001 ? accum.rgb / accum.a : vec3(0.0);
    vec3 color = opaqueColor.rgb * revealage + transparentColor * transparentAlpha + additive;

    outColor = vec4(clamp(color, vec3(0.0), vec3(1.0)), 1.0);
}
