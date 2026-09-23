// arenahack reader-thread public interface.
#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AH_MAX_ENT   64
#define AH_MAX_LOOT  256

typedef struct {
    unsigned long long pawn;
    float x, y, z;
    float yaw;
    int   team;
    int   hp;
    char  name[32];
    unsigned char valid;
    unsigned char is_bot;
    unsigned char is_me;
    unsigned char dead;
    unsigned char knocked;    // sum_cur == 0 && !dead — down-but-not-out
    int   helm;         // 0..6, -1 = none
    int   vest;         // 0..6, -1 = none
    float helm_dur;     // current durability (raw/10), -1 = unknown
    float vest_dur;
    float cap_r;        // capsule radius (cm) — live, shrinks with pose
    float cap_hh;       // capsule half-height (cm) — live: stand≈88 crouch≈60 prone≈35
    unsigned int weapon_id;   // ItemID of current weapon (0 = none/fake bot)
    short mag_cur;      // rounds in mag (-1 = unknown)
    short mag_max;      // mag capacity (-1 = unknown)
} AH_ENT;

typedef struct {
    unsigned long long a;
    float x, y, z;
    unsigned int  id;        // ItemID (raw from ASGInventory+0x6E0)
    unsigned int  price;     // CD.StandardPrice
    int           rarity;    // 0..6, -1 unknown
    char          name[48];  // items::lookup(id) or ""
} AH_LOOT;

typedef struct {
    BOOL     attached;      // ACE-decrypt returned valid coords this tick
    float    x, y, z;       // self world position (cm)
    float    pitch, yaw, roll;   // degrees, from PC.ControlRot
    float    fov;
    float    scope_mag;   // live scope multiplier (1.0 hip, 2/4/7 in ADS)
    float    scope_fov;   // ADSSceneFOV — current sight FOV for W2S in scope
    float    zoom_off_x;  // CurrentZoomingCameraOffset (LOCAL fwd), cm — ADS drift fix
    float    zoom_off_y;  //   (LOCAL right)
    float    zoom_off_z;  //   (LOCAL up)
    short    my_mag_cur;  // self weapon rounds loaded (-1 = unknown)
    short    my_mag_max;  // self weapon mag capacity
    unsigned long long uagame_base;
    int      ent_n;
    AH_ENT   ents[AH_MAX_ENT];
    int      loot_n;
    unsigned int loot_gen;   // bumped when a fresh loot scan lands
    AH_LOOT  loot[AH_MAX_LOOT];
} AH_LIVE_SNAP;

void ah_reader_start(void);
void ah_reader_stop(void);
void ah_reader_snapshot(AH_LIVE_SNAP* out);   // atomic copy
float ah_reader_hz(void);                     // rolling 500ms reader Hz

#ifdef __cplusplus
}
#endif
