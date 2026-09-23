// overlay_hud.cpp — игровой оверлей, единая реализация для игры и превью.
//
// Тела перенесены из preview/overlay_preview_main.cpp как есть: числа, порядок
// отрисовки и комментарии со ссылками на пункты ТЗ сохранены. Изменено ровно
// три вещи:
//   • сырые IM_COL32(...) → константы из palette.hpp;
//   • магические числа → именованные константы ТЗ (блок ниже);
//   • угол радара берётся из cfg.radar_position, а не из дефолтного аргумента.

#include "overlay_hud.hpp"
#include "palette.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace abi::hud {
namespace {

namespace P = abi::pal;

constexpr float PI_F = 3.14159265358979323846f;

// ── Константы ТЗ. Раньше были магическими числами по коду ──────────────────
// TZ-Overlay-HUD §1-§4
constexpr float HUD_ANCHOR      = 18.0f;   // отступ блока от краёв экрана
constexpr float HUD_COL_GAP     = 12.0f;   // чипы → список
constexpr float HUD_WIDTH       = 480.0f;  // v0.9.430: 624→480, close gap name↔weapon
constexpr float CHIP_H          = 28.0f;
constexpr float CHIP_PAD_X      = 10.0f;
constexpr float CHIP_PAD_Y      = 6.0f;
constexpr float CHIP_GAP        = 6.0f;    // между чипами
constexpr float CHIP_IN_GAP     = 6.0f;    // ярлык → значение внутри чипа
constexpr float CHIP_RADIUS     = 8.0f;
constexpr float CHIP_FS         = 12.0f;
constexpr float CARD_H          = 50.0f;   // v0.9.430: +28% (39→50) larger cards
constexpr float CARD_GAP        = 6.0f;
constexpr float CARD_RADIUS     = 10.0f;
constexpr float CARD_PAD_X      = 12.0f;
constexpr float CARD_ITEM_GAP   = 12.0f;
constexpr float CARD_DOT_R      = 3.0f;    // точка состояния 6×6
constexpr float SLOT_MAG        = 40.0f;   // вмещает 30/30, 12/20
constexpr float SLOT_ARMOR      = 50.0f;   // вмещает пилюлю H<t> V<t>
constexpr float SLOT_DIST       = 42.0f;   // §4.2 "width: 42px"
constexpr float SLOT_STATUS     = 44.0f;   // вмещает пилюлю DEAD
// v0.9.430: nearest card fonts scaled +33% for readability.
constexpr float CARD_FS_NAME    = 17.0f;   // was 13
constexpr float CARD_FS_MONO    = 16.0f;   // was 12
constexpr float CARD_FS_ARMOR   = 15.0f;   // was 11.5
constexpr float CARD_FS_DIST    = 16.5f;   // was 12.5
constexpr float CARD_FS_STATUS  = 14.0f;   // was 10.5
constexpr float PILL_H_ARMOR    = 18.0f;
constexpr float PILL_H_STATUS   = 19.0f;
constexpr int   NEAREST_MAX     = 6;       // §7

// TZ-Top-Loot
constexpr float LOOT_W          = 380.0f;  // v0.9.463: 320 → 380 (шире под увеличенный шрифт)
constexpr float LOOT_PAD_X      = 14.0f;
constexpr float LOOT_PAD_Y      = 12.0f;
constexpr float LOOT_RADIUS     = 12.0f;
constexpr float LOOT_HEAD_H     = 18.0f;   // v0.9.463: 14 → 18 под +40% шрифт header
constexpr float LOOT_HEAD_GAP   = 11.0f;
constexpr float LOOT_ITEM_H     = 32.0f;   // v0.9.463: 24 → 32 под увеличенный row height
constexpr float LOOT_ROW_GAP    = 10.0f;   // v0.9.463: 9 → 10
constexpr float LOOT_TEXT_ROW_H = 20.0f;   // v0.9.463: 16 → 20
constexpr float LOOT_BAR_H      = 5.0f;    // v0.9.463: 4 → 5 (пропорционально)
constexpr float LOOT_BAR_GAP    = 5.0f;    // v0.9.463: 4 → 5
constexpr float LOOT_PRICE_SLOT = 52.0f;   // v0.9.463: 40 → 52 (шрифт monoбольше)
constexpr float LOOT_FS_HEAD    = 15.0f;   // v0.9.463: 11 → 15 (+36%)
constexpr float LOOT_FS_NAME    = 17.0f;   // v0.9.463: 13 → 17 (+31%)
constexpr float LOOT_FS_MONO    = 16.0f;   // v0.9.463: 12 → 16 (+33%)
constexpr int   LOOT_ROWS_MAX   = 6;       // §10

// TZ-Radar (система viewBox 300×300, k = size/300)
constexpr float RADAR_VB        = 300.0f;
constexpr float RADAR_R_OUT     = 143.0f;
constexpr float RADAR_R_MID     = 95.0f;   // §4
constexpr float RADAR_R_IN      = 48.0f;   // §4
constexpr float RADAR_CONE_PMC  = 57.0f;
constexpr float RADAR_CONE_BOT  = 48.0f;
constexpr float RADAR_CONE_DEG  = 30.0f;   // полное раскрытие
constexpr float RADAR_DOT_PMC   = 5.5f;
constexpr float RADAR_DOT_BOT   = 5.0f;
constexpr float RADAR_DOT_LOOT  = 3.4f;
constexpr float RADAR_DOT_LINE  = 1.6f;
constexpr float RADAR_DOT_SELF  = 5.5f;   // своя точка = размер точки игрока
constexpr int   RADAR_CONE_SEGS = 14;      // §10: 12-16 сегментов

// TZ-Ammo-Counter
constexpr float AMMO_SIZE       = 92.0f;
constexpr float AMMO_PAD        = 16.0f;
constexpr float AMMO_R          = 40.0f;
constexpr float AMMO_W          = 4.0f;
constexpr float AMMO_FS_CUR     = 26.0f;
constexpr float AMMO_FS_SUB     = 12.0f;
constexpr float AMMO_FS_RELOAD  = 10.0f;
constexpr float AMMO_GAP        = 1.0f;

constexpr const char* EN_DASH = "\xE2\x80\x93";   // U+2013

// ── Шрифты ────────────────────────────────────────────────────────────────
// [1] моно (Consolas / JetBrains Mono), [2] полужирный сан (Segoe UI Semibold /
// Inter SemiBold). Раскладка слотов задаётся хостом при загрузке атласа.
ImFont* font_mono() {
    ImGuiIO& io = ImGui::GetIO();
    return io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : ImGui::GetFont();
}
ImFont* font_semi() {
    ImGuiIO& io = ImGui::GetIO();
    return io.Fonts->Fonts.Size > 2 ? io.Fonts->Fonts[2] : font_mono();
}

// Точная посадка по капителям: берём коробку глифа 'H' из метрик шрифта и
// центрируем по её середине. Приближение 0.35×size уплывает примерно на 1.5 px
// после ~15 pt, и глаз читает это как «текст сидит ниже точки». Тот же приём
// использует control_panel.cpp.
float cap_top(ImFont* f, float cy, float sz) {
    const ImFontGlyph* g = f ? f->FindGlyph((ImWchar)'H') : nullptr;
    float scale = f ? sz / f->FontSize : 1.0f;
    float cc = g ? (g->Y0 + g->Y1) * 0.5f * scale
                 : (f ? f->Ascent * scale * 0.5f : sz * 0.5f);
    return cy - cc;
}

// Угол экрана из cfg.radar_position. Порядок совпадает с чипом «Расположение»
// в панели (control_panel.cpp, rpos[]): 0 TR, 1 BR, 2 BL, 3 TL.
ImVec2 corner_origin(int position_idx, float size, float pad,
                     float screen_w, float screen_h) {
    switch (position_idx) {
    case 1:  return ImVec2(screen_w - pad - size, screen_h - pad - size);  // BR
    case 2:  return ImVec2(pad,                   screen_h - pad - size);  // BL
    case 3:  return ImVec2(pad,                   pad);                    // TL
    default: return ImVec2(screen_w - pad - size, pad);                    // TR
    }
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Чипы статуса + карточки ближайших (TZ-Overlay-HUD §1-7)
// ═══════════════════════════════════════════════════════════════════════════
// Числа измерены, не угаданы:
//   • якорь 18/18, между рядом чипов и списком 12.
//   • чип: высота 28, радиус 8, паддинг 6/10, моно 12; чип enemies янтарный.
//   • карточка: высота 39, радиус 10, паддинг 9/12, шаг между элементами 12.
//   • цвет рамки зависит от состояния (VIS шалфей / обычная белая .08 /
//     приоритет глина / мёртвый белый .05); фон мёртвого притушен.
//   • слоты слева направо: точка 6×6 · имя (Semibold 13) · оружие моно
//     · магазин моно (олива/терракота/тире) · пилюля брони по тиру
//     · дистанция моно 12.5/600 · пилюля статуса (VIS/DEAD) или тире.
// Приоритет — игровой флаг; в превью помечаем GhostRider_42 по имени, чтобы
// эталонная строка из §5 была на экране.
void stats_and_nearest(const Snapshot* snap, const RenderConfig& cfg,
                       float framerate, float* out_bottom_y) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    ImFont* fmono = font_mono();
    ImFont* fsemi = font_semi();

    float y_cursor = HUD_ANCHOR;
    const float OX = HUD_ANCHOR;

    // ── РЯД ЧИПОВ (§3) ─────────────────────────────────────────────────
    if (cfg.show_stats) {
        size_t n_user = 0, n_pmc = 0, n_bot = 0;
        for (const auto& e : (snap ? snap->entities : std::vector<Entity>{})) {
            if (e.me || e.cls.starts_with("USER")) n_user++;
            else if (e.cls.starts_with("BOT"))     n_bot++;
            else                                    n_pmc++;
        }
        size_t n_enemies = n_pmc + n_bot;

        float x = OX;
        auto chip = [&](const char* label, const char* value, bool accent) {
            float lw = label ? fmono->CalcTextSizeA(CHIP_FS, FLT_MAX, 0.0f, label).x : 0.0f;
            float vw = fmono->CalcTextSizeA(CHIP_FS, FLT_MAX, 0.0f, value).x;
            float w  = CHIP_PAD_X * 2.0f + lw + (label ? CHIP_IN_GAP : 0.0f) + vw;
            ImVec2 a(x, y_cursor);
            ImVec2 b(x + w, y_cursor + CHIP_H);
            dl->AddRectFilled(a, b, accent ? P::hud::CHIP_ACC_BG : P::hud::CHIP_BG,   CHIP_RADIUS);
            dl->AddRect      (a, b, accent ? P::hud::CHIP_ACC_LN : P::hud::CHIP_LINE, CHIP_RADIUS, 0, 1.0f);
            float tx  = a.x + CHIP_PAD_X;
            float ty_ = a.y + CHIP_PAD_Y + 1.0f;
            ImU32 lc = accent ? P::hud::CHIP_ACC_TX : P::hud::CHIP_LABEL;
            ImU32 vc = accent ? P::hud::CHIP_ACC_TX : P::hud::CHIP_VALUE;
            if (label) {
                dl->AddText(fmono, CHIP_FS, ImVec2(tx, ty_), lc, label);
                tx += lw + CHIP_IN_GAP;
            }
            dl->AddText(fmono, CHIP_FS, ImVec2(tx, ty_), vc, value);
            x += w + CHIP_GAP;
        };

        char buf[64];
        if (cfg.show_connection) {
            std::snprintf(buf, sizeof(buf), "%d", (int)framerate);
            chip("fps", buf, false);
        }
        if (cfg.show_entities_count) {
            std::snprintf(buf, sizeof(buf), "%zu", n_enemies);
            chip("enemies", buf, /*accent*/ true);
            std::snprintf(buf, sizeof(buf), "u%zu p%zu b%zu", n_user, n_pmc, n_bot);
            chip(nullptr, buf, false);
        }
        // DEBUG POS: cam (PCM POV) vs me_ent (ACE decrypt) side-by-side
        if (snap) {
            const Entity* me_ent_ptr = nullptr;
            for (const auto& e : snap->entities) if (e.me) { me_ent_ptr = &e; break; }
            std::snprintf(buf, sizeof(buf), "%.0f %.0f %.0f", snap->cam.x, snap->cam.y, snap->cam.z);
            chip("cam", buf, false);
            if (me_ent_ptr) {
                std::snprintf(buf, sizeof(buf), "%.0f %.0f %.0f", me_ent_ptr->x, me_ent_ptr->y, me_ent_ptr->z);
                chip("me", buf, false);
            }
            // 2026-08-18 self_v2 — spawn-reliable locator diagnostic.
            if (snap->self_v2.valid) {
                const char* mname = "?";
                switch (snap->self_v2.method) {
                    case 1: mname = "spawn"; break;
                    case 2: mname = "pawn";  break;
                    case 3: mname = "sync";  break;
                    case 4: mname = "ack";   break;
                }
                std::snprintf(buf, sizeof(buf), "%s %.0f %.0f %.0f", mname,
                              snap->self_v2.x, snap->self_v2.y, snap->self_v2.z);
                chip("v2", buf, /*accent*/ true);
            } else {
                chip("v2", "MISS", false);
            }
        }
        // v0.9.430: FOV / scope chips removed at operator request (screenshot clutter).
        y_cursor += CHIP_H + HUD_COL_GAP;
    }

    // ── СПИСОК БЛИЖАЙШИХ (§4) ──────────────────────────────────────────
    if (!cfg.show_nearest_table || !snap || !snap->in_raid) {
        if (out_bottom_y) *out_bottom_y = y_cursor;
        return;
    }

    std::vector<const Entity*> es;
    es.reserve(snap->entities.size());
    for (const auto& e : snap->entities) {
        if (e.me) continue;
        if (e.cls.starts_with("BOT")) continue;
        es.push_back(&e);
    }
    std::sort(es.begin(), es.end(), [&](const Entity* a, const Entity* b) {
        float ax = a->x - snap->cam.x, ay = a->y - snap->cam.y;
        float bx = b->x - snap->cam.x, by = b->y - snap->cam.y;
        return ax*ax + ay*ay < bx*bx + by*by;
    });
    int n = (int)std::min<size_t>(NEAREST_MAX, es.size());
    if (n == 0) {
        if (out_bottom_y) *out_bottom_y = y_cursor;
        return;
    }

    // ── Фиксированная колоночная геометрия ─────────────────────────────
    // У каждой колонки значений статичный ПРАВЫЙ край (абсолютный X), всё
    // внутри выравнивается по нему вправо. Короткое оружие или узкая пилюля
    // статуса в одной строке больше не двигают колонки в строках ниже.
    const float R_STATUS = OX + HUD_WIDTH - CARD_PAD_X;
    const float R_DIST   = R_STATUS - SLOT_STATUS - CARD_ITEM_GAP;
    const float R_ARMOR  = R_DIST   - SLOT_DIST   - CARD_ITEM_GAP;
    const float R_MAG    = R_ARMOR  - SLOT_ARMOR  - CARD_ITEM_GAP;
    const float R_WPN    = R_MAG    - SLOT_MAG    - CARD_ITEM_GAP;

    for (int i = 0; i < n; i++) {
        const Entity* e = es[i];
        float dxx = e->x - snap->cam.x, dyy = e->y - snap->cam.y;
        float dm  = std::sqrt(dxx*dxx + dyy*dyy) / 100.0f;

        bool is_priority = !e->dead && (e->name == "GhostRider_42");
        bool is_visible  = !e->dead && e->visible && cfg.visibility_check;

        float ry = y_cursor + (CARD_H + CARD_GAP) * (float)i;
        ImVec2 a(OX, ry), b(OX + HUD_WIDTH, ry + CARD_H);
        ImU32 bg = e->dead ? P::hud::ROW_DEAD : P::hud::ROW_LIVE;
        ImU32 br = e->dead      ? P::hud::BORDER_DEAD
                 : is_visible   ? P::hud::BORDER_VIS
                 : is_priority  ? P::hud::BORDER_PRI
                                : P::hud::BORDER_DEF;
        dl->AddRectFilled(a, b, bg, CARD_RADIUS);
        dl->AddRect      (a, b, br, CARD_RADIUS, 0, 1.0f);

        float cy = (a.y + b.y) * 0.5f;

        // 1. Точка состояния.
        ImU32 dot = e->dead      ? P::hud::DOT_DEAD
                  : is_visible   ? P::hud::DOT_VIS
                  : is_priority  ? P::hud::DOT_PRI
                                 : P::hud::DOT_NORM;
        dl->AddCircleFilled(ImVec2(a.x + CARD_PAD_X + CARD_DOT_R, cy), CARD_DOT_R, dot, 20);
        float name_x = a.x + CARD_PAD_X + CARD_DOT_R * 2.0f + CARD_ITEM_GAP;

        // Цвета текста по §4.4.
        ImU32 c_name = e->dead ? P::TEXT_DIM : P::TEXT;
        ImU32 c_wpn  = e->dead ? P::TEXT_DIM : P::TEXT_MUTED;
        ImU32 c_dist = e->dead      ? P::TEXT_DIM
                     : is_priority  ? P::TEXT_MUTED
                                    : P::TEXT;

        // 3. Оружие (по правому краю R_WPN).
        {
            const char* wpn = e->weapon.empty() ? EN_DASH : e->weapon.c_str();
            ImU32 wc = e->weapon.empty() ? P::TEXT_FAINT : c_wpn;
            float ww = fmono->CalcTextSizeA(CARD_FS_MONO, FLT_MAX, 0.0f, wpn).x;
            dl->AddText(fmono, CARD_FS_MONO,
                        ImVec2(R_WPN - ww, cap_top(fmono, cy, CARD_FS_MONO)), wc, wpn);
        }

        // 4. Магазин (по правому краю R_MAG).
        {
            char mg[16];
            bool has_mag = (e->mag_max > 0 && e->mag_cur >= 0);
            ImU32 mc;
            if (!has_mag)                             { std::snprintf(mg, sizeof(mg), "%s", EN_DASH); mc = P::TEXT_FAINT; }
            else if (e->dead)                         { std::snprintf(mg, sizeof(mg), "%d/%d", e->mag_cur, e->mag_max); mc = P::TEXT_DIM; }
            else if (e->mag_cur * 3 < e->mag_max * 2) { std::snprintf(mg, sizeof(mg), "%d/%d", e->mag_cur, e->mag_max); mc = P::hud::MAG_LOW; }
            else                                      { std::snprintf(mg, sizeof(mg), "%d/%d", e->mag_cur, e->mag_max); mc = P::hud::MAG_FULL; }
            float mw = fmono->CalcTextSizeA(CARD_FS_MONO, FLT_MAX, 0.0f, mg).x;
            dl->AddText(fmono, CARD_FS_MONO,
                        ImVec2(R_MAG - mw, cap_top(fmono, cy, CARD_FS_MONO)), mc, mg);
        }

        // 5. Броня — пилюля по правому краю R_ARMOR, токены окрашены по тиру.
        {
            bool has_armor = (e->helm >= 0 || e->vest >= 0);
            if (has_armor) {
                char hbuf[8] = "", vbuf[8] = "";
                if (e->helm >= 0) std::snprintf(hbuf, sizeof(hbuf), "H%d", e->helm);
                if (e->vest >= 0) std::snprintf(vbuf, sizeof(vbuf), "V%d", e->vest);
                float hw = e->helm >= 0 ? fmono->CalcTextSizeA(CARD_FS_ARMOR, FLT_MAX, 0.0f, hbuf).x : 0.0f;
                float sp = (e->helm >= 0 && e->vest >= 0)
                            ? fmono->CalcTextSizeA(CARD_FS_ARMOR, FLT_MAX, 0.0f, " ").x : 0.0f;
                float vw = e->vest >= 0 ? fmono->CalcTextSizeA(CARD_FS_ARMOR, FLT_MAX, 0.0f, vbuf).x : 0.0f;
                float pw = hw + sp + vw + 14.0f;
                ImVec2 pa(R_ARMOR - pw, cy - PILL_H_ARMOR*0.5f), pb(R_ARMOR, cy + PILL_H_ARMOR*0.5f);
                dl->AddRectFilled(pa, pb, P::hud::ARMOR_BG, 6.0f);
                float tx_ = pa.x + 7.0f;
                float ty_ = cap_top(fmono, cy, CARD_FS_ARMOR);
                if (e->helm >= 0) {
                    dl->AddText(fmono, CARD_FS_ARMOR, ImVec2(tx_, ty_),
                                e->dead ? P::TEXT_DIM : P::armor_tier_col(e->helm), hbuf);
                    tx_ += hw + sp;
                }
                if (e->vest >= 0) {
                    dl->AddText(fmono, CARD_FS_ARMOR, ImVec2(tx_, ty_),
                                e->dead ? P::TEXT_DIM : P::armor_tier_col(e->vest), vbuf);
                }
            } else {
                float dw = fmono->CalcTextSizeA(CARD_FS_ARMOR, FLT_MAX, 0.0f, EN_DASH).x;
                dl->AddText(fmono, CARD_FS_ARMOR,
                            ImVec2(R_ARMOR - dw, cap_top(fmono, cy, CARD_FS_ARMOR)),
                            P::TEXT_FAINT, EN_DASH);
            }
        }

        // 6. Дистанция (по правому краю R_DIST).
        {
            char dbuf[16];
            std::snprintf(dbuf, sizeof(dbuf), "%.0fm", dm);
            float dw = fmono->CalcTextSizeA(CARD_FS_DIST, FLT_MAX, 0.0f, dbuf).x;
            dl->AddText(fmono, CARD_FS_DIST,
                        ImVec2(R_DIST - dw, cap_top(fmono, cy, CARD_FS_DIST)), c_dist, dbuf);
        }

        // 7. Статус — пилюля или тире, по правому краю R_STATUS.
        {
            const char* status = e->dead    ? "DEAD"
                              : is_visible  ? "VIS"
                                            : nullptr;
            if (status) {
                float sw = fmono->CalcTextSizeA(CARD_FS_STATUS, FLT_MAX, 0.0f, status).x;
                float pw = sw + 16.0f;
                ImVec2 pa(R_STATUS - pw, cy - PILL_H_STATUS*0.5f), pb(R_STATUS, cy + PILL_H_STATUS*0.5f);
                ImU32 pbg = e->dead ? P::hud::DEAD_BG   : P::hud::VIS_BG;
                ImU32 pln = e->dead ? P::hud::DEAD_LINE : P::hud::VIS_LINE;
                ImU32 ptx = e->dead ? P::TEXT_DIM       : P::hud::DOT_VIS;
                dl->AddRectFilled(pa, pb, pbg, 999.0f);
                dl->AddRect      (pa, pb, pln, 999.0f, 0, 1.0f);
                dl->AddText(fmono, CARD_FS_STATUS,
                            ImVec2(pa.x + 8.0f, cap_top(fmono, cy, CARD_FS_STATUS)),
                            ptx, status);
            } else {
                float dw = fmono->CalcTextSizeA(CARD_FS_STATUS, FLT_MAX, 0.0f, EN_DASH).x;
                dl->AddText(fmono, CARD_FS_STATUS,
                            ImVec2(R_STATUS - dw, cap_top(fmono, cy, CARD_FS_STATUS)),
                            P::TEXT_FAINT, EN_DASH);
            }
        }

        // 2. Имя — Semibold, гибкий слот слева, 13/600 по §4.2. Раньше стояло
        // 17: Segoe UI читался на размер мельче соседних моно-чипов. Inter
        // крупнее по x-height, паритет держится и на размере из ТЗ.
        {
            std::string name = e->name.empty() ? e->cls : e->name;
            dl->AddText(fsemi, CARD_FS_NAME,
                        ImVec2(name_x, cap_top(fsemi, cy, CARD_FS_NAME)),
                        c_name, name.c_str());
        }
    }

    if (out_bottom_y)
        *out_bottom_y = y_cursor + (CARD_H + CARD_GAP) * (float)n - CARD_GAP;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Панель добычи (TZ-Top-Loot §1-10)
// ═══════════════════════════════════════════════════════════════════════════
// Панель по левому краю с заголовком «TOP LOOT · <радиус>» и максимум шестью
// двухуровневыми строками: [имя · дистанция · цена] над полосой ценности,
// заполненной пропорционально price / max(price). Цвет тира лежит и на цене,
// и на полосе, поэтому легендарная строка читается золотом целиком.
void top_loot(const Snapshot* snap, const RenderConfig& cfg, float y_anchor) {
    if (!cfg.show_top_loot) return;
    if (!snap || !snap->in_raid) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    ImFont* fmono = font_mono();
    ImFont* fsemi = font_semi();

    // ── Сбор строк: в радиусе, выше фильтра цены, сортировка по цене вниз,
    // обрезка до шести (§10).
    struct Row { std::string name; uint32_t price; float dist_m; int tier; };
    std::vector<Row> rows;
    const float RANGE_CM = cfg.top_loot_range_m * 100.0f;
    for (const auto& lb : snap->loot) {
        if (lb.corpse_val > 0) continue;           // агрегаты трупов не сюда
        float dx = lb.x - snap->cam.x, dy = lb.y - snap->cam.y;
        float d_cm = std::sqrt(dx*dx + dy*dy);
        if (d_cm > RANGE_CM) continue;
        float dm = d_cm / 100.0f;
        for (const auto& it : lb.items) {
            // Top-loot имеет СВОЮ сортировку по цене — не режем min_loot_value
            // фильтром (тот только для точек на карте). Топ-N цен всегда
            // осмысленный ranking даже в бедных на лут рейдах.
            if (it.price == 0) continue;
            rows.push_back({ it.name.empty() ? std::string("?") : it.name,
                             it.price, dm, P::loot::tier_of(it.price) });
        }
    }
    if (rows.empty()) return;
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.price > b.price; });
    if ((int)rows.size() > LOOT_ROWS_MAX) rows.resize(LOOT_ROWS_MAX);
    uint32_t max_price = rows.front().price;
    if (max_price == 0) return;   // guard: bar-fill div-by-zero

    // v0.9.430: loot panel — RIGHT edge, vertically CENTERED (operator req).
    // v2026-09-23 arenahack: cfg.top_loot_screen_x/y override for drag support.
    const int   n       = (int)rows.size();
    const float panel_w = LOOT_W + LOOT_PAD_X * 2.0f;
    const float list_h  = LOOT_ITEM_H * (float)n + LOOT_ROW_GAP * (float)(n - 1);
    const float panel_h = LOOT_PAD_Y * 2.0f + LOOT_HEAD_H + LOOT_HEAD_GAP + list_h;
    // cfg.top_loot_screen_x/y is the top-RIGHT anchor point.
    const bool  manual  = (cfg.top_loot_screen_x > 0.5f || cfg.top_loot_screen_y > 0.5f);
    const float OX      = manual ? (cfg.top_loot_screen_x - panel_w)
                                 : ((float)cfg.screen_w - panel_w - HUD_ANCHOR);
    const float OY      = manual ? cfg.top_loot_screen_y
                                 : (((float)cfg.screen_h - panel_h) * 0.5f);
    (void)y_anchor;

    ImVec2 pa(OX, OY), pb(OX + panel_w, OY + panel_h);
    dl->AddRectFilled(pa, pb, P::loot::PANEL_BG, LOOT_RADIUS);
    dl->AddRect      (pa, pb, P::loot::PANEL_LINE, LOOT_RADIUS, 0, 1.0f);

    // ── §4 заголовок: 11/600 капсом (ТЗ 10.5, снапнуто на шаг шкалы).
    float head_x     = pa.x + LOOT_PAD_X;
    float head_mid_y = pa.y + LOOT_PAD_Y + LOOT_HEAD_H * 0.5f;
    dl->AddText(fsemi, LOOT_FS_HEAD,
                ImVec2(head_x, cap_top(fsemi, head_mid_y, LOOT_FS_HEAD)),
                P::AMBER, "TOP LOOT");

    char range_buf[16];
    std::snprintf(range_buf, sizeof(range_buf), "%dm", (int)cfg.top_loot_range_m);
    float range_w = fmono->CalcTextSizeA(LOOT_FS_MONO, FLT_MAX, 0.0f, range_buf).x;
    dl->AddText(fmono, LOOT_FS_MONO,
                ImVec2(pb.x - LOOT_PAD_X - range_w, cap_top(fmono, head_mid_y, LOOT_FS_MONO)),
                P::TEXT_DIM, range_buf);

    // ── §5 список
    float y = pa.y + LOOT_PAD_Y + LOOT_HEAD_H + LOOT_HEAD_GAP;
    const float content_left  = pa.x + LOOT_PAD_X;
    const float content_right = content_left + LOOT_W;

    for (int i = 0; i < n; i++) {
        const Row& r = rows[i];
        bool  muted  = (r.tier >= 4);              // два самых дешёвых тира
        ImU32 name_c = muted ? P::TEXT_MUTED : P::TEXT;
        ImU32 tier_c = P::loot::TIER[r.tier];

        char price_buf[16];
        if      (r.price >= 1000000) std::snprintf(price_buf, sizeof(price_buf), "%.1fM", r.price / 1e6f);
        else if (r.price >= 1000)    std::snprintf(price_buf, sizeof(price_buf), "%dk",   r.price / 1000);
        else                         std::snprintf(price_buf, sizeof(price_buf), "%d",    r.price);

        char dist_buf[16];
        std::snprintf(dist_buf, sizeof(dist_buf), "%dm", (int)r.dist_m);

        float text_mid_y = y + LOOT_TEXT_ROW_H * 0.5f;

        // Цена по правому краю контента.
        float pw = fmono->CalcTextSizeA(LOOT_FS_MONO, FLT_MAX, 0.0f, price_buf).x;
        dl->AddText(fmono, LOOT_FS_MONO,
                    ImVec2(content_right - pw, cap_top(fmono, text_mid_y, LOOT_FS_MONO)),
                    tier_c, price_buf);

        // Дистанция — правее на слот цены плюс зазор.
        float dist_right = content_right - LOOT_PRICE_SLOT - LOOT_ROW_GAP;
        float dw_ = fmono->CalcTextSizeA(LOOT_FS_MONO, FLT_MAX, 0.0f, dist_buf).x;
        dl->AddText(fmono, LOOT_FS_MONO,
                    ImVec2(dist_right - dw_, cap_top(fmono, text_mid_y, LOOT_FS_MONO)),
                    P::TEXT_DIM, dist_buf);

        // Имя — Semibold, 13/600 (§5.1 задаёт 12.5, снапнуто на шаг шкалы).
        dl->AddText(fsemi, LOOT_FS_NAME,
                    ImVec2(content_left, cap_top(fsemi, text_mid_y, LOOT_FS_NAME)),
                    name_c, r.name.c_str());

        // Полоса второго уровня (§5.1).
        float bar_y = y + LOOT_TEXT_ROW_H + LOOT_BAR_GAP;
        ImVec2 ba(content_left, bar_y), bb(content_left + LOOT_W, bar_y + LOOT_BAR_H);
        dl->AddRectFilled(ba, bb, P::loot::BAR_TRACK, 2.0f);
        float fill = LOOT_W * ((float)r.price / (float)max_price);
        if (fill > 0.0f) dl->AddRectFilled(ba, ImVec2(ba.x + fill, bb.y), tier_c, 2.0f);

        y += LOOT_ITEM_H + LOOT_ROW_GAP;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Радар «Растворяющийся конус» (TZ-Radar §1-11, редакция 2)
// ═══════════════════════════════════════════════════════════════════════════
// Диск + два кольца + ПОТАРГЕТНЫЕ конусы обзора с РАДИАЛЬНЫМ ГРАДИЕНТОМ +
// точки + треугольник игрока. Своего конуса FOV нет (§11). Каждый конус — веер
// из 14 треугольников от точки цели наружу, с пер-вершинной альфой: заливка
// гаснет от вершины (α = .42 игрок / .40 бот) к дуге (α = 0). В SVG это был бы
// radialGradient, в ImGui его нет, поэтому собираем из градиентных треугольников
// по §10.
//
// Правило порядка (§2): конус и его точка рисуются ПАРОЙ — сначала конус, сразу
// за ним своя точка, чтобы у перекрывающихся целей чужая точка не уезжала под
// чужой конус. Соглашение «нос вверх»: треугольник игрока всегда смотрит вверх,
// точки и конусы поворачиваются на −cam.yaw. Числа масштабируются от эталонного
// viewBox 300×300 коэффициентом k = size/300.
// Тело радара. Вынесено из radar(), чтобы превью в панели рисовалось ЭТИМ ЖЕ
// кодом в свой прямоугольник: у превью и боевого радара нет второй реализации,
// значит они не могут разъехаться.
void radar_at(const Snapshot* snap, const RenderConfig& cfg,
              ImDrawList* dl, ImVec2 org, float S) {
    if (!snap || !dl || S <= 0.0f) return;

    // §1 — сторона панели = 2×экранный радиус. Эталон 300 = 2×150, k = S/300.
    const float k = S / RADAR_VB;

    auto Pt = [&](float sx, float sy) { return ImVec2(org.x + sx * k, org.y + sy * k); };
    auto sc = [&](float v) { return v * k; };

    const ImVec2 C_     = Pt(RADAR_VB * 0.5f, RADAR_VB * 0.5f);
    const float  r_out  = sc(RADAR_R_OUT);
    const float  line_w = sc(1.0f);

    // 1. Диск
    dl->AddCircleFilled(C_, r_out, P::radar::DISC, 96);
    dl->AddCircle      (C_, r_out, P::radar::DISC_LINE, 96, line_w);

    // 2. Кольца (§4: r=95 и r=48)
    if (cfg.radar_rings) {
        dl->AddCircle(C_, sc(RADAR_R_MID), P::radar::RING, 96, line_w);
        dl->AddCircle(C_, sc(RADAR_R_IN),  P::radar::RING, 96, line_w);
    }

    // Разворот «нос вверх»: мир крутится вокруг yaw камеры.
    const float yr  = snap->cam.yaw * PI_F / 180.0f;
    const float cyr = std::cos(yr);
    const float syr = std::sin(yr);
    const float range_cm = cfg.radar_range_m * 100.0f;
    const float w2r      = r_out / range_cm;

    int my_team = -1;
    for (const auto& e : snap->entities) if (e.me) { my_team = e.team; break; }

    struct Proj { bool ok; ImVec2 pos; };
    auto project = [&](float wdx, float wdy) -> Proj {
        float d = std::sqrt(wdx*wdx + wdy*wdy);
        if (d > range_cm) return { false, ImVec2() };
        float fwd   =  wdx * cyr + wdy * syr;
        float right = -wdx * syr + wdy * cyr;
        return { true, ImVec2(C_.x + right * w2r, C_.y - fwd * w2r) };
    };

    // §10: радиальный градиент собирается из N залитых треугольников — вершина
    // в апексе на полной альфе, две точки дуги на нуле. Пер-вершинный цвет в
    // ImDrawList доступен только через PrimReserve + PrimWriteVtx/Idx.
    auto cone_gradient = [&](ImVec2 apex, float target_yaw_deg, float R, ImU32 col_full) {
        const int   N    = RADAR_CONE_SEGS;
        const float half = RADAR_CONE_DEG * 0.5f * PI_F / 180.0f;
        float rel = (target_yaw_deg - snap->cam.yaw) * PI_F / 180.0f;

        ImVec2 arc[RADAR_CONE_SEGS + 1];
        for (int i = 0; i <= N; i++) {
            float t  = rel - half + (float)i / (float)N * 2.0f * half;
            arc[i]   = ImVec2(apex.x + std::sin(t) * R, apex.y - std::cos(t) * R);
        }

        const ImU32  col_zero = col_full & 0x00FFFFFFu;   // тот же RGB, альфа 0
        const ImVec2 uv       = ImGui::GetIO().Fonts->TexUvWhitePixel;

        dl->PrimReserve(3 * N, 3 * N);
        for (int i = 0; i < N; i++) {
            ImDrawIdx v0 = (ImDrawIdx)dl->_VtxCurrentIdx;
            dl->PrimWriteVtx(apex,     uv, col_full);
            dl->PrimWriteVtx(arc[i],   uv, col_zero);
            dl->PrimWriteVtx(arc[i+1], uv, col_zero);
            dl->PrimWriteIdx(v0);
            dl->PrimWriteIdx((ImDrawIdx)(v0 + 1));
            dl->PrimWriteIdx((ImDrawIdx)(v0 + 2));
        }
    };

    // ── §2 порядок отрисовки: на каждую цель сначала конус (если есть), затем
    // её точка, ПАРОЙ.
    auto draw_target = [&](const Entity& e) {
        Proj p = project(e.x - snap->cam.x, e.y - snap->cam.y);
        if (!p.ok) return;

        bool is_bot = e.cls.starts_with("BOT");
        bool is_pmc = !is_bot && (e.cls.starts_with("PMC")
                                   || e.cls.starts_with("Player")
                                   || e.cls.starts_with("USER"));

        // Конус только у живых целей — нужен yaw, куда его направить.
        // Тумблер «Направление взгляда» (radar_aim_dir) гасит конусы: раньше
        // он не влиял ни на что.
        // v0.9.430: dot color follows cfg.col_box_pmc/bot swatch.  Cone
        // shares RGB with alpha 140.
        auto with_alpha = [](ImU32 c, int a) {
            return (c & 0x00FFFFFFu) | ((ImU32)(a & 0xFF) << 24);
        };
        ImU32 pmc_col = cfg.col_box_pmc;
        ImU32 bot_col = cfg.col_box_bot;
        if (cfg.radar_aim_dir && !e.dead && e.yaw.has_value()) {
            if (is_pmc) cone_gradient(p.pos, *e.yaw, sc(RADAR_CONE_PMC), with_alpha(pmc_col, 140));
            if (is_bot) cone_gradient(p.pos, *e.yaw, sc(RADAR_CONE_BOT), with_alpha(bot_col, 140));
        }
        // Точка.
        if (e.dead) {
            // v0.9.430: radar corpse gated by SAME toggle as world corpse
            // (cfg.show_corpse for PMC, cfg.show_bot_corpse for BOT).
            // Colour from the same swatch (cfg.col_corpses_pmc/bot).
            if (is_pmc && !cfg.show_corpse) return;
            if (is_bot && !cfg.show_bot_corpse) return;
            ImU32 c_col = is_pmc ? cfg.col_corpses_pmc
                        : (is_bot ? cfg.col_corpses_bot : P::radar::CORPSE);
            dl->AddCircleFilled(p.pos, sc(RADAR_DOT_BOT), c_col, 20);
            dl->AddCircle(p.pos, sc(RADAR_DOT_BOT), P::radar::DOT_LINE, 20, sc(RADAR_DOT_LINE));
        } else if (is_pmc) {
            dl->AddCircleFilled(p.pos, sc(RADAR_DOT_PMC), pmc_col, 20);
            dl->AddCircle(p.pos, sc(RADAR_DOT_PMC), P::radar::DOT_LINE, 20, sc(RADAR_DOT_LINE));
        } else if (is_bot) {
            dl->AddCircleFilled(p.pos, sc(RADAR_DOT_BOT), bot_col, 20);
            dl->AddCircle(p.pos, sc(RADAR_DOT_BOT), P::radar::DOT_LINE, 20, sc(RADAR_DOT_LINE));
        }
    };

    // Лут на радаре — заглушка из ТЗ, тумблера пока нет.
    const bool show_radar_loot = false;
    if (show_radar_loot) {
        for (const auto& lb : snap->loot) {
            if (lb.is_corpse) continue;
            Proj p = project(lb.x - snap->cam.x, lb.y - snap->cam.y);
            if (!p.ok) continue;
            dl->AddCircleFilled(p.pos, sc(RADAR_DOT_LOOT), P::radar::LOOT, 16);
        }
    }

    // Цели: сначала боты, потом игроки, чтобы пары игроков ложились поверх пар
    // ботов при наложении. Трупы обрабатываются в том же цикле веткой dead —
    // без конуса, только серая точка.
    for (const auto& e : snap->entities) {
        if (e.me) continue;
        if (e.dead) draw_target(e);
    }
    if (cfg.show_radar_bots) {
        for (const auto& e : snap->entities) {
            if (e.me || e.dead) continue;
            if (!e.cls.starts_with("BOT")) continue;
            draw_target(e);
        }
    }
    if (cfg.show_radar_pmc) {
        for (const auto& e : snap->entities) {
            if (e.me || e.dead) continue;
            bool is_pmc = e.cls.starts_with("PMC") || e.cls.starts_with("Player")
                       || e.cls.starts_with("USER");
            if (!is_pmc) continue;
            if (!cfg.show_mates && my_team >= 0 && e.team == my_team) continue;
            draw_target(e);
        }
    }

    // §7 — своя точка в центре. Была треугольником-стрелкой; направление она
    // всё равно не несла (радар «нос вверх», игрок всегда смотрит вверх), а по
    // весу выбивалась из остальных отметок. Теперь такой же кружок, как у
    // целей, только янтарный.
    dl->AddCircleFilled(C_, sc(RADAR_DOT_SELF), P::radar::SELF, 20);
    dl->AddCircle(C_, sc(RADAR_DOT_SELF), P::radar::DOT_LINE, 20, sc(RADAR_DOT_LINE));
}

// Боевой вызов: угол и размер берутся из конфига, рисуем в передний слой.
void radar(const Snapshot* snap, const RenderConfig& cfg) {
    if (!cfg.show_radar) return;
    if (!snap || !snap->in_raid) return;
    const float S = (float)cfg.radar_px_radius * 2.0f;
    radar_at(snap, cfg, ImGui::GetForegroundDrawList(),
             corner_origin(cfg.radar_position, S, HUD_ANCHOR,
                           (float)cfg.screen_w, (float)cfg.screen_h), S);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Счётчик патронов «Дуга магазина» (TZ-Ammo-Counter §1-8)
// ═══════════════════════════════════════════════════════════════════════════
// Диск 92×92 в правом нижнем углу: кольцо-трек плюс дуга, растущая по часовой
// от 12 часов пропорционально (mag_cur / mag_max). Цвет состояния прыгает на
// ⅓ и на нуле (§4). Цифры в центре: крупная текущая сверху, ёмкость снизу, с
// тенью, чтобы читалось на светлом кадре. На нуле дуга исчезает, а нижняя
// строка становится «RELOAD» бордовым.
void ammo_counter(const Snapshot* snap, const RenderConfig& cfg) {
    if (!cfg.show_my_ammo || !snap) return;
    const auto& cam = snap->cam;
    int cur = cam.mag_cur_a;
    int mx  = cam.mag_max;
    if (mx <= 0) return;
    // v0.9.430: reader occasionally returns -1 when the read races the game's
    // ammo write.  Clamp to [0, mx] so we never render "-1" or a value above
    // mag capacity.  0 → shows RELOAD label as before.
    if (cur < 0)  cur = 0;
    if (cur > mx) cur = mx;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    float frac     = cur < 0 ? 0.0f : (float)cur / (float)mx;
    bool  is_empty = (cur == 0);
    ImU32 col_state = P::ammo::state_col(cur, mx);

    // §1 геометрия — якорь в правый низ, отступ 16.
    ImVec2 origin((float)cfg.screen_w - AMMO_PAD - AMMO_SIZE,
                  (float)cfg.screen_h - AMMO_PAD - AMMO_SIZE);
    ImVec2 center(origin.x + AMMO_SIZE * 0.5f, origin.y + AMMO_SIZE * 0.5f);

    // §2.1 трек — рисуется всегда, даже на нуле.
    dl->PathArcTo(center, AMMO_R, 0.0f, 2.0f * PI_F, 48);
    dl->PathStroke(P::ammo::TRACK, ImDrawFlags_None, AMMO_W);

    // §2.2 заливка — старт на 12 часах (=-π/2), растёт по часовой.
    if (!is_empty && frac > 0.0f) {
        float start = -PI_F * 0.5f;
        float end   = start + 2.0f * PI_F * frac;
        dl->PathArcTo(center, AMMO_R, start, end, 48);
        dl->PathStroke(col_state, ImDrawFlags_None, AMMO_W);
        // §8 шаг 3: у дуг в ImDrawList нет round-cap — доставляем торцы кружками
        // в половину толщины, чтобы совпало с SVG `stroke-linecap: round`.
        ImVec2 p_start(center.x + AMMO_R * std::cos(start), center.y + AMMO_R * std::sin(start));
        ImVec2 p_end  (center.x + AMMO_R * std::cos(end),   center.y + AMMO_R * std::sin(end));
        dl->AddCircleFilled(p_start, AMMO_W * 0.5f, col_state, 12);
        dl->AddCircleFilled(p_end,   AMMO_W * 0.5f, col_state, 12);
    }

    // §3 цифры.
    ImFont* fmono = font_mono();

    char cur_buf[8];
    std::snprintf(cur_buf, sizeof(cur_buf), "%d", cur);
    char sub_num_buf[8];
    const char* sub_buf = "RELOAD";
    if (!is_empty) {
        std::snprintf(sub_num_buf, sizeof(sub_num_buf), "%d", mx);
        sub_buf = sub_num_buf;
    }
    float sz_sub = is_empty ? AMMO_FS_RELOAD : AMMO_FS_SUB;

    ImVec2 cur_sz = fmono->CalcTextSizeA(AMMO_FS_CUR, FLT_MAX, 0.0f, cur_buf);
    ImVec2 sub_sz = fmono->CalcTextSizeA(sz_sub,      FLT_MAX, 0.0f, sub_buf);

    // Вертикаль: line-height 1, то есть коробка строки равна кеглю; блок
    // центрируется по центру диска.
    float block_h   = AMMO_FS_CUR + AMMO_GAP + sz_sub;
    float block_top = center.y - block_h * 0.5f;

    ImVec2 cur_pos(center.x - cur_sz.x * 0.5f, block_top - (cur_sz.y - AMMO_FS_CUR) * 0.5f);
    ImVec2 sub_pos(center.x - sub_sz.x * 0.5f,
                   block_top + AMMO_FS_CUR + AMMO_GAP - (sub_sz.y - sz_sub) * 0.5f);

    ImU32 col_cur = is_empty ? P::ammo::EMPTY : col_state;
    ImU32 col_sub = is_empty ? P::ammo::EMPTY : P::ammo::SUB;

    // §3 text-shadow (0 2 5 rgba(0,0,0,.85)) — блюра в ImGui нет, эмулируем
    // сплошной копией со сдвигом.
    dl->AddText(fmono, AMMO_FS_CUR, ImVec2(cur_pos.x, cur_pos.y + 2.0f), P::ammo::SHADOW, cur_buf);
    dl->AddText(fmono, sz_sub,      ImVec2(sub_pos.x, sub_pos.y + 2.0f), P::ammo::SHADOW, sub_buf);
    dl->AddText(fmono, AMMO_FS_CUR, cur_pos, col_cur, cur_buf);
    dl->AddText(fmono, sz_sub,      sub_pos, col_sub, sub_buf);
}

}  // namespace abi::hud
