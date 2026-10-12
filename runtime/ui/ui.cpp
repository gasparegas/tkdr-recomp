// Settings overlay (Dear ImGui), toggled with F1. For now: input configuration.
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../third_party/imgui/backends/imgui_impl_sdl2.h"
#include "../../third_party/imgui/backends/imgui_impl_sdlrenderer2.h"
#include "../../third_party/imgui/imgui.h"
#include "input.h"
#include "touch.h"

static SDL_Renderer *g_ren;
static SDL_Window *g_win;
static bool g_open;
static int g_capture_action = -1;  // action waiting for an input to bind
static bool g_capture_armed;  // wait until everything is released first
static bool g_show_fps;  // F2
extern "C" int g_fps_game, g_fps_display;
static void video_load();

extern "C" {

void ui_init(SDL_Window *win, SDL_Renderer *ren) {
    g_ren = ren;
    g_win = win;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.f;
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);
    input_load();
    video_load();
    touch_load();
}

int ui_menu_open(void) { return g_open; }
void ui_set_open(int open) {
    g_open = open != 0;
    touch_reset();
}

// Returns 1 when the event belongs to the overlay.
int ui_event(SDL_Event *e) {
    if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_F2 && !e->key.repeat) {
        g_show_fps = !g_show_fps;
        return 1;
    }
    if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_F1 && !e->key.repeat) {
        g_open = !g_open;
        g_capture_action = -1;
        return 1;
    }
    if (e->type == SDL_JOYDEVICEADDED) input_device_added(e->jdevice.which);
    if (e->type == SDL_JOYDEVICEREMOVED) input_device_removed(e->jdevice.which);
    if (!g_open) {
        int w, h;
        SDL_GetWindowSize(g_win, &w, &h);
        return touch_event(e, (float)w, (float)h);
    }
    ImGui_ImplSDL2_ProcessEvent(e);
    return 1;
}

uint32_t ui_input_mask(void) { return g_open ? 0 : input_poll() | touch_mask(); }

}  // extern "C"

static const char *k_socd_names[] = {"Last input wins", "Neutral", "First input wins", "Up wins (hitbox)", "Off"};

extern "C" {
void gpu_set_render_scale(int n);
int gpu_render_scale(void);
void web_persist(void);
void texrep_request_reload(void);
void texrep_request_dump(int on);
void texrep_status(int *replacements, int *dumping, const char **dir);
}

// ms0/tkdr-video.cfg: "scale=N"
static void video_load() {
    if (getenv("RENDER_SCALE")) return;  // the command line wins
    FILE *f = fopen("ms0/tkdr-video.cfg", "r");
    if (!f) return;
    int n;
    if (fscanf(f, "scale=%d", &n) == 1) gpu_set_render_scale(n);
    fclose(f);
}

static void video_save(int scale) {
    FILE *f = fopen("ms0/tkdr-video.cfg", "w");
    if (!f) return;
    fprintf(f, "scale=%d\n", scale);
    fclose(f);
    web_persist();
}

static void video_tab() {
    int cur = gpu_render_scale();
    ImGui::Text("Internal resolution");
    for (int n = 1; n <= 4; n++) {
        char label[48];
        snprintf(label, sizeof label, "%dx  (%dx%d)%s", n, 480 * n, 272 * n, n == 1 ? "  PSP" : "");
        if (ImGui::RadioButton(label, cur == n)) {
            gpu_set_render_scale(n);
            video_save(n);
        }
    }
    ImGui::TextDisabled("Higher scales need a faster CPU (software renderer).");
}

static void textures_tab() {
    int n, dumping;
    const char *dir;
    texrep_status(&n, &dumping, &dir);
    ImGui::TextWrapped("Texture packs use the PPSSPP format: put textures.ini and the PNGs in");
    ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f), "%s/", dir);
    ImGui::Text("%d replacement(s) loaded", n);
    if (ImGui::Button("Reload pack")) texrep_request_reload();
    bool d = dumping != 0;
    if (ImGui::Checkbox("Dump new textures", &d)) texrep_request_dump(d);
    ImGui::TextDisabled("Dumped textures go to %s/new/ named by hash.", dir);
    ImGui::TextDisabled("Move edited ones (any size) up one folder to use them.");
}

static void input_tab() {
    // device selection
    std::string label = "Any device";
    if (g_input.active_device == "keyboard") label = "Keyboard only";
    for (const Device &d : g_devices)
        if (d.guid == g_input.active_device) label = d.name;
    if (ImGui::BeginCombo("Input device", label.c_str())) {
        if (ImGui::Selectable("Any device", g_input.active_device == "any")) g_input.active_device = "any";
        if (ImGui::Selectable("Keyboard only", g_input.active_device == "keyboard")) g_input.active_device = "keyboard";
        for (const Device &d : g_devices) {
            std::string item = d.name + (d.pad ? "  (gamepad)" : "  (raw: stick / hitbox)") + "##" + d.guid;
            if (ImGui::Selectable(item.c_str(), g_input.active_device == d.guid)) g_input.active_device = d.guid;
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("%d controller(s) connected", (int)g_devices.size());

    int lr = g_input.socd_lr, ud = g_input.socd_ud;
    if (ImGui::Combo("SOCD Left+Right", &lr, k_socd_names, 3)) g_input.socd_lr = (Socd)lr;
    if (ImGui::Combo("SOCD Up+Down", &ud, k_socd_names, 5)) g_input.socd_ud = (Socd)ud;
    ImGui::SliderInt("Stick deadzone", &g_input.axis_deadzone, 2000, 30000);
    static const char *k_touch_modes[] = {"Auto (on first touch)", "Always", "Never"};
    if (ImGui::Combo("Touch controls", &g_touch_mode, k_touch_modes, 3)) touch_save();
    if (ImGui::SliderFloat("Touch opacity", &g_touch_opacity, 0.1f, 1.f, "%.2f")) touch_save();

    // live test: what the game would receive right now
    uint32_t mask = input_poll();
    static const uint32_t bits[A_COUNT] = {0x10, 0x40, 0x80, 0x20, 0x4000, 0x2000, 0x8000, 0x1000, 0x100, 0x200, 0x8, 0x1};
    ImGui::Text("Test:");
    for (int a = 0; a < A_COUNT; a++) {
        ImGui::SameLine();
        bool on = (mask & bits[a]) != 0;
        if (on) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.85f, 0.2f, 1.f));
        ImGui::TextUnformatted(action_name(a));
        if (on) ImGui::PopStyleColor();
    }
    ImGui::Separator();

    if (ImGui::BeginTable("binds", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Bindings (click to remove)");
        ImGui::TableHeadersRow();
        for (int a = 0; a < A_COUNT; a++) {
            ImGui::PushID(a);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(action_name(a));
            ImGui::TableNextColumn();
            auto &v = g_input.binds[a];
            for (size_t k = 0; k < v.size(); k++) {
                ImGui::PushID((int)k);
                std::string text = binding_label(v[k]);
                if (v[k].device != "keyboard" && v[k].device != "*")
                    for (const Device &d : g_devices)
                        if (d.guid == v[k].device) text += " [" + d.name.substr(0, 14) + "]";
                if (ImGui::SmallButton(text.c_str())) {
                    v.erase(v.begin() + (long)k);
                    input_save();
                    ImGui::PopID();
                    break;
                }
                ImGui::SameLine();
                ImGui::PopID();
            }
            if (g_capture_action == a) ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f), "press a key/button... (Esc cancels)");
            else if (ImGui::SmallButton("+")) {
                g_capture_action = a;
                g_capture_armed = false;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // capture: wait for a clean release, then take the next input
    if (g_capture_action >= 0) {
        Binding b;
        bool pressed = input_capture(&b);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_capture_action = -1;
        else if (!g_capture_armed) g_capture_armed = !pressed && !ImGui::IsMouseDown(0);
        else if (pressed) {
            g_input.binds[g_capture_action].push_back(b);
            g_capture_action = -1;
            input_save();
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Reset to defaults")) {
        input_defaults();
        input_save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear all")) {
        for (auto &v : g_input.binds) v.clear();
        input_save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) input_save();
}

extern "C" void ui_draw(void) {
    bool touch = !g_open && touch_visible();
    if (!g_open && !touch && !g_show_fps) return;
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (touch) {
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        touch_draw(ImGui::GetBackgroundDrawList(), ds.x, ds.y);
    }
    if (g_show_fps) {  // game frames / display frames in the last second
        char t[48];
        snprintf(t, sizeof t, "%d fps  (display %d)", g_fps_game, g_fps_display);
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        ImVec2 sz = ImGui::CalcTextSize(t);
        dl->AddRectFilled(ImVec2(6, 6), ImVec2(14 + sz.x, 12 + sz.y), IM_COL32(0, 0, 0, 160), 4.f);
        dl->AddText(ImVec2(10, 9), g_fps_game >= 55 ? IM_COL32(120, 255, 120, 255) : IM_COL32(255, 200, 80, 255), t);
    }
    // fits small screens (phones) too
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float mw = ds.x - 16.f < 560.f ? ds.x - 16.f : 560.f, mh = ds.y - 16.f < 460.f ? ds.y - 16.f : 460.f;
    ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ds.x < 600.f ? 8.f : 20.f, ds.y < 500.f ? 8.f : 20.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(200.f, 150.f), ImVec2(ds.x - 8.f, ds.y - 8.f));
    if (g_open) touch_reset();  // fingers lifted over the menu never reached the controls
    if (g_open) {
    if (ImGui::Begin("tkdr settings (F1)", &g_open)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Input")) {
                input_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Video")) {
                video_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Textures")) {
                textures_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("About")) {
                ImGui::TextWrapped("Static recompilation of Tekken: Dark Resurrection. Runs from your own dump only.");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    }
    ImGui::Render();
    // HiDPI: ImGui works in window points, the renderer in pixels
    ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    SDL_RenderSetScale(g_ren, scale.x, scale.y);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_ren);
    SDL_RenderSetScale(g_ren, 1.f, 1.f);
}
