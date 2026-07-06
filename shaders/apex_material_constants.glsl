#ifndef APEX_MATERIAL_CONSTANTS_GLSL
#define APEX_MATERIAL_CONSTANTS_GLSL

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

const int DEBUG_FINAL_LIT = 0;
const int DEBUG_BASE_COLOR = 1;
const int DEBUG_NORMAL = 2;
const int DEBUG_TANGENT = 3;
const int DEBUG_ROUGHNESS = 4;
const int DEBUG_SPECULAR_F0 = 5;
const int DEBUG_AO = 6;
const int DEBUG_CAVITY = 7;
const int DEBUG_OPACITY_COVERAGE = 8;
const int DEBUG_ANISOTROPY_DIRECTION = 9;
const int DEBUG_EMISSIVE = 10;
const int DEBUG_SCATTER_THICKNESS = 11;
const int DEBUG_TANGENT_VALIDITY = 12;
const int DEBUG_TRANSMITTANCE = 13;
const int DEBUG_MEAN_FREE_PATH = 14;
const int DEBUG_MEDIUM_THICKNESS = 15;
const int DEBUG_CLOSURE_COUNT = 16;
const int DEBUG_LAYERED_TRANSMITTANCE = 17;

const uint ALPHA_OPAQUE = 0;
const uint ALPHA_MASKED = 1;
const uint ALPHA_TRANSLUCENT = 2;
const uint ALPHA_ADDITIVE = 3;

const uint OPACITY_ONE = 0;
const uint OPACITY_TEXTURE = 1;
const uint OPACITY_ALBEDO_ALPHA = 2;
const uint OPACITY_MIN_ALBEDO_AND_TEXTURE = 3;

#endif
