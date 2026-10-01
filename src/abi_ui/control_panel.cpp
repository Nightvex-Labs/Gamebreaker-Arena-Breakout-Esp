// control_panel.cpp — Nightvex control panel (ImGui, immediate-mode, ручная отрисовка)
//
// Собран 1:1 с эталоном "Players Screen.dc.html" (см. handoff/TZ-Nightvex.md).
// Ничего не рисуется стандартными виджетами ImGui — вся геометрия ручная,
// чтобы совпасть с макетом по пикселям (радиусы, отступы, толщины, цвета).
//
// ─────────────────────────────────────────────────────────────────────────────
// ЧТО НУЖНО ОТ ВАШЕГО КОДА (подробно — в INTEGRATION.md):
//   1. control_panel.hpp объявляет: namespace abi { void render_control_panel(RenderConfig&); }
//   2. RenderConfig должен содержать поля из блока «КОНТРАКТ» ниже.
//      Все обращения к RenderConfig собраны ТОЛЬКО там — если имена у вас другие,
//      правится один блок, остальной файл не трогается.
//   3. abi::icons::get("name") — иконки по имени. Список имён — в INTEGRATION.md,
//      SVG для новых — в control_panel_icons.hpp.
//   4. Фото оператора для ESP-превью передаётся через
//      abi::control_panel_set_operator_texture(tex, w, h) (новый символ, см. INTEGRATION.md).
// ─────────────────────────────────────────────────────────────────────────────

#include "control_panel.hpp"
#include "icons.hpp"
#include "overlay_hud.hpp"
#include "palette.hpp"
extern "C" {
#include "../../inc/ah_test_trace.h"   // TEST-REMOVE
}

#include <imgui.h>
#include <imgui_internal.h>   // ImClamp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace abi {

// ════════════════════════════════════════════════════════════════════════════
//  КОНТРАКТ С RenderConfig  —  единственное место, где читается/пишется конфиг
// ════════════════════════════════════════════════════════════════════════════
//
// Ожидаемая форма (если у вас иначе — поправьте только функции esp_view / radar_view /
// overlay_view / loot_view / corpses_view / language_ptr ниже):
//
//   struct EspGroup {
//     bool enable, box, skeleton, name, team, health, weapon, ammo, armor, distance;
//     int  box_style;        // 0 = 2D, 1 = 3D
//     int  armor_display;    // 0 = Text, 1 = Bar
//     int  box_distance;     // м
//     int  skeleton_distance;// м
//     ImU32 col_box, col_skeleton, col_name, col_team, col_health,
//           col_weapon, col_ammo, col_distance;
//     ImU32 armor_tier[6];   // T1..T6
//   };
//   struct CorpsesCfg { bool enable; int min_items; ImU32 color; };
//   struct RadarCfg   { bool enable, players, bots, rings, aim_line;
//                       int world_radius /*<=400*/, screen_radius /*<=400*/,
//                           position /*0 TR,1 TL,2 BR,3 BL,4 Center*/, size; };
//   struct OverlayCfg { bool crosshair, spectators, prefire_grace, fps, keybinds;
//                       int prefire_ms /*200|300|400*/, position; ImU32 col_accent; };
//   struct LootCfg    { bool enable, rare_only, containers, quest_items;
//                       int min_price, max_distance; ImU32 col_common, col_rare, col_quest; };
//   struct RenderConfig { EspGroup players, bots; CorpsesCfg corpses;
//                         RadarCfg radar; OverlayCfg overlay; LootCfg loot;
//                         int language; /* 0 = Русский, 1 = English */ };

namespace {

// ────────────────────────────────────────────────────────────────────
// АДАПТЕР под ПЛОСКУЮ RenderConfig (см. src/render.hpp).
//
// Автор написал панель под сгруппированные структуры (players/bots/radar/…).
// У нас конфиг плоский, поэтому view-функции собирают указатели на реальные
// поля RenderConfig, а недостающие поля обслуживаются статическими "тенями"
// (адрес живёт всю сессию; значения переживают перезагрузку из JSON).
//
// Между кадрами: `pre_sync(cfg)` копирует float→int (дистанции) и mode→style
// перед рендером; `post_sync(cfg)` — обратно. Обе вызываются из
// render_control_panel(), чтобы автор's `esp_view(c, bots)` продолжал видеть
// int*, не подозревая о float в исходном конфиге.
// ────────────────────────────────────────────────────────────────────

struct EspView {
    bool*  enable;
    bool*  box;      bool* name;     bool* team;   bool* health;
    bool*  weapon;   bool* ammo;     bool* armor;  bool* distance;
    int*   box_style;       // 0 = 2D, 1 = 3D
    int*   armor_display;   // 0 = Text, 1 = Bar
    int*   box_distance;
    ImU32* col_box;    ImU32* col_name;   ImU32* col_team;
    ImU32* col_health; ImU32* col_weapon; ImU32* col_ammo;   ImU32* col_distance;
    ImU32* armor_tier;      // [6]
};

// Shadows (per-class) for fields the flat RenderConfig doesn't store directly.
// `enable`/`health`/`team` for bots, `col_health`, and the int↔float bridges
// for distances and box_mode↔box_style. All UI mutations land here first;
// post_sync() writes them back into the real fields.
//
// Тиры брони тенью больше не являются: они живут в cfg.armor_tier[6] и
// редактируются прямо там, поэтому выбор оператора доезжает до отрисовки.
struct EspShadows {
    bool  enable       = true;   // master toggle (page_visuals sets checks manually)
    bool  team_bots    = false;  // bots have no team in TZ — UI hides row anyway
    bool  health_bots  = false;  // bots have no HP    — UI hides row anyway
    ImU32 col_health   = abi::pal::HP;   // TZ §3.2: HP = #4BBF8A
    int   box_style_shadow      = 1; // 0=2D, 1=3D
    int   armor_display_shadow  = 1; // 0=Text, 1=Bar
    int   box_distance          = 200;
};
static EspShadows s_pmc_shadow;
static EspShadows s_bot_shadow;

EspView esp_view(RenderConfig& c, bool bots) {
    EspShadows& sh = bots ? s_bot_shadow : s_pmc_shadow;
    EspView v{};
    v.enable   = &sh.enable;
    v.box      = bots ? &c.show_box_bot      : &c.show_box_pmc;
    v.name     = bots ? &c.show_bot_name     : &c.show_name;
    v.team     = bots ? &sh.team_bots        : &c.show_team_id;
    v.health   = bots ? &sh.health_bots      : &c.show_hp;
    v.weapon   = bots ? &c.show_bot_weapon   : &c.show_weapon;
    v.ammo     = bots ? &c.show_bot_ammo     : &c.show_ammo;
    v.armor    = bots ? &c.show_bot_armor    : &c.show_armor;
    v.distance = bots ? &c.show_bot_distance : &c.show_distance;
    v.box_style         = &sh.box_style_shadow;
    v.armor_display     = &sh.armor_display_shadow;
    v.box_distance      = &sh.box_distance;
    v.col_box      = bots ? &c.col_box_bot      : &c.col_box_pmc;
    v.col_name     = bots ? &c.col_name_bot     : &c.col_name_pmc;
    v.col_team     = &c.col_team;
    v.col_health   = &sh.col_health;
    v.col_weapon   = bots ? &c.col_weapon_bot   : &c.col_weapon_pmc;
    v.col_ammo     = bots ? &c.col_ammo_bot     : &c.col_ammo_pmc;
    v.col_distance = bots ? &c.col_distance_bot : &c.col_distance_pmc;
    v.armor_tier   = c.armor_tier;
    return v;
}

// Copy float ranges + mode → int shadows, so slider_row / segmented see int*.
void pre_sync(RenderConfig& c) {
    s_pmc_shadow.box_distance      = (int)c.pmc_range_m;
    s_bot_shadow.box_distance      = (int)c.bot_range_m;

    // box_mode: 0=off,2=2D,3=3D → box_style 0/1 (author has no Off).
    s_pmc_shadow.box_style_shadow = (c.box_mode     == 3) ? 1 : 0;
    s_bot_shadow.box_style_shadow = (c.box_mode_bot == 3) ? 1 : 0;

    // armor_display: my 1=Text,2=Bar,0=Off  → author 0/1.
    s_pmc_shadow.armor_display_shadow = (c.armor_display == 2) ? 1 : 0;
    s_bot_shadow.armor_display_shadow = (c.armor_display == 2) ? 1 : 0;

    // Master enable: mirror of at-least-one-child.
    s_pmc_shadow.enable = c.show_box_pmc || c.show_name ||
                           c.show_team_id || c.show_hp || c.show_weapon ||
                           c.show_ammo || c.show_armor || c.show_distance;
    s_bot_shadow.enable = c.show_box_bot || c.show_bot_name ||
                           c.show_bot_weapon || c.show_bot_ammo || c.show_bot_armor ||
                           c.show_bot_distance;
}

// Write UI-mutated shadows back to the flat config.
void post_sync(RenderConfig& c) {
    c.pmc_range_m         = (float)s_pmc_shadow.box_distance;
    c.bot_range_m         = (float)s_bot_shadow.box_distance;

    // Preserve Off (0) if user hasn't toggled the row; otherwise adopt style.
    if (c.box_mode     != 0) c.box_mode     = (s_pmc_shadow.box_style_shadow == 1) ? 3 : 2;
    if (c.box_mode_bot != 0) c.box_mode_bot = (s_bot_shadow.box_style_shadow == 1) ? 3 : 2;

    // Armor display Off (0 in my enum) is not surfaced in the new UI — leave
    // it alone if the user set it explicitly outside. Otherwise mirror UI.
    if (c.armor_display != 0) {
        c.armor_display = (s_pmc_shadow.armor_display_shadow == 1) ? 2 : 1;
    }
}

// Corpses — flat config has `show_corpse` (PMC only) + `pmc_corpse_min_value`
// + `col_corpses_pmc`. Author's CorpsesCfg is unified across the two classes,
// so we map to PMC's fields; UI treats it as one setting per TZ §4.
struct CorpsesView { bool* enable; int* min_items; ImU32* color; };
static int   s_corpses_min_items = 0;
CorpsesView corpses_view(RenderConfig& c) {
    s_corpses_min_items = c.pmc_corpse_min_value;
    return { &c.show_corpse, &s_corpses_min_items, &c.col_corpses_pmc };
}
void corpses_post_sync(RenderConfig& c) { c.pmc_corpse_min_value = s_corpses_min_items; }

// Radar — всё в конфиге; тенями остались только int-мосты для float-радиусов.
struct RadarView {
    bool* enable; bool* players; bool* bots; bool* rings; bool* aim_line;
    int* world_radius; int* screen_radius; int* position;
};
static int s_radar_world  = 200;
static int s_radar_screen = 250;
RadarView radar_view(RenderConfig& c) {
    s_radar_world  = (int)c.radar_range_m;
    s_radar_screen = c.radar_px_radius;
    return { &c.show_radar, &c.show_radar_pmc, &c.show_radar_bots,
             &c.radar_rings, &c.radar_aim_dir,
             &s_radar_world, &s_radar_screen, &c.radar_position };
}
void radar_post_sync(RenderConfig& c) {
    c.radar_range_m   = (float)s_radar_world;
    c.radar_px_radius = s_radar_screen;
}

// Overlay — всё в конфиге (ovl_*), тенью осталась одна: булев вид у
// prefire_grace_ms, где 0 = «выключено».
struct OverlayView {
    bool* crosshair; bool* spectators; bool* prefire_grace; bool* fps; bool* keybinds;
    int* prefire_ms; int* position; ImU32* col_accent;
};
static bool s_ovl_prefire_bool = true;   // булев вид prefire_grace_ms
OverlayView overlay_view(RenderConfig& c) {
    s_ovl_prefire_bool = (c.prefire_grace_ms > 0);
    return { &c.ovl_crosshair, &c.ovl_spectators, &s_ovl_prefire_bool,
             &c.show_fps_overlay, &c.ovl_keybinds,
             &c.prefire_grace_ms, &c.ovl_position, &c.ovl_accent };
}
void overlay_post_sync(RenderConfig& c) {
    // If the row was turned off, force ms to 0 (feature-disabled sentinel).
    if (!s_ovl_prefire_bool) c.prefire_grace_ms = 0;
    else if (c.prefire_grace_ms == 0) c.prefire_grace_ms = 300;
}

// Loot — flat has enable (`show_top_loot`), `min_loot_value`, `top_loot_range_m`.
// Categorical filters + tier colours are UI shadows.
struct LootView {
    bool* enable; bool* rare_only; bool* containers; bool* quest_items;
    int* min_price; int* max_distance;
    ImU32* col_common; ImU32* col_rare; ImU32* col_quest;
};
static bool  s_loot_rare        = false;
static bool  s_loot_containers  = true;
static bool  s_loot_quest       = true;
static int   s_loot_max_dist    = 150;
static ImU32 s_loot_col_common  = abi::pal::loot::MARK_COMMON;
static ImU32 s_loot_col_rare    = abi::pal::loot::MARK_RARE;
static ImU32 s_loot_col_quest   = abi::pal::loot::MARK_QUEST;
LootView loot_view(RenderConfig& c) {
    s_loot_max_dist = (int)c.top_loot_range_m;
    return { &c.show_top_loot, &s_loot_rare, &s_loot_containers, &s_loot_quest,
             &c.min_loot_value, &s_loot_max_dist,
             &s_loot_col_common, &s_loot_col_rare, &s_loot_col_quest };
}
void loot_post_sync(RenderConfig& c) {
    c.top_loot_range_m = (float)s_loot_max_dist;
}

int* language_ptr(RenderConfig& c) { return &c.ui_language; }

// ════════════════════════════════════════════════════════════════════════════
//  ПАЛИТРА (из ТЗ, п. 1 и п. 6) — hex как в макете
// ════════════════════════════════════════════════════════════════════════════

// Значения живут в palette.hpp — здесь только короткие локальные имена, под
// которыми их знает остальной файл. Своих чисел в этом блоке нет и быть не
// должно: правится палитра, не панель.
namespace P = abi::pal;

using P::rgb;
using P::wht;

// TZ §3.1 — interface colours, exact.
constexpr ImU32 C_DESK        = P::DESKTOP;
constexpr ImU32 C_WINDOW      = P::WINDOW;
constexpr ImU32 C_SIDEBAR     = P::SIDEBAR;          // алиас окна, см. palette.hpp
constexpr ImU32 C_PANEL       = P::CARD;             // row / card bg
constexpr ImU32 C_ROW         = P::CARD;
constexpr ImU32 C_ROW_HOVER   = P::CARD_HOVER;       // subtle hover lift
constexpr ImU32 C_CHIP        = P::CHIP;             // chip / segment-group bg
constexpr ImU32 C_SEG_ON      = P::CHIP_ON;          // active segment
constexpr ImU32 C_TRACK       = P::TRACK;            // slider track
constexpr ImU32 C_POP         = P::POPOVER;          // tooltip / dropdown bg
constexpr ImU32 C_INSET       = P::PREVIEW_BG;       // preview panel bg
constexpr ImU32 C_LINE        = P::LINE;             // TZ line / border
constexpr ImU32 C_LINE2       = P::LINE2;            // tooltip / dropdown border
constexpr ImU32 C_LINE_SOFT   = P::LINE;
constexpr ImU32 C_TEXT        = P::TEXT;
constexpr ImU32 C_MUTED       = P::TEXT_MUTED;
constexpr ImU32 C_DIM         = P::TEXT_DIM;
constexpr ImU32 C_FAINT       = P::TEXT_FAINT;
constexpr ImU32 C_AMBER       = P::AMBER;
constexpr ImU32 C_AMBER_TEXT  = P::AMBER_TEXT;
constexpr ImU32 C_AMBER_FILL  = P::AMBER_FILL;
constexpr ImU32 C_AMBER_LINE  = P::AMBER_LINE;
constexpr ImU32 C_AMBER_NAV   = P::AMBER_NAV;        // active menu fill
constexpr ImU32 C_AMBER_NAVLN = P::AMBER_NAVLN;      // active menu border
constexpr ImU32 C_HP          = P::HP;               // HP green (fixed)
constexpr ImU32 C_SCRIM       = P::SCRIM;

// TZ §3.3 — armour tiers T1..T6 (дефолты; живые значения в cfg.armor_tier).
constexpr const ImU32* kTier = P::ARMOR_TIER;

// TZ-Modal §6.4 — 30 «полевых» цветов, по светлоте (10 колонок × 3 ряда).
constexpr const unsigned* kPalette = P::FIELD;

// ════════════════════════════════════════════════════════════════════════════
//  ГЕОМЕТРИЯ ОКНА (ТЗ, п. 2)
// ════════════════════════════════════════════════════════════════════════════

const float kWinW      = 1402.0f;
const float kWinH      = 699.0f;
const float kWinRadius = 16.0f;   // TZ §1.2
const float kSidebarW  = 248.0f;  // TZ §4
const float kPagePad   = 26.0f;   // TZ §5/§6 content padding
const float kRowH      = 60.0f;   // две строки текста
const float kRowGap    = 8.0f;
const float kRowRadius = 14.0f;
const float kPreviewW  = 520.0f;  // TZ §6/§7 fixed preview column

// ════════════════════════════════════════════════════════════════════════════
//  СОСТОЯНИЕ UI (не конфиг — в RenderConfig НЕ пишется)
// ════════════════════════════════════════════════════════════════════════════

enum Page { PG_PLAYERS = 0, PG_BOTS, PG_RADAR, PG_OVERLAY, PG_LOOT, PG_SETTINGS };

enum ModalKind {
    MK_NONE = 0,
    MK_BOX, MK_NAME, MK_TEAM, MK_HEALTH,
    MK_WEAPON, MK_AMMO, MK_ARMOR, MK_DISTANCE, MK_CORPSES,
    MK_PREFIRE,   // только настройки: пресеты + Grace duration
    MK_TOPLOOT,   // только настройки: List size + Top loot range
    MK_POSITION,  // выбор расположения: вертикальный список вариантов
    MK_PLAIN      // только цвет, без вкладки «Настройки»
};

struct Modal {
    bool      open      = false;
    ModalKind kind      = MK_NONE;
    int       tab       = 0;        // 0 = Настройки, 1 = Цвет
    bool      has_settings = false;
    char      title[64] = {0};
    ImU32*    target    = nullptr;  // куда пишется выбранный цвет
    ImU32     backup    = 0;        // для «Отмена»
    int       tier      = 3;        // выбранный тир брони (0..5)
    float     h = 0, s = 0, v = 1;  // HSV редактора
    bool      drag_sv = false, drag_hue = false;
    // Клик, который ОТКРЫЛ модалку, приходит в том же кадре: строка обрабатывает
    // его в page_visuals, а draw_modal ниже видит тот же самый клик и принимает
    // его за «клик по затемнению» → окно закрывалось мгновенно. Первый кадр
    // модалка клики не слушает.
    bool      just_opened = false;
    // MK_POSITION payload: the option list plus the int it writes into, with a
    // backup so Cancel/Esc can put the previous choice back.
    int*        int_target = nullptr;
    int         int_backup = 0;
    // The option labels are OWNED here, not borrowed. Callers build their list
    // in a local array (`const char* rpos[4] = {…}`) that dies the moment the
    // page function returns, so keeping the caller's pointer left the modal
    // reading freed stack on the next frame — an instant crash on open.
    // The strings themselves are literals from T(), so copying the pointers is
    // enough; only the array needs to live here.
    static const int MAX_OPTS = 8;
    const char* opt_items[MAX_OPTS] = {};
    int         opt_count = 0;
};

struct State {
    Page  page = PG_PLAYERS;
    Modal modal;
    float scroll[6] = {0, 0, 0, 0, 0, 0};
    // Высота содержимого с ПРОШЛОГО кадра. Нужна, чтобы зажать смещение до
    // отрисовки: раньше кламп стоял в конце, и кадр перелёта успевал уехать за
    // край, а на следующем кадре список отскакивал назад — на краях списка это
    // читалось как рывок.
    float content_h[6] = {0, 0, 0, 0, 0, 0};
    int   open_dropdown = -1;   // id открытого списка, -1 — нет
    bool  esp_show_bots = false; // переключатель Players/Bots над ESP-превью
    // Slider being dragged (address of its backing int). Without a capture the
    // knob stops following as soon as the cursor leaves the narrow track rect.
    const void* drag_slider = nullptr;
    ImTextureID operator_tex = (ImTextureID)0;
    float operator_w = 0, operator_h = 0;
    ImTextureID bg_tex = (ImTextureID)0;      // desktop wallpaper behind the panel
    float bg_w = 0, bg_h = 0;
};

State st;

// ════════════════════════════════════════════════════════════════════════════
//  РИСОВАНИЕ: примитивы
// ════════════════════════════════════════════════════════════════════════════

struct Ctx {
    ImDrawList* dl        = nullptr;
    ImVec2      origin{};      // левый верх окна
    bool        blocked   = false;  // клики съедены модалкой
    ImFont*     font      = nullptr;  // Fonts[0] — Segoe UI Regular
    ImFont*     mono      = nullptr;  // Fonts[1] — Consolas
    ImFont*     semibold  = nullptr;  // Fonts[2] — Segoe UI Semibold
};

Ctx g;

// ── i18n ───────────────────────────────────────────────────────────────
// Mirrors RenderConfig::ui_language (0 = EN, 1 = RU); refreshed once per
// frame in render_control_panel so every helper can reach it without
// threading the config through. T() picks the string for the active语言.
int g_lang = 0;
inline const char* T(const char* en, const char* ru) { return g_lang == 1 ? ru : en; }

inline ImFont* F()  { return g.font ? g.font : ImGui::GetFont(); }
inline ImFont* FM() { return g.mono ? g.mono : F(); }
inline ImFont* FB() { return g.semibold ? g.semibold : F(); }

// Global type-size multiplier. The panel calls text()/textb() with the exact
// TZ px values (14, 12.5, …); this nudges every one up uniformly for
// readability on high-res displays without re-touching each call site.
// Applied ONLY in the four leaf primitives below — the _center/_right helpers
// delegate to them, so scaling stays consistent (measure ↔ draw).
float kTextK = 1.15f;   // global size multiplier (set via control_panel_set_typography)
float kMonoK = 1.00f;   // extra multiplier for the mono face only

// Effective draw size for a run: the TZ px value, scaled globally, with the
// mono face optionally trimmed so Consolas stops out-sizing Segoe UI.
inline float dsz(float size, bool mono) { return size * kTextK * (mono ? kMonoK : 1.0f); }

void text(ImVec2 p, float size, ImU32 col, const char* s, bool mono = false) {
    g.dl->AddText(mono ? FM() : F(), dsz(size, mono), p, col, s);
}
ImVec2 measure(float size, const char* s, bool mono = false) {
    return (mono ? FM() : F())->CalcTextSizeA(dsz(size, mono), FLT_MAX, 0.0f, s);
}
void text_right(float right_x, float y, float size, ImU32 col, const char* s, bool mono = false) {
    ImVec2 m = measure(size, s, mono);
    text(ImVec2(right_x - m.x, y), size, col, s, mono);
}
void text_center(float cx, float y, float size, ImU32 col, const char* s, bool mono = false) {
    ImVec2 m = measure(size, s, mono);
    text(ImVec2(cx - m.x * 0.5f, y), size, col, s, mono);
}

// Ascent of a face at a given draw size. Mixing Segoe UI with Consolas on one
// line needs baseline alignment: ImDrawList::AddText positions by the TOP of
// the em box, and the two faces have different ascents, so top-aligning makes
// the shorter-ascent run sit lower and read as a different size.
float face_ascent(ImFont* f, float size) {
    return f->Ascent * ((size * kTextK) / f->FontSize);
}

// A sans label on the left and a mono value on the right, sharing one
// baseline. Used by every slider header — top-aligning the two faces made the
// value look like a different size than its label.
void label_value_row(float x, float w, float y,
                     const char* label, float lsz, ImU32 lcol,
                     const char* value, float vsz, ImU32 vcol);

// Semibold variants — headings, row titles, nav labels, brand, nick, TEAM pill.
void textb(ImVec2 p, float size, ImU32 col, const char* s) {
    g.dl->AddText(FB(), size * kTextK, p, col, s);
}
ImVec2 measureb(float size, const char* s) {
    return FB()->CalcTextSizeA(size * kTextK, FLT_MAX, 0.0f, s);
}
float cap_center_mono(ImFont* f, float size);   // fwd (defined with cap_center)
void textb_center(float cx, float y, float size, ImU32 col, const char* s) {
    ImVec2 m = measureb(size, s);
    textb(ImVec2(cx - m.x * 0.5f, y), size, col, s);
}

// Vertical midpoint of a face's capital letters, measured from the draw
// position. Uses the real 'H' glyph box — CalcTextSize().y is just the line
// height and is identical for both faces at the same pt size, so it can't tell
// us how the ink actually sits inside the line.
float cap_center_at(ImFont* f, float draw_size) {
    const ImFontGlyph* g_ = f->FindGlyph((ImWchar)'H');
    float scale = draw_size / f->FontSize;
    if (!g_) return f->Ascent * scale * 0.5f;
    return (g_->Y0 + g_->Y1) * 0.5f * scale;
}
float cap_center(ImFont* f, float size)      { return cap_center_at(f, size * kTextK); }
float cap_center_mono(ImFont* f, float size) { return cap_center_at(f, dsz(size, true)); }

// ── Vertically centred text ────────────────────────────────────────────
// Draw so the CAPITALS straddle `cy`. Everything that sits inside a row,
// chip, pill or button must use these: hard-coded "cy - 7" offsets only ever
// match one font size, so they drift the moment the type scale changes.
void text_mid(float x, float cy, float size, ImU32 col, const char* s, bool mono = false) {
    float cc = mono ? cap_center_mono(FM(), size) : cap_center(F(), size);
    text(ImVec2(x, cy - cc), size, col, s, mono);
}
void textb_mid(float x, float cy, float size, ImU32 col, const char* s) {
    textb(ImVec2(x, cy - cap_center(FB(), size)), size, col, s);
}
void text_center_mid(float cx, float cy, float size, ImU32 col, const char* s, bool mono = false) {
    ImVec2 m = measure(size, s, mono);
    text_mid(cx - m.x * 0.5f, cy, size, col, s, mono);
}
void textb_center_mid(float cx, float cy, float size, ImU32 col, const char* s) {
    ImVec2 m = measureb(size, s);
    textb_mid(cx - m.x * 0.5f, cy, size, col, s);
}

// ── Baseline alignment ─────────────────────────────────────────────────
// For two runs SIDE BY SIDE on one line, cap-centring is wrong: it lines up
// the capitals, which leaves the smaller run's baseline floating above the
// larger one's and reads as "not centred". Runs stacked on separate lines
// (row title over its subtitle) still want cap-centring — use *_mid there.
float baseline_at_mid(ImFont* f, float size, float cy) {
    float draw = size * kTextK;
    return cy - cap_center_at(f, draw) + f->Ascent * (draw / f->FontSize);
}
void text_base(float x, float base_y, float size, ImU32 col, const char* s) {
    float draw = size * kTextK;
    text(ImVec2(x, base_y - F()->Ascent * (draw / F()->FontSize)), size, col, s);
}

void label_value_row(float x, float w, float y,
                     const char* label, float lsz, ImU32 lcol,
                     const char* value, float vsz, ImU32 vcol) {
    // Align the two runs on a shared optical middle, not on the baseline:
    // Consolas sits much taller in its line box than Segoe UI, so a
    // baseline-aligned number visibly rides above its sans label.
    float cl = cap_center(F(),       lsz);
    float cv = cap_center_mono(FM(), vsz);
    float C  = ImMax(cl, cv);
    text(ImVec2(x, y + C - cl), lsz, lcol, label);
    text_right(x + w, y + C - cv, vsz, vcol, value, true);
}

void card(ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border, float r) {
    g.dl->AddRectFilled(a, b, fill, r);
    if (border) g.dl->AddRect(a, b, border, r, 0, 1.0f);
}

// Fey elevation rule: "Card = 1px hairline + inset .03, no shadow" — depth
// comes from a faint inner top edge, never from a glow. Draw it just inside
// the border, inset by the corner radius so it doesn't cross the rounding.
void card_inset(ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border, float r) {
    card(a, b, fill, border, r);
    g.dl->AddLine(ImVec2(a.x + r, a.y + 1.0f), ImVec2(b.x - r, a.y + 1.0f), wht(0.03f), 1.0f);
}
void hairline(float x0, float x1, float y) { g.dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), C_LINE, 1.0f); }

bool hovered(ImVec2 a, ImVec2 b);   // fwd

// Shared drag handling for every slider. Grabbing anywhere on the (padded)
// track captures the knob until the button is released, so the value keeps
// tracking the cursor even when it wanders off the 7 px-tall track.
// Returns the new 0..1 position, or -1 when this slider isn't being dragged.
float slider_drag(const void* id, ImVec2 hit_a, ImVec2 hit_b,
                  float track_x, float track_w, bool enabled = true) {
    if (!enabled) return -1.0f;
    if (hovered(hit_a, hit_b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        st.drag_slider = id;
    if (st.drag_slider != id) return -1.0f;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { st.drag_slider = nullptr; return -1.0f; }
    return ImClamp((ImGui::GetIO().MousePos.x - track_x) / ImMax(1.0f, track_w), 0.0f, 1.0f);
}

// Optional clip region for scrolled lists: widgets scrolled out of view must
// not react to clicks even though their rect still contains the cursor.
bool  g_clip_on = false;
ImVec2 g_clip_a, g_clip_b;

bool hovered(ImVec2 a, ImVec2 b) {
    if (g.blocked) return false;
    if (g_clip_on && !ImGui::IsMouseHoveringRect(g_clip_a, g_clip_b, false)) return false;
    return ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
           ImGui::IsMouseHoveringRect(a, b, false);
}
bool clicked(ImVec2 a, ImVec2 b) { return hovered(a, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left); }

// иконка по имени. Единственная точка соприкосновения с abi::icons.
// Ожидается, что icons::get отдаёт нечто, приводимое к ImTextureID
// (или сам ImTextureID). Если у вас другой тип — правится только эта функция.
void icon(const char* name, ImVec2 pos, float size, ImU32 col) {
    ImTextureID tex = (ImTextureID)abi::icons::get(name);
    if (!tex) { g.dl->AddRect(pos, ImVec2(pos.x + size, pos.y + size), col, 3.0f); return; }
    g.dl->AddImage(tex, pos, ImVec2(pos.x + size, pos.y + size),
                   ImVec2(0, 0), ImVec2(1, 1), col);
}

void check_glyph(ImVec2 c, float s, ImU32 col) {
    ImVec2 p[3] = { ImVec2(c.x - s * 0.30f, c.y + 0.02f * s),
                    ImVec2(c.x - s * 0.06f, c.y + s * 0.24f),
                    ImVec2(c.x + s * 0.32f, c.y - s * 0.26f) };
    g.dl->AddPolyline(p, 3, col, 0, 1.7f);
}

void chevron(ImVec2 c, float s, ImU32 col, bool up = false) {
    float d = up ? -1.0f : 1.0f;
    ImVec2 p[3] = { ImVec2(c.x - s, c.y - d * s * 0.45f),
                    ImVec2(c.x,     c.y + d * s * 0.45f),
                    ImVec2(c.x + s, c.y - d * s * 0.45f) };
    g.dl->AddPolyline(p, 3, col, 0, 1.7f);
}

void tooltip(const char* title, const char* body) {
    ImGui::BeginTooltip();
    ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (body && *body) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::PushTextWrapPos(260.0f);
        ImGui::TextUnformatted(body);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::EndTooltip();
}

// ════════════════════════════════════════════════════════════════════════════
//  ВИДЖЕТЫ (по макету)
// ════════════════════════════════════════════════════════════════════════════

// Чекбокс 16×16, r5. Вкл: fill rgba(230,163,90,.12) + border .38 + янтарная галка.
// Выкл: только border rgba(255,255,255,.12).
bool checkbox(ImVec2 pos, bool* v, bool enabled = true) {
    const float S = 16.0f;
    ImVec2 a = pos, b = ImVec2(pos.x + S, pos.y + S);
    bool hov = hovered(a, b);
    if (*v) {
        g.dl->AddRectFilled(a, b, enabled ? C_AMBER_FILL : P::alpha(P::AMBER, 0.06f), 5.0f);
        g.dl->AddRect(a, b, enabled ? C_AMBER_LINE : P::alpha(P::AMBER, 0.18f), 5.0f, 0, 1.0f);
        check_glyph(ImVec2(pos.x + S * 0.5f, pos.y + S * 0.5f), S * 0.72f,
                    enabled ? C_AMBER : P::alpha(P::AMBER, 0.45f));
    } else {
        g.dl->AddRect(a, b, hov ? P::T3 : C_LINE2, 5.0f, 0, 1.0f);
    }
    if (enabled && clicked(a, b)) { *v = !*v; return true; }
    return false;
}

// Кружок цвета 15×15 (r7.5), рамка rgba(255,255,255,.25) + мягкое кольцо-тень
// rgba(255,255,255,.04). Клик открывает модалку. TZ §6.3.
bool color_dot(ImVec2 center, ImU32 col, bool enabled) {
    const float R = 7.5f;
    ImVec2 a(center.x - R, center.y - R), b(center.x + R, center.y + R);
    bool hov = hovered(a, b);
    g.dl->AddCircleFilled(center, R + 2.5f, wht(0.04f), 24);           // ring shadow
    g.dl->AddCircleFilled(center, R, enabled ? col : P::SWATCH_OFF, 24);
    g.dl->AddCircle(center, R, hov ? wht(0.40f) : wht(0.25f), 24, 1.0f);
    return enabled && clicked(a, b);
}

// Шестерёнка 15×15. TZ §6.3: цвет #56565F (активная строка #8B8B95), opacity ~.9.
bool gear_button(ImVec2 center, bool enabled) {
    const float R = 11.0f;   // hit radius
    ImVec2 a(center.x - R, center.y - R), b(center.x + R, center.y + R);
    bool hov = hovered(a, b);
    icon("settings", ImVec2(center.x - 8.0f, center.y - 8.0f), 16.0f,
         enabled ? (hov ? C_AMBER : C_MUTED) : C_FAINT);
    return enabled && clicked(a, b);
}

struct RowResult { bool toggled = false; bool color_clicked = false; bool gear_clicked = false; };

// Строка настройки: [чекбокс] [иконка 18] Название / подпись .. [кружок] [шестерёнка].
// Иконка держит две строки текста, поэтому назначение читается без наведения —
// подсказки на строках у нас нет вообще. Подпись обязательна: без неё строка
// снова становится голым термином вроде «Броня (мастер)».
RowResult setting_row(float x, float& y, float w, const char* icon_name,
                      const char* title, const char* sub,
                      bool* on, ImU32* col, bool has_gear, bool enabled = true) {
    RowResult r;
    ImVec2 a(x, y), b(x + w, y + kRowH);
    bool hov = hovered(a, b);
    card_inset(a, b, hov && enabled ? C_ROW_HOVER : C_ROW, C_LINE, kRowRadius);

    float cy = y + kRowH * 0.5f;
    bool  on_state = enabled && (!on || *on);

    // Чекбокс — паддинг карточки 16.
    if (on) r.toggled = checkbox(ImVec2(x + 16.0f, cy - 8.0f), on, enabled);

    // Иконка 18 между чекбоксом и текстом: включено — t2, выключено — t4.
    float tx = x + (on ? 44.0f : 16.0f);
    if (icon_name && *icon_name) {
        icon(icon_name, ImVec2(tx, cy - 9.0f), 18.0f, on_state ? C_MUTED : C_FAINT);
        tx += 18.0f + 12.0f;
    }

    // Заголовок 14/600 + подпись 12/400 t3, обе по оптической середине.
    ImU32 tc = !enabled ? C_DIM : (on_state ? C_TEXT : C_DIM);
    if (sub && *sub) {
        textb_mid(tx, cy - 9.0f, 14.0f, tc, title);
        text_mid (tx, cy + 9.0f, 12.0f, C_DIM, sub);
    } else {
        textb_mid(tx, cy, 14.0f, tc, title);
    }

    // Правый кластер: паддинг 16, шаг 12. Шестерёнка 16, кружок 15.
    float rx = x + w - 16.0f;   // right inner edge
    if (has_gear) {
        if (gear_button(ImVec2(rx - 7.5f, cy), enabled)) r.gear_clicked = true;
        rx -= 16.0f + 12.0f;    // gear width + gap
    }
    if (col) {
        if (color_dot(ImVec2(rx - 7.5f, cy), *col, enabled)) r.color_clicked = true;
    }
    y += kRowH + kRowGap;
    return r;
}

// Сегментный переключатель (TZ §7.1): группа #141418 / рамка .08 / r9,
// активный сегмент #1A1A1F + рамка rgba(255,255,255,.10), текст 12/400
// (активный #F4F4F6, иначе #8B8B95). Нейтральный, без янтаря.
void segmented(float x, float y, float w, float h, const char* const* labels, int count, int* value,
               bool enabled = true) {
    ImVec2 a(x, y), b(x + w, y + h);
    card(a, b, C_TRACK, C_LINE, 11.0f);
    float seg = (w - 6.0f) / (float)count;
    for (int i = 0; i < count; ++i) {
        ImVec2 sa(x + 3.0f + seg * i, y + 3.0f), sb(sa.x + seg, y + h - 3.0f);
        bool on = (*value == i);
        if (on) {
            g.dl->AddRectFilled(sa, sb, C_SEG_ON, 8.0f);
            g.dl->AddRect(sa, sb, C_LINE2, 8.0f, 0, 1.0f);
        } else if (hovered(sa, sb) && enabled) {
            g.dl->AddRectFilled(sa, sb, C_ROW_HOVER, 8.0f);
        }
        text_center_mid((sa.x + sb.x) * 0.5f, y + h * 0.5f, 12.0f,
                        on ? (enabled ? C_TEXT : C_DIM) : C_MUTED, labels[i]);
        if (enabled && clicked(sa, sb)) *value = i;
    }
}

// Слайдер с янтарной заливкой и моно-значением справа
void slider_row(float x, float y, float w, const char* label, int* v, int vmin, int vmax,
                const char* unit, bool enabled = true) {
    text(ImVec2(x, y), 12.5f, C_MUTED, label);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d%s", *v, unit ? unit : "");
    text_right(x + w, y, 12.5f, enabled ? C_TEXT : C_DIM, buf, true);

    float ty = y + 22.0f, th = 4.0f;
    ImVec2 a(x, ty), b(x + w, ty + th);
    g.dl->AddRectFilled(a, b, wht(0.08f), 2.0f);
    float t = (float)(*v - vmin) / (float)std::max(1, vmax - vmin);
    t = ImClamp(t, 0.0f, 1.0f);
    g.dl->AddRectFilled(a, ImVec2(x + w * t, ty + th), enabled ? C_AMBER : C_FAINT, 2.0f);

    ImVec2 knob(x + w * t, ty + th * 0.5f);
    ImVec2 ha(x - 6.0f, ty - 9.0f), hb(x + w + 6.0f, ty + th + 9.0f);
    bool hov = hovered(ha, hb);
    g.dl->AddCircleFilled(knob, hov ? 7.0f : 6.0f, enabled ? C_AMBER : C_FAINT, 20);
    g.dl->AddCircleFilled(knob, hov ? 3.0f : 2.5f, C_WINDOW, 12);

    if (enabled && hov && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float nt = (ImGui::GetIO().MousePos.x - x) / std::max(1.0f, w);
        *v = vmin + (int)std::lround(ImClamp(nt, 0.0f, 1.0f) * (vmax - vmin));
    }
}

// Выпадающий список: чип со значением + шеврон по центру справа
void dropdown(int id, float x, float y, float w, float h, const char* const* items, int count,
              int* value, bool enabled = true) {
    ImVec2 a(x, y), b(x + w, y + h);
    bool open = (st.open_dropdown == id);
    card(a, b, C_ROW, open ? C_AMBER_LINE : C_LINE2, 11.0f);
    text_mid(x + 13.0f, y + h * 0.5f, 13.0f, enabled ? C_TEXT : C_DIM,
             items[ImClamp(*value, 0, count - 1)]);
    chevron(ImVec2(x + w - 16.0f, y + h * 0.5f), 5.0f, C_MUTED, open);
    if (enabled && clicked(a, b)) st.open_dropdown = open ? -1 : id;

    if (!open) return;
    float ih = 34.0f;
    ImVec2 la(x, y + h + 6.0f), lb(x + w, y + h + 6.0f + ih * count + 8.0f);
    g.dl->AddRectFilled(ImVec2(la.x + 2, la.y + 4), ImVec2(lb.x + 2, lb.y + 6), IM_COL32(0, 0, 0, 90), 12.0f);
    card(la, lb, C_POP, C_LINE2, 14.0f);
    for (int i = 0; i < count; ++i) {
        ImVec2 ia(la.x + 4.0f, la.y + 4.0f + ih * i), ib(lb.x - 4.0f, ia.y + ih);
        bool sel = (*value == i);
        if (hovered(ia, ib)) g.dl->AddRectFilled(ia, ib, wht(0.05f), 8.0f);
        if (sel) {
            g.dl->AddRectFilled(ia, ib, C_AMBER_NAV, 8.0f);
            check_glyph(ImVec2(ib.x - 16.0f, (ia.y + ib.y) * 0.5f), 12.0f, C_AMBER);
        }
        text_mid(ia.x + 12.0f, (ia.y + ib.y) * 0.5f, 13.0f, sel ? C_AMBER_TEXT : C_MUTED, items[i]);
        if (clicked(ia, ib)) { *value = i; st.open_dropdown = -1; }
    }
}

void section_label(float x, float& y, const char* s) {
    text(ImVec2(x, y), 11.0f, C_DIM, s);
    y += 20.0f;
}

// ════════════════════════════════════════════════════════════════════════════
//  САЙДБАР
// ════════════════════════════════════════════════════════════════════════════

struct NavItem { const char* label; const char* icon_name; Page page; const char* tip; const char* tip_body; };

// One flat menu row (TZ §4.4): height 46, radius 12, padding 0 14, gap 13.
// Active = amber .10 fill + .25 border, icon #E6A35A, label 14/600 #E9C9A5.
// Inactive = transparent, icon #56565F (hover #8B8B95), label 14/500 #8B8B95.
// Tooltip appears while the mouse is over the ICON only. Returns clicked.
bool draw_nav_item(float x, float& y, float w, const NavItem& it, bool active) {
    const float H = 46.0f;
    ImVec2 a(x, y), b(x + w, y + H);
    bool hov = hovered(a, b);
    if (active) {
        g.dl->AddRectFilled(a, b, C_AMBER_NAV, 11.0f);
        g.dl->AddRect(a, b, C_AMBER_NAVLN, 11.0f, 0, 1.0f);
    } else if (hov) {
        g.dl->AddRectFilled(a, b, C_ROW_HOVER, 11.0f);
    }
    ImVec2 ipos(x + 14.0f, y + H * 0.5f - 9.0f);           // icon 18, padding 14
    icon(it.icon_name, ipos, 18.0f, active ? C_AMBER : (hov ? C_MUTED : C_DIM));
    // label: gap 13 after icon → x + 14 + 18 + 13 = x + 45
    float lcy = y + H * 0.5f;
    if (active) textb_mid(x + 45.0f, lcy, 14.0f, C_AMBER_TEXT, it.label);
    else        text_mid (x + 45.0f, lcy, 14.0f, C_MUTED,      it.label);

    if (hovered(ImVec2(ipos.x - 3, ipos.y - 3), ImVec2(ipos.x + 20, ipos.y + 20)))
        tooltip(it.tip, it.tip_body);
    bool clk = clicked(a, b);
    y += H + 4.0f;
    return clk;
}

void draw_sidebar(float x, float y, float w, float h) {
    // No sidebar fill: the window ground (and the wallpaper on it) shows
    // through, so both halves of the panel read as one surface. Only the
    // divider marks the split (TZ §4).
    g.dl->AddLine(ImVec2(x + w, y), ImVec2(x + w, y + h), C_LINE, 1.0f);

    // ── Logo block (TZ §4.1): white 38×38 tile r11 with nightvex-logo.svg,
    //    then wordmark "Nightvex" 15.5/600. Padding 22 top / 20 left, gap 13.
    ImVec2 tp(x + 20.0f, y + 22.0f);
    ImVec2 tp1(tp.x + 38.0f, tp.y + 38.0f);
    g.dl->AddRectFilled(tp, tp1, P::LOGO_TILE, 11.0f);
    ImTextureID logo = (ImTextureID)abi::icons::get("nightvex_logo");
    if (logo)
        g.dl->AddImageRounded(logo, tp, tp1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 11.0f);
    textb(ImVec2(tp1.x + 13.0f, tp.y + (38.0f - 16.0f) * 0.5f - 1.0f), 16.0f, C_TEXT, "Nightvex");

    // ── Divider (TZ §4.2): margin 0 20.
    float dy = y + 22.0f + 38.0f + 18.0f;
    g.dl->AddLine(ImVec2(x + 20.0f, dy), ImVec2(x + w - 20.0f, dy), C_LINE, 1.0f);

    // ── Flat menu (TZ §4.5): Visuals, Radar, Overlay, Loot, Settings.
    //    padding 16 14, gap 4. No sections, no nested Players/Bots.
    float cy = dy + 16.0f;
    float ix = x + 14.0f, iw = w - 28.0f;

    const NavItem items[] = {
        { T("Players","Игроки"), "eye",      PG_PLAYERS,  T("Players","Игроки"),
          T("Enemy PMCs — boxes, skeletons, labels.", "Вражеские игроки — боксы, скелеты, подписи.") },
        { "AI",                  "eye",      PG_BOTS,     "AI",
          T("Bots — separate render settings.", "Боты — независимые настройки отрисовки.") },
        { T("Radar","Радар"),    "radar",    PG_RADAR,    T("Radar","Радар"),
          T("Radar: minimap with player and loot positions.", "Радар: миникарта с позициями игроков и лута.") },
        { T("Overlay","Оверлей"),"overlay",  PG_OVERLAY,  T("Overlay","Оверлей"),
          T("On-screen overlay: FPS, status panel, hotkeys.", "Наложение поверх игры: FPS, панель статуса, хоткеи.") },
        { T("Loot","Лут"),       "loot",     PG_LOOT,     T("Loot","Лут"),
          T("Loot: ground items, containers, value filter.", "Лут: предметы на земле, контейнеры, фильтр ценности.") },
    };
    for (const NavItem& it : items) {
        bool active = (st.page == it.page);
        if (draw_nav_item(ix, cy, iw, it, active)) {
            st.page = it.page;
            st.esp_show_bots = (it.page == PG_BOTS);
            st.open_dropdown = -1;
        }
    }

    // ── Footer (TZ §4.7): border-top, Release 1.1.10 12 #56565F, padding 16 20.
    float fy = y + h - 46.0f;
    g.dl->AddLine(ImVec2(x + 20.0f, fy), ImVec2(x + w - 20.0f, fy), C_LINE, 1.0f);
    text(ImVec2(x + 20.0f, fy + 16.0f), 12.0f, C_DIM, "Release 1.1.10");
}

// ════════════════════════════════════════════════════════════════════════════
//  ESP-ПРЕВЬЮ  (TZ-ESP-Preview: точные координаты в системе viewBox 220×330)
// ════════════════════════════════════════════════════════════════════════════
//
// Слой SVG — 323×484, сдвинут внутри кадра 290×435 на left −16 / top +4.
// Координаты бокса и скелета заданы в единицах viewBox 220×330; масштаб в
// пиксели: X ×1.4682, Y ×1.4667. Точка привязки слоя = (frame_x − 16,
// frame_y + 4). Хелпер SV(fx,fy) отображает единицы viewBox в экранные пиксели.
constexpr float kVbX = 323.0f / 220.0f;   // 1.4682
constexpr float kVbY = 484.0f / 330.0f;   // 1.4667
static inline ImVec2 SV(float fx, float fy, float vx, float vy) {
    return ImVec2(fx - 16.0f + vx * kVbX, fy + 4.0f + vy * kVbY);
}

// Бокс (TZ §7). 2D — четыре уголка по точным путям; 3D — плюс четыре штриха.
// stroke-width 2, linecap round, drop-shadow как мягкое подсвечивание.
// `corners` повторяет cfg.box_corners: выключено — сплошная рамка, как в игре.
void draw_box(float fx, float fy, ImU32 col, bool three_d, bool corners) {
    const float TH = 2.0f;
    ImU32 glow = (col & 0x00FFFFFF) | (70u << 24);
    auto seg = [&](float ax, float ay, float bx, float by) {
        ImVec2 a = SV(fx, fy, ax, ay), b = SV(fx, fy, bx, by);
        g.dl->AddLine(a, b, glow, TH + 2.5f);
        g.dl->AddLine(a, b, col, TH);
    };
    if (corners) {
        // 2D corners
        seg(50, 20, 50, 6);   seg(50, 6, 64, 6);       // левый верхний
        seg(154, 6, 168, 6);  seg(168, 6, 168, 20);    // правый верхний
        seg(168, 274, 168, 288); seg(168, 288, 154, 288); // правый нижний
        seg(64, 288, 50, 288); seg(50, 288, 50, 274);  // левый нижний
    } else {
        // Сплошная рамка — ветка cfg.box_corners == false в render.cpp.
        seg(50, 6, 168, 6);   seg(168, 6, 168, 288);
        seg(168, 288, 50, 288); seg(50, 288, 50, 6);
    }
    if (three_d) {
        seg(50, 6, 57, 2);   seg(168, 6, 175, 2);
        seg(168, 288, 175, 284); seg(50, 288, 57, 284);
    }
}

void draw_esp_preview(float x, float y, float w, float h, RenderConfig& cfg) {
    // TZ §7: preview panel bg #0E0E11, border .08, radius 16, padding 20 22.
    card_inset(ImVec2(x, y), ImVec2(x + w, y + h), C_INSET, C_LINE, 14.0f);

    // Header: frame icon 16 + "Interactive ESP preview" 13.5/500 + Players/Bots.
    {
        // frame glyph (4 L-corners), stroke #8B8B95
        float gx = x + 22.0f, gy = y + 22.0f, s = 16.0f, r = 5.0f;
        ImU32 gc = C_MUTED;
        g.dl->AddLine(ImVec2(gx, gy + r), ImVec2(gx, gy), gc, 1.7f);
        g.dl->AddLine(ImVec2(gx, gy), ImVec2(gx + r, gy), gc, 1.7f);
        g.dl->AddLine(ImVec2(gx + s - r, gy), ImVec2(gx + s, gy), gc, 1.7f);
        g.dl->AddLine(ImVec2(gx + s, gy), ImVec2(gx + s, gy + r), gc, 1.7f);
        g.dl->AddLine(ImVec2(gx + s, gy + s - r), ImVec2(gx + s, gy + s), gc, 1.7f);
        g.dl->AddLine(ImVec2(gx + s, gy + s), ImVec2(gx + s - r, gy + s), gc, 1.7f);
        g.dl->AddLine(ImVec2(gx + r, gy + s), ImVec2(gx, gy + s), gc, 1.7f);
        g.dl->AddLine(ImVec2(gx, gy + s), ImVec2(gx, gy + s - r), gc, 1.7f);
        text(ImVec2(gx + s + 11.0f, gy + 0.5f), 14.0f, C_MUTED,
             T("Interactive ESP preview","Интерактивное превью ESP"));
    }
    // 2026-09-23: inline Players/Bots segmented control removed — split into
    // two top-level sidebar tabs (Players + AI). st.esp_show_bots now mirrors
    // sidebar selection, set in the nav loop above.

    EspView e = esp_view(cfg, st.esp_show_bots);
    bool on = *e.enable;

    // ── Figure frame (TZ §6): 290×435 kept centred, layer 323×484 at −16/+4.
    const float fw = 290.0f, fh = 435.0f;
    float fx = x + w * 0.5f - fw * 0.5f;
    float bodyTop = y + 54.0f, bodyBot = y + h - 10.0f;
    float blockH = 44.0f + fh + 30.0f;             // name/HP above + figure + bottom row
    float fy = bodyTop + 44.0f + std::max(0.0f, ((bodyBot - bodyTop) - blockH) * 0.5f);
    float cxF = fx + fw * 0.5f;

    // operator.png in the 323×484 layer (contain), clipped to the 290×435 frame.
    ImVec2 frameA(fx, fy), frameB(fx + fw, fy + fh);
    ImVec2 layerA(fx - 16.0f, fy + 4.0f), layerB(layerA.x + 323.0f, layerA.y + 484.0f);
    g.dl->PushClipRect(frameA, frameB, true);
    if (st.operator_tex) {
        g.dl->AddImage(st.operator_tex, layerA, layerB);
    } else {
        text_center(cxF, fy + fh * 0.5f - 7.0f, 12.0f, C_FAINT, "operator.png");
    }
    g.dl->PopClipRect();

    // Box — exact viewBox coordinates.
    if (on && *e.box) draw_box(fx, fy, *e.col_box, *e.box_style == 1, cfg.box_corners);

    // ── Name + TEAM pill (TZ §4): one centred row, gap 9. Pill = outline only.
    const char* nick = st.esp_show_bots ? "Scav Raider" : "Nightreaper_07";
    const char* team = st.esp_show_bots ? "SCAV" : "TEAM B";
    float rowY = fy - 44.0f;
    if (on && (*e.name || *e.team)) {
        float nickW = *e.name ? measureb(14.0f, nick).x : 0.0f;
        float pillW = *e.team ? measureb(10.5f, team).x + 14.0f : 0.0f;   // padding 7×2
        float gap   = (*e.name && *e.team) ? 9.0f : 0.0f;
        float gx    = cxF - (nickW + gap + pillW) * 0.5f;
        // Both the nick and the pill hang off one shared centre line so the
        // pill's caps sit in its middle instead of resting on the bottom edge.
        float rowCy = rowY + 9.0f;
        if (*e.name) { textb_mid(gx, rowCy, 14.0f, *e.col_name, nick); gx += nickW + gap; }
        if (*e.team) {
            const float pillH = 19.0f;
            ImVec2 pa(gx, rowCy - pillH * 0.5f), pb(gx + pillW, rowCy + pillH * 0.5f);
            g.dl->AddRect(pa, pb, *e.col_team, 6.0f, 0, 1.0f);            // contour only
            textb_mid(gx + 7.0f, rowCy, 10.5f, *e.col_team, team);
        }
    }
    // HP (TZ §5): mono 12.5/600 #4BBF8A, margin-top −4 (sits ~8 px under name).
    if (on && *e.health) text_center_mid(cxF, rowY + 31.0f, 12.5f, C_HP, "445/445", true);

    // ── Armor bar (TZ §9): 7×413 at wrapper right −19 / top 13, halves 205 gap 3.
    if (on && *e.armor && *e.armor_display == 1) {
        float bx = fx + fw + 12.0f;      // right −19, width 7 → left = frame_right + 12
        float by = fy + 13.0f, barH = 413.0f, half = (barH - 3.0f) * 0.5f;
        ImU32 t4 = e.armor_tier[3], t5 = e.armor_tier[4];
        g.dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + 7.0f, by + half), t4, 4.0f);
        g.dl->AddRectFilled(ImVec2(bx, by + half + 3.0f), ImVec2(bx + 7.0f, by + barH), t5, 4.0f);
        text_mid(bx + 7.0f + 12.0f, by + barH * 0.25f, 10.5f, t4, "H4", true);
        text_mid(bx + 7.0f + 12.0f, by + barH * 0.75f, 10.5f, t5, "V5", true);
    }

    // ── Bottom row (TZ §10): gap 14, no separators. Weapon 13/600, rest mono.
    float ly = fy + fh + 16.0f;
    struct Part { const char* s; ImU32 col; bool mono; };
    Part parts[4]; int np = 0;
    const char* wpn = st.esp_show_bots ? "AKM" : "M4A1";
    if (on && *e.weapon)   parts[np++] = { wpn, *e.col_weapon, false };
    if (on && *e.ammo)     parts[np++] = { "30 / 90", *e.col_ammo, true };
    if (on && *e.armor && *e.armor_display == 0)
        parts[np++] = { "H4 \xC2\xB7 V5", e.armor_tier[3], true };
    if (on && *e.distance) parts[np++] = { "142 m", *e.col_distance, true };
    if (np) {
        // One face for the whole strip. TZ §10 specifies Segoe UI 13/600 for the
        // weapon and mono 12.5/400 for the numbers, but Consolas is noticeably
        // wider and heavier than Segoe UI, so the mixed line read as two
        // different sizes. These are all technical HUD readouts — a single mono
        // face is calmer, and the weapon still stands out by its colour.
        const float SZ = 12.5f;
        float total = 0.0f;
        for (int i = 0; i < np; ++i)
            total += measure(SZ, parts[i].s, true).x + (i ? 14.0f : 0.0f);
        float cx = cxF - total * 0.5f;
        for (int i = 0; i < np; ++i) {
            if (i) cx += 14.0f;
            text(ImVec2(cx, ly), SZ, parts[i].col, parts[i].s, true);
            cx += measure(SZ, parts[i].s, true).x;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  МОДАЛКА ЦВЕТА / НАСТРОЕК
// ════════════════════════════════════════════════════════════════════════════

void open_modal(const char* title, ModalKind kind, ImU32* target, bool has_settings) {
    Modal& m = st.modal;
    m.open = true; m.kind = kind; m.target = target;
    m.has_settings = has_settings;
    m.tab = has_settings ? 0 : 1;
    m.backup = target ? *target : 0;
    // v1.0.38.17 HIGH fix: clear position-modal fields when a non-position
    // modal opens. Prior: int_target/int_backup set only by
    // open_modal_position, never cleared by open_modal — so after any
    // position chooser closed via Done (toggles m.open only), subsequent
    // color-modal cancel at line 1359 blindly deref'd a now-stale
    // int_target pointing into a destroyed/renamed field. Audit workflow
    // HIGH (control_panel.cpp:1359 modal correctness, trek B).
    m.int_target = nullptr; m.int_backup = 0;
    m.just_opened = true;   // swallow the opening click (see Modal::just_opened)
    std::snprintf(m.title, sizeof(m.title), "%s", title);
    if (target) {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(*target);
        ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, m.h, m.s, m.v);
    }
    st.open_dropdown = -1;
}

// Open the "where should this sit on screen" chooser. Same modal chrome as the
// colour/settings one, but the body is a vertical list of placements.
void open_modal_position(const char* title, const char* const* items, int count, int* value) {
    Modal& m = st.modal;
    m.open = true; m.kind = MK_POSITION; m.target = nullptr;
    m.has_settings = true; m.tab = 0;
    m.just_opened = true;
    m.int_target = value; m.int_backup = value ? *value : 0;
    m.opt_count = ImClamp(count, 0, Modal::MAX_OPTS);
    for (int i = 0; i < m.opt_count; ++i) m.opt_items[i] = items ? items[i] : "";
    std::snprintf(m.title, sizeof(m.title), "%s", title);
    st.open_dropdown = -1;
}

void apply_hsv_to_target() {
    Modal& m = st.modal;
    if (!m.target) return;
    float r, gg, b;
    ImGui::ColorConvertHSVtoRGB(m.h, m.s, m.v, r, gg, b);
    *m.target = IM_COL32((int)(r * 255 + 0.5f), (int)(gg * 255 + 0.5f), (int)(b * 255 + 0.5f), 255);
}

// ── Slider values that have no home in RenderConfig ───────────────────────
// Declared before the modal because both the modal and the pages read them.
// Everything else slides its real config field (or its int shadow) directly —
// the old 0..100 percentage domain is gone, because 101 drag positions can
// never map onto round numbers (100 + pct*3 gave you 259 px and nothing you
// could do about it).
int s_loot_min_value = 25000;   // $, step 1000
int s_fight_range_m  = 100;     // m, step 10

// Snap an int to the nearest multiple of `step` inside [lo, hi]. Every slider
// pushes its result through this, so a drag always lands on a round number.
int snap_step(int v, int lo, int hi, int step) {
    if (step <= 1) return ImClamp(v, lo, hi);
    int n = (int)std::lround((float)(v - lo) / (float)step);
    return ImClamp(lo + n * step, lo, hi);
}

// Shared slider body (TZ-Modal §5.2): label on its own line, then a flex row of
// [track] gap 13 [value] with the value optically centred on the track.
// `v` holds the real value (metres, px, dollars…). `valueText` overrides the
// default "<v><unit>" rendering when the caller formats it itself ("$25,000").
// `label_gap` is the label-top → track distance and `vsz` the value size — the
// modal runs a touch larger than the pages.
float unit_slider(float x, float y, float w, const char* label,
                  int* v, int lo, int hi, int step, const char* unit,
                  const char* valueText = nullptr, float label_gap = 24.0f,
                  float vsz = 12.0f, bool enabled = true) {
    text(ImVec2(x, y), 12.5f, C_MUTED, label);
    char buf[40];
    if (!valueText) {
        std::snprintf(buf, sizeof(buf), "%d%s", *v, unit ? unit : "");
        valueText = buf;
    }
    const float gap = 13.0f, th = 7.0f;
    ImVec2 vm   = measure(vsz, valueText, true);
    float  valW = ImMax(52.0f, vm.x);
    float  ty   = y + label_gap;
    float  tw   = ImMax(40.0f, w - gap - valW);
    ImVec2 a(x, ty), b(x + tw, ty + th);
    card(a, b, C_TRACK, C_LINE, 4.0f);
    float t = ImClamp((float)(*v - lo) / (float)ImMax(1, hi - lo), 0.0f, 1.0f);
    if (t > 0.0f)
        g.dl->AddRectFilledMultiColor(a, ImVec2(x + tw * t, ty + th),
                                      P::SLIDER_LO, P::AMBER, P::AMBER, P::SLIDER_LO);
    g.dl->AddCircleFilled(ImVec2(x + tw * t, ty + th * 0.5f), 7.5f,
                          enabled ? IM_COL32_WHITE : C_FAINT, 20);
    text_mid(x + w - vm.x, ty + th * 0.5f, vsz, enabled ? C_MUTED : C_DIM, valueText, true);
    float dt = slider_drag(v, ImVec2(x - 5.0f, ty - 9.0f),
                           ImVec2(x + tw + 5.0f, ty + th + 9.0f), x, tw, enabled);
    if (dt >= 0.0f) *v = snap_step(lo + (int)std::lround(dt * (hi - lo)), lo, hi, step);
    return label_gap + th + 6.0f;
}

// ── modal-specific widgets (TZ-Modal §5) ───────────────────────────────
// Labeled segment group: label 12.5/#8B8B95, group #0C0C0F r9 pad3,
// option 12px active #1A1A1F+.12/600, passive #56565F/500. Returns height.
float modal_seg(float x, float y, float w, const char* label,
                const char* const* opts, int n, int* v) {
    text(ImVec2(x, y), 12.5f, C_MUTED, label);
    float gy = y + 20.0f, gh = 34.0f, pad = 3.0f;
    ImVec2 a(x, gy), b(x + w, gy + gh);
    card(a, b, C_TRACK, C_LINE, 11.0f);
    float seg = (w - pad * 2.0f) / (float)n;
    for (int i = 0; i < n; ++i) {
        ImVec2 sa(x + pad + seg * i, gy + pad), sb(sa.x + seg, gy + gh - pad);
        bool on = (*v == i);
        if (on) { g.dl->AddRectFilled(sa, sb, C_SEG_ON, 8.0f); g.dl->AddRect(sa, sb, C_LINE2, 8.0f, 0, 1.0f); }
        else if (hovered(sa, sb)) g.dl->AddRectFilled(sa, sb, C_ROW_HOVER, 8.0f);
        float tcx = (sa.x + sb.x) * 0.5f, tcy = gy + gh * 0.5f;
        if (on) textb_center_mid(tcx, tcy, 12.0f, C_TEXT, opts[i]);
        else    text_center_mid (tcx, tcy, 12.0f, C_DIM,  opts[i]);
        if (clicked(sa, sb)) *v = i;
    }
    return 20.0f + gh;
}

// Slider (TZ §5.2): label on top, [track flex] gap13 [value mono right].
// Thin wrapper over unit_slider with the modal's slightly larger value size.
float modal_slider(float x, float y, float w, const char* label,
                   int* v, int lo, int hi, const char* unit, int step = 1) {
    return unit_slider(x, y, w, label, v, lo, hi, step, unit, nullptr, 24.0f, 12.5f);
}

// Settings-tab body. Advances y, returns nothing (y passed by ref).
void modal_settings_body(float x, float& y, float w, RenderConfig& cfg) {
    Modal& m = st.modal;
    EspView e = esp_view(cfg, st.esp_show_bots);
    switch (m.kind) {
    case MK_BOX: {
        const char* styles[2] = { "2D", "3D" };
        y += modal_seg(x, y, w, T("Style","Стиль"), styles, 2, e.box_style) + 16.0f;
        y += modal_slider(x, y, w, T("Box distance","Дистанция бокса"),
                          e.box_distance, 0, 400, " m", 10);
        break;
    }
    case MK_ARMOR: {
        const char* disp[2] = { T("Text","Текст"), T("Bar","Полоса") };
        y += modal_seg(x, y, w, T("Display","Отображение"), disp, 2, e.armor_display);
        break;
    }
    case MK_CORPSES: {
        CorpsesView cv = corpses_view(cfg);
        y += modal_slider(x, y, w, T("Min value ($)","Мин. стоимость ($)"),
                          cv.min_items, 0, 500, "$", 5);
        break;
    }
    case MK_PREFIRE: {   // TZ-Other-Pages §3.1
        const char* presets[3] = { "200", "300", "400+" };
        int sel = (cfg.prefire_grace_ms >= 400) ? 2 : (cfg.prefire_grace_ms >= 300 ? 1 : 0);
        int prev = sel;
        y += modal_seg(x, y, w, T("Presets","Пресеты"), presets, 3, &sel) + 16.0f;
        if (sel != prev) cfg.prefire_grace_ms = (sel == 0 ? 200 : sel == 1 ? 300 : 400);
        y += modal_slider(x, y, w, T("Grace duration","Длительность"),
                          &cfg.prefire_grace_ms, 0, 500, " ms", 25);
        break;
    }
    case MK_TOPLOOT: {   // TZ-Other-Pages §4.3
        // s_loot_max_dist is the shadow for cfg.top_loot_range_m; loot_post_sync
        // folds it back at the end of the frame, so sliding it here is enough.
        y += modal_slider(x, y, w, T("List size","Длина списка"),
                          &cfg.top_loot_max, 3, 30, "", 1) + 16.0f;
        y += modal_slider(x, y, w, T("Top loot range","Радиус топ-лута"),
                          &s_loot_max_dist, 0, 300, " m", 10);
        break;
    }
    case MK_POSITION: {   // vertical list of placements, one row each
        const float rowH = 38.0f, gap = 6.0f;
        for (int i = 0; i < m.opt_count; ++i) {
            ImVec2 a(x, y + (rowH + gap) * i), b(x + w, a.y + rowH);
            bool sel = (m.int_target && *m.int_target == i);
            bool hov = hovered(a, b);
            if (sel)      card(a, b, C_AMBER_FILL, C_AMBER_LINE, 11.0f);
            else if (hov) card(a, b, C_SEG_ON, C_LINE, 11.0f);
            else          card(a, b, C_CHIP, C_LINE, 11.0f);
            float mid = (a.y + b.y) * 0.5f;
            if (sel) {
                textb_mid(a.x + 14.0f, mid, 13.0f, C_AMBER_TEXT, m.opt_items[i]);
                check_glyph(ImVec2(b.x - 20.0f, mid), 13.0f, C_AMBER);
            } else {
                text_mid(a.x + 14.0f, mid, 13.0f, C_MUTED, m.opt_items[i]);
            }
            if (clicked(a, b) && m.int_target) *m.int_target = i;
        }
        y += m.opt_count * rowH + (m.opt_count - 1) * gap;
        break;
    }
    default:
        text(ImVec2(x, y), 12.5f, C_DIM, T("No extra settings.","Дополнительных настроек нет."));
        y += 22.0f;
        break;
    }
}

// Color-tab body (TZ §6): tier chips (Armor) → SV field → hue → palette.
void modal_color_body(float x, float& y, float w, RenderConfig& cfg) {
    Modal& m = st.modal;

    // 6.1 Tier chips T1..T6 (Armor only): gap 6, chip pad 6/0 r8, 11/600, dot 8.
    if (m.kind == MK_ARMOR) {
        EspView e = esp_view(cfg, st.esp_show_bots);
        float gap = 6.0f, cw = (w - gap * 5.0f) / 6.0f, ch = 30.0f;
        for (int i = 0; i < 6; ++i) {
            ImVec2 a(x + (cw + gap) * i, y), b(a.x + cw, y + ch);
            bool sel = (m.tier == i);
            card(a, b, sel ? C_SEG_ON : IM_COL32(0,0,0,0), sel ? C_LINE2 : C_LINE, 8.0f);
            float mid = (a.x + b.x) * 0.5f;
            char lbl[4]; std::snprintf(lbl, sizeof(lbl), "T%d", i + 1);
            float lblW = measureb(11.0f, lbl).x;
            float grp = 8.0f + 5.0f + lblW;                 // dot(8) + gap(5) + text
            float gx = mid - grp * 0.5f;
            g.dl->AddCircleFilled(ImVec2(gx + 4.0f, y + ch * 0.5f), 4.0f, e.armor_tier[i], 16);
            textb_mid(gx + 13.0f, y + ch * 0.5f, 11.0f, sel ? C_TEXT : C_MUTED, lbl);
            if (clicked(a, b)) {
                m.tier = i; m.target = &e.armor_tier[i];
                ImVec4 c = ImGui::ColorConvertU32ToFloat4(*m.target);
                ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, m.h, m.s, m.v);
            }
        }
        if (!m.target) m.target = &e.armor_tier[m.tier];
        y += ch + 16.0f;
    }

    // 6.2 SV field — full width, height 130, contour via outer ring (no border).
    float sv_h = 130.0f;
    ImVec2 sa(x, y), sb(x + w, y + sv_h);
    float rr, gg, bb;
    ImGui::ColorConvertHSVtoRGB(m.h, 1.0f, 1.0f, rr, gg, bb);
    ImU32 hue_col = IM_COL32((int)(rr*255), (int)(gg*255), (int)(bb*255), 255);
    g.dl->AddRectFilledMultiColor(sa, sb, IM_COL32_WHITE, hue_col, hue_col, IM_COL32_WHITE);
    g.dl->AddRectFilledMultiColor(sa, sb, IM_COL32(0,0,0,0), IM_COL32(0,0,0,0), IM_COL32_BLACK, IM_COL32_BLACK);
    g.dl->AddRect(ImVec2(sa.x-1, sa.y-1), ImVec2(sb.x+1, sb.y+1), C_LINE, 14.0f, 0, 1.0f); // contour ring
    ImVec2 cur(sa.x + m.s * w, sa.y + (1.0f - m.v) * sv_h);
    ImU32 curcol = m.target ? *m.target : IM_COL32_WHITE;
    g.dl->AddCircleFilled(cur, 8.0f, curcol, 24);
    g.dl->AddCircle(cur, 8.0f, IM_COL32_WHITE, 24, 2.5f);
    if (hovered(sa, sb) && ImGui::IsMouseClicked(0)) m.drag_sv = true;
    if (m.drag_sv) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        m.s = ImClamp((mp.x - sa.x) / w, 0.0f, 1.0f);
        m.v = ImClamp(1.0f - (mp.y - sa.y) / sv_h, 0.0f, 1.0f);
        apply_hsv_to_target();
        if (!ImGui::IsMouseDown(0)) m.drag_sv = false;
    }
    y += sv_h + 16.0f;

    // 6.3 Hue bar — full width, height 12, horizontal rainbow.
    float hue_h = 12.0f;
    ImVec2 ha(x, y), hb(x + w, y + hue_h);
    const int SEG = 6;
    for (int i = 0; i < SEG; ++i) {
        float h0 = (float)i / SEG, h1 = (float)(i + 1) / SEG, r0,g0,b0,r1,g1,b1;
        ImGui::ColorConvertHSVtoRGB(h0,1,1,r0,g0,b0);
        ImGui::ColorConvertHSVtoRGB(h1,1,1,r1,g1,b1);
        ImU32 c0 = IM_COL32((int)(r0*255),(int)(g0*255),(int)(b0*255),255);
        ImU32 c1 = IM_COL32((int)(r1*255),(int)(g1*255),(int)(b1*255),255);
        g.dl->AddRectFilledMultiColor(ImVec2(ha.x + w*h0, ha.y), ImVec2(ha.x + w*h1, hb.y), c0, c1, c1, c0);
    }
    float hx = ha.x + m.h * w;
    ImU32 hcol = IM_COL32((int)(rr*255),(int)(gg*255),(int)(bb*255),255);
    g.dl->AddCircleFilled(ImVec2(hx, ha.y + hue_h*0.5f), 8.0f, hcol, 24);
    g.dl->AddCircle(ImVec2(hx, ha.y + hue_h*0.5f), 8.0f, IM_COL32_WHITE, 24, 2.5f);
    if (hovered(ha, hb) && ImGui::IsMouseClicked(0)) m.drag_hue = true;
    if (m.drag_hue) {
        m.h = ImClamp((ImGui::GetIO().MousePos.x - ha.x) / w, 0.0f, 0.9999f);
        apply_hsv_to_target();
        if (!ImGui::IsMouseDown(0)) m.drag_hue = false;
    }
    y += hue_h + 16.0f;

    // 6.4 Palette label + hex.
    text(ImVec2(x, y), 12.5f, C_MUTED, T("Presets (field)","Готовые (полевые)"));
    if (m.target) {
        char hexbuf[16]; ImU32 c = *m.target;
        std::snprintf(hexbuf, sizeof(hexbuf), "#%02X%02X%02X", (c>>IM_COL32_R_SHIFT)&0xFF,
                      (c>>IM_COL32_G_SHIFT)&0xFF, (c>>IM_COL32_B_SHIFT)&0xFF);
        text_right(x + w, y, 12.0f, C_MUTED, hexbuf, true);
    }
    y += 22.0f;

    // Palette — 30 circles Ø26, 10 cols × 3 rows, space-between, v-gap 11.
    const int COLS = 10, ROWS = 3;
    float d = 26.0f, gapx = (w - COLS * d) / (COLS - 1), gapy = 11.0f;
    for (int i = 0; i < COLS * ROWS; ++i) {
        int cxi = i % COLS, cyi = i / COLS;
        ImVec2 c(x + (d + gapx) * cxi + d * 0.5f, y + (d + gapy) * cyi + d * 0.5f);
        ImU32 col = rgb(kPalette[i]);
        g.dl->AddCircleFilled(c, d * 0.5f, col, 28);
        bool sel = m.target && ((*m.target & 0x00FFFFFF) == (col & 0x00FFFFFF));
        if (sel) {
            g.dl->AddCircle(c, d * 0.5f + 3.0f, C_AMBER, 28, 2.0f);   // amber outer ring
            g.dl->AddCircle(c, d * 0.5f + 1.0f, P::MODAL, 28, 2.0f);
            // checkmark
            ImVec2 chk[3] = { ImVec2(c.x - 5.0f, c.y + 0.5f), ImVec2(c.x - 1.5f, c.y + 4.0f),
                              ImVec2(c.x + 5.0f, c.y - 4.0f) };
            g.dl->AddPolyline(chk, 3, P::ON_LIGHT, 0, 2.6f);
        } else {
            g.dl->AddCircle(c, d * 0.5f, wht(0.14f), 28, 1.0f);
        }
        ImVec2 a(c.x - d*0.5f, c.y - d*0.5f), b(c.x + d*0.5f, c.y + d*0.5f);
        if (clicked(a, b) && m.target) {
            *m.target = col;
            ImVec4 f = ImGui::ColorConvertU32ToFloat4(col);
            ImGui::ColorConvertRGBtoHSV(f.x, f.y, f.z, m.h, m.s, m.v);
        }
    }
    y += ROWS * d + (ROWS - 1) * gapy + 4.0f;
}

// Compute the body height so the container can be drawn behind the content.
float modal_body_height(RenderConfig& cfg) {
    Modal& m = st.modal;
    if (m.tab == 0 && m.has_settings) {
        switch (m.kind) {
        case MK_BOX:      return 54.0f + 16.0f + 37.0f;   // seg + gap + slider
        case MK_ARMOR:    return 54.0f;                   // seg
        case MK_CORPSES:  return 37.0f;
        case MK_PREFIRE:  return 54.0f + 16.0f + 37.0f;   // presets + slider
        case MK_TOPLOOT:  return 37.0f + 16.0f + 37.0f;   // two sliders
        case MK_POSITION: return m.opt_count * 38.0f + (m.opt_count - 1) * 6.0f;
        default:          return 22.0f;
        }
    }
    // Color tab
    float h = 0.0f;
    if (m.kind == MK_ARMOR) h += 30.0f + 16.0f;           // tier chips
    h += 130.0f + 16.0f;                                  // SV
    h += 12.0f + 16.0f;                                   // hue
    h += 22.0f;                                           // label
    h += 3 * 26.0f + 2 * 11.0f + 4.0f;                    // palette
    return h;
}

// Close without applying: restore whichever payload this modal was editing.
void modal_cancel() {
    Modal& m = st.modal;
    if (m.target)     *m.target = m.backup;
    if (m.int_target) *m.int_target = m.int_backup;
    m.open = false;
}

void draw_modal(RenderConfig& cfg, ImVec2 win_a, ImVec2 win_b) {
    Modal& m = st.modal;
    if (!m.open) return;

    // First frame: the mouse button is still "clicked" from the row that opened
    // us. Draw everything, but let no widget react — g.blocked makes every
    // hovered()/clicked() return false — and skip the scrim/Esc close below.
    const bool first_frame = m.just_opened;
    m.just_opened = false;
    const bool saved_blocked = g.blocked;
    if (first_frame) g.blocked = true;

    // Scrim (TZ §1): rgba(4,4,6,.62) inside the window; click = cancel.
    g.dl->AddRectFilled(win_a, win_b, P::MODAL_SCRIM, kWinRadius);

    const float W = 390.0f, pad = 20.0f, gap = 16.0f;
    float x = (win_a.x + win_b.x) * 0.5f - W * 0.5f;
    float y = win_a.y + 150.0f;   // padding-top 150

    bool has_tabs = m.has_settings && m.target;   // both settings + colour
    float px = x + pad, pw = W - pad * 2.0f;

    // Height: pad + header(30) + gap + [tabs(30)+gap] + body + gap + buttons(38) + pad.
    float body_h = modal_body_height(cfg);
    float H = pad + 30.0f + gap + (has_tabs ? (34.0f + gap) : 0.0f) + body_h + gap + 38.0f + pad;

    ImVec2 a(x, y), b(x + W, y + H);
    g.dl->AddRectFilled(ImVec2(a.x, a.y + 10.0f), ImVec2(b.x, b.y + 14.0f), IM_COL32(0, 0, 0, 130), 16.0f); // shadow
    card_inset(a, b, P::MODAL, C_LINE2, 16.0f);   // floating: hair-strong + shadow + inset

    float cy = y + pad;

    // ── Header (TZ §3): title 16/600 · [colour dot 16 + hex] · close 30 r9.
    textb(ImVec2(px, cy + 4.0f), 16.0f, C_TEXT, m.title);
    {
        ImVec2 ca(b.x - pad - 30.0f, cy), cb(ca.x + 30.0f, cy + 30.0f);
        bool hov = hovered(ca, cb);
        card(ca, cb, hov ? C_SEG_ON : C_CHIP, C_LINE, 8.0f);
        icon("close", ImVec2(ca.x + 7.0f, ca.y + 7.0f), 16.0f, C_MUTED);
        if (clicked(ca, cb)) { modal_cancel(); return; }
    }
    if (m.target) {   // colour dot + hex to the left of the close button
        ImU32 c = *m.target;
        char hexbuf[12]; std::snprintf(hexbuf, sizeof(hexbuf), "#%02X%02X%02X",
                      (c>>IM_COL32_R_SHIFT)&0xFF, (c>>IM_COL32_G_SHIFT)&0xFF, (c>>IM_COL32_B_SHIFT)&0xFF);
        float hexW = measure(12.0f, hexbuf, true).x;
        float hx = b.x - pad - 30.0f - 8.0f - hexW;
        text(ImVec2(hx, cy + 8.0f), 12.0f, C_MUTED, hexbuf, true);
        ImVec2 dc(hx - 8.0f - 8.0f, cy + 15.0f);
        g.dl->AddCircleFilled(dc, 8.0f, c, 20);
        g.dl->AddCircle(dc, 8.0f, wht(0.25f), 20, 1.0f);
    }
    cy += 30.0f + gap;

    // ── Tabs (TZ §4) — only when both settings and colour exist.
    if (has_tabs) {
        const char* tabs[2] = { T("Settings","Настройки"), T("Colour","Цвет") };
        float th = 34.0f, tpad = 3.0f, seg = (pw - tpad*2.0f) * 0.5f;
        card(ImVec2(px, cy), ImVec2(px + pw, cy + th), C_TRACK, C_LINE, 11.0f);
        for (int i = 0; i < 2; ++i) {
            ImVec2 ta(px + tpad + seg*i, cy + tpad), tb(ta.x + seg, cy + th - tpad);
            bool on = (m.tab == i);
            if (on) { g.dl->AddRectFilled(ta, tb, C_SEG_ON, 8.0f); g.dl->AddRect(ta, tb, C_LINE2, 8.0f, 0, 1.0f); }
            float tcx = (ta.x+tb.x)*0.5f, tcy = cy + th*0.5f;
            if (on) textb_center_mid(tcx, tcy, 12.5f, C_TEXT, tabs[i]);
            else    text_center_mid (tcx, tcy, 12.5f, C_DIM,  tabs[i]);
            if (clicked(ta, tb)) m.tab = i;
        }
        cy += th + gap;
    }

    // ── Body.
    if (m.tab == 0 && m.has_settings) modal_settings_body(px, cy, pw, cfg);
    else                              modal_color_body(px, cy, pw, cfg);

    // ── Buttons (TZ §7): Отмена / Готово, 38 r10, 50/50, gap 10.
    float by = b.y - pad - 38.0f, bh = 38.0f, half = (pw - 10.0f) * 0.5f;
    {
        ImVec2 ca(px, by), cb(px + half, by + bh);
        bool hov = hovered(ca, cb);
        card(ca, cb, hov ? C_SEG_ON : C_CHIP, C_LINE2, 11.0f);
        textb_center_mid((ca.x+cb.x)*0.5f, by + bh*0.5f, 13.0f, C_TEXT, T("Cancel","Отмена"));
        if (clicked(ca, cb)) { modal_cancel(); return; }
    }
    {
        ImVec2 ca(px + half + 10.0f, by), cb(px + pw, by + bh);
        bool hov = hovered(ca, cb);
        g.dl->AddRectFilled(ca, cb, hov ? P::alpha(P::AMBER, 0.20f) : C_AMBER_FILL, 11.0f);
        g.dl->AddRect(ca, cb, C_AMBER_LINE, 11.0f, 0, 1.0f);
        textb_center_mid((ca.x+cb.x)*0.5f, by + bh*0.5f, 13.0f, C_AMBER_TEXT, T("Done","Готово"));
        if (clicked(ca, cb)) { m.open = false; return; }
    }

    // Click on the scrim (outside the modal) = cancel. Never on the opening frame.
    if (!first_frame) {
        if (ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(a, b, false)) {
            modal_cancel();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            modal_cancel();
        }
    }
    g.blocked = saved_blocked;
}

// ════════════════════════════════════════════════════════════════════════════
//  СТРАНИЦЫ
// ════════════════════════════════════════════════════════════════════════════

// ── Прокручиваемая колонка списка ─────────────────────────────────────────
// Строки с подписью выше, поэтому длинные списки перестали помещаться в окно.
// begin возвращает стартовый y с учётом смещения и включает клип (иначе
// уехавшие строки продолжают ловить клики), end клампит смещение и рисует
// ползунок. Раньше эта механика жила только внутри page_overlay.
float list_scroll_begin(Page pg, float x, float y, float w, float h) {
    float& sc = st.scroll[pg];
    if (ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + w, y + h), false)) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) sc -= wheel * 48.0f;
    }
    // Зажимаем ДО отрисовки, по высоте с прошлого кадра. На первом кадре она
    // ещё 0, поэтому список просто стартует сверху.
    sc = ImClamp(sc, 0.0f, ImMax(0.0f, st.content_h[pg] - h));
    g.dl->PushClipRect(ImVec2(x, y - 4.0f), ImVec2(x + w, y + h), true);
    g_clip_on = true;
    g_clip_a = ImVec2(x, y - 4.0f);
    g_clip_b = ImVec2(x + w, y + h);
    return y - sc;
}
// gutter — ширина промежутка справа от списка; ползунок ставится по его
// середине, чтобы не липнуть ни к строкам, ни к соседней колонке.
void list_scroll_end(Page pg, float x, float y, float w, float h, float cy,
                     float gutter) {
    float& sc = st.scroll[pg];
    g.dl->PopClipRect();
    g_clip_on = false;
    float content_h = (cy + sc) - y;
    st.content_h[pg] = content_h;
    sc = ImClamp(sc, 0.0f, ImMax(0.0f, content_h - h));
    if (content_h > h) {
        // Тонкая янтарная пилюля: 4 px, полное скругление торцов.
        const float BAR_W = 4.0f;
        bool over = ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + w + gutter, y + h), false);
        float thumb_h = ImMax(30.0f, h * (h / content_h));
        float t = (content_h - h) > 0.0f ? sc / (content_h - h) : 0.0f;
        float bx = x + w + (gutter - BAR_W) * 0.5f, by = y + t * (h - thumb_h);
        g.dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + BAR_W, by + thumb_h),
                            P::alpha(P::AMBER, over ? 0.45f : 0.25f), BAR_W * 0.5f);
    }
}

void page_visuals(float x, float y, float w, float h, RenderConfig& cfg, bool bots) {
    EspView e = esp_view(cfg, bots);
    CorpsesView cv = corpses_view(cfg);
    st.esp_show_bots = bots;

    // TZ §6/§7: preview fixed 520 on the right, list flex (remaining), gap 18.
    float preview_w = kPreviewW;
    float preview_x = x + w - preview_w;
    const float gutter = 24.0f;          // место под ползунок, 4pt-шаг
    float list_w    = w - preview_w - gutter;

    // Список прокручивается: со строками в две строки текста он выше окна.
    float cy = list_scroll_begin(bots ? PG_BOTS : PG_PLAYERS, x, y, list_w, h);
    // Enable — master. Off → clear every flag (list greys out); On → base set.
    {
        RowResult r = setting_row(x, cy, list_w, "power", T("Enable","Включить"),
                                  bots ? T("Bot rendering","Отрисовка ботов")
                                       : T("Player rendering","Отрисовка игроков"),
                                  e.enable, nullptr, false, true);
        if (r.toggled && !*e.enable) {
            *e.box = *e.name = *e.team = *e.health =
            *e.weapon = *e.ammo = *e.armor = *e.distance = false;
        } else if (r.toggled && *e.enable) {
            *e.box = *e.name = *e.health = true;
        }
    }
    bool en = *e.enable;

    // TZ §6.4 order: Box, Skeleton, Name, Team, Weapon, Ammo, Health, Armor,
    // Distance, Corpses. No subtitles. Health has NO colour dot (HP = #4BBF8A).
    // Armor row dot = tier T5.
    // Bots (TZ-Other-Pages §1) drop the Team and Health rows — 9 rows vs 10.
    // global — строка не гаснет вместе с мастер-тумблером ESP: она про HUD
    // или про правила отбора целей, а не про элемент отрисовки.
    struct Item { const char* icon; const char* title; const char* sub;
                  bool* on; ImU32* col; bool gear; ModalKind kind; bool global; };
    Item items[20]; int ni = 0;
    items[ni++] = { "rect",     T("Box","Бокс"),           T("Frame around the target","Рамка вокруг цели"),
                    e.box,      e.col_box,      true,  MK_BOX,      false };
    items[ni++] = { "frame",    T("Corner brackets","Уголки бокса"), T("Corners instead of a full frame","Уголки вместо сплошной рамки"),
                    bots ? &cfg.box_corners_bot : &cfg.box_corners, nullptr,  false, MK_PLAIN,    false };
    // Skeleton row removed — not implemented in arenahack (30-bone read per
    // pawn crushes reader Hz; would need batched FTransform read to be viable).
    if (!bots)
    items[ni++] = { "tag",      T("Name","Ник"),           T("Name above the frame","Имя над рамкой"),
                    e.name,     e.col_name,     false, MK_PLAIN,    false };
    // Team ID inherits Name colour — no separate swatch (nullptr disables picker).
    if (!bots)
    items[ni++] = { "users",    T("Team","Команда"),       T("Team tag next to the name","Метка команды у ника"),
                    e.team,     nullptr,        false, MK_PLAIN,    false };
    items[ni++] = { "weapon",   T("Weapon","Оружие"),      T("Weapon in hands","Оружие в руках"),
                    e.weapon,   e.col_weapon,   false, MK_PLAIN,    false };
    items[ni++] = { "ammo",     T("Ammo","Патроны"),       T("Rounds in the magazine","Патроны в магазине"),
                    e.ammo,     e.col_ammo,     false, MK_PLAIN,    false };
    if (!bots)
    items[ni++] = { "pulse",    T("Health","Здоровье"),    T("HP value under the name","Значение HP под ником"),
                    e.health,   nullptr,        false, MK_PLAIN,    false }; // no dot
    // Armor — humans only (bot armor pipeline not implemented).
    if (!bots)
    items[ni++] = { "shield",   T("Armor","Броня"),        T("Helmet and vest","Шлем и жилет"),
                    e.armor,    nullptr,        true,  MK_ARMOR,    false };
    items[ni++] = { "ruler",    T("Distance","Дистанция"), T("Metres to the target","Метры до цели"),
                    e.distance, e.col_distance, false, MK_PLAIN,    false };
    items[ni++] = { "user_x",   T("Corpses","Трупы"),      T("Bodies and loot on them","Тела и лут на них"),
                    cv.enable,  cv.color,       true,  MK_CORPSES,  false };
    if (!bots)
    items[ni++] = { "user_check", T("Show teammates","Показывать своих"), T("Draw squadmates too","Рисовать сокомандников"),
                    &cfg.show_mates, nullptr,   false, MK_PLAIN,    true };
    if (!bots)
    items[ni++] = { "list",     T("Nearest PMC table","Список ближайших"), T("Cards in the top-left HUD","Карточки слева сверху"),
                    &cfg.show_nearest_table, nullptr, false, MK_PLAIN, true };
    for (int ii = 0; ii < ni; ++ii) {
        const Item& it = items[ii];
        RowResult r = setting_row(x, cy, list_w, it.icon, it.title, it.sub,
                                  it.on, it.col, it.gear, it.global ? true : en);
        bool has_settings = (it.kind == MK_BOX ||
                             it.kind == MK_ARMOR || it.kind == MK_CORPSES);
        if (r.color_clicked) open_modal(it.title, it.kind, it.col, has_settings);
        if (r.gear_clicked)  { open_modal(it.title, it.kind, it.col, true); st.modal.tab = 0; }
    }

    list_scroll_end(bots ? PG_BOTS : PG_PLAYERS, x, y, list_w, h, cy, gutter);

    draw_esp_preview(preview_x, y, preview_w, h, cfg);
}

// ════════════════════════════════════════════════════════════════════════════
//  ЭЛЕМЕНТЫ СПИСКА  (TZ-Other-Pages §0.4 / §0.5)
// ════════════════════════════════════════════════════════════════════════════

// Deferred dropdown list: the popup must paint OVER the rows drawn after it,
// so the chip only records the request and render_control_panel flushes it at
// the end of the frame (immediate-mode z-order fix).
struct PendingDD {
    bool  active = false;
    int   id = -1;
    float x = 0, y = 0;             // top-right anchor of the list
    const char* const* items = nullptr;
    int   count = 0;
    int*  value = nullptr;
};
PendingDD g_pending_dd;

// Section header (isHead): caps 11/600 #56565F + optional right-side hint.
// `quiet` = non-clickable 11px #3A3A42 caption. Returns height consumed.
//
// The old 12/14/2 split was measured at the 14 px type scale; at ×1.30 an 11 px
// run no longer fits its 14 px slot, so the caption spilled into the 2 px of
// bottom padding and ended up glued to the first row. Both runs are now placed
// on a real band by their optical middle, with the same air as the chip
// variant — 16 above, 10 below.
float section_head(float x, float& y, float w, const char* title, const char* quiet) {
    const float padTop = 16.0f, band = 16.0f, padBot = 10.0f;
    float h = padTop + band + padBot;
    float mid = y + padTop + band * 0.5f;
    textb_mid(x + 4.0f, mid, 11.0f, C_DIM, title);
    if (quiet)
        text_mid(x + w - 4.0f - measure(11.0f, quiet).x, mid, 11.0f, C_FAINT, quiet);
    y += h;
    return h;
}

// Section header with a chip-dropdown on the right (chipHint).
// s2, hairline, r8, label 12/600 t2 + chevron.
//
// The chip is sized from its longest option, not just the current one, so it
// never changes width as you switch placement — and it is inset from the list
// edge so it clears the scrollbar instead of crowding it. Russian labels like
// "Слева сверху" are far longer than "Top-left", which is what made the old
// tight padding read as broken.
float section_head_chip(float x, float& y, float w, const char* title, int dd_id,
                        const char* const* items, int count, int* value) {
    const float chipH = 34.0f, padL = 12.0f, padR = 12.0f, chevGap = 8.0f, chevW = 12.0f;
    const float edge  = 0.0f;              // right edge flush with the rows below
    // The plain section_head can get away with 2 px underneath because its title
    // is bare text with descender slack below it. The chip is a bordered box
    // that fills its slot to the pixel, so the same 2 px read as glued to the
    // first row. Air above (16) > air below (10) keeps the header bound to the
    // group it introduces, and 10 matches the row-to-row gap.
    const float padTop = 16.0f, padBot = 10.0f;
    float h = padTop + chipH + padBot;
    textb_mid(x + 4.0f, y + padTop + chipH * 0.5f, 11.0f, C_DIM, title);
    const char* cur = items[ImClamp(*value, 0, count - 1)];
    float tw = 0.0f;
    for (int i = 0; i < count; ++i) tw = ImMax(tw, measureb(12.0f, items[i]).x);
    float cw = padL + tw + chevGap + chevW + padR;
    ImVec2 a(x + w - edge - cw, y + padTop), b(x + w - edge, y + padTop + chipH);
    bool hov = hovered(a, b);
    card(a, b, hov ? C_SEG_ON : C_CHIP, C_LINE, 8.0f);
    float mid = a.y + chipH * 0.5f;
    textb_mid(a.x + padL, mid, 12.0f, C_MUTED, cur);
    chevron(ImVec2(b.x - padR - chevW * 0.5f, mid + 1.0f), 5.5f, C_MUTED,
            st.open_dropdown == dd_id);
    // Clicking the chip opens the placement chooser modal (operator request:
    // a proper dialog rather than an inline dropdown).
    if (clicked(a, b)) open_modal_position(T("Position","Расположение"), items, count, value);
    y += h;
    return h;
}

// Slider card (isSlid): #101014 r12, padding 13/16, label + mono value, track 7.
//
// Layout follows TZ-Modal §5.2: the label owns the first line on its own, and
// the value sits AFTER the track on the second line, optically centred on it —
// not stacked above the track next to the label. The card is only chrome; the
// slider itself is the shared unit_slider, so the step snapping is identical
// everywhere in the app.
float slider_card(float x, float y, float w, const char* label,
                  int* v, int lo, int hi, int step, const char* unit,
                  const char* valueText = nullptr, bool enabled = true) {
    const float H = 13.0f + 16.0f + 10.0f + 7.0f + 13.0f;   // pad + label + gap + track + pad
    ImVec2 a(x, y), b(x + w, y + H);
    card_inset(a, b, C_PANEL, C_LINE, 14.0f);
    unit_slider(x + 16.0f, y + 13.0f, w - 32.0f, label, v, lo, hi, step, unit,
                valueText, 26.0f, 12.0f, enabled);
    return H;
}

// Flush the deferred dropdown list (called last so it paints on top).
void flush_dropdown() {
    if (!g_pending_dd.active) return;
    PendingDD d = g_pending_dd;
    g_pending_dd.active = false;
    float ih = 34.0f, pad = 5.0f, minw = 150.0f;
    float wmax = minw;
    for (int i = 0; i < d.count; ++i) wmax = ImMax(wmax, measure(12.0f, d.items[i]).x + 34.0f);
    ImVec2 a(d.x - wmax, d.y), b(d.x, d.y + pad * 2.0f + ih * d.count + 2.0f * (d.count - 1));
    g.dl->AddRectFilled(ImVec2(a.x, a.y + 6.0f), ImVec2(b.x, b.y + 8.0f), IM_COL32(0,0,0,140), 10.0f);
    card(a, b, C_POP, C_LINE2, 14.0f);
    for (int i = 0; i < d.count; ++i) {
        ImVec2 ia(a.x + pad, a.y + pad + (ih + 2.0f) * i), ib(b.x - pad, ia.y + ih);
        bool sel = (*d.value == i);
        if (sel)            g.dl->AddRectFilled(ia, ib, C_AMBER_NAV, 8.0f);
        else if (hovered(ia, ib)) g.dl->AddRectFilled(ia, ib, C_ROW_HOVER, 8.0f);
        float ty = (ia.y + ib.y) * 0.5f;
        if (sel) textb_mid(ia.x + 11.0f, ty, 12.0f, C_AMBER_TEXT, d.items[i]);
        else     text_mid (ia.x + 11.0f, ty, 12.0f, C_MUTED,      d.items[i]);
        if (clicked(ia, ib)) { *d.value = i; st.open_dropdown = -1; }
    }
    // Click outside the list closes it.
    if (ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(a, b, false))
        st.open_dropdown = -1;
}

// Мок-сцена для превью радара: камера в нуле, нос на север. Позиции заданы в
// метрах и азимуте, поэтому при движении ползунка «Радиус (мир)» точки честно
// съезжаются к центру — ровно как в рейде.
const Snapshot& radar_mock_scene() {
    static Snapshot s = [] {
        Snapshot m;
        m.in_raid = true;
        auto add = [&](const char* cls, float dist_m, float bearing_deg,
                       float yaw_deg, bool dead) {
            Entity e;
            e.cls  = cls;
            float a = bearing_deg * 3.14159265f / 180.0f;
            e.x    = std::cos(a) * dist_m * 100.0f;   // +x — вперёд
            e.y    = std::sin(a) * dist_m * 100.0f;   // +y — вправо
            e.team = 2;
            e.dead = dead;
            if (!dead) e.yaw = yaw_deg;
            m.entities.push_back(std::move(e));
        };
        add("PMC",         42.0f,  -38.0f, 205.0f, false);
        add("PMC",         78.0f,   62.0f, 300.0f, false);
        add("BOT_PRIMARY", 55.0f,  138.0f,  15.0f, false);
        add("BOT_PRIMARY", 31.0f, -108.0f,  95.0f, false);
        add("PMC",         66.0f,  178.0f,   0.0f, true);   // труп: без конуса
        return m;
    }();
    return s;
}

// ════════════════════════════════════════════════════════════════════════════
//  RADAR  (TZ-Other-Pages §2)
// ════════════════════════════════════════════════════════════════════════════
void page_radar(float x, float y, float w, float h, RenderConfig& cfg) {
    RadarView r = radar_view(cfg);
    float preview_w = kPreviewW;
    float preview_x = x + w - preview_w;
    float list_w    = w - preview_w - 18.0f;
    float cy = y;

    // §2.1 — five plain rows (no colour dot, no gear).
    setting_row(x, cy, list_w, "radar", T("Show radar","Показывать радар"),
                T("Minimap over the game","Мини-карта поверх игры"),   r.enable,  nullptr, false);
    bool en = *r.enable;
    setting_row(x, cy, list_w, "users", T("Show Players","Показывать игроков"),
                T("Player dots on the disc","Точки игроков на диске"),  r.players, nullptr, false, en);
    setting_row(x, cy, list_w, "bot",   T("Show bots","Показывать ботов"),
                T("Bot dots on the disc","Точки ботов на диске"),       r.bots,    nullptr, false, en);
    setting_row(x, cy, list_w, "rings", T("Distance rings","Кольца дистанций"),
                T("Two rings inside the disc","Два кольца внутри диска"), r.rings, nullptr, false, en);
    setting_row(x, cy, list_w, "cone",  T("Aim direction","Направление взгляда"),
                T("View cones of the targets","Конусы взгляда целей"),  r.aim_line,nullptr, false, en);

    // §2.2 — "Size & range" card: icon + title + position chip + two sliders.
    cy += 8.0f;
    const char* rpos[4] = {
        T("Top-right","Справа сверху"),  T("Bottom-right","Справа снизу"),
        T("Bottom-left","Слева снизу"),  T("Top-left","Слева сверху"),
    };
    const float cardH = 16.0f + 34.0f + 15.0f + 44.0f + 15.0f + 44.0f + 16.0f;
    ImVec2 ca(x, cy), cb(x + list_w, cy + cardH);
    card_inset(ca, cb, C_PANEL, C_LINE, 14.0f);
    {   // header: maximize icon 15 + label + chip
        icon("maximize", ImVec2(x + 16.0f, cy + 16.0f + 9.0f), 16.0f, C_MUTED);
        textb(ImVec2(x + 16.0f + 16.0f + 11.0f, cy + 16.0f + 9.0f), 14.0f, C_TEXT,
              T("Size & range","Размер и радиус"));
        const char* cur = rpos[ImClamp(*r.position, 0, 3)];
        float tw = measure(12.0f, cur).x, cw = 12.0f + tw + 8.0f + 12.0f + 12.0f;
        ImVec2 pa(x + list_w - 16.0f - cw, cy + 16.0f), pb(x + list_w - 16.0f, cy + 16.0f + 34.0f);
        bool hov = hovered(pa, pb);
        card(pa, pb, hov ? C_SEG_ON : C_CHIP, C_LINE, 8.0f);
        text_mid(pa.x + 12.0f, pa.y + 17.0f, 12.0f, C_MUTED, cur);
        chevron(ImVec2(pb.x - 12.0f - 6.0f, pa.y + 17.0f + 1.0f), 5.5f, C_DIM, false);
        if (clicked(pa, pb))
            open_modal_position(T("Radar position","Расположение радара"), rpos, 4, r.position);
    }
    {   // two sliders, sliding the real radii so both land on tens
        float sx = x + 16.0f, sw = list_w - 32.0f, sy = cy + 16.0f + 34.0f + 15.0f;
        unit_slider(sx, sy, sw, T("Radius (world)","Радиус (мир)"),
                    r.world_radius, 50, 400, 10, " m", nullptr, 22.0f);
        unit_slider(sx, sy + 44.0f + 15.0f, sw, T("Radius (screen)","Радиус (экран)"),
                    r.screen_radius, 100, 400, 10, " px", nullptr, 22.0f);
    }
    cy += cardH;

    // §2.3 — Radar Preview panel, circle always 300×300 (viewBox 200×200).
    card_inset(ImVec2(preview_x, y), ImVec2(preview_x + preview_w, y + h), C_INSET, C_LINE, 14.0f);
    {
        icon("maximize", ImVec2(preview_x + 22.0f, y + 22.0f), 16.0f, C_MUTED);
        text(ImVec2(preview_x + 22.0f + 16.0f + 11.0f, y + 22.0f + 0.5f), 14.0f, C_MUTED,
             T("Radar Preview","Превью радара"));
    }
    if (!*r.enable) {
        text_center_mid(preview_x + preview_w * 0.5f, y + h * 0.5f, 13.0f, C_DIM,
                        T("Radar is off — enable \xE2\x80\x9CShow radar\xE2\x80\x9D",
                          "Радар выключен — включите «Показывать радар»"));
    } else {
        // Превью рисуется ТЕМ ЖЕ кодом, что боевой радар (abi::hud::radar_at),
        // на мок-сцене. Своей отрисовки у превью больше нет, поэтому диск,
        // кольца, точки, конусы и своя отметка всегда совпадают с игрой; на
        // мок влияют и тумблеры страницы, и ползунок «Радиус (мир)».
        const float D = 300.0f;                       // сторона превью
        float ox = preview_x + (preview_w - D) * 0.5f;
        float oy = y + 60.0f + ImMax(0.0f, ((h - 60.0f - 40.0f) - (D + 40.0f)) * 0.5f);
        abi::hud::radar_at(&radar_mock_scene(), cfg, g.dl, ImVec2(ox, oy), D);

        {   // легенда под диском — единственное, чего нет в игре
            float ly = oy + D + 14.0f;
            struct L { ImU32 col; const char* s; };
            L items[3] = { { P::radar::PMC, T("Players","Игроки") },
                           { P::radar::BOT, T("Bots","Боты") },
                           { P::radar::SELF, T("You","Вы") } };
            float total = 0.0f;
            for (auto& it : items) total += 8.0f + 7.0f + measure(11.0f, it.s).x + 14.0f;
            total -= 14.0f;
            float lx = ox + D * 0.5f - total * 0.5f;
            for (auto& it : items) {
                g.dl->AddCircleFilled(ImVec2(lx + 4.0f, ly + 7.0f), 4.0f, it.col, 16);
                text(ImVec2(lx + 8.0f + 7.0f, ly), 11.0f, C_MUTED, it.s);
                lx += 8.0f + 7.0f + measure(11.0f, it.s).x + 14.0f;
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  OVERLAY  (TZ-Other-Pages §3)
// ════════════════════════════════════════════════════════════════════════════
void page_overlay(float x, float y, float w, float h, RenderConfig& cfg) {
    // 2026-09-23: page stripped bare for full re-design. Add sections below
    // one by one as they land. Empty scroll container kept for layout parity.
    (void)cfg;
    float list_w = w - 26.0f;
    float cy = list_scroll_begin(PG_OVERLAY, x, y, list_w, h);
    list_scroll_end(PG_OVERLAY, x, y, list_w, h, cy, 26.0f);
}

// ════════════════════════════════════════════════════════════════════════════
//  LOOT  (TZ-Other-Pages §4) — правой панели нет
// ════════════════════════════════════════════════════════════════════════════
void page_loot(float x, float y, float w, float, RenderConfig& cfg) {
    LootView l = loot_view(cfg);
    float cy = y;

    const char* side[2] = { T("Right side","Справа"), T("Left side","Слева") };
    static int s_loot_side = 0;
    section_head_chip(x, cy, w, T("TOP LOOT SIDEBAR","САЙДБАР ТОП-ЛУТА"), 3, side, 2, &s_loot_side);
    {
        RowResult rr = setting_row(x, cy, w, "list", T("Show top-loot list","Показывать список"),
                                   T("Loot panel on the left","Панель добычи слева"),
                                   l.enable, nullptr, true);
        if (rr.gear_clicked) {
            open_modal(T("Top loot list","Список топ-лута"), MK_TOPLOOT, nullptr, true);
            st.modal.tab = 0;
        }
    }

    section_head(x, cy, w, T("FILTER","ФИЛЬТР"), T("Markers","Маркеры"));
    {   // thousands separator; step 1000 keeps the group always "000"
        char mb[32];
        std::snprintf(mb, sizeof(mb), "$%d,%03d", s_loot_min_value / 1000, s_loot_min_value % 1000);
        if (s_loot_min_value < 1000) std::snprintf(mb, sizeof(mb), "$%d", s_loot_min_value);
        cy += slider_card(x, cy, w, T("Min loot value","Мин. стоимость"),
                          &s_loot_min_value, 0, 100000, 1000, nullptr, mb) + 8.0f;
        *l.min_price = s_loot_min_value;
    }

}

// ════════════════════════════════════════════════════════════════════════════
//  SETTINGS  (TZ-Other-Pages §5) — одна карточка, правой панели нет
// ════════════════════════════════════════════════════════════════════════════
void page_settings(float x, float y, float w, float, RenderConfig& cfg) {
    int* lang = language_ptr(cfg);
    const float H = 54.0f;
    ImVec2 a(x, y), b(x + w, y + H);
    card_inset(a, b, C_PANEL, C_LINE, 14.0f);
    float cyc = y + H * 0.5f;
    icon("globe", ImVec2(x + 16.0f, cyc - 8.0f), 16.0f, C_MUTED);
    const char* langTitle = T("Language","Язык");
    textb_mid(x + 16.0f + 16.0f + 12.0f, cyc, 14.0f, C_TEXT, langTitle);
    // The caption sits on the title's baseline — the two runs share one line,
    // so cap-centring the smaller one would leave it riding high.
    float sub_x = x + 16.0f + 16.0f + 12.0f + measureb(14.0f, langTitle).x + 12.0f;
    text_base(sub_x, baseline_at_mid(FB(), 14.0f, cyc), 12.0f, C_DIM,
              T("Interface language","Язык интерфейса"));
    {   // segment group Русский / English — same style as the modal segments
        const char* opts[2] = { "Русский", "English" };
        float ow = ImMax(measure(12.0f, opts[0]).x, measure(12.0f, opts[1]).x) + 28.0f;
        float gw = ow * 2.0f + 6.0f, gh = 34.0f, pad = 3.0f;
        ImVec2 ga(x + w - 16.0f - gw, cyc - gh * 0.5f), gb(ga.x + gw, ga.y + gh);
        card(ga, gb, C_TRACK, C_LINE, 11.0f);
        for (int i = 0; i < 2; ++i) {
            ImVec2 sa(ga.x + pad + ow * i, ga.y + pad), sb(sa.x + ow, gb.y - pad);
            bool on = (*lang == (i == 0 ? 1 : 0));   // 0=EN,1=RU in RenderConfig
            if (on) { g.dl->AddRectFilled(sa, sb, C_SEG_ON, 8.0f); g.dl->AddRect(sa, sb, C_LINE2, 8.0f, 0, 1.0f); }
            else if (hovered(sa, sb)) g.dl->AddRectFilled(sa, sb, C_ROW_HOVER, 8.0f);
            float tcx = (sa.x + sb.x) * 0.5f, tcy = ga.y + gh * 0.5f;
            if (on) textb_center_mid(tcx, tcy, 12.0f, C_TEXT, opts[i]);
            else    text_center_mid (tcx, tcy, 12.0f, C_DIM,  opts[i]);
            if (clicked(sa, sb)) *lang = (i == 0 ? 1 : 0);
        }
    }
    (void)w;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
//  ТОЧКА ВХОДА
// ════════════════════════════════════════════════════════════════════════════

// Текстура фото оператора для ESP-превью (см. INTEGRATION.md — объявить в control_panel.hpp)
void control_panel_set_operator_texture(ImTextureID tex, int wpx, int hpx) {
    st.operator_tex = tex;
    st.operator_w = (float)wpx;
    st.operator_h = (float)hpx;
}

void control_panel_set_typography(float text_k, float mono_k) {
    kTextK = text_k;
    kMonoK = mono_k;
}

void control_panel_set_background_texture(ImTextureID tex, int wpx, int hpx) {
    st.bg_tex = tex;
    st.bg_w = (float)wpx;
    st.bg_h = (float)hpx;
}

// Wallpaper INSIDE the panel: hero.png cover-fitted to the window rect and
// dimmed to ~5 % brightness, drawn right after the window ground so every
// row, card and popup sits on top of it. Clipped to the rounded window so it
// never bleeds past the corners.
void draw_panel_background(ImVec2 wa, ImVec2 wb) {
    if (!st.bg_tex || st.bg_w <= 0.0f || st.bg_h <= 0.0f) return;
    float pw = wb.x - wa.x, ph = wb.y - wa.y;
    float s = ImMax(pw / st.bg_w, ph / st.bg_h);          // cover
    float dw = st.bg_w * s, dh = st.bg_h * s;
    ImVec2 a(wa.x + (pw - dw) * 0.5f, wa.y + (ph - dh) * 0.5f);
    ImVec2 b(a.x + dw, a.y + dh);
    g.dl->PushClipRect(wa, wb, true);
    g.dl->AddImage(st.bg_tex, a, b, ImVec2(0, 0), ImVec2(1, 1),
                   IM_COL32(13, 13, 13, 255));            // ×0.05 brightness
    g.dl->PopClipRect();
}

void render_control_panel(RenderConfig& cfg) {
    // TEST-REMOVE
    static unsigned long long tt_cp = 0; tt_cp++;
    if ((tt_cp % 300) == 0) ah_test_trace_write("render_control_panel ENTER #%llu show=%d",
        tt_cp, (int)cfg.show_control_panel);
    ImGuiIO& io = ImGui::GetIO();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kWinRadius);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, C_WINDOW);

    ImGui::SetNextWindowSize(ImVec2(kWinW, kWinH), ImGuiCond_Always);
    // Position: seed on first use; user-drag position persists thereafter.
    // Manual drag handle lives at the top of the sidebar (see g_drag block
    // near end of Begin) so slider ImDrawList clicks are never hijacked by
    // ImGui's default window-drag.
    static bool s_win_pos_seeded = false;
    static ImVec2 s_win_pos;
    if (!s_win_pos_seeded) {
        s_win_pos = ImVec2((io.DisplaySize.x - kWinW) * 0.5f,
                           (io.DisplaySize.y - kWinH) * 0.5f);
        s_win_pos_seeded = true;
    }
    ImGui::SetNextWindowPos(s_win_pos, ImGuiCond_Always);

    // NoMove kept: ImDrawList-only content leaves ImGui blind to slider
    // hit-regions, so default drag would swallow every slider click.
    // Manual drag on the sidebar header strip is added below.
    if (ImGui::Begin("##nightvex_control_panel", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove)) {

        // Copy flat RenderConfig (float ranges, box_mode enum) into the int
        // shadows the panel widgets edit through view-function pointers.
        pre_sync(cfg);
        g_lang = cfg.ui_language;   // 0 = EN, 1 = RU — drives every T() call

        g.dl     = ImGui::GetWindowDrawList();
        g.origin = ImGui::GetWindowPos();
        g.font     = io.Fonts->Fonts.Size > 0 ? io.Fonts->Fonts[0] : ImGui::GetFont();
        g.mono     = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : g.font;
        g.semibold = io.Fonts->Fonts.Size > 2 ? io.Fonts->Fonts[2] : g.font;
        g.blocked = st.modal.open;

        ImVec2 wa = g.origin, wb(wa.x + kWinW, wa.y + kWinH);
        g.dl->AddRectFilled(wa, wb, C_WINDOW, kWinRadius);
        draw_panel_background(wa, wb);      // wallpaper under the whole panel
        g.dl->AddRect(wa, wb, C_LINE, kWinRadius, 0, 1.0f);

        draw_sidebar(wa.x, wa.y, kSidebarW, kWinH);

        // ── Content header (TZ §5): breadcrumb "<root> · <section>".
        //   root 16/600 #F4F4F6, dot 4×4 #56565F, section 15/400 #8B8B95.
        //   padding 24 26 14. There is no separate large page title.
        float cx = wa.x + kSidebarW + kPagePad;
        float cw = kWinW - kSidebarW - kPagePad * 2.0f;
        struct Crumb { const char* root; const char* sect; };
        const Crumb crumbs[6] = {
            { T("Players","Игроки"),   nullptr },
            { "AI",                    nullptr },
            { T("Radar","Радар"),      nullptr },
            { T("Overlay","Оверлей"),  nullptr },
            { T("Loot","Лут"),         nullptr },
            { T("Settings","Настройки"), nullptr },
        };
        {
            float by = wa.y + 24.0f;
            const Crumb& cr = crumbs[st.page];
            textb(ImVec2(cx, by), 16.0f, C_TEXT, cr.root);
            if (cr.sect) {
                // Dot and section share the root's optical middle (cap-height
                // centre). A fixed offset drifts as soon as the type scale
                // changes, which is exactly how the dot ended up riding high.
                float mid   = by + cap_center(FB(), 16.0f);
                float rootW = measureb(16.0f, cr.root).x;
                float dotX  = cx + rootW + 11.0f;
                g.dl->AddCircleFilled(ImVec2(dotX, mid), 2.0f, C_DIM, 8);
                text_mid(dotX + 11.0f, mid, 15.0f, C_MUTED, cr.sect);
            }
        }

        // Content body starts below the header (padding 24 top + ~16 line + 14).
        float py = wa.y + 24.0f + 20.0f + 14.0f;
        float ph = kWinH - (py - wa.y) - kPagePad;

        switch (st.page) {
        case PG_PLAYERS:  page_visuals(cx, py, cw, ph, cfg, false); break;
        case PG_BOTS:     page_visuals(cx, py, cw, ph, cfg, true);  break;
        case PG_RADAR:    page_radar(cx, py, cw, ph, cfg);          break;
        case PG_OVERLAY:  page_overlay(cx, py, cw, ph, cfg);        break;
        case PG_LOOT:     page_loot(cx, py, cw, ph, cfg);           break;
        case PG_SETTINGS: page_settings(cx, py, cw, ph, cfg);       break;
        }

        // Deferred dropdown list — painted last so it sits above the rows.
        flush_dropdown();

        g.blocked = false;
        draw_modal(cfg, wa, wb);

        // клик мимо открытого списка — закрыть
        if (st.open_dropdown >= 0 && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) {
            // закрытие обрабатывается самим dropdown при клике по нему; здесь — по пустому месту
        }

        // Fold int shadows back into the flat RenderConfig fields
        // (float ranges, box_mode enum, sub-features not yet in RenderConfig).
        post_sync(cfg);
        corpses_post_sync(cfg);
        radar_post_sync(cfg);
        overlay_post_sync(cfg);
        loot_post_sync(cfg);

        // ── Manual window-drag: top strip of the sidebar (~44px = wordmark
        // band). No slider lives there → safe to hijack for panel move.
        {
            static bool  s_dragging = false;
            static ImVec2 s_start_mouse;
            static ImVec2 s_start_win;
            const float grip_h = 44.0f;
            ImVec2 gm(wa.x, wa.y), gM(wa.x + kSidebarW, wa.y + grip_h);
            ImVec2 mp = ImGui::GetIO().MousePos;
            bool  in = (mp.x >= gm.x && mp.x < gM.x && mp.y >= gm.y && mp.y < gM.y);
            if (in && ImGui::IsMouseClicked(0) && !st.modal.open) {
                s_dragging = true;
                s_start_mouse = mp;
                s_start_win   = wa;
            }
            if (s_dragging) {
                if (ImGui::IsMouseDown(0)) {
                    s_win_pos.x = s_start_win.x + (mp.x - s_start_mouse.x);
                    s_win_pos.y = s_start_win.y + (mp.y - s_start_mouse.y);
                    // Clamp on-screen (keep at least 60px of grip on-screen).
                    ImVec2 ds = ImGui::GetIO().DisplaySize;
                    if (s_win_pos.x < -kWinW + 60) s_win_pos.x = -kWinW + 60;
                    if (s_win_pos.y < 0)           s_win_pos.y = 0;
                    if (s_win_pos.x > ds.x - 60)   s_win_pos.x = ds.x - 60;
                    if (s_win_pos.y > ds.y - 60)   s_win_pos.y = ds.y - 60;
                } else {
                    s_dragging = false;
                }
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

} // namespace abi
