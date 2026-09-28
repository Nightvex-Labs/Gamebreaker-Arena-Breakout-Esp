#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace abi {

struct Vec3 { float x{}, y{}, z{}; };

struct Bone { float x{}, y{}, z{}; };

struct LimbHp { float base{}, cur{}; };

struct Entity {
    uint64_t a{};                         // pawn addr
    std::string cls;                      // "USER" / "PMC" / "BOT_PRIMARY" etc
    float x{}, y{}, z{};
    int hp{}, hp_max{};
    bool me{};
    float cap_r{57.8f}, cap_hh{88.0f};
    std::string name;
    int team{-1};
    bool visible{};
    bool dead{};
    bool knocked{};
    // Velocity (cm/s) for client-side prediction — filled by NetClient from
    // delta between two ENT updates. Render extrapolates position to current
    // frame time, hides DMA throughput jitter (~30Hz raw becomes 144Hz smooth).
    float vx{0.0f}, vy{0.0f}, vz{0.0f};
    double last_pos_t{0.0};               // monotonic seconds at last x/y/z update

    int helm{-1};                         // 0..6 tier, -1 = none / unknown
    int vest{-1};                         // 0..6 tier
    int helm_id{0};
    int vest_id{0};
    float helm_dur{-1.0f};                // v0.9.463: current helm durability (raw/10 — game shows tenths), -1 = unknown
    float vest_dur{-1.0f};                // v0.9.463: current vest durability (raw/10 — game shows tenths), -1 = unknown
    bool  has_thermal{false};             // v0.9.463: T7 Thermal Imager (helmet attachment, item prefix 30112xx) present in ArmorList
    std::string weapon;                   // "AR" / "SMG" / etc — parsed asset name
    std::string weapon_asset;             // raw asset name
    int   mag_cur{-1};                    // current loaded rounds (ContainDataList.Num)
    int   mag_max{-1};                    // mag capacity (MaxStackCount)
    float last_render{-1.0f};             // mesh+0x2F0
    float hp_dir{-1.0f};                  // direct HP float from Char+0x1B54
    std::unordered_map<std::string, Bone> bones;          // "head" -> Bone (world coords at bone_anchor_pos)
    float bone_anchor_x{0.0f}, bone_anchor_y{0.0f}, bone_anchor_z{0.0f};   // pos when bones were captured
    std::unordered_map<std::string, LimbHp> hp_limbs;     // "head" -> LimbHp
    std::optional<float> yaw;             // entity yaw (root+0x17C)
    // v0.9.421: cam snapshot at moment this entity's pos/bones were captured.
    // Async threads (bot_thread) publish entities up to ~7ms after main
    // thread's cam moves — using CURRENT cam for W2S on such entities creates
    // visible drift in zoom (7ms of yaw × 7x scope = several pixels).  Render
    // uses this snap-cam when present, else falls back to snap->cam.
    std::optional<float> cam_snap_x, cam_snap_y, cam_snap_z;
    std::optional<float> cam_snap_yaw, cam_snap_pitch;
};

struct Cam {
    float x{}, y{}, z{};
    float yaw{}, pitch{}, roll{};
    float fov{90.0f};
    float scope_mag{1.0f};
    int   zoom_state{0};
    // My ammo (mag capacity + 3 probe slots for current, pick whichever matches HUD)
    int   mag_max{-1};
    int   mag_cur_a{-1};
    int   mag_cur_b{-1};
    int   mag_cur_c{-1};
    // USER HP
    int   hp{-1};
    int   hp_max{-1};
    // v0.9.370: live per-tick scope FOV from ASGCharacter+0xB34
    // (CurrentSightFov). When ADS with a variable scope, this reads the
    // ACTUAL current-magnification FOV so cycling 2x/4x/7x mid-ADS rescales
    // ESP boxes immediately. render.cpp uses scope_fov as the divisor when
    // it's smaller than the base FOV (indicates ADS with scope active).
    float scope_fov{0.0f};
    // v0.9.419 exp: CurrentZoomingCameraOffset (fwd, right, up in cm) read
    // from FSGCharacterAnimInstanceProxy each tick. Non-zero only in ADS.
    float zoom_offset_x{0.0f};
    float zoom_offset_y{0.0f};
    float zoom_offset_z{0.0f};
};

struct LootItem {
    uint32_t id{0};
    std::string name;
    int rarity{-1};
    uint32_t price{0};
};

struct LootBox {
    uint64_t a{};
    float x{}, y{}, z{};
    int count{0};
    std::vector<LootItem> items;
    uint32_t corpse_val{0};   // aggregate corpse loot value (may be 0 if extraction failed)
    bool     is_corpse{false};       // true when this entry represents a dead body
    bool     is_bot_corpse{false};   // true if owner was BOT, false = PMC/unknown
};

struct Snapshot {
    uint64_t frame{};
    double t{};
    std::vector<Entity> entities;
    std::vector<LootBox> loot;
    Cam cam;
    // v0.9.337: gate ESP so it stops drawing in menu / lobby / preview.
    // Set true only when the reader has confirmed a real raid session
    // (cam at valid raid coords + at least one non-me pawn also in raid).
    // Render skips everything when false.
    bool in_raid{false};
};

}  // namespace abi
