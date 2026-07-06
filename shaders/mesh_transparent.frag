#version 450
#extension GL_GOOGLE_include_directive : require

#include "apex_material_common.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec4 outAccum;
layout(location = 1) out vec4 outReveal;
layout(location = 2) out vec4 outAdditive;

void main() {
    vec3 geometricNormal = safeNormalize(inNormal, vec3(0.0, 1.0, 0.0));
    vec2 uv = inTexCoord;
    ApexMaterialSample material = sampleApexMaterial(uv);
    float coverage = clamp(material.coverage, 0.0, 1.0);
    if (coverage <= 0.005) {
        discard;
    }

    TangentFrame baseFrame = buildTangentFrame(geometricNormal, inTangent, inWorldPos, uv);
    SubstrateSlabApprox slab = buildSubstrateSlabApprox(material, baseFrame);
    vec3 color = materialDebugView() == DEBUG_FINAL_LIT
        ? evaluateSubstrateSlabApprox(slab, inWorldPos)
        : debugColorForDisplay(slab, inWorldPos);

    if (drawPush.alphaMode == ALPHA_TRANSLUCENT && materialDebugView() == DEBUG_FINAL_LIT) {
        color *= mix(slab.transmittance, vec3(1.0), coverage);
    }

    if (drawPush.alphaMode == ALPHA_ADDITIVE) {
        outAccum = vec4(0.0);
        outReveal = vec4(0.0);
        outAdditive = vec4(color * coverage, coverage);
        return;
    }

    float weight = clamp(coverage * 8.0 + 0.01, 0.01, 1.0);
    outAccum = vec4(color * coverage * weight, coverage * weight);
    outReveal = vec4(coverage);
    outAdditive = vec4(0.0);
}
