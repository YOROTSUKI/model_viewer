#version 450
#extension GL_GOOGLE_include_directive : require

#include "apex_material_common.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec4 outColor;

vec3 fallbackPreview(vec3 normal, vec2 uv) {
    vec3 light = normalize(-camera.lightDirection.xyz);
    float lit = max(dot(normal, light), 0.0) * 0.75 + 0.25;
    vec3 normalTint = normalize(normal) * 0.5 + 0.5;
    vec3 baseColor = mix(vec3(0.55, 0.60, 0.66), normalTint, 0.35);
    baseColor += vec3(uv, 0.0) * 0.08;
    return baseColor * lit;
}

void main() {
    vec3 geometricNormal = safeNormalize(inNormal, vec3(0.0, 1.0, 0.0));
    if (camera.apexFlags.x < 0.5) {
        outColor = vec4(fallbackPreview(geometricNormal, inTexCoord), 1.0);
        return;
    }

    vec2 uv = inTexCoord;
    ApexMaterialSample material = sampleApexMaterial(uv);
    if (drawPush.alphaMode == ALPHA_MASKED && material.coverage < camera.apexFactors1.y) {
        discard;
    }
    if ((drawPush.alphaMode == ALPHA_TRANSLUCENT || drawPush.alphaMode == ALPHA_ADDITIVE) && material.coverage <= 0.005) {
        discard;
    }

    TangentFrame baseFrame = buildTangentFrame(geometricNormal, inTangent, inWorldPos, uv);
    SubstrateSlabApprox slab = buildSubstrateSlabApprox(material, baseFrame);
    vec3 color = materialDebugView() == DEBUG_FINAL_LIT
        ? evaluateSubstrateSlabApprox(slab, inWorldPos)
        : debugColorForDisplay(slab, inWorldPos);

    float coverage = material.coverage;
    if (drawPush.alphaMode == ALPHA_OPAQUE || drawPush.alphaMode == ALPHA_MASKED) {
        coverage = 1.0;
    }
    outColor = vec4(color, coverage);
}
