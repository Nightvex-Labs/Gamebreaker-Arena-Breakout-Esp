// arenahack reader-thread public interface.
#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// v0.9.453: raised from 64 → 512. On >60-AI raids the 64 cap silently dropped
// live enemies (corpses share the budget), and the all-or-nothing gate at
// PlayerArray read below fully skipped the walk when Num > 64. 512 covers the
// worst-case ABI lobby (60 AI + PMCs + corpses + streamed spawns) with headroom
// and adds ~72 KB per snapshot — trivial vs the CPU/render budget.
#define AH_MAX_ENT   512
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
    // v0.9.454: replicated room-id from ASGGameState+0x430. 0 in main menu /
    // matchmaking screen, non-zero once server assigns a raid room. Overlay
    // uses this as the authoritative in-raid flag — ACE-decrypt-success alone
    // false-positives in the lobby (root pawn exists for the menu preview
    // character, so decrypt returns plaintext coords with algo=0).
    unsigned long long roomid;
    unsigned long long uagame_base;
    int      ent_n;
    AH_ENT   ents[AH_MAX_ENT];
    int      loot_n;
    unsigned int loot_gen;   // bumped when a fresh loot scan lands
    AH_LOOT  loot[AH_MAX_LOOT];
} AH_LIVE_SNAP;

void ah_reader_start(void);
void ah_reader_stop(void);
void ah_reader_reattach(void);                // v0.9.454: soft-restart reader
void ah_reader_snapshot(AH_LIVE_SNAP* out);   // atomic copy
float ah_reader_hz(void);                     // rolling 500ms reader Hz

// v0.9.455: reader state machine (single atomic). Overlay reads via
// ah_reader_state() to decide UI status and self-exit on GAME_GONE.
//   INIT=0 → provider not attempted
//   PROVIDER_OK=1 → kdu loaded
//   CR3_OK=2 → system CR3 resolved
//   WAITING_GAME=3 → looking for UAGame process
//   ATTACHED=4 → normal 30 Hz snapshot flow
//   GAME_GONE=5 → gworld = 0 held for ≥N probes (game closed)
//   PROVIDER_FAIL=-1 → DhProviderSelect failed
//   CR3_FAIL=-2 → RpmFindSystemCR3 failed
#define AH_READER_INIT             0
#define AH_READER_PROVIDER_OK      1
#define AH_READER_CR3_OK           2
#define AH_READER_WAITING_GAME     3
#define AH_READER_ATTACHED         4  // process latched (imageBase resolved)
#define AH_READER_LIVE             5  // gworld + gs valid — snapshot is real
#define AH_READER_GAME_GONE        6
#define AH_READER_PROVIDER_FAIL   -1
#define AH_READER_CR3_FAIL        -2
int ah_reader_state(void);

#ifdef __cplusplus
}
#endif
