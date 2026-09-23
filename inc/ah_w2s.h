// arenahack — world→screen projection (SDK Hor+ formula).
// Camera: view-matrix from yaw/pitch/roll, FOV via horizontal H+ scaling.
// Adapted (fresh C, no scope/ultrawide extras) from ABIFINAL render.cpp.
#pragma once
#include "dh_common.h"

typedef struct {
    float x, y, z;      // camera world position
    float yaw, pitch, roll;   // degrees
    float fov;          // horizontal FOV degrees (game POV.FOV)
} AH_CAM;

typedef struct { float m[3][3]; } AH_MAT3;

typedef struct {
    float sx, sy;       // screen pixels
    float depth;        // forward distance from cam (world units)
    BOOL  ok;           // false if behind cam or off-screen forward-cull
} AH_SCREEN_PT;

// Build view-rotation matrix from cam yaw/pitch/roll.
AH_MAT3 ah_cam_matrix(const AH_CAM* c);

// Project world point (wx,wy,wz) → screen (sw × sh).
AH_SCREEN_PT ah_world_to_screen(float wx, float wy, float wz,
                                const AH_CAM* c, const AH_MAT3* m,
                                int sw, int sh);
