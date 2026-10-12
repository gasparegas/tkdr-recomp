// On-screen touch controls for phones, tablets and touch laptops, drawn with
// Dear ImGui's draw lists over the game. Multi-touch: the d-pad keeps the
// finger that started on it (8-way), fingers can slide across face buttons.
#include "touch.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../third_party/imgui/imgui.h"

extern "C" void web_persist(void);
extern "C" void ui_set_open(int open);

int g_touch_mode = TOUCH_AUTO;
float g_touch_opacity = 0.45f;
static bool g_seen_touch;  // a finger touched the screen at least once

enum { B_UP = 0x10, B_DOWN = 0x40, B_LEFT = 0x80, B_RIGHT = 0x20, B_CROSS = 0x4000, B_CIRCLE = 0x2000,
       B_SQUARE = 0x8000, B_TRIANGLE = 0x1000, B_L = 0x100, B_R = 0x200, B_START = 0x8, B_SELECT = 0x1 };

enum CtlKind { C_DPAD, C_ROUND, C_PILL, C_MENU };
struct Ctl {
    CtlKind kind;
    float x, y, w, h;  // center; radius in w for round controls
    uint32_t bits;
};
enum { CTL_DPAD, CTL_TRI, CTL_CIR, CTL_CRO, CTL_SQU, CTL_L, CTL_R, CTL_SEL, CTL_STA, CTL_MENU, CTL_COUNT };
static Ctl g_ctl[CTL_COUNT];
static float g_w, g_h;  // window size in points

struct Finger {
    SDL_FingerID id;
    float x, y;
    int ctl;  // control it is on, -1 none
};
static Finger g_fingers[10];
static int g_nfingers;

static void layout(float w, float h) {
    g_w = w;
    g_h = h;
    float dr, br, sp, cy, lx, ly, sy, my;
    float dx, fx;
    if (w >= h) {  // landscape: controls over the sides of the picture
        float u = h;
        dr = 0.17f * u; br = 0.075f * u; sp = 0.115f * u;
        dx = 0.06f * w + dr; fx = w - 0.06f * w - sp - br;
        cy = 0.66f * h;
        lx = 0.07f * w; ly = 0.10f * h; sy = 0.93f * h; my = 0.06f * h;
    } else {  // portrait: the game sits at the top, controls below
        float game_h = w * 272.f / 480.f, top = game_h, area = h - game_h, u = fminf(w, area * 1.4f);
        dr = 0.19f * u; br = 0.085f * u; sp = 0.13f * u;  // the two clusters share the width
        dx = 0.06f * w + dr; fx = w - 0.06f * w - sp - br;
        cy = top + 0.55f * area;
        lx = 0.12f * w; ly = top + 0.12f * area; sy = top + 0.93f * area; my = top + 0.12f * area;
    }
    g_ctl[CTL_DPAD] = {C_DPAD, dx, cy, dr, dr, 0};
    g_ctl[CTL_TRI] = {C_ROUND, fx, cy - sp, br, br, B_TRIANGLE};
    g_ctl[CTL_CRO] = {C_ROUND, fx, cy + sp, br, br, B_CROSS};
    g_ctl[CTL_SQU] = {C_ROUND, fx - sp, cy, br, br, B_SQUARE};
    g_ctl[CTL_CIR] = {C_ROUND, fx + sp, cy, br, br, B_CIRCLE};
    float pw = 1.6f * br, ph = 0.7f * br;
    g_ctl[CTL_L] = {C_PILL, lx + pw * 0.5f, ly, pw, ph, B_L};
    g_ctl[CTL_R] = {C_PILL, w - lx - pw * 0.5f, ly, pw, ph, B_R};
    g_ctl[CTL_SEL] = {C_PILL, w * 0.5f - pw * 0.65f, sy, pw, ph * 0.8f, B_SELECT};
    g_ctl[CTL_STA] = {C_PILL, w * 0.5f + pw * 0.65f, sy, pw, ph * 0.8f, B_START};
    g_ctl[CTL_MENU] = {C_MENU, w * 0.5f, my, ph * 0.7f, ph * 0.7f, 0};
}

static int hit(float x, float y) {
    for (int k = 0; k < CTL_COUNT; k++) {
        const Ctl &c = g_ctl[k];
        float dx = x - c.x, dy = y - c.y;
        switch (c.kind) {
        case C_DPAD:
            if (dx * dx + dy * dy <= 1.3f * c.w * 1.3f * c.w) return k;
            break;
        case C_ROUND:  // generous: neighbours are close, the nearest wins below
            break;
        case C_PILL:
        case C_MENU:
            if (fabsf(dx) <= c.w * 0.65f + 6 && fabsf(dy) <= c.h * 0.8f + 6) return k;
            break;
        }
    }
    int best = -1;
    float bd = 0;
    for (int k = CTL_TRI; k <= CTL_SQU; k++) {
        const Ctl &c = g_ctl[k];
        float d = (x - c.x) * (x - c.x) + (y - c.y) * (y - c.y);
        if (d <= 1.45f * c.w * 1.45f * c.w && (best < 0 || d < bd)) {
            best = k;
            bd = d;
        }
    }
    return best;
}

static uint32_t dpad_bits(float x, float y) {
    const Ctl &c = g_ctl[CTL_DPAD];
    float dx = x - c.x, dy = y - c.y;
    if (dx * dx + dy * dy < 0.18f * c.w * 0.18f * c.w) return 0;
    float a = atan2f(-dy, dx) * 180.f / 3.14159265f;  // 0 = right, 90 = up
    if (a < 0) a += 360.f;
    int sector = (int)((a + 22.5f) / 45.f) % 8;
    static const uint32_t dirs[8] = {B_RIGHT, B_RIGHT | B_UP, B_UP, B_UP | B_LEFT,
                                     B_LEFT, B_LEFT | B_DOWN, B_DOWN, B_DOWN | B_RIGHT};
    return dirs[sector];
}

extern "C" int touch_visible(void) {
    if (g_touch_mode == TOUCH_ON) return 1;
    if (g_touch_mode == TOUCH_OFF) return 0;
    return g_seen_touch || getenv("TOUCH") != NULL;
}

static Finger *finger(SDL_FingerID id, bool create) {
    for (int k = 0; k < g_nfingers; k++)
        if (g_fingers[k].id == id) return &g_fingers[k];
    if (!create || g_nfingers == 10) return nullptr;
    g_fingers[g_nfingers] = {id, 0, 0, -1};
    return &g_fingers[g_nfingers++];
}

static void release(SDL_FingerID id) {
    for (int k = 0; k < g_nfingers; k++)
        if (g_fingers[k].id == id) {
            g_fingers[k] = g_fingers[--g_nfingers];
            return;
        }
}

static void press(SDL_FingerID id, float x, float y) {
    Finger *f = finger(id, true);
    if (!f) return;
    f->x = x;
    f->y = y;
    f->ctl = hit(x, y);
    if (f->ctl == CTL_MENU) {
        ui_set_open(1);
        g_nfingers = 0;
    }
}

static void move(SDL_FingerID id, float x, float y) {
    Finger *f = finger(id, false);
    if (!f) return;
    f->x = x;
    f->y = y;
    if (f->ctl != CTL_DPAD) f->ctl = hit(x, y);  // slide between buttons; the d-pad keeps its finger
    if (f->ctl == CTL_MENU) f->ctl = -1;
}

// Returns 1 if the event was a touch control event.
int touch_event(const SDL_Event *e, float ww, float wh) {
    if (ww != g_w || wh != g_h) layout(ww, wh);
    switch (e->type) {
    case SDL_FINGERDOWN:
        g_seen_touch = true;
        press(e->tfinger.fingerId, e->tfinger.x * ww, e->tfinger.y * wh);
        return 1;
    case SDL_FINGERMOTION:
        move(e->tfinger.fingerId, e->tfinger.x * ww, e->tfinger.y * wh);
        return 1;
    case SDL_FINGERUP:
        release(e->tfinger.fingerId);
        return 1;
    // the mouse acts as one finger (testing on desktop; touch laptops); SDL's own
    // mouse events synthesized from touches are ignored
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.which == SDL_TOUCH_MOUSEID || e->button.button != SDL_BUTTON_LEFT || !touch_visible()) return 0;
        press(-1, (float)e->button.x, (float)e->button.y);
        return 1;
    case SDL_MOUSEMOTION:
        if (e->motion.which == SDL_TOUCH_MOUSEID || !(e->motion.state & SDL_BUTTON_LMASK)) return 0;
        move(-1, (float)e->motion.x, (float)e->motion.y);
        return 1;
    case SDL_MOUSEBUTTONUP:
        if (e->button.which == SDL_TOUCH_MOUSEID || e->button.button != SDL_BUTTON_LEFT) return 0;
        release(-1);
        return 1;
    }
    return 0;
}

extern "C" uint32_t touch_mask(void) {
    uint32_t m = 0;
    for (int k = 0; k < g_nfingers; k++) {
        const Finger &f = g_fingers[k];
        if (f.ctl == CTL_DPAD) m |= dpad_bits(f.x, f.y);
        else if (f.ctl >= 0) m |= g_ctl[f.ctl].bits;
    }
    return m;
}

extern "C" void touch_reset(void) { g_nfingers = 0; }

static ImU32 col(int r, int g, int b, float a) { return IM_COL32(r, g, b, (int)(a * 255.f)); }

void touch_draw(ImDrawList *dl, float w, float h) {
    layout(w, h);
    uint32_t mask = touch_mask();
    float a = g_touch_opacity;
    // d-pad
    const Ctl &d = g_ctl[CTL_DPAD];
    dl->AddCircleFilled(ImVec2(d.x, d.y), d.w, col(20, 20, 24, a * 0.6f), 48);
    dl->AddCircle(ImVec2(d.x, d.y), d.w, col(255, 255, 255, a * 0.5f), 48, 2.f);
    struct { uint32_t bit; float dx, dy; } arms[4] = {{B_UP, 0, -1}, {B_DOWN, 0, 1}, {B_LEFT, -1, 0}, {B_RIGHT, 1, 0}};
    for (auto &arm : arms) {
        bool on = (mask & arm.bit) != 0;
        float cx = d.x + arm.dx * d.w * 0.58f, cy = d.y + arm.dy * d.w * 0.58f, s = d.w * 0.22f;
        // arrow head pointing outwards
        ImVec2 tip(cx + arm.dx * s, cy + arm.dy * s), b1(cx - arm.dx * s * 0.6f + arm.dy * s, cy - arm.dy * s * 0.6f + arm.dx * s),
            b2(cx - arm.dx * s * 0.6f - arm.dy * s, cy - arm.dy * s * 0.6f - arm.dx * s);
        dl->AddTriangleFilled(tip, b1, b2, col(255, 255, 255, on ? 0.95f : a));
    }
    // face buttons in PlayStation colours, drawn as their symbols
    struct { int ctl; int r, g, b; } face[4] = {{CTL_TRI, 64, 226, 160}, {CTL_CIR, 255, 102, 102},
                                                {CTL_CRO, 124, 178, 232}, {CTL_SQU, 255, 105, 248}};
    for (auto &f : face) {
        const Ctl &c = g_ctl[f.ctl];
        bool on = (mask & c.bits) != 0;
        ImVec2 p(c.x, c.y);
        dl->AddCircleFilled(p, c.w, on ? col(f.r, f.g, f.b, 0.55f) : col(20, 20, 24, a * 0.6f), 40);
        dl->AddCircle(p, c.w, col(255, 255, 255, a * 0.5f), 40, 2.f);
        ImU32 sc = col(f.r, f.g, f.b, on ? 1.f : fminf(1.f, a + 0.35f));
        float s = c.w * 0.45f, t = fmaxf(2.f, c.w * 0.09f);
        switch (f.ctl) {
        case CTL_TRI: dl->AddTriangle(ImVec2(p.x, p.y - s), ImVec2(p.x + s * 0.95f, p.y + s * 0.7f),
                                      ImVec2(p.x - s * 0.95f, p.y + s * 0.7f), sc, t); break;
        case CTL_CIR: dl->AddCircle(p, s * 0.9f, sc, 32, t); break;
        case CTL_CRO:
            dl->AddLine(ImVec2(p.x - s * 0.75f, p.y - s * 0.75f), ImVec2(p.x + s * 0.75f, p.y + s * 0.75f), sc, t);
            dl->AddLine(ImVec2(p.x - s * 0.75f, p.y + s * 0.75f), ImVec2(p.x + s * 0.75f, p.y - s * 0.75f), sc, t);
            break;
        default: dl->AddRect(ImVec2(p.x - s * 0.75f, p.y - s * 0.75f), ImVec2(p.x + s * 0.75f, p.y + s * 0.75f), sc, 0, 0, t);
        }
    }
    // shoulder and system buttons
    struct { int ctl; const char *label; } pills[4] = {{CTL_L, "L"}, {CTL_R, "R"}, {CTL_SEL, "SELECT"}, {CTL_STA, "START"}};
    for (auto &pl : pills) {
        const Ctl &c = g_ctl[pl.ctl];
        bool on = (mask & c.bits) != 0;
        ImVec2 a0(c.x - c.w * 0.5f, c.y - c.h * 0.5f), a1(c.x + c.w * 0.5f, c.y + c.h * 0.5f);
        dl->AddRectFilled(a0, a1, on ? col(255, 255, 255, 0.5f) : col(20, 20, 24, a * 0.6f), c.h * 0.5f);
        dl->AddRect(a0, a1, col(255, 255, 255, a * 0.5f), c.h * 0.5f, 0, 2.f);
        ImVec2 ts = ImGui::CalcTextSize(pl.label);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col(255, 255, 255, on ? 1.f : fminf(1.f, a + 0.3f)),
                    pl.label);
    }
    // settings
    const Ctl &m = g_ctl[CTL_MENU];
    for (int i = -1; i <= 1; i++)
        dl->AddLine(ImVec2(m.x - m.w, m.y + i * m.h * 0.55f), ImVec2(m.x + m.w, m.y + i * m.h * 0.55f),
                    col(255, 255, 255, fminf(1.f, a + 0.2f)), 2.f);
}

// ms0/tkdr-touch.cfg: "mode=N opacity=F"
void touch_load() {
    FILE *f = fopen("ms0/tkdr-touch.cfg", "r");
    if (!f) return;
    int m;
    float o;
    if (fscanf(f, "mode=%d opacity=%f", &m, &o) == 2) {
        g_touch_mode = m;
        g_touch_opacity = o;
    }
    fclose(f);
}

void touch_save() {
    FILE *f = fopen("ms0/tkdr-touch.cfg", "w");
    if (!f) return;
    fprintf(f, "mode=%d opacity=%.2f\n", g_touch_mode, g_touch_opacity);
    fclose(f);
    web_persist();
}
