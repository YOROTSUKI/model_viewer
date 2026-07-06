#pragma once

#include <algorithm>
#include <cmath>

namespace viewer {

constexpr float Pi = 3.14159265358979323846f;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct Mat4 {
    float m[16] = {};
};

inline float radians(float degrees) {
    return degrees * Pi / 180.0f;
}

inline Vec3 operator+(Vec3 lhs, Vec3 rhs) {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

inline Vec3 operator-(Vec3 lhs, Vec3 rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

inline Vec3 operator*(Vec3 value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

inline Vec3 operator/(Vec3 value, float scale) {
    return {value.x / scale, value.y / scale, value.z / scale};
}

inline float dot(Vec3 lhs, Vec3 rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

inline Vec3 cross(Vec3 lhs, Vec3 rhs) {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

inline float length(Vec3 value) {
    return std::sqrt(dot(value, value));
}

inline Vec3 normalize(Vec3 value) {
    const float len = length(value);
    if (len <= 0.000001f) {
        return {0.0f, 1.0f, 0.0f};
    }
    return value / len;
}

inline Mat4 identity() {
    Mat4 result{};
    result.m[0] = 1.0f;
    result.m[5] = 1.0f;
    result.m[10] = 1.0f;
    result.m[15] = 1.0f;
    return result;
}

inline Mat4 perspective(float fovyRadians, float aspect, float nearPlane, float farPlane) {
    const float f = 1.0f / std::tan(fovyRadians / 2.0f);

    Mat4 result{};
    result.m[0] = f / aspect;
    result.m[5] = -f;
    result.m[10] = farPlane / (nearPlane - farPlane);
    result.m[11] = -1.0f;
    result.m[14] = (farPlane * nearPlane) / (nearPlane - farPlane);
    return result;
}

inline Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
    const Vec3 f = normalize(center - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);

    Mat4 result = identity();
    result.m[0] = s.x;
    result.m[1] = u.x;
    result.m[2] = -f.x;
    result.m[4] = s.y;
    result.m[5] = u.y;
    result.m[6] = -f.y;
    result.m[8] = s.z;
    result.m[9] = u.z;
    result.m[10] = -f.z;
    result.m[12] = -dot(s, eye);
    result.m[13] = -dot(u, eye);
    result.m[14] = dot(f, eye);
    return result;
}

inline Mat4 rotateY(float radiansValue) {
    Mat4 result = identity();
    const float c = std::cos(radiansValue);
    const float s = std::sin(radiansValue);
    result.m[0] = c;
    result.m[2] = -s;
    result.m[8] = s;
    result.m[10] = c;
    return result;
}

} // namespace viewer
