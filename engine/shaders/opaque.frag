#version 460

#include "globals.h"

// http://www.jcgt.org/published/0009/03/02/
uvec3 pcg3d(uvec3 v) {

    v = v * 1664525u + 1013904223u;

    v.x += v.y*v.z;
    v.y += v.z*v.x;
    v.z += v.x*v.y;

    v ^= v >> 16u;

    v.x += v.y*v.z;
    v.y += v.z*v.x;
    v.z += v.x*v.y;

    return v;
}

uvec3 hash(vec2 s)
{
    uvec4 u = uvec4(s, uint(s.x) ^ uint(s.y), uint(s.x) + uint(s.y));
    return pcg3d(u.xyz);
}

layout(location = 0) out vec4 o_color;
void main()
{
        vec2 seed = vec2(gl_PrimitiveID % 256, gl_PrimitiveID / 256);
	o_color = vec4(vec3(hash(seed)) * (1.0/float(0xffffffffu)), 1.0);
}
