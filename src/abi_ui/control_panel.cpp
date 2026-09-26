// control_panel.cpp — Gamebreaker control panel (Dear ImGui + ImDrawList, C++20)
// Внешний контракт: ровно 4 функции из control_panel.hpp. Всё остальное — namespace { }.
// Цвета — только из palette.hpp (abi::pal). Никаких сырых IM_COL32 с числами.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "control_panel.hpp"
#include "icons.hpp"
#include "overlay_hud.hpp"
#include "palette.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#define U8(s) reinterpret_cast<const char*>(u8##s)

namespace abi {
namespace {

// ── colors: псевдонимы токенов palette.hpp (abi::pal::gb — точные цвета макета) ──
namespace P = abi::pal;
namespace G = abi::pal::gb;
constexpr ImU32 C_WINDOW  = G::WINDOW;
constexpr ImU32 C_BAR     = G::BAR;
constexpr ImU32 C_CARD    = G::CARD;
constexpr ImU32 C_PANEL   = G::PANEL;
constexpr ImU32 C_CHIP    = G::CHIP;
constexpr ImU32 C_CHIP_ON = G::CHIP_ON;
constexpr ImU32 C_LINE    = G::LINE;
constexpr ImU32 C_TRACK   = G::TRACK;
constexpr ImU32 C_TEXT    = G::TEXT;
constexpr ImU32 C_MUTED   = G::TEXT_MUTED;
constexpr ImU32 C_DIM     = G::TEXT_DIM;
constexpr ImU32 C_FAINT   = G::TEXT_FAINT;
constexpr ImU32 C_ACCENT  = G::ACCENT;
constexpr ImU32 C_SLIDER  = G::ACCENT;
constexpr ImU32 C_ON_TX   = G::ON_ACCENT;   // текст/иконка на активной пилюле
constexpr ImU32 C_KNOB    = G::KNOB;        // ручки ползунков
constexpr ImU32 C_SEG_BG  = G::SEG_BG;
constexpr ImU32 C_SW_LINE = G::SWATCH_LINE;
constexpr ImU32 C_SELECT  = G::SELECTION;
constexpr ImU32 C_SHADOW  = G::KNOB_SHADOW;
constexpr ImU32 C_LOGO    = G::LOGO_TILE;
constexpr ImU32 C_SCROLL  = G::SCROLL;
constexpr ImU32 C_SCR_HOT = G::SCROLL_HOT;
constexpr ImU32 C_CH_R    = G::CH_R;
constexpr ImU32 C_CH_G    = G::CH_G;
constexpr ImU32 C_CH_B    = G::CH_B;

// ── UI state ───────────────────────────────────────────────────────────────
enum class Lang : int { ru = 0, en = 1 };

enum EspKey : int {
    K_enable, K_box, K_corners, K_name, K_team, K_weapon, K_ammo,
    K_health, K_armor, K_distance, K_corpses, K_teammates, K_COUNT
};

struct EspItem {
    bool  on        = true;
    bool  has_color = false;
    ImU32 c         = 0;
    int   style     = 0;   // box: 0 = 2D, 1 = 3D
    int   dist      = 400; // box: 0..400 m
    int   display   = 1;   // armor: 0 = text, 1 = bar
    int   min_val   = 0;   // corpses: 0..500 $
};

struct Radar {
    bool show = false, players = true, bots = true, rings = true, aim = true;
    int  corner = 1;       // 0 tl, 1 tr, 2 bl, 3 br
    int  world  = 200;     // 50..400 m
    int  screen = 250;     // 100..400 px
};

struct Loot {
    bool list    = false;    // top-loot список удалён из UI
    int  side    = 1;        // 0 left, 1 right (без эффекта — список выключен)
    int  min_val = 75000;    // мин. ценность мировых маркеров
};

struct State {
    Lang    lang = Lang::ru;
    int     sec  = 0;      // 0 visuals, 1 radar, 2 loot
    int     vt   = 0;      // 0 players, 1 bots
    EspItem esp[2][K_COUNT];
    Radar   radar;
    Loot    loot;
};

constexpr EspKey P_KEYS[] = {K_enable, K_box, K_corners, K_name, K_team, K_weapon,
                             K_ammo, K_health, K_armor, K_distance, K_corpses, K_teammates};
constexpr EspKey B_KEYS[] = {K_enable, K_box, K_corners, K_weapon, K_ammo, K_distance, K_corpses};
constexpr int P_N = sizeof(P_KEYS) / sizeof(P_KEYS[0]);
constexpr int B_N = sizeof(B_KEYS) / sizeof(B_KEYS[0]);
constexpr int PAL_N = sizeof(G::ESP_PAL) / sizeof(G::ESP_PAL[0]);
constexpr const char* CORNER_IC[4] = {"corner_tl", "corner_tr", "corner_bl", "corner_br"};

// ── strings ────────────────────────────────────────────────────────────────
struct Tx {
    const char* secs[3];
    const char* vts[2];
    const char* esp[K_COUNT];
    const char *style, *dist, *display, *text, *bar, *minVal;
    const char* rToggles[5];
    const char *gShow, *gSize, *world, *screen;
    const char *gList, *list, *left, *right, *gFilter, *lootMin;
};

const Tx RU = {
    {U8("Визуал"), U8("Радар"), U8("Лут")},
    {U8("Игроки"), U8("Боты")},
    {U8("Включить"), U8("Рамка"), U8("Уголки вместо рамки"), U8("Ник"), U8("Команда"), U8("Оружие"),
     U8("Патроны"), U8("Здоровье"), U8("Броня"), U8("Дистанция"), U8("Трупы"), U8("Показывать тиммейтов")},
    U8("Стиль"), U8("Дистанция рамки"), U8("Отображение"), U8("Текст"), U8("Полоса"), U8("Мин. ценность"),
    {U8("Показывать радар"), U8("Игроки"), U8("Боты"), U8("Кольца дистанции"), U8("Направление взгляда")},
    U8("Отображение"), U8("Размер и дальность"), U8("Радиус в мире"), U8("Радиус на экране"),
    U8("Топ-лут"), U8("Показывать топ-лут"), U8("Слева"), U8("Справа"), U8("Фильтр"), U8("Мин. ценность лута"),
};

const Tx EN = {
    {"Visuals", "Radar", "Loot"},
    {"Players", "Bots"},
    {"Enable", "Box", "Corner brackets", "Name", "Team", "Weapon",
     "Ammo", "Health", "Armor", "Distance", "Corpses", "Show teammates"},
    "Style", "Box distance", "Display", "Text", "Bar", "Min value",
    {"Show radar", "Players", "Bots", "Distance rings", "Aim direction"},
    "Display", "Size & range", "World radius", "Screen radius",
    "Top loot", "Show top-loot list", "Left", "Right", "Filter", "Min loot value",
};

// ── fonts ──────────────────────────────────────────────────────────────────
// Хост грузит 9 шрифтов с ImFontConfig::Name = "gb:<family><weight>:<size>"
// (см. HOST_PATCH.md). Панель находит их по имени; не найден — Fonts[0].
struct Font { ImFont* f = nullptr; float size = 0; };
struct Fonts { Font ub24, ub20, ub16, ub52, ub14b, ub12b, ub9, ub14, ub12, mono12; } F;
float s_text_k = 1.f, s_mono_k = 1.f;

// ── textures (0 до вызова сеттеров — просто не рисуем) ─────────────────────
ImTextureID s_operator_tex{};   int s_operator_w = 0, s_operator_h = 0;
ImTextureID s_background_tex{}; int s_background_w = 0, s_background_h = 0;

// ── context ────────────────────────────────────────────────────────────────
struct Rc {
    ImVec2 a, b;
    bool has(ImVec2 p) const { return p.x >= a.x && p.y >= a.y && p.x < b.x && p.y < b.y; }
};

State make_default() {
    State s;
    auto col = [](EspItem& it, ImU32 c) { it.has_color = true; it.c = c; };
    auto& Pl = s.esp[0];
    col(Pl[K_box], G::ESP_BOX);       col(Pl[K_name], G::ESP_NAME);
    col(Pl[K_weapon], G::ESP_TEXT);   col(Pl[K_ammo], G::ESP_TEXT);
    col(Pl[K_distance], G::ESP_DIST); col(Pl[K_corpses], G::ESP_CORPSE);
    Pl[K_teammates].on = false;
    auto& Bt = s.esp[1];
    col(Bt[K_box], G::ESP_BOX);       col(Bt[K_weapon], G::ESP_TEXT);
    col(Bt[K_ammo], G::ESP_TEXT);     col(Bt[K_distance], G::ESP_TEXT);
    col(Bt[K_corpses], G::ESP_CORPSE);
    return s;
}

struct Ctx {
    ImDrawList* dl = nullptr;
    ImVec2 o;
    float W = 960, H = 540;
    float alpha = 1;
    bool live = true;
    Rc clip;
    float scroll = 0, scroll_target = 0, content_h = 0;
    ImGuiID drag_id = 0;
    ImGuiID edit_id = 0;
    char edit_buf[24] = {};
    int edit_len = 0;
    bool edit_fresh = false;
    int open_vt = -1, open_key = -1, open_mode = 0; // 1 = цвет, 2 = настройки
    ImVec2 pos{-1, -1};
    float S = 1;                 // масштаб UI (DPI), = text_k
    bool win_drag = false;
    ImVec2 drag_off;
    float sb_off = 0;
    State st = make_default();
} g;

const Tx& tx() { return g.st.lang == Lang::ru ? RU : EN; }

// ── utils (без <string>/<cstdlib>) ─────────────────────────────────────────
ImGuiID hash(const char* s) {
    ImGuiID h = 2166136261u;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

int utf8_len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }

bool parse_num(const char* s, double& out) {
    double v = 0, frac = 0.1; bool neg = false, any = false, dot = false;
    if (*s == '-') { neg = true; ++s; }
    for (; *s; ++s) {
        if (*s >= '0' && *s <= '9') {
            any = true;
            if (dot) { v += (*s - '0') * frac; frac *= 0.1; } else v = v * 10 + (*s - '0');
        } else if ((*s == '.' || *s == ',') && !dot) dot = true;
        else break;
    }
    out = neg ? -v : v;
    return any;
}

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ch_r(ImU32 c) { return (c >> IM_COL32_R_SHIFT) & 0xFF; }
int ch_g(ImU32 c) { return (c >> IM_COL32_G_SHIFT) & 0xFF; }
int ch_b(ImU32 c) { return (c >> IM_COL32_B_SHIFT) & 0xFF; }
ImU32 mk(int r, int gg, int b) { return IM_COL32(ImClamp(r, 0, 255), ImClamp(gg, 0, 255), ImClamp(b, 0, 255), 255); }

// Применяет прозрачность a и текущий g.alpha (для приглушённых строк).
ImU32 C(ImU32 c, float a = 1) {
    float A = ((c >> IM_COL32_A_SHIFT) & 0xFF) * a * g.alpha;
    return (c & ~IM_COL32_A_MASK) | ((ImU32)ImClamp((int)(A + .5f), 0, 255) << IM_COL32_A_SHIFT);
}

ImU32 hsl(float h, float s, float l) {
    s /= 100; l /= 100;
    auto k = [&](float n) { return std::fmod(n + h / 30.f, 12.f); };
    float a = s * ImMin(l, 1 - l);
    auto f = [&](float n) { return l - a * ImMax(-1.f, ImMin(ImMin(k(n) - 3, 9 - k(n)), 1.f)); };
    return mk((int)std::lround(f(0) * 255), (int)std::lround(f(8) * 255), (int)std::lround(f(4) * 255));
}

float hue_of(ImU32 c) {
    float r = (float)ch_r(c), gg = (float)ch_g(c), b = (float)ch_b(c);
    float mx = ImMax(r, ImMax(gg, b)), mn = ImMin(r, ImMin(gg, b)), d = mx - mn;
    if (d == 0) return 0;
    float h = mx == r ? std::fmod((gg - b) / d, 6.f) : mx == gg ? (b - r) / d + 2 : (r - gg) / d + 4;
    return std::fmod(h * 60 + 360, 360.f);
}

// ── primitives ─────────────────────────────────────────────────────────────
// Вся раскладка — в логических px (макет 1080×680). В конце кадра вершины
// масштабируются на g.S вокруг g.o, поэтому мышь переводим в логические px.
ImVec2 mpos() { return g.o + (ImGui::GetIO().MousePos - g.o) / g.S; }
ImVec2 snap_px(ImVec2 p) {
    return g.o + ImVec2(std::floor((p.x - g.o.x) * g.S + .5f), std::floor((p.y - g.o.y) * g.S + .5f)) / g.S;
}

bool hov(ImVec2 a, ImVec2 b) {
    if (!g.live) return false;
    ImVec2 m = mpos();
    return Rc{a, b}.has(m) && g.clip.has(m);
}
// hot — кликабельная зона: hover + курсор-рука
bool hot(ImVec2 a, ImVec2 b) {
    bool h = hov(a, b);
    if (h) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return h;
}
bool clk(ImVec2 a, ImVec2 b) { return hot(a, b) && ImGui::IsMouseClicked(0); }

// ── animation ──────────────────────────────────────────────────────────────
// Экспоненциальное сглаживание к цели. Слоты без обращений > 2 c переиспользуются.
struct AnimSlot { ImGuiID id; float v; int frame; };
AnimSlot s_anim[512];

AnimSlot* anim_slot(ImGuiID id, float init) {
    int now = ImGui::GetFrameCount();
    unsigned i = id & 511;
    for (int n = 0; n < 512; n++, i = (i + 1) & 511) {
        AnimSlot& sl = s_anim[i];
        if (sl.id == id) { sl.frame = now; return &sl; }
        if (sl.id == 0 || now - sl.frame > 120) { sl = {id, init, now}; return &sl; }
    }
    return nullptr;
}
float anim(ImGuiID id, float target, float speed = 16.f) {
    AnimSlot* sl = anim_slot(id, target);
    if (!sl) return target;
    sl->v += (target - sl->v) * ImMin(1.f, ImGui::GetIO().DeltaTime * speed);
    if (std::fabs(target - sl->v) < .002f) sl->v = target;
    return sl->v;
}
void anim_set(ImGuiID id, float v) { if (AnimSlot* sl = anim_slot(id, v)) sl->v = v; }

ImGuiID hid(const char* tag, int a = 0, int b = 0) {
    return hash(tag) ^ ((ImGuiID)a * 0x9E3779B1u) ^ ((ImGuiID)b * 0x85EBCA77u);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    auto ch = [&](int sh) {
        int x = (a >> sh) & 255, y = (b >> sh) & 255;
        return (ImU32)ImClamp((int)(x + (y - x) * t + .5f), 0, 255) << sh;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}

void rect(ImVec2 a, ImVec2 b, ImU32 c, float r, ImDrawFlags f = 0) { g.dl->AddRectFilled(a, b, c, r, f); }
void inset(ImVec2 a, ImVec2 b, ImU32 c, float r) { g.dl->AddRect(a + ImVec2(.5f, .5f), b - ImVec2(.5f, .5f), c, r, 0, 1); }
void circle(ImVec2 c, float r, ImU32 col) { g.dl->AddCircleFilled(c, r, col); }

// tr — трекинг в em (CSS letter-spacing)
float tw(const Font& f, const char* s, float tr = 0) {
    if (tr == 0) return f.f->CalcTextSizeA(f.size, FLT_MAX, 0, s).x;
    float w = 0;
    for (const char* p = s; *p;) {
        int l = utf8_len((unsigned char)*p);
        w += f.f->CalcTextSizeA(f.size, FLT_MAX, 0, p, p + l).x + tr * f.size;
        p += l;
    }
    return w;
}
void txt(const Font& f, ImVec2 p, ImU32 c, const char* s, float tr = 0) {
    p = snap_px(p);
    if (tr == 0) { g.dl->AddText(f.f, f.size, p, c, s); return; }
    for (const char* q = s; *q;) {
        int l = utf8_len((unsigned char)*q);
        g.dl->AddText(f.f, f.size, p, c, q, q + l);
        p.x += f.f->CalcTextSizeA(f.size, FLT_MAX, 0, q, q + l).x + tr * f.size;
        q += l;
    }
}
void txt_vc(const Font& f, float x, float cy, ImU32 c, const char* s, float tr = 0) {
    txt(f, {x, cy - f.size * .5f}, c, s, tr);
}

void icon(const char* name, ImVec2 c, ImU32 tint) {
    ImTextureID t = icons::get(name);
    if (!t) return;
    ImVec2 a = snap_px(c - ImVec2(8, 8));   // растр 16×DPI ложится 1:1 на пиксели
    g.dl->AddImage(t, a, a + ImVec2(16, 16), {0, 0}, {1, 1}, tint);
}

// Вкл: кольцо 6px сжимается из пустого круга (10 → 4)
void radio(ImVec2 c, bool on, ImGuiID id) {
    float t = anim(id, on ? 1.f : 0.f, 18.f);
    circle(c, 10, C(mix(C_CHIP, C_CHIP_ON, ImMin(1.f, t * 2.f))));
    circle(c, 10 - 6 * t, C(C_CHIP));
}

// Сегментный переключатель: подложка 32px, пилюли 26px.
float seg_w(const char* const* labels, int n, const Font& f, float tr) {
    float w = 6.f + (n - 1);
    for (int i = 0; i < n; i++) w += tw(f, labels[i], tr) + 20;
    return w;
}
// Выбранная пилюля переезжает, не-выбранные подсвечивают текст при наведении.
int seg(const char* key, ImVec2 p, const char* const* labels, int n, int sel, const Font& f, float tr) {
    rect(p, {p.x + seg_w(labels, n, f, tr), p.y + 32}, C(C_SEG_BG), 9);
    float xs[8], ws[8], x = p.x + 3;
    for (int i = 0; i < n; i++) { ws[i] = tw(f, labels[i], tr) + 20; xs[i] = x; x += ws[i] + 1; }
    float ax = anim(hid(key, 1), xs[sel] - p.x, 20.f), aw = anim(hid(key, 2), ws[sel], 20.f);
    rect({p.x + ax, p.y + 3}, {p.x + ax + aw, p.y + 29}, C(C_CHIP_ON), 7);
    int hit = -1;
    for (int i = 0; i < n; i++) {
        ImVec2 a{xs[i], p.y + 3}, b{xs[i] + ws[i], p.y + 29};
        bool h = i != sel && hot(a, b);
        float ht = anim(hid(key, 10 + i), h ? 1.f : 0.f), on = anim(hid(key, 30 + i), i == sel ? 1.f : 0.f, 20.f);
        txt_vc(f, a.x + 10, p.y + 16, C(mix(mix(C_MUTED, C_TEXT, ht), C_ON_TX, on)), labels[i], tr);
        if (h && ImGui::IsMouseClicked(0)) hit = i;
    }
    return hit;
}
int seg_icons(const char* key, ImVec2 p, const char* const* ics, int n, int sel) {
    rect(p, {p.x + 6 + n * 30 + (n - 1), p.y + 32}, C(C_SEG_BG), 9);
    float ax = anim(hid(key, 1), 3.f + sel * 31, 20.f);
    rect({p.x + ax, p.y + 3}, {p.x + ax + 30, p.y + 29}, C(C_CHIP_ON), 7);
    int hit = -1;
    for (int i = 0; i < n; i++) {
        ImVec2 a{p.x + 3 + i * 31, p.y + 3}, b{a.x + 30, a.y + 26};
        bool h = i != sel && hot(a, b);
        float ht = anim(hid(key, 10 + i), h ? 1.f : 0.f), on = anim(hid(key, 30 + i), i == sel ? 1.f : 0.f, 20.f);
        icon(ics[i], {a.x + 15, a.y + 13}, C(mix(mix(C_MUTED, C_TEXT, ht), C_ON_TX, on)));
        if (h && ImGui::IsMouseClicked(0)) hit = i;
    }
    return hit;
}

// ── text field ─────────────────────────────────────────────────────────────
bool allow_num(unsigned c) { return (c >= '0' && c <= '9') || c == '.' || c == ',' || c == '-'; }
bool allow_int(unsigned c) { return c >= '0' && c <= '9'; }
bool allow_hex(unsigned c) { return hexv((char)c) >= 0 || c == '#'; }

void edit_set(const char* s) {
    g.edit_len = (int)ImMin(strlen(s), sizeof(g.edit_buf) - 1);
    memcpy(g.edit_buf, s, g.edit_len);
    g.edit_buf[g.edit_len] = 0;
}

// align: 0 left, 1 center, 2 right. true = коммит (Enter / клик мимо), текст в out.
bool field(const char* key, ImVec2 a, ImVec2 b, const char* display, char (&out)[24],
           bool (*allow)(unsigned), int align, bool underline) {
    ImGuiID id = hash(key);
    bool h = hov(a, b), active = g.edit_id == id, committed = false;
    auto commit = [&] { memcpy(out, g.edit_buf, sizeof out); committed = true; g.edit_id = 0; active = false; };
    if (h && ImGui::IsMouseClicked(0) && !active) { g.edit_id = id; edit_set(display); g.edit_fresh = true; active = true; }
    else if (active && ImGui::IsMouseClicked(0) && !h) commit();
    if (active) {
        ImGui::SetNextFrameWantCaptureKeyboard(true);
        for (ImWchar ch : ImGui::GetIO().InputQueueCharacters) {
            if (!allow(ch)) continue;
            if (g.edit_fresh) { g.edit_len = 0; g.edit_fresh = false; }
            if (g.edit_len < 16) { g.edit_buf[g.edit_len++] = (char)ch; g.edit_buf[g.edit_len] = 0; }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            if (g.edit_fresh) { g.edit_len = 0; g.edit_fresh = false; }
            else if (g.edit_len > 0) g.edit_len--;
            g.edit_buf[g.edit_len] = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) commit();
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { g.edit_id = 0; active = false; }
    }
    const char* s = active ? g.edit_buf : display;
    float w = tw(F.mono12, s), cy = underline ? (a.y + b.y - 3) * .5f : (a.y + b.y) * .5f;
    float x = align == 0 ? a.x : align == 1 ? (a.x + b.x - w) * .5f : b.x - w;
    if (active && g.edit_fresh && w > 0) rect({x - 2, cy - 8}, {x + w + 2, cy + 8}, C(C_SELECT), 3);
    txt_vc(F.mono12, x, cy, C(C_TEXT), s);
    if (active && !g.edit_fresh && std::fmod(ImGui::GetTime(), 1.0) < .5)
        g.dl->AddLine({x + w + 1, cy - 7}, {x + w + 1, cy + 7}, C(C_TEXT), 1);
    if (underline) {
        if (active) rect({a.x, b.y - 1.5f}, b, C(C_CHIP_ON), 0);
        else rect({a.x, b.y - 1}, b, C(h ? C_DIM : C_TRACK), 0);
    }
    return committed;
}

// ── slider (52px: подпись 19 + 7 + ряд 26) ────────────────────────────────
bool slider(const char* key, ImVec2 p, float w, const char* label, int& v, int mn, int mx, int step, const char* unit) {
    bool changed = false;
    auto snap = [&](double n) { return ImClamp((int)std::lround(n / step) * step, mn, mx); };
    char buf[48], hint[96], fk[96], out[24];
    txt(F.ub14, {p.x, p.y + 2.5f}, C(C_TEXT), label, -.005f);
    snprintf(hint, sizeof hint, "%d%s \xE2\x80\x93 %d%s", mn, unit, mx, unit);
    txt(F.mono12, {p.x + w - tw(F.mono12, hint), p.y + 4.5f}, C(C_DIM), hint);

    float ry = p.y + 26;
    snprintf(buf, sizeof buf, "%d%s", v, unit);
    snprintf(fk, sizeof fk, "%s#in", key);
    double n;
    if (field(fk, {p.x, ry}, {p.x + 64, ry + 26}, buf, out, allow_num, 1, true) && parse_num(out, n)) {
        int nv = snap(n);
        if (nv != v) { v = nv; changed = true; }
    }

    ImVec2 ta{p.x + 78, ry + 4}, tb{p.x + w, ry + 22};
    ImGuiID id = hash(key);
    if (clk(ta, tb)) g.drag_id = id;
    if (g.drag_id == id) {
        if (ImGui::IsMouseDown(0)) {
            float q = ImClamp((mpos().x - ta.x) / (tb.x - ta.x), 0.f, 1.f);
            int nv = snap(mn + q * (mx - mn));
            if (nv != v) { v = nv; changed = true; }
        } else g.drag_id = 0;
    }
    bool th = hot(ta, tb) || g.drag_id == id;
    float ht = anim(id ^ 0x6B1Du, th ? 1.f : 0.f, 18.f);
    float fp = anim(id ^ 0x2F7Au, (float)(v - mn) / (mx - mn), 24.f);
    float cy = ry + 13, kx = ta.x + (tb.x - ta.x) * fp;
    rect({ta.x, cy - 2}, {tb.x, cy + 2}, C(C_TRACK), 2);
    rect({ta.x, cy - 2}, {kx, cy + 2}, C(C_SLIDER), 2);
    if (ht > 0) circle({kx, cy + 1}, 8 + 1.5f * ht + 1, C(C_SHADOW, ht));
    circle({kx, cy}, 8 + 1.5f * ht, C(C_KNOB));
    return changed;
}

bool toggle_row(float x, float y, float w, const char* label, bool on) {
    ImVec2 a{x, y}, b{x + w, y + 44};
    ImGuiID id = hash(label);
    bool h = hot(a, b);
    float ht = anim(id ^ 0x51u, h ? 1.f : 0.f);
    rect(a, b, C(mix(C_CARD, C_PANEL, ht)), 12);
    radio({x + 23, y + 22}, on, id);
    txt_vc(F.ub14, x + 45, y + 22, C(C_TEXT), label, -.005f);
    return h && ImGui::IsMouseClicked(0);
}

// Заголовок группы 16/700, прижат к низу строки высотой rh.
void group_title(float x, float y, float rh, const char* s) {
    txt(F.ub16, {x, y + rh - 19.2f + 1.6f}, C(C_TEXT), s, -.02f);
}

// ── visuals (ESP) ──────────────────────────────────────────────────────────
bool has_set(int k) { return k == K_box || k == K_armor || k == K_corpses; }

void toggle_open(int vt, int k, int m) {
    if (g.open_vt == vt && g.open_key == k && g.open_mode == m) { g.open_key = -1; g.open_mode = 0; }
    else { g.open_vt = vt; g.open_key = k; g.open_mode = m; anim_set(hid("open", vt, k), 0.f); }
}

float set_panel(int vt, int k, float x, float y, float w) {
    EspItem& it = g.st.esp[vt][k];
    const Tx& t = tx();
    float h = k == K_box ? 126.f : k == K_armor ? 60.f : 80.f;
    rect({x, y}, {x + w, y + h}, C(C_PANEL), 12);
    float ix = x + 14, iw = w - 28, cy = y + 13;
    char key[64];
    if (k == K_box || k == K_armor) {
        const char* o2d[2] = {"2D", "3D"};
        const char* oar[2] = {t.text, t.bar};
        const char* const* opts = k == K_box ? o2d : oar;
        int& sel = k == K_box ? it.style : it.display;
        txt_vc(F.ub14, ix, cy + 16, C(C_TEXT), k == K_box ? t.style : t.display, -.005f);
        char sk[48]; snprintf(sk, sizeof sk, "seg/esp/%d/%d", vt, k);
        int hit = seg(sk, {ix + iw - seg_w(opts, 2, F.ub12b, 0), cy}, opts, 2, sel, F.ub12b, 0);
        if (hit >= 0) sel = hit;
        cy += 32 + 14;
    }
    if (k == K_box) {
        snprintf(key, sizeof key, "esp/%d/%d/dist", vt, k);
        slider(key, {ix, cy}, iw, t.dist, it.dist, 0, 400, 10, " m");
    }
    if (k == K_corpses) {
        snprintf(key, sizeof key, "esp/%d/%d/min", vt, k);
        slider(key, {ix, cy}, iw, t.minVal, it.min_val, 0, 500, 10, " $");
    }
    return h;
}

float col_panel(int vt, int k, float x, float y, float w) {
    EspItem& it = g.st.esp[vt][k];
    constexpr float GAP = 7;
    float ix = x + 14, iw = w - 28;
    float cell = (iw - (PAL_N - 1) * GAP) / PAL_N, d = ImMin(cell, 32.f);
    float h = 13 + d + 14 + 16 + 14 + 32 + 15;
    rect({x, y}, {x + w, y + h}, C(C_PANEL), 12);
    float cy = y + 13;
    char key[64];

    for (int i = 0; i < PAL_N; i++) {
        ImVec2 c{ix + i * (cell + GAP) + cell * .5f, cy + d * .5f};
        float r = d * .5f;
        ImU32 sw = G::ESP_PAL[i];
        bool sel = (sw | IM_COL32_A_MASK) == (it.c | IM_COL32_A_MASK);
        if (sel) { circle(c, r + 4, C(C_CHIP_ON)); circle(c, r + 2, C(C_PANEL)); }
        circle(c, r, C(sw));
        if (!sel) g.dl->AddCircle(c, r - .5f, C(C_SW_LINE), 0, 1);
        if (clk(c - ImVec2(r, r), c + ImVec2(r, r))) it.c = sw;
    }
    cy += d + 14;

    // hue: hsl(h, 60%, 62%)
    {
        ImVec2 a{ix, cy}, b{ix + iw, cy + 16};
        float by = cy + 8;
        circle({ix + 4, by}, 4, C(hsl(0, 60, 62)));
        circle({ix + iw - 4, by}, 4, C(hsl(360, 60, 62)));
        float sx = ix + 4, sw = (iw - 8) / 6.f;
        for (int i = 0; i < 6; i++) {
            ImU32 c0 = C(hsl(i * 60.f, 60, 62)), c1 = C(hsl((i + 1) * 60.f, 60, 62));
            g.dl->AddRectFilledMultiColor({sx + i * sw, by - 4}, {sx + (i + 1) * sw + .5f, by + 4}, c0, c1, c1, c0);
        }
        snprintf(key, sizeof key, "esp/%d/%d/hue", vt, k);
        ImGuiID id = hash(key);
        if (clk(a, b)) g.drag_id = id;
        if (g.drag_id == id) {
            if (ImGui::IsMouseDown(0)) it.c = hsl(ImClamp((mpos().x - ix) / iw, 0.f, 1.f) * 360, 60, 62);
            else g.drag_id = 0;
        }
        ImVec2 kc{ix + iw * hue_of(it.c) / 360.f, by};
        circle(kc + ImVec2(0, 1), 9, C(C_SHADOW));
        circle(kc, 8, C(C_KNOB));
    }
    cy += 16 + 14;

    // HEX · R · G · B  (1.6fr + 3 × 1fr, gap 5)
    float fw = (iw - 15) / 4.6f, hw = fw * 1.6f;
    char out[24];
    {
        ImVec2 a{ix, cy}, b{ix + hw, cy + 32};
        rect(a, b, C(C_CHIP), 9);
        txt_vc(F.ub9, ix + 10, cy + 16, C(C_FAINT), "HEX", .12f);
        float lx = ix + 10 + tw(F.ub9, "HEX", .12f) + 8;
        char hex[16];
        snprintf(hex, sizeof hex, "#%02X%02X%02X", ch_r(it.c), ch_g(it.c), ch_b(it.c));
        snprintf(key, sizeof key, "esp/%d/%d/hex", vt, k);
        if (field(key, {lx, cy}, {b.x - 10, b.y}, hex, out, allow_hex, 0, false)) {
            const char* p = out[0] == '#' ? out + 1 : out;
            int v[6], ok = strlen(p) == 6;
            for (int i = 0; ok && i < 6; i++) ok = (v[i] = hexv(p[i])) >= 0;
            if (ok) it.c = mk(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]);
        }
    }
    const char* chL[3] = {"R", "G", "B"};
    const ImU32 chC[3] = {C_CH_R, C_CH_G, C_CH_B};
    for (int c = 0; c < 3; c++) {
        float bx = ix + hw + 5 + c * (fw + 5);
        ImVec2 a{bx, cy}, b{bx + fw, cy + 32};
        rect(a, b, C(C_CHIP), 9);
        txt_vc(F.mono12, bx + 9, cy + 16, C(chC[c]), chL[c]);
        int rgb[3] = {ch_r(it.c), ch_g(it.c), ch_b(it.c)};
        char val[8];
        snprintf(val, sizeof val, "%d", rgb[c]);
        snprintf(key, sizeof key, "esp/%d/%d/ch%d", vt, k, c);
        float lx = bx + 9 + tw(F.mono12, chL[c]) + 6;
        double n;
        if (field(key, {lx, cy}, {b.x - 9, b.y}, val, out, allow_int, 2, false) && parse_num(out, n)) {
            rgb[c] = ImClamp((int)n, 0, 255);
            it.c = mk(rgb[0], rgb[1], rgb[2]);
        }
    }
    return h;
}

float esp_row(int vt, int k, float x, float y, float w) {
    State& S = g.st;
    EspItem& it = S.esp[vt][k];
    bool dim = k != K_enable && !S.esp[vt][K_enable].on;
    float sa = g.alpha; bool sl = g.live;
    if (dim) { g.alpha *= .45f; g.live = false; }

    bool hasC = it.has_color, hasS = has_set(k);
    int mode = (g.open_vt == vt && g.open_key == k) ? g.open_mode : 0;
    float r = x + w, gx = 0, cx = 0;
    if (hasS) { gx = r - 44; r = gx - 5; }
    if (hasC) { cx = r - 44; r = cx - 5; }

    const int rk = vt * 100 + k;
    bool rh = hot({x, y}, {r, y + 44});
    float rht = anim(hid("row", rk), rh ? 1.f : 0.f);
    rect({x, y}, {r, y + 44}, C(mix(C_CARD, C_PANEL, rht)), 12);
    radio({x + 23, y + 22}, it.on, hid("radio", rk));
    txt_vc(F.ub14, x + 45, y + 22, C(C_TEXT), tx().esp[k], -.005f);
    if (clk({x + 13, y + 12}, {x + 33, y + 32})) it.on = !it.on;
    else if (clk({x + 45, y}, {r, y + 44})) {
        if (hasC || hasS) toggle_open(vt, k, hasC ? 1 : 2);
        else it.on = !it.on;
    }

    // 44px-блок: hover → CHIP, активный → CHIP_ON
    auto block = [&](float bx, bool act, const char* ic, bool fill, int slot) {
        ImVec2 a{bx, y}, b{bx + 44, y + 44}, c{bx + 22, y + 22};
        bool h = hot(a, b);
        float ht = anim(hid("blkh", rk, slot), h ? 1.f : 0.f), at = anim(hid("blka", rk, slot), act ? 1.f : 0.f, 18.f);
        rect(a, b, C(mix(mix(C_PANEL, C_CHIP, ht), C_CHIP_ON, at)), 12);
        if (at < 1) inset(a, b, C(C_LINE, 1 - at), 12);
        if (fill) icon("drop_fill", c, C(it.c));
        icon(ic, c, C(mix(mix(C_MUTED, C_TEXT, ht), C_ON_TX, at)));
        return h && ImGui::IsMouseClicked(0);
    };
    if (hasC && block(cx, mode == 1, "drop", true, 0)) toggle_open(vt, k, 1);
    if (hasS && block(gx, mode == 2, "gear", false, 1)) toggle_open(vt, k, 2);

    mode = (g.open_vt == vt && g.open_key == k) ? g.open_mode : 0;
    float h = 44;
    if (mode) {
        // раскрытие: fade + сдвиг 6px сверху
        float ot = anim(hid("open", vt, k), 1.f, 14.f);
        float pa = g.alpha;
        g.alpha *= ot;
        float py = y + 49 - 6 * (1 - ot);
        h += 5 + (mode == 2 ? set_panel(vt, k, x, py, w) : col_panel(vt, k, x, py, w));
        g.alpha = pa;
    }

    g.alpha = sa; g.live = sl;
    return h;
}

float draw_visuals(float x, float y, float w) {
    int vt = g.st.vt;
    const EspKey* keys = vt == 0 ? P_KEYS : B_KEYS;
    int n = vt == 0 ? P_N : B_N;
    float y0 = y;
    for (int i = 0; i < n; i++) y += esp_row(vt, keys[i], x, y, w) + 5;
    return y - 5 - y0;
}

// ── radar ──────────────────────────────────────────────────────────────────
float draw_radar(float x, float y, float w) {
    Radar& R = g.st.radar;
    const Tx& t = tx();
    float y0 = y;
    group_title(x, y, 30, t.gShow);
    y += 30 + 11;
    bool* vals[5] = {&R.show, &R.players, &R.bots, &R.rings, &R.aim};
    for (int i = 0; i < 5; i++) {
        if (toggle_row(x, y, w, t.rToggles[i], *vals[i])) *vals[i] = !*vals[i];
        y += 44 + 5;
    }
    y += 26 - 5;

    group_title(x, y, 32, t.gSize);
    int hit = seg_icons("seg/radar/corner", {x + w - 129, y}, CORNER_IC, 4, R.corner);
    if (hit >= 0) R.corner = hit;
    y += 32 + 11;

    rect({x, y}, {x + w, y + 148}, C(C_CARD), 12);
    slider("radar/world", {x + 15, y + 13}, w - 30, t.world, R.world, 50, 400, 10, " m");
    slider("radar/screen", {x + 15, y + 81}, w - 30, t.screen, R.screen, 100, 400, 10, " px");
    y += 148;
    return y - y0;
}

// ── loot ───────────────────────────────────────────────────────────────────
float draw_loot(float x, float y, float w) {
    Loot& L = g.st.loot;
    const Tx& t = tx();
    float y0 = y;
    group_title(x, y, 30, t.gFilter);
    y += 30 + 11;
    rect({x, y}, {x + w, y + 80}, C(C_CARD), 12);
    slider("loot/min", {x + 15, y + 13}, w - 30, t.lootMin, L.min_val, 0, 200000, 1000, " $");
    y += 80;
    return y - y0;
}

// ── chrome ─────────────────────────────────────────────────────────────────
void draw_titlebar(float x, float y, float w) {
    ImVec2 a{x, y}, b{x + w, y + 56};
    rect(a, b, C(C_BAR), 12);

    // логотип 20×22 на тёмной плитке 32×32, r10
    float lx = x + 14;
    rect({lx, y + 12}, {lx + 32, y + 44}, C(C_LOGO), 10);
    if (ImTextureID logo = icons::get("logo")) {
        ImVec2 la = snap_px({lx + 6, y + 17});
        g.dl->AddImage(logo, la, la + ImVec2(20, 22));
    }
    txt_vc(F.ub20, lx + 32 + 11, y + 28, C(C_TEXT), "GameBreaker", -.02f);

    const char* langs[2] = {"RU", "EN"};
    float sw = seg_w(langs, 2, F.ub12b, .12f);
    ImVec2 sp{x + w - 14 - sw, y + 12};
    bool over_ui = hov(sp, sp + ImVec2(sw, 32));
    int hit = seg("seg/lang", sp, langs, 2, (int)g.st.lang, F.ub12b, .12f);
    if (hit >= 0) g.st.lang = (Lang)hit;

    // перетаскивание панели за шапку
    ImGuiIO& io = ImGui::GetIO();
    if (!over_ui && clk(a, b)) { g.win_drag = true; g.drag_off = io.MousePos - g.pos; }
    if (g.win_drag) {
        if (ImGui::IsMouseDown(0)) g.pos = io.MousePos - g.drag_off;
        else g.win_drag = false;
    }
}

void draw_sidebar(float x, float y, float w, float h) {
    rect({x, y}, {x + w, y + h}, C(C_BAR), 12);
    const Tx& t = tx();
    const char* ics[3] = {"visuals", "radar", "loot"};
    float cy = y + 10, bar_y = 0;
    for (int i = 0; i < 3; i++) {
        bool on = g.st.sec == i;
        ImVec2 a{x + 10, cy}, b{x + w - 10, cy + 44};
        bool hv = !on && hot(a, b);
        float ht = anim(hid("nav/h", i), hv ? 1.f : 0.f), at = anim(hid("nav/a", i), on ? 1.f : 0.f, 18.f);
        float bg = ImMax(at, ht * .55f);
        if (bg > 0) rect(a, b, C(C_CHIP, bg), 12);
        if (on) bar_y = cy - y;
        ImU32 col = mix(C_DIM, C_TEXT, ImMax(at, ht));
        icon(ics[i], {a.x + 22, cy + 22}, C(col));
        txt_vc(on ? F.ub14b : F.ub14, a.x + 42, cy + 22, C(col), t.secs[i], -.005f);
        if (hv && ImGui::IsMouseClicked(0)) g.st.sec = i;
        cy += 44;

        if (i == 0) {
            // подпункты «Игроки / Боты» выезжают/сворачиваются по высоте
            float sub = anim(hid("nav/sub"), on ? 1.f : 0.f, 14.f);
            if (sub > 0) {
                float sa = g.alpha; bool sl = g.live;
                g.alpha *= sub; g.live = sl && sub > .99f;
                g.dl->PushClipRect({x, cy}, {x + w, cy + 72 * sub}, true);
                for (int v = 0; v < 2; v++) {
                    ImVec2 va{x + 10, cy + v * 36}, vb{x + w - 10, cy + v * 36 + 36};
                    bool son = g.st.vt == v, sh = !son && hot(va, vb);
                    float sht = anim(hid("nav/vh", v), sh ? 1.f : 0.f), sat = anim(hid("nav/va", v), son ? 1.f : 0.f, 18.f);
                    txt_vc(F.ub14, va.x + 42, va.y + 18, C(mix(mix(C_DIM, C_TEXT, sht), C_ACCENT, sat)), t.vts[v], -.005f);
                    if (sh && ImGui::IsMouseClicked(0)) g.st.vt = v;
                }
                g.dl->PopClipRect();
                g.alpha = sa; g.live = sl;
                cy += 72 * sub;
            }
        }
    }
    float by = y + anim(hid("nav/bar"), bar_y, 16.f);
    rect({x, by + 13}, {x + 3, by + 31}, C(C_ACCENT), 2, ImDrawFlags_RoundCornersRight);

    float fy = y + h - 42;
    g.dl->AddLine({x + 10, fy}, {x + w - 10, fy}, C(C_WINDOW), 1);
    float fx = x + 24;
    txt(F.ub12, {fx, fy + 13}, C(C_DIM), "Release ");
    txt(F.mono12, {fx + tw(F.ub12, "Release "), fy + 13}, C(C_DIM), "9.3");
}

constexpr float HEAD_H = 52, HEAD_GAP = 14;   // было 68 / 22

void draw_header(float x, float y, float w) {
    const Tx& t = tx();
    const State& s = g.st;
    const char* title = s.sec == 0 ? t.vts[s.vt] : t.secs[s.sec];
    const char* deco = s.sec == 0 ? (s.vt == 0 ? "PLAYERS" : "BOTS") : s.sec == 1 ? "RADAR" : "LOOT";
    float bottom = y + HEAD_H;
    // декор по вертикальному центру полосы заголовка
    txt(F.ub52, {x + w - 20 - tw(F.ub52, deco, -.04f), y + (HEAD_H - F.ub52.size) * .5f}, C(C_TRACK), deco, -.04f);
    txt(F.ub24, {x, bottom - 26.4f}, C(C_TEXT), title, -.03f);
}

// ── ESP-превью: отдельная панель 420×540 справа от окна, только в «Визуале» ──
// Цель рисует hud::esp_target — одна функция с игрой, читает тот же RenderConfig.
constexpr float PV_W = 420, PV_GAP = 16;

void draw_preview(float px, float py, const RenderConfig& cfg) {
    const bool pmc = g.st.vt == 0;

    rect({px, py}, {px + PV_W, py + g.H}, C(C_WINDOW), 16);
    float ix = px + 9, iy = py + 9, iw = PV_W - 18, ih = g.H - 18;
    rect({ix, iy}, {ix + iw, iy + ih}, C(C_BAR), 12);

    txt_vc(F.ub14, ix + 18, iy + 24, C(C_MUTED), g.st.lang == Lang::ru ? U8("Превью") : "Preview", -.005f);
    {
        const char* tag = pmc ? "PLAYERS" : "BOTS";
        txt_vc(F.ub9, ix + iw - 18 - tw(F.ub9, tag, .12f), iy + 24, C(C_FAINT), tag, .12f);
    }

    // кадр 261×392; фото и рамка — в слое 291×436 (−14/+4)
    // полоса брони справа (≈ 28 px с подписями) — сдвигаем группу, чтобы она стояла по центру
    const bool pv_bar = cfg.show_armor_master && (pmc ? cfg.show_armor : cfg.show_bot_armor) && cfg.armor_display == 2;
    const float fw = 261, fh = 392, fx = ix + (iw - fw) * .5f - (pv_bar ? 14.f : 0.f), fy = iy + 95;   // группа (ник … метры) по центру карточки под шапкой
    const ImVec2 la{fx - 14, fy + 4}, lsz{291, 436};
    g.dl->PushClipRect({fx, fy}, {fx + fw, fy + fh}, true);
    if (s_operator_tex && s_operator_w > 0 && s_operator_h > 0) {
        float k = ImMin(lsz.x / s_operator_w, lsz.y / s_operator_h);
        ImVec2 sz{s_operator_w * k, s_operator_h * k}, a = la + (lsz - sz) * .5f;
        g.dl->AddImage(s_operator_tex, a, a + sz, {0, 0}, {1, 1}, C(IM_COL32_WHITE));
    }
    g.dl->PopClipRect();

    // Цель рисует ТА ЖЕ функция, что и игра (hud::esp_target, esp_style.cpp):
    // размер и детализация считаются от высоты бокса, как в рейде.
    hud::EspCard c;
    c.b0 = {fx + 52, fy + 12};
    c.b1 = {fx + 208, fy + 384};
    c.alpha = g.alpha;
    c.ui = 1;                    // панель сама масштабирует вершины на DPI
    c.preview = true;
    c.pmc = pmc;
    c.box     = pmc ? cfg.show_box_pmc : cfg.show_box_bot;
    c.mode    = pmc ? cfg.box_mode : cfg.box_mode_bot;
    c.corners = pmc ? cfg.box_corners : cfg.box_corners_bot;
    c.c_box   = pmc ? cfg.col_box_pmc : cfg.col_box_bot;
    if (c.mode == 3) {
        // мок-каркас: капсула под углом 20°
        const float offs[4] = {45, 135, 225, 315}, cx = (c.b0.x + c.b1.x) * .5f, rw = (c.b1.x - c.b0.x) * .5f;
        for (int i = 0; i < 8; i++) {
            float ang = (20 + offs[i & 3]) * 3.14159265f / 180.f;
            float yb = i < 4 ? c.b1.y - 8 : c.b0.y + 8;
            c.c3d[i] = {cx + std::cos(ang) * rw, yb + std::sin(ang) * 8};
            c.c3d_ok[i] = true;
        }
        c.has3d = true;
    }
    c.dist_m = 142;
    c.name   = (pmc ? cfg.show_name : cfg.show_bot_name) ? (pmc ? "Nightreaper_07" : "Scav") : nullptr;
    c.team   = pmc && cfg.show_team_id ? 2 : -1;
    c.s_hp   = pmc && cfg.show_hp; c.hp = 445; c.hp_max = 445;
    c.weapon = (pmc ? cfg.show_weapon : cfg.show_bot_weapon) ? (pmc ? "M4A1" : "AKM") : nullptr;
    c.weapon_asset = "AR";
    c.s_ammo = pmc ? cfg.show_ammo : cfg.show_bot_ammo; c.mag_cur = 30; c.mag_max = 30;
    c.s_arm  = cfg.show_armor_master && (pmc ? cfg.show_armor : cfg.show_bot_armor);
    c.armor_display = cfg.armor_display;
    c.helm = 4; c.vest = 5; c.helm_dur = 25.5f; c.vest_dur = 48.f;
    c.s_dist = pmc ? cfg.show_distance : cfg.show_bot_distance;
    c.c_name = pmc ? cfg.col_name_pmc : cfg.col_name_bot;
    c.c_team = cfg.col_team;
    c.c_wpn  = pmc ? cfg.col_weapon_pmc : cfg.col_weapon_bot;
    c.c_ammo = pmc ? cfg.col_ammo_pmc : cfg.col_ammo_bot;
    c.c_dist = pmc ? cfg.col_distance_pmc : cfg.col_distance_bot;
    hud::esp_target(g.dl, c);
}

// ── view-adapter: RenderConfig <-> State ───────────────────────────────────
// pull — один раз при первом кадре (стартовые значения из конфига),
// push — каждый кадр: UI → конфиг. «Включить» — общий гейт вкладки.
bool s_pulled = false;

void pull(const RenderConfig& c) {
    if (s_pulled) return;
    s_pulled = true;
    State& S = g.st;
    S.lang = c.ui_language == 1 ? Lang::ru : Lang::en;
    auto& Pl = S.esp[0];
    Pl[K_enable].on = true;
    Pl[K_box].on = c.show_box_pmc && c.box_mode > 0; Pl[K_box].style = c.box_mode == 3 ? 1 : 0;
    Pl[K_box].dist = (int)c.pmc_range_m;              Pl[K_box].c = c.col_box_pmc;
    Pl[K_corners].on = c.box_corners;
    Pl[K_name].on = c.show_name;       Pl[K_name].c = c.col_name_pmc;
    Pl[K_team].on = c.show_team_id;
    Pl[K_weapon].on = c.show_weapon;   Pl[K_weapon].c = c.col_weapon_pmc;
    Pl[K_ammo].on = c.show_ammo;       Pl[K_ammo].c = c.col_ammo_pmc;
    Pl[K_health].on = c.show_hp;
    Pl[K_armor].on = c.show_armor && c.show_armor_master && c.armor_display > 0;
    Pl[K_armor].display = c.armor_display == 2 ? 1 : 0;
    Pl[K_distance].on = c.show_distance; Pl[K_distance].c = c.col_distance_pmc;
    Pl[K_corpses].on = c.show_corpse;  Pl[K_corpses].min_val = c.pmc_corpse_min_value; Pl[K_corpses].c = c.col_corpses_pmc;
    Pl[K_teammates].on = c.show_mates;
    auto& Bt = S.esp[1];
    Bt[K_enable].on = false;   // боты по умолчанию выключены
    Bt[K_box].on = c.show_box_bot && c.box_mode_bot > 0; Bt[K_box].style = c.box_mode_bot == 3 ? 1 : 0;
    Bt[K_box].dist = (int)c.bot_range_m;                 Bt[K_box].c = c.col_box_bot;
    Bt[K_corners].on = c.box_corners_bot;
    Bt[K_weapon].on = c.show_bot_weapon;   Bt[K_weapon].c = c.col_weapon_bot;
    Bt[K_ammo].on = c.show_bot_ammo;       Bt[K_ammo].c = c.col_ammo_bot;
    Bt[K_distance].on = c.show_bot_distance; Bt[K_distance].c = c.col_distance_bot;
    Bt[K_corpses].on = c.show_bot_corpse;  Bt[K_corpses].min_val = c.bot_corpse_min_value; Bt[K_corpses].c = c.col_corpses_bot;
    // радар: у нас 0 tl, 1 tr, 2 bl, 3 br; в конфиге 0 TR, 1 BR, 2 BL, 3 TL
    static const int from_cfg[4] = {1, 3, 2, 0};
    Radar& R = S.radar;
    R.show = c.show_radar; R.players = c.show_radar_pmc; R.bots = c.show_radar_bots;
    R.rings = c.radar_rings; R.aim = c.radar_aim_dir;
    R.corner = from_cfg[ImClamp(c.radar_position, 0, 3)];
    R.world = (int)c.radar_range_m; R.screen = c.radar_px_radius;
    S.loot.list = c.show_top_loot; S.loot.min_val = c.min_loot_value;
}

void push(RenderConfig& c) {
    const State& S = g.st;
    c.ui_language = S.lang == Lang::ru ? 1 : 0;
    const auto& Pl = S.esp[0];
    auto p = [&](int k) { return Pl[K_enable].on && Pl[k].on; };
    c.show_box_pmc = p(K_box); c.box_mode = Pl[K_box].style == 1 ? 3 : 2;
    c.pmc_range_m = (float)Pl[K_box].dist; c.col_box_pmc = Pl[K_box].c;
    c.box_corners = p(K_corners);
    c.show_name = p(K_name);       c.col_name_pmc = Pl[K_name].c;
    c.show_team_id = p(K_team);
    c.show_weapon = p(K_weapon);   c.col_weapon_pmc = Pl[K_weapon].c;
    c.show_ammo = p(K_ammo);       c.col_ammo_pmc = Pl[K_ammo].c;
    c.show_hp = p(K_health);
    c.show_armor = p(K_armor);
    c.armor_display = p(K_armor) ? (Pl[K_armor].display == 1 ? 2 : 1) : 0;
    c.show_distance = p(K_distance); c.col_distance_pmc = Pl[K_distance].c;
    c.show_corpse = p(K_corpses);  c.pmc_corpse_min_value = Pl[K_corpses].min_val; c.col_corpses_pmc = Pl[K_corpses].c;
    c.show_mates = p(K_teammates);
    const auto& Bt = S.esp[1];
    auto b = [&](int k) { return Bt[K_enable].on && Bt[k].on; };
    c.show_box_bot = b(K_box); c.box_mode_bot = Bt[K_box].style == 1 ? 3 : 2;
    c.bot_range_m = (float)Bt[K_box].dist; c.col_box_bot = Bt[K_box].c;
    c.box_corners_bot = b(K_corners);
    c.show_bot_weapon = b(K_weapon);   c.col_weapon_bot = Bt[K_weapon].c;
    c.show_bot_ammo = b(K_ammo);       c.col_ammo_bot = Bt[K_ammo].c;
    c.show_bot_distance = b(K_distance); c.col_distance_bot = Bt[K_distance].c;
    c.show_bot_corpse = b(K_corpses);  c.bot_corpse_min_value = Bt[K_corpses].min_val; c.col_corpses_bot = Bt[K_corpses].c;
    static const int to_cfg[4] = {3, 0, 2, 1};
    const Radar& R = S.radar;
    c.show_radar = R.show; c.show_radar_pmc = R.players; c.show_radar_bots = R.bots;
    c.radar_rings = R.rings; c.radar_aim_dir = R.aim;
    {
        int new_pos = to_cfg[ImClamp(R.corner, 0, 3)];
        // При смене угла — сброс radar_screen_x/y, чтобы радар прыгнул
        // в выбранный угол (иначе render_radar использует dragged позицию).
        if (new_pos != c.radar_position) {
            c.radar_screen_x = 0.0f;
            c.radar_screen_y = 0.0f;
        }
        c.radar_position = new_pos;
    }
    c.radar_range_m = (float)R.world; c.radar_px_radius = R.screen;
    c.show_top_loot = false;   // Top-loot список удалён — принудительно выключен
    c.min_loot_value = S.loot.min_val;
}

float baked_size(const ImFont* f) {
#if IMGUI_VERSION_NUM >= 19200
    return f->LegacySize;
#else
    return f->FontSize;
#endif
}

ImFont* find_font(const char* name) {
    ImFontAtlas* at = ImGui::GetIO().Fonts;
    for (ImFont* f : at->Fonts)
        if (f && strcmp(f->GetDebugName(), name) == 0) return f;
    return at->Fonts.Size > 0 ? at->Fonts[0] : ImGui::GetFont();
}

void setup_fonts() {
    struct Spec { Font* dst; const char* name; float size; bool mono; };
    const Spec specs[] = {
        {&F.ub24,  "gb:ub700:24", 24, false}, {&F.ub20,  "gb:ub700:20", 20, false},
        {&F.ub16,  "gb:ub700:16", 16, false},
        {&F.ub52,  "gb:ub700:52", 52, false}, {&F.ub14b, "gb:ub700:14", 14, false},
        {&F.ub12b, "gb:ub700:12", 12, false}, {&F.ub9,   "gb:ub700:9",   9, false},
        {&F.ub14,  "gb:ub500:14", 14, false}, {&F.ub12,  "gb:ub400:12", 12, false},
        {&F.mono12, "gb:jb500:12", 12, true},
    };
    // Хост печёт шрифт в (CSS-кегль × em-коэффициент × DPI) — см. HOST_PATCH.md.
    // Логический кегль = запечённый / DPI: после масштаба вершин глиф ложится 1:1.
    for (const Spec& sp : specs) {
        ImFont* f = find_font(sp.name);
        bool own = strcmp(f->GetDebugName(), sp.name) == 0;
        *sp.dst = {f, own ? baked_size(f) / s_text_k : sp.size * (sp.mono ? s_mono_k : 1.f)};
    }

}

} // namespace

// ── public: ровно 4 функции ────────────────────────────────────────────────
void render_control_panel(RenderConfig& cfg) {
    g.S = s_text_k;
    setup_fonts();
    pull(cfg);

    constexpr float PW = 960, PH = 540;   // 16:9

    // Центрируем весь блок (панель + превью) по экрану каждый кадр,
    // так что при появлении/сокрытии превью содержимое едет к центру,
    // а не якорится за левый край панели.
    const float pv_t = anim(hid("pv/show"), g.st.sec == 0 ? 1.f : 0.f, 14.f);
    const float WW = pv_t > 0 ? PW + PV_GAP + PV_W : PW;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    g.pos = vp->Pos + (vp->Size - ImVec2(WW, PH) * g.S) * .5f;
    g.pos = ImVec2(std::floor(g.pos.x), std::floor(g.pos.y));

    ImGui::SetNextWindowPos(g.pos);
    ImGui::SetNextWindowSize(ImVec2(WW, PH) * g.S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##abi_control_panel", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);

    g.dl = ImGui::GetWindowDrawList();
    g.o = g.pos; g.W = PW; g.H = PH;
    ImVec2 o = g.o, e = o + ImVec2(PW, PH);
    g.clip = {o, e};
    g.alpha = 1; g.live = true;

    g.dl->PushClipRect(o, o + ImVec2(WW, PH), false);
    const int vtx0 = g.dl->VtxBuffer.Size;
    const int cmd0 = g.dl->CmdBuffer.Size - 1;

    // Панель сплошная, как в макете. Фоновая текстура — подложка «рабочего стола»
    // за панелью, внутрь окна не рисуется.
    rect(o, e, C(C_WINDOW), 16);
    draw_titlebar(o.x + 9, o.y + 9, PW - 18);

    // Колонка контента: шапка раздела фиксирована, прокручивается только список.
    // Низ списка = низ сайдбара (отступ 9px), скроллбар идёт вдоль списка.
    float colW = PW - 229, colTop = o.y + 65;
    float cx = o.x + 22, cw = colW - 52;              // справа 30px — жёлоб под скролл
    float listTop = colTop + 6 + HEAD_H + HEAD_GAP, listBot = e.y - 9;
    Rc head{{o.x, colTop}, {o.x + colW, listTop}};
    Rc list{{o.x, listTop}, {o.x + colW, listBot}};
    float viewH = listBot - listTop;

    g.clip = head;
    g.dl->PushClipRect(head.a, head.b, true);
    draw_header(cx, colTop + 6, cw);
    g.dl->PopClipRect();

    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0 && Rc{{o.x, colTop}, {o.x + colW, e.y}}.has(mpos())) g.scroll_target -= io.MouseWheel * 48;
    g.scroll_target = ImClamp(g.scroll_target, 0.f, ImMax(0.f, g.content_h - viewH));
    g.scroll += (g.scroll_target - g.scroll) * ImMin(1.f, io.DeltaTime * 18);
    if (std::fabs(g.scroll_target - g.scroll) < .5f) g.scroll = g.scroll_target;

    g.clip = list;
    g.dl->PushClipRect(list.a, list.b, true);
    float cy = listTop - g.scroll;
    float sh = g.st.sec == 0 ? draw_visuals(cx, cy, cw) : g.st.sec == 1 ? draw_radar(cx, cy, cw) : draw_loot(cx, cy, cw);
    g.content_h = sh;
    g.dl->PopClipRect();

    // ── скроллбар вдоль списка: только когда есть что прокручивать; 4px → 6px при наведении
    {
        float maxS = ImMax(0.f, g.content_h - viewH);
        float vis = anim(hid("sb/vis"), maxS > 1 ? 1.f : 0.f, 12.f);
        if (vis > 0) {
            float tx = o.x + colW - 15, ty0 = listTop, ty1 = listBot, tl = ty1 - ty0;
            float thumbH = ImMax(28.f, tl * viewH / ImMax(g.content_h, 1.f));
            float ty = ty0 + (maxS > 0 ? g.scroll / maxS : 0.f) * (tl - thumbH);
            ImVec2 za{tx - 8, ty0}, zb{tx + 8, ty1};
            ImGuiID sid = hid("sb/drag");
            g.clip = {o, e};
            bool zh = maxS > 1 && (hot(za, zb) || g.drag_id == sid);
            if (zh && g.drag_id != sid && ImGui::IsMouseClicked(0)) {
                float my = mpos().y;
                g.sb_off = (my < ty || my > ty + thumbH) ? thumbH * .5f : my - ty;   // клик мимо — прыжок
                g.drag_id = sid;
            }
            if (g.drag_id == sid) {
                if (ImGui::IsMouseDown(0)) {
                    float q = ImClamp((mpos().y - g.sb_off - ty0) / ImMax(tl - thumbH, 1.f), 0.f, 1.f);
                    g.scroll = g.scroll_target = q * maxS;
                    ty = ty0 + q * (tl - thumbH);
                } else g.drag_id = 0;
            }
            float ht = anim(hid("sb/h"), zh ? 1.f : 0.f, 18.f);
            float hw = 2 + ht;
            rect({tx - hw, ty}, {tx + hw, ty + thumbH}, C(mix(C_SCROLL, C_SCR_HOT, ht), vis), hw);
        }
    }

    g.clip = {o, e};
    draw_sidebar(o.x + PW - 229, colTop + 9, 220, PH - 65 - 18);

    push(cfg);   // превью читает конфиг — тот же, что рендер
    if (pv_t > 0) {
        float pa = g.alpha; bool pl = g.live;
        g.alpha = pv_t; g.live = false;
        float px = e.x + PV_GAP - 12 * (1 - pv_t);
        g.clip = {{px, o.y}, {px + PV_W, e.y}};
        draw_preview(px, o.y, cfg);
        g.alpha = pa; g.live = pl;
        g.clip = {o, e};
    }

    // логические px → экранные
    if (g.S != 1.f) {
        for (int i = vtx0; i < g.dl->VtxBuffer.Size; i++) {
            ImVec2& p = g.dl->VtxBuffer[i].pos;
            p = o + (p - o) * g.S;
        }
        for (int i = ImMax(cmd0, 0); i < g.dl->CmdBuffer.Size; i++) {
            ImVec4& r = g.dl->CmdBuffer[i].ClipRect;
            r = ImVec4(o.x + (r.x - o.x) * g.S, o.y + (r.y - o.y) * g.S, o.x + (r.z - o.x) * g.S, o.y + (r.w - o.y) * g.S);
        }
    }
    g.dl->PopClipRect();

    ImGui::End();
    push(cfg);
}

void control_panel_set_operator_texture(ImTextureID tex, int wpx, int hpx) {
    s_operator_tex = tex; s_operator_w = wpx; s_operator_h = hpx;
}

void control_panel_set_typography(float text_k, float mono_k) {
    s_text_k = text_k > 0 ? text_k : 1.f;
    s_mono_k = mono_k > 0 ? mono_k : 1.f;
}

void control_panel_set_background_texture(ImTextureID tex, int wpx, int hpx) {
    s_background_tex = tex; s_background_w = wpx; s_background_h = hpx;
}

} // namespace abi
