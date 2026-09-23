// arenahack — world→screen (SDK Hor+ camera). Verified against ABIFINAL
// render.cpp:cam_matrix + world_to_screen (multi-build tested formula).
//
// Formula:
//   view-space:
//     fwd   = dot(world - cam, mat.row0)   // camera forward axis
//     right = dot(world - cam, mat.row1)
//     up    = dot(world - cam, mat.row2)
//   projection:
//     scale = sw * 0.5 / tan(fov/2)
//     sx    = sw/2 + right * scale / fwd
//     sy    = sh/2 - up    * scale / fwd
//   cull: fwd < 1.0 = behind camera.

#include "../inc/ah_w2s.h"
#include <math.h>

#ifndef AH_PI
#define AH_PI 3.14159265358979323846f
#endif
static inline float deg2rad(float d) { return d * (AH_PI / 180.0f); }

AH_MAT3 ah_cam_matrix(const AH_CAM* c) {
    float y = deg2rad(c->yaw), p = deg2rad(c->pitch), r = deg2rad(c->roll);
    float cy = cosf(y), sy = sinf(y);
    float cp = cosf(p), sp = sinf(p);
    float cr = cosf(r), sr = sinf(r);
    AH_MAT3 m;
    m.m[0][0] =  cp * cy;
    m.m[0][1] =  cp * sy;
    m.m[0][2] =  sp;
    m.m[1][0] =  sr*sp*cy - cr*sy;
    m.m[1][1] =  sr*sp*sy + cr*cy;
    m.m[1][2] = -sr * cp;
    m.m[2][0] = -(cr*sp*cy + sr*sy);
    m.m[2][1] =  cy*sr - cr*sp*sy;
    m.m[2][2] =  cr * cp;
    return m;
}

AH_SCREEN_PT ah_world_to_screen(float wx, float wy, float wz,
                                const AH_CAM* c, const AH_MAT3* m,
                                int sw, int sh)
{
    AH_SCREEN_PT out = {0};
    float dx = wx - c->x, dy = wy - c->y, dz = wz - c->z;
    float fwd   = dx*m->m[0][0] + dy*m->m[0][1] + dz*m->m[0][2];
    if (fwd < 1.0f) { out.ok = FALSE; return out; }
    float right = dx*m->m[1][0] + dy*m->m[1][1] + dz*m->m[1][2];
    float up    = dx*m->m[2][0] + dy*m->m[2][1] + dz*m->m[2][2];

    float fov = (c->fov > 30.0f && c->fov < 170.0f) ? c->fov : 90.0f;
    float scale = (float)sw * 0.5f / tanf(deg2rad(fov) * 0.5f);
    out.sx = (float)sw * 0.5f + right * scale / fwd;
    out.sy = (float)sh * 0.5f - up    * scale / fwd;
    out.depth = fwd;
    out.ok = TRUE;
    return out;
}
