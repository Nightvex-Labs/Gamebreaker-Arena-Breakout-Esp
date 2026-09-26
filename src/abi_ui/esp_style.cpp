// esp_style.cpp — ESP-карточка цели в стиле Gamebreaker.
// Одна функция для игры и превью. Цвета — только palette.hpp, шрифты — gb:* из атласа.
//
// Масштаб — от видимой высоты бокса (она уже учитывает дистанцию, FOV и прицел):
//   s = 1.00 … 1.15, плавно (smoothstep) между высотой 40 и 320 px, × ui.
//   Вдали текст НЕ уменьшается ниже читаемого (1.0), вблизи — потолок 1.15.
// Детализация по той же высоте — ник и дистанция видны ВСЕГДА:
//   любой   — бокс, ник + команда, дистанция;
//   ≥ 30 px — + оружие;
//   ≥ 40 px — + полоса брони;
//   ≥ 60 px — + HP, патроны, броня текстом, подписи у полосы.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "overlay_hud.hpp"
#include "palette.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace abi::hud {
namespace {
namespace P = abi::pal;
namespace G = abi::pal::gb;

struct EF { ImFont* f = nullptr; };

EF find(const char* a, const char* b) {
    ImFontAtlas* at = ImGui::GetIO().Fonts;
    for (const char* n : {a, b}) {
        if (!n) continue;
        for (ImFont* f : at->Fonts)
            if (f && std::strcmp(f->GetDebugName(), n) == 0) return {f};
    }
    ImGuiIO& io = ImGui::GetIO();
    return {io.FontDefault ? io.FontDefault : (at->Fonts.Size ? at->Fonts[0] : ImGui::GetFont())};
}
struct Fonts { EF name, pill, label, mono; ImFontAtlas* at = nullptr; } F;
void fonts() {
    ImFontAtlas* at = ImGui::GetIO().Fonts;
    if (F.at == at && F.name.f) return;
    F.at    = at;
    F.name  = find("gb:ub700:16", "gb:ub700:14");   // ник
    F.pill  = find("gb:ub700:12", "gb:ub700:9");    // TEAM, THERMAL, DEAD
    F.label = find("gb:ub500:14", nullptr);         // оружие, KNOCKED
    F.mono  = find("gb:jb500:14", "gb:jb500:12");   // HP, патроны, броня, дистанция
}

ImU32 A(ImU32 c, float a) {
    unsigned al = (c >> IM_COL32_A_SHIFT) & 0xFF;
    al = (unsigned)ImClamp((int)(al * a + .5f), 0, 255);
    return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}
float alpha_of(ImU32 c) { return ((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.f; }

// высота от верха строки до базовой линии для кегля sz
float asc(const EF& f, float sz) {
#if IMGUI_VERSION_NUM >= 19200
    const float baked = f.f->LegacySize;
#else
    const float baked = f.f->FontSize;
#endif
    return baked > 0 ? f.f->Ascent * sz / baked : sz * .8f;
}
float tw(const EF& f, float sz, const char* s) { return f.f->CalcTextSizeA(sz, FLT_MAX, 0, s).x; }
// текст с тёмным контуром 1 px + тенью вниз — читается на любом фоне игры
// превью в меню (фон спокойный): мягкая тень 1 px; игра: контур со всех сторон
bool g_outline = true;
void text(ImDrawList* dl, const EF& f, float sz, ImVec2 p, ImU32 c, const char* s, float a) {
    p = ImVec2(std::floor(p.x + .5f), std::floor(p.y + .5f));
    if (g_outline) {
        const ImU32 o = A(P::blk(.72f), a * alpha_of(c));
        static const ImVec2 off[5] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {1, 2}};
        for (const ImVec2& d : off) dl->AddText(f.f, sz, p + d, o, s);
    } else {
        dl->AddText(f.f, sz, p + ImVec2(1, 1), A(P::blk(.8f), a * alpha_of(c)), s);
    }
    dl->AddText(f.f, sz, p, A(c, a), s);
}

struct Seg { const EF* f; float sz; ImU32 c; char s[48]; };

ImU32 tier_col(int t) { return P::ARMOR_TIER[ImClamp(t, 1, 6) - 1]; }

}  // namespace

void esp_target(ImDrawList* dl, const EspCard& c) {
    fonts();
    const float bh = c.b1.y - c.b0.y, bw = c.b1.x - c.b0.x, cx = (c.b0.x + c.b1.x) * .5f;
    if (bh < 1 || bw < 1) return;
    float t = ImClamp((bh - 40.f) / 280.f, 0.f, 1.f);
    t = t * t * (3 - 2 * t);
    const float s = (1.00f + 0.15f * t) * c.ui;
    const bool far_ = bh < 30;                 // только ник + дистанция
    const bool full = bh >= 60;                // все подписи
    const int lod = far_ ? 0 : full ? 2 : 1;
    const float a = c.alpha * (c.dead ? .6f : 1.f);
    // кегли: в игре крупнее (читаемость на фоне сцены), в превью — компактнее
    g_outline = !c.preview;
    const float TXT = (c.preview ? 11.f : 12.f) * s;     // ник, команда, оружие
    const float NUM = (c.preview ? 11.5f : 13.f) * s;    // HP, патроны, броня, дистанция

    // ── бокс
    if (c.box) {
        const float th = ImClamp(1.1f + bh / 360.f, 1.1f, 2.f) * c.ui;
        const ImU32 sh = A(P::blk(.6f), a * alpha_of(c.c_box)), col = A(c.c_box, a);
        auto line = [&](ImVec2 p0, ImVec2 p1) {
            dl->AddLine(p0, p1, sh, th + 1.6f);
            dl->AddLine(p0, p1, col, th);
        };
        auto ell = [&](ImVec2 p0, ImVec2 p1, ImVec2 p2) {
            ImVec2 q[3] = {p0, p1, p2};
            dl->AddPolyline(q, 3, sh, 0, th + 1.6f);
            dl->AddPolyline(q, 3, col, 0, th);
        };
        const float x0 = c.b0.x, y0 = c.b0.y, x1 = c.b1.x, y1 = c.b1.y;
        if (c.mode == 3 && c.has3d) {
            static const int E[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
            for (auto& e : E) {
                if (!c.c3d_ok[e[0]] || !c.c3d_ok[e[1]]) continue;
                ImVec2 p = c.c3d[e[0]], q = c.c3d[e[1]];
                if (c.corners) { line(p, p + (q - p) * .25f); line(q, q + (p - q) * .25f); }
                else line(p, q);
            }
        } else if (c.corners) {
            const float lx = ImMax(bw * .22f, 5.f * c.ui), ly = ImMax(bh * .18f, 5.f * c.ui);
            ell({x0, y0 + ly}, {x0, y0}, {x0 + lx, y0});
            ell({x1 - lx, y0}, {x1, y0}, {x1, y0 + ly});
            ell({x1, y1 - ly}, {x1, y1}, {x1 - lx, y1});
            ell({x0 + lx, y1}, {x0, y1}, {x0, y1 - ly});
        } else {
            dl->AddRect(c.b0, c.b1, sh, 0, 0, th + 1.6f);
            dl->AddRect(c.b0, c.b1, col, 0, 0, th);
        }
    }

    // ── полоса брони справа от бокса: шлем сверху, бронежилет снизу
    if (c.s_arm && c.armor_display == 2 && bh >= 40) {
        const float w = ImClamp(bh * .016f, 3.f, 6.f) * c.ui, x = c.b1.x + 4 * s, gap = 2 * c.ui;
        const float half = (bh - gap) * .5f, r = w * .5f;
        const int tiers[2] = {c.helm, c.vest};
        const char* tag[2] = {"H", "V"};
        for (int i = 0; i < 2; i++) {
            ImVec2 p0{x, c.b0.y + i * (half + gap)}, p1{x + w, p0.y + half};
            dl->AddRectFilled(p0 - ImVec2(1, 1), p1 + ImVec2(1, 1), A(P::blk(.55f), a), r + 1);
            if (tiers[i] > 0) dl->AddRectFilled(p0, p1, A(tier_col(tiers[i]), a), r);
            if (lod == 2 && tiers[i] > 0) {
                char b[8]; std::snprintf(b, sizeof b, "%s%d", tag[i], tiers[i]);
                const float sz = 10.f * s;
                text(dl, F.mono, sz, {x + w + 4 * s, (p0.y + p1.y - sz) * .5f}, tier_col(tiers[i]), b, a);
            }
        }
    }

    if (!c.labels) return;

    // ── над боксом (снизу вверх): HP → ник + команда → THERMAL
    float y = c.b0.y - 5 * s;
    if (lod == 2 && (c.knocked || (c.pmc && c.s_hp))) {
        char b[24]; ImU32 col; const EF* f = &F.mono; float sz = NUM;
        if (c.knocked) { std::snprintf(b, sizeof b, "KNOCKED"); col = P::AMBER; f = &F.label; sz = 11.f * s; }
        else {
            if (c.hp >= 0) std::snprintf(b, sizeof b, "%d/%d", c.hp, c.hp_max);
            else           std::snprintf(b, sizeof b, "-/%d", c.hp_max);
            float fr = c.hp_max > 0 && c.hp >= 0 ? (float)c.hp / c.hp_max : 1.f;
            col = fr > .66f ? P::POS : fr > .33f ? P::AMBER : P::NEG;
        }
        y -= sz * 1.2f;
        text(dl, *f, sz, {cx - tw(*f, sz, b) * .5f, y}, col, b, a);
    }
    // outline = false — просто текст (TEAM), true — пилюля с заливкой и обводкой (THERMAL)
    auto pill = [&](float x, float py, float h, const char* s_, ImU32 col, bool outline) {
        const float sz = 9.f * s, pad = outline ? 5 * s : 0, pw = tw(F.pill, sz, s_) + pad * 2;
        ImVec2 p0{std::floor(x) + .5f, std::floor(py) + .5f}, p1{p0.x + pw, p0.y + h};
        if (outline) {
            dl->AddRectFilled(p0, p1, A(col, a * .18f), 4 * s);
            dl->AddRect(p0 + ImVec2(1, 1), p1 + ImVec2(1, 1), A(P::blk(.6f), a), 4 * s, 0, 1);
            dl->AddRect(p0, p1, A(col, a), 4 * s, 0, 1);
        }
        text(dl, F.pill, sz, {p0.x + pad, p0.y + (h - sz) * .5f}, col, s_, a);
        return pw;
    };
    // ник + команда: один шрифт и кегль (Unbounded 700, 11 × s), общая базовая линия
    if (c.name || c.team >= 0) {
        const float nsz = TXT, tsz = nsz, rh = nsz * 1.3f;
        char tb[16] = "";
        if (c.team >= 0) std::snprintf(tb, sizeof tb, "Team %d", c.team);
        const float nw = c.name ? tw(F.name, nsz, c.name) : 0;
        const float pw = c.team >= 0 ? tw(F.name, tsz, tb) : 0;
        const float gp = c.name && c.team >= 0 ? 6 * s : 0;
        y -= rh + 2 * s;
        const float base = y + (rh - nsz) * .5f + asc(F.name, nsz);   // общая базовая линия
        float x = cx - (nw + gp + pw) * .5f;
        if (c.name) { text(dl, F.name, nsz, {x, base - asc(F.name, nsz)}, c.c_name, c.name, a); x += nw + gp; }
        if (c.team >= 0) text(dl, F.name, tsz, {x, base - asc(F.name, tsz)}, c.c_team, tb, a);
    }
    if (c.thermal) {
        const float ph = 15.f * s, pw = tw(F.pill, 9.f * s, "THERMAL") + 10 * s;
        y -= ph + 3 * s;
        pill(cx - pw * .5f, y, ph, "THERMAL", P::FOCUS, true);
    }

    // ── под боксом: оружие · патроны · броня · дистанция
    Seg sg[8]; int n = 0;
    auto add = [&](const EF* f, float sz, ImU32 col, const char* s_) {
        Seg& g = sg[n++]; g.f = f; g.sz = sz; g.c = col;
        std::snprintf(g.s, sizeof g.s, "%s", s_);
    };
    char b[32];
    if (lod >= 1 && c.weapon && !c.dead) add(&F.name, TXT, c.c_wpn, c.weapon);   // как ник и команда
    if (lod == 2 && c.s_ammo && !c.dead) {
        if (c.mag_cur >= 0 && c.mag_max > 0) std::snprintf(b, sizeof b, "%d/%d", c.mag_cur, c.mag_max);
        else if (c.mag_cur >= 0)             std::snprintf(b, sizeof b, "%d", c.mag_cur);
        else                                 std::snprintf(b, sizeof b, "-");
        add(&F.mono, NUM, c.mag_cur == 0 ? P::NEG : c.c_ammo, b);
    }
    if (lod == 2 && c.s_arm && c.armor_display == 1) {
        if (c.helm > 0) { std::snprintf(b, sizeof b, "H%d", c.helm); add(&F.mono, NUM, tier_col(c.helm), b); }
        if (c.vest > 0) { std::snprintf(b, sizeof b, "V%d", c.vest); add(&F.mono, NUM, tier_col(c.vest), b); }
    }
    if (c.s_dist) { std::snprintf(b, sizeof b, "%d m", (int)(c.dist_m + .5f)); add(&F.mono, NUM, c.c_dist, b); }
    if (c.dead) add(&F.pill, 9.f * s, G::TEXT_DIM, "DEAD");
    if (n) {
        // все сегменты стоят на одной базовой линии (Unbounded и Mono разной высоты)
        const float gap = 10 * s;
        float total = 0, top_asc = 0;
        for (int i = 0; i < n; i++) {
            total += tw(*sg[i].f, sg[i].sz, sg[i].s) + (i ? gap : 0);
            top_asc = ImMax(top_asc, asc(*sg[i].f, sg[i].sz));
        }
        float x = cx - total * .5f;
        const float base = c.b1.y + 5 * s + top_asc;
        for (int i = 0; i < n; i++) {
            if (i) x += gap;
            text(dl, *sg[i].f, sg[i].sz, {x, base - asc(*sg[i].f, sg[i].sz)}, sg[i].c, sg[i].s, a);
            x += tw(*sg[i].f, sg[i].sz, sg[i].s);
        }
    }
}

}  // namespace abi::hud
