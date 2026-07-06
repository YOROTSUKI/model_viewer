#version 450

const float PI = 3.14159265359;
const int APEX_TEXTURE_KIND_COUNT = 11;
const int APEX_TEXTURE_DESCRIPTOR_COUNT = 176;

const int TEX_ALBEDO = 0;
const int TEX_NORMAL = 1;
const int TEX_GLOSS = 2;
const int TEX_SPECULAR = 3;
const int TEX_AO = 4;
const int TEX_CAVITY = 5;
const int TEX_OPACITY = 6;
const int TEX_THICKNESS = 7;
const int TEX_ANISOTROPY = 8;
const int TEX_EMISSIVE = 9;
const int TEX_EMISSIVE_MULTIPLY = 10;

const uint ALPHA_OPAQUE = 0;
const uint ALPHA_MASKED = 1;
const uint ALPHA_TRANSLUCENT = 2;
const uint ALPHA_ADDITIVE = 3;

const uint OPACITY_ONE = 0;
const uint OPACITY_TEXTURE = 1;
const uint OPACITY_ALBEDO_ALPHA = 2;
const uint OPACITY_MIN_ALBEDO_AND_TEXTURE = 3;

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
} camera;

layout(binding = 1) uniform sampler2D apexTextures[APEX_TEXTURE_DESCRIPTOR_COUNT];

layout(push_constant) uniform DrawPush {
    uint materialIndex;
    uint alphaMode;
    uint opacitySource;
    uint opacityChannel;
} drawPush;

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;

int textureIndex(int kind) {
    return int(drawPush.materialIndex) * APEX_TEXTURE_KIND_COUNT + kind;
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

float sampleOpacity(vec4 albedoSample, vec2 uv) {
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

mat3 cotangentFrame(vec3 normal, vec3 position, vec2 uv) {
    vec3 dp1 = dFdx(position);
    vec3 dp2 = dFdy(position);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    vec3 dp2perp = cross(dp2, normal);
    vec3 dp1perp = cross(normal, dp1);
    vec3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;

    float invMax = inversesqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-8));
    return mat3(tangent * invMax, bitangent * invMax, normal);
}

vec3 sampleNormal(vec3 geometricNormal, vec3 worldPos, vec2 uv) {
    vec3 tangentNormal = texture(apexTextures[textureIndex(TEX_NORMAL)], uv).xyz * 2.0 - 1.0;
    if (camera.apexFlags.y > 0.5) {
        tangentNormal.y = -tangentNormal.y;
    }

    mat3 tbn = cotangentFrame(geometricNormal, worldPos, uv);
    return normalize(tbn * tangentNormal);
}

float distributionGGX(vec3 normal, vec3 halfVector, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float nDotH = max(dot(normal, halfVector), 0.0);
    float nDotH2 = nDotH * nDotH;
    float denom = nDotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.0001);
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

vec3 fallbackPreview(vec3 normal, vec2 uv) {
    vec3 light = normalize(-camera.lightDirection.xyz);
    float lit = max(dot(normal, light), 0.0) * 0.75 + 0.25;
    vec3 normalTint = normalize(normal) * 0.5 + 0.5;
    vec3 baseColor = mix(vec3(0.55, 0.60, 0.66), normalTint, 0.35);
    baseColor += vec3(uv, 0.0) * 0.08;
    return baseColor * lit;
}

void main() {
    vec3 geometricNormal = normalize(inNormal);
    if (camera.apexFlags.x < 0.5) {
        outColor = vec4(fallbackPreview(geometricNormal, inTexCoord), 1.0);
        return;
    }

    vec2 uv = inTexCoord;
    vec4 albedoSample = texture(apexTextures[textureIndex(TEX_ALBEDO)], uv);
    vec3 baseColor = albedoSample.rgb;
    float opacity = sampleOpacity(albedoSample, uv);
    if (drawPush.alphaMode == ALPHA_MASKED && opacity < camera.apexFactors1.y) {
        discard;
    }
    if ((drawPush.alphaMode == ALPHA_TRANSLUCENT || drawPush.alphaMode == ALPHA_ADDITIVE) && opacity <= 0.005) {
        discard;
    }

    vec3 normal = sampleNormal(geometricNormal, inWorldPos, uv);
    vec3 viewDir = normalize(camera.cameraPosition.xyz - inWorldPos);
    vec3 lightDir = normalize(-camera.lightDirection.xyz);
    vec3 halfVector = normalize(viewDir + lightDir);

    float gloss = texture(apexTextures[textureIndex(TEX_GLOSS)], uv).r;
    float roughness = clamp((1.0 - gloss) * camera.apexFactors0.x, 0.02, 1.0);
    vec3 f0 = clamp(texture(apexTextures[textureIndex(TEX_SPECULAR)], uv).rgb * camera.apexFactors0.y, vec3(0.0), vec3(0.98));

    float ao = pow(max(texture(apexTextures[textureIndex(TEX_AO)], uv).r, 0.001), max(camera.apexFactors0.z, 0.0));
    float cavity = pow(max(texture(apexTextures[textureIndex(TEX_CAVITY)], uv).r, 0.001), max(camera.apexFactors0.w, 0.0));

    float nDotL = max(dot(normal, lightDir), 0.0);
    float nDotV = max(dot(normal, viewDir), 0.0);
    vec3 radiance = vec3(3.2);

    float d = distributionGGX(normal, halfVector, roughness);
    float g = geometrySmith(normal, viewDir, lightDir, roughness);
    vec3 f = fresnelSchlick(max(dot(halfVector, viewDir), 0.0), f0);
    vec3 specular = (d * g * f) / max(4.0 * nDotV * nDotL, 0.001);

    if (camera.apexFlags.w > 0.5) {
        vec3 anisoSample = texture(apexTextures[textureIndex(TEX_ANISOTROPY)], uv).xyz * 2.0 - 1.0;
        mat3 tbn = cotangentFrame(normal, inWorldPos, uv);
        vec3 anisoDir = normalize(tbn * normalize(vec3(anisoSample.xy, 0.001)));
        float anisoHighlight = pow(abs(dot(halfVector, anisoDir)), 16.0) * camera.apexSubsurfaceColor.w;
        specular += f * anisoHighlight * (1.0 - roughness) * 0.35;
    }

    vec3 diffuseEnergy = vec3(1.0) - f;
    vec3 diffuse = diffuseEnergy * baseColor / PI;
    vec3 direct = (diffuse * cavity + specular) * radiance * nDotL;

    vec3 ambient = baseColor * 0.20 * ao * mix(1.0, cavity, 0.65);

    if (camera.apexFlags.z > 0.5) {
        float thickness = texture(apexTextures[textureIndex(TEX_THICKNESS)], uv).r * camera.apexFactors1.w;
        float backLight = pow(max(dot(-normal, lightDir), 0.0), 1.4);
        float wrap = max((dot(normal, lightDir) + 0.35) / 1.35, 0.0) - nDotL;
        vec3 subsurface = camera.apexSubsurfaceColor.rgb * baseColor *
            (backLight + max(wrap, 0.0) * 0.6) * thickness * camera.apexFactors1.z;
        direct += subsurface;
    }

    vec3 emissive = texture(apexTextures[textureIndex(TEX_EMISSIVE)], uv).rgb *
        texture(apexTextures[textureIndex(TEX_EMISSIVE_MULTIPLY)], uv).rgb *
        camera.apexEmissiveTint.rgb *
        camera.apexFactors1.x;

    vec3 color = direct + ambient + emissive;
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    if (drawPush.alphaMode == ALPHA_OPAQUE || drawPush.alphaMode == ALPHA_MASKED) {
        opacity = 1.0;
    }
    outColor = vec4(color, opacity);
}
