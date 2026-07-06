#version 450

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

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outTexCoord;
layout(location = 3) out vec4 outTangent;

void main() {
    vec4 worldPos = camera.model * vec4(inPosition, 1.0);
    vec3 normal = normalize(mat3(camera.model) * inNormal);
    vec3 tangent = mat3(camera.model) * inTangent.xyz;

    gl_Position = camera.proj * camera.view * worldPos;
    outWorldPos = worldPos.xyz;
    outNormal = normal;
    outTexCoord = inTexCoord;
    outTangent = vec4(tangent, inTangent.w);
}
