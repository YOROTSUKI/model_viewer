#ifndef APEX_MATERIAL_COMMON_GLSL
#define APEX_MATERIAL_COMMON_GLSL

#include "apex_material_constants.glsl"

layout(binding = 0) uniform CameraUbo {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec4 lightDirection;
    vec4 cameraPosition;
    vec4 apexFlags;
    vec4 apexFactors0;
    vec4 apexFactors1;
    vec4 apexSubsurfaceColor;
    vec4 apexEmissiveTint;
    vec4 apexDebug;
} camera;

layout(binding = 1) uniform sampler2D apexTextures[APEX_TEXTURE_DESCRIPTOR_COUNT];

layout(push_constant) uniform DrawPush {
    uint materialIndex;
    uint alphaMode;
    uint opacitySource;
    uint opacityChannel;
} drawPush;

struct TangentFrame {
    vec3 n;
    vec3 t;
    vec3 b;
    float sign;
    float valid;
};

struct ApexMaterialSample {
    vec4 albedo;
    vec4 normalMap;
    float roughness;
    vec3 f0;
    float ao;
    float cavity;
    float coverage;
    float thickness;
    vec3 anisotropyVector;
    vec3 emissive;
};

struct SubstrateSlabApprox {
    vec3 baseColor;
    vec3 f0;
    float roughness;
    vec3 normal;
    vec3 tangent;
    vec3 bitangent;
    vec3 anisotropyDirection;
    float anisotropyStrength;
    float ao;
    float cavity;
    vec3 emissive;
    float coverage;
    vec3 transmittance;
    float thickness;
    vec3 mfpColorOrScale;
    float tangentFrameValidity;
};

int textureIndex(int kind) {
    return int(drawPush.materialIndex) * APEX_TEXTURE_KIND_COUNT + kind;
}

int materialDebugView() {
    return int(camera.apexDebug.x + 0.5);
}

float channelValue(vec4 value, uint channel) {
    if (channel == 1) {
        return value.g;
    }
    if (channel == 2) {
        return value.b;
    }
    if (channel == 3) {
        return value.a;
    }
    return value.r;
}

vec3 safeNormalize(vec3 value, vec3 fallback) {
    float len2 = dot(value, value);
    if (len2 > 1e-8) {
        return value * inversesqrt(len2);
    }
    return fallback;
}

vec3 fallbackTangent(vec3 normal) {
    vec3 axis = abs(normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    return safeNormalize(cross(axis, normal), vec3(1.0, 0.0, 0.0));
}

TangentFrame makeFrame(vec3 normal, vec3 tangent, vec3 bitangent, float sign, float valid) {
    TangentFrame frame;
    frame.n = safeNormalize(normal, vec3(0.0, 1.0, 0.0));
    frame.t = safeNormalize(tangent, fallbackTangent(frame.n));
    frame.b = safeNormalize(bitangent, cross(frame.n, frame.t) * sign);
    frame.sign = sign < 0.0 ? -1.0 : 1.0;
    frame.valid = valid;
    return frame;
}

TangentFrame derivativeTangentFrame(vec3 normal, vec3 position, vec2 uv) {
    vec3 n = safeNormalize(normal, vec3(0.0, 1.0, 0.0));
    vec3 dp1 = dFdx(position);
    vec3 dp2 = dFdy(position);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    vec3 dp2perp = cross(dp2, n);
    vec3 dp1perp = cross(n, dp1);
    vec3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;
    float maxLen2 = max(dot(tangent, tangent), dot(bitangent, bitangent));
    if (maxLen2 <= 1e-8) {
        tangent = fallbackTangent(n);
        bitangent = cross(n, tangent);
        return makeFrame(n, tangent, bitangent, 1.0, 0.0);
    }

    float sign = dot(cross(n, tangent), bitangent) < 0.0 ? -1.0 : 1.0;
    tangent = safeNormalize(tangent, fallbackTangent(n));
    bitangent = safeNormalize(bitangent, cross(n, tangent) * sign);
    return makeFrame(n, tangent, bitangent, sign, 0.0);
}

TangentFrame buildTangentFrame(vec3 geometricNormal, vec4 vertexTangent, vec3 position, vec2 uv) {
    vec3 n = safeNormalize(geometricNormal, vec3(0.0, 1.0, 0.0));
    vec3 tangent = vertexTangent.xyz;
    if (abs(vertexTangent.w) > 0.5 && dot(tangent, tangent) > 1e-8) {
        tangent = tangent - n * dot(n, tangent);
        if (dot(tangent, tangent) > 1e-8) {
            float sign = vertexTangent.w < 0.0 ? -1.0 : 1.0;
            tangent = normalize(tangent);
            return makeFrame(n, tangent, cross(n, tangent) * sign, sign, 1.0);
        }
    }
    return derivativeTangentFrame(n, position, uv);
}

TangentFrame reframeForNormal(TangentFrame baseFrame, vec3 normal) {
    vec3 n = safeNormalize(normal, baseFrame.n);
    vec3 tangent = baseFrame.t - n * dot(n, baseFrame.t);
    if (dot(tangent, tangent) <= 1e-8) {
        tangent = fallbackTangent(n);
    }
    tangent = normalize(tangent);
    vec3 bitangent = safeNormalize(cross(n, tangent) * baseFrame.sign, cross(n, tangent));
    return makeFrame(n, tangent, bitangent, baseFrame.sign, baseFrame.valid);
}

float sampleCoverage(vec4 albedoSample, vec2 uv) {
    if (drawPush.alphaMode == ALPHA_OPAQUE || drawPush.opacitySource == OPACITY_ONE) {
        return 1.0;
    }

    vec4 opacitySample = texture(apexTextures[textureIndex(TEX_OPACITY)], uv);
    float textureOpacity = channelValue(opacitySample, drawPush.opacityChannel);
    if (drawPush.opacitySource == OPACITY_TEXTURE) {
        return textureOpacity;
    }
    if (drawPush.opacitySource == OPACITY_ALBEDO_ALPHA) {
        return albedoSample.a;
    }
    return min(albedoSample.a, textureOpacity);
}

ApexMaterialSample sampleApexMaterial(vec2 uv) {
    ApexMaterialSample material;
    material.albedo = texture(apexTextures[textureIndex(TEX_ALBEDO)], uv);
    material.normalMap = texture(apexTextures[textureIndex(TEX_NORMAL)], uv);

    float gloss = texture(apexTextures[textureIndex(TEX_GLOSS)], uv).r;
    material.roughness = clamp((1.0 - gloss) * camera.apexFactors0.x, 0.02, 1.0);
    material.f0 = clamp(texture(apexTextures[textureIndex(TEX_SPECULAR)], uv).rgb * camera.apexFactors0.y, vec3(0.0), vec3(0.98));

    material.ao = pow(max(texture(apexTextures[textureIndex(TEX_AO)], uv).r, 0.001), max(camera.apexFactors0.z, 0.0));
    material.cavity = pow(max(texture(apexTextures[textureIndex(TEX_CAVITY)], uv).r, 0.001), max(camera.apexFactors0.w, 0.0));
    material.coverage = clamp(sampleCoverage(material.albedo, uv), 0.0, 1.0);
    material.thickness = max(texture(apexTextures[textureIndex(TEX_THICKNESS)], uv).r * camera.apexFactors1.w, 0.0);
    material.anisotropyVector = texture(apexTextures[textureIndex(TEX_ANISOTROPY)], uv).xyz * 2.0 - 1.0;
    material.emissive =
        texture(apexTextures[textureIndex(TEX_EMISSIVE)], uv).rgb *
        texture(apexTextures[textureIndex(TEX_EMISSIVE_MULTIPLY)], uv).rgb *
        camera.apexEmissiveTint.rgb *
        camera.apexFactors1.x;
    return material;
}

vec3 cheapMfpFromSubsurfaceControls() {
    float strength = clamp(camera.apexFactors1.z, 0.0, 1.0);
    vec3 tint = max(camera.apexSubsurfaceColor.rgb, vec3(0.02));
    return mix(vec3(1.0), tint * 2.0 + vec3(0.05), strength);
}

SubstrateSlabApprox buildSubstrateSlabApprox(ApexMaterialSample material, TangentFrame baseFrame) {
    vec3 tangentNormal = material.normalMap.xyz * 2.0 - 1.0;
    if (camera.apexFlags.y > 0.5) {
        tangentNormal.y = -tangentNormal.y;
    }
    tangentNormal = safeNormalize(tangentNormal, vec3(0.0, 0.0, 1.0));

    vec3 mappedNormal = safeNormalize(mat3(baseFrame.t, baseFrame.b, baseFrame.n) * tangentNormal, baseFrame.n);
    TangentFrame shadingFrame = reframeForNormal(baseFrame, mappedNormal);

    vec2 anisoVector = material.anisotropyVector.xy;
    float anisoVectorLength = length(anisoVector);
    float anisoEnabled = camera.apexFlags.w > 0.5 ? 1.0 : 0.0;
    float anisoStrength = anisoEnabled *
        clamp(camera.apexSubsurfaceColor.w, 0.0, 0.95) *
        clamp(anisoVectorLength, 0.0, 1.0);
    vec3 anisoDirection = shadingFrame.t;
    if (anisoVectorLength > 0.02) {
        anisoDirection = safeNormalize(
            shadingFrame.t * anisoVector.x + shadingFrame.b * anisoVector.y,
            shadingFrame.t);
    } else {
        anisoStrength = 0.0;
    }

    // Apex texture scans do not provide a real Substrate mean-free-path input.
    // This is a cheap visual Beer-Lambert approximation derived from the
    // existing subsurface tint/strength controls, not UE Substrate parity.
    float subsurfaceStrength = max(camera.apexFactors1.z, 0.0);
    vec3 mfp = cheapMfpFromSubsurfaceControls();
    vec3 transmittance = camera.apexFlags.z > 0.5 && subsurfaceStrength > 0.001
        ? exp(-material.thickness / max(mfp, vec3(1e-3)))
        : vec3(1.0);

    SubstrateSlabApprox slab;
    slab.baseColor = material.albedo.rgb;
    slab.f0 = material.f0;
    slab.roughness = material.roughness;
    slab.normal = shadingFrame.n;
    slab.tangent = shadingFrame.t;
    slab.bitangent = shadingFrame.b;
    slab.anisotropyDirection = anisoDirection;
    slab.anisotropyStrength = anisoStrength;
    slab.ao = material.ao;
    slab.cavity = material.cavity;
    slab.emissive = material.emissive;
    slab.coverage = material.coverage;
    slab.transmittance = clamp(transmittance, vec3(0.0), vec3(1.0));
    slab.thickness = material.thickness;
    slab.mfpColorOrScale = mfp;
    slab.tangentFrameValidity = shadingFrame.valid;
    return slab;
}

float distributionGGX(vec3 normal, vec3 halfVector, float roughness) {
    float alpha = max(roughness * roughness, 0.001);
    float alpha2 = alpha * alpha;
    float nDotH = max(dot(normal, halfVector), 0.0);
    float nDotH2 = nDotH * nDotH;
    float denom = nDotH2 * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(PI * denom * denom, 0.0001);
}

float D_GGX_Anisotropic(vec3 normal, vec3 halfVector, vec3 tangent, vec3 bitangent, float alphaX, float alphaY) {
    float nDotH = max(dot(normal, halfVector), 0.0);
    if (nDotH <= 0.0) {
        return 0.0;
    }

    float tDotH = dot(tangent, halfVector);
    float bDotH = dot(bitangent, halfVector);
    float denom =
        (tDotH * tDotH) / max(alphaX * alphaX, 0.000001) +
        (bDotH * bDotH) / max(alphaY * alphaY, 0.000001) +
        nDotH * nDotH;
    return 1.0 / max(PI * alphaX * alphaY * denom * denom, 0.0001);
}

void anisotropicAlphas(float roughness, float anisotropy, out float alphaX, out float alphaY) {
    float alpha = max(roughness * roughness, 0.001);
    float aspect = sqrt(clamp(1.0 - 0.9 * anisotropy, 0.1, 1.0));
    alphaX = max(alpha / aspect, 0.001);
    alphaY = max(alpha * aspect, 0.001);
}

float geometrySchlickGGX(float nDotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

float geometrySmith(vec3 normal, vec3 viewDir, vec3 lightDir, float roughness) {
    float nDotV = max(dot(normal, viewDir), 0.0);
    float nDotL = max(dot(normal, lightDir), 0.0);
    return geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 evaluateSubstrateSlabApprox(SubstrateSlabApprox slab, vec3 worldPos) {
    vec3 viewDir = normalize(camera.cameraPosition.xyz - worldPos);
    vec3 lightDir = normalize(-camera.lightDirection.xyz);
    vec3 halfVector = normalize(viewDir + lightDir);

    float nDotL = max(dot(slab.normal, lightDir), 0.0);
    float nDotV = max(dot(slab.normal, viewDir), 0.0);
    vec3 radiance = vec3(3.2);

    float d = distributionGGX(slab.normal, halfVector, slab.roughness);
    if (slab.anisotropyStrength > 0.001) {
        float alphaX;
        float alphaY;
        anisotropicAlphas(slab.roughness, slab.anisotropyStrength, alphaX, alphaY);
        vec3 anisotropicBitangent = safeNormalize(cross(slab.normal, slab.anisotropyDirection), slab.bitangent);
        d = D_GGX_Anisotropic(slab.normal, halfVector, slab.anisotropyDirection, anisotropicBitangent, alphaX, alphaY);
    }

    float g = geometrySmith(slab.normal, viewDir, lightDir, slab.roughness);
    vec3 f = fresnelSchlick(max(dot(halfVector, viewDir), 0.0), slab.f0);
    vec3 specular = (d * g * f) / max(4.0 * nDotV * nDotL, 0.001);

    vec3 diffuseEnergy = vec3(1.0) - f;
    vec3 diffuse = diffuseEnergy * slab.baseColor / PI;
    vec3 direct = (diffuse * slab.cavity + specular) * radiance * nDotL;
    vec3 ambient = slab.baseColor * 0.20 * slab.ao * mix(1.0, slab.cavity, 0.65);

    if (camera.apexFlags.z > 0.5) {
        float backLight = pow(max(dot(-slab.normal, lightDir), 0.0), 1.4);
        float wrap = max((dot(slab.normal, lightDir) + 0.35) / 1.35, 0.0) - nDotL;
        vec3 absorption = vec3(1.0) - slab.transmittance;
        vec3 subsurface = camera.apexSubsurfaceColor.rgb * slab.baseColor *
            (backLight + max(wrap, 0.0) * 0.6) * absorption * camera.apexFactors1.z;
        direct += subsurface;
    }

    return direct + ambient + slab.emissive;
}

vec3 toneMapLinearToLdr(vec3 color) {
    color = max(color, vec3(0.0));
    return color / (color + vec3(1.0));
}

vec3 displayDebugToLinearHdr(vec3 color) {
    color = clamp(color, vec3(0.0), vec3(0.98));
    return color / max(vec3(1.0) - color, vec3(0.001));
}

vec3 debugColorForDisplay(SubstrateSlabApprox slab, vec3 worldPos) {
    int mode = materialDebugView();
    vec3 color = toneMapLinearToLdr(evaluateSubstrateSlabApprox(slab, worldPos));
    if (mode == DEBUG_BASE_COLOR) {
        color = clamp(slab.baseColor, vec3(0.0), vec3(1.0));
    } else if (mode == DEBUG_NORMAL) {
        color = normalize(slab.normal) * 0.5 + 0.5;
    } else if (mode == DEBUG_TANGENT) {
        color = normalize(slab.tangent) * 0.5 + 0.5;
    } else if (mode == DEBUG_TANGENT_VALIDITY) {
        color = slab.tangentFrameValidity > 0.5 ? vec3(0.1, 1.0, 0.25) : vec3(1.0, 0.05, 0.05);
    } else if (mode == DEBUG_ROUGHNESS) {
        color = vec3(slab.roughness);
    } else if (mode == DEBUG_SPECULAR_F0) {
        color = clamp(slab.f0, vec3(0.0), vec3(1.0));
    } else if (mode == DEBUG_AO) {
        color = vec3(slab.ao);
    } else if (mode == DEBUG_CAVITY) {
        color = vec3(slab.cavity);
    } else if (mode == DEBUG_OPACITY_COVERAGE) {
        color = vec3(clamp(slab.coverage, 0.0, 1.0));
    } else if (mode == DEBUG_ANISOTROPY_DIRECTION) {
        color = normalize(slab.anisotropyDirection) * 0.5 + 0.5;
    } else if (mode == DEBUG_EMISSIVE) {
        color = toneMapLinearToLdr(slab.emissive);
    } else if (mode == DEBUG_SCATTER_THICKNESS) {
        color = vec3(clamp(slab.thickness, 0.0, 1.0));
    } else if (mode == DEBUG_TRANSMITTANCE) {
        color = clamp(slab.transmittance, vec3(0.0), vec3(1.0));
    }
    return displayDebugToLinearHdr(color);
}

#endif
