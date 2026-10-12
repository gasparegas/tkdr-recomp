// Settings overlay (Dear ImGui), toggled with F1. For now: input configuration.
#include <SDL.h>

#include "../../third_party/imgui/backends/imgui_impl_sdl2.h"
#include "../../third_party/imgui/backends/imgui_impl_sdlrenderer2.h"
#include "../../third_party/imgui/imgui.h"
#include "input.h"

static SDL_Renderer *g_ren;
static bool g_open;
static int g_capture_action = -1;  // action waiting for an input to bind
static bool g_capture_armed;  // wait until everything is released first

extern "C" {

void ui_init(SDL_Window *win, SDL_Renderer *ren) {
    g_ren = ren;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.f;
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);
    input_load();
}

int ui_menu_open(void) { return g_open; }
void ui_set_open(int open) { g_open = open != 0; }

// Returns 1 when the event belongs to the overlay.
int ui_event(SDL_Event *e) {
    if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_F1 && !e->key.repeat) {
        g_open = !g_open;
        g_capture_action = -1;
        return 1;
    }
    if (e->type == SDL_JOYDEVICEADDED) input_device_added(e->jdevice.which);
    if (e->type == SDL_JOYDEVICEREMOVED) input_device_removed(e->jdevice.which);
    if (!g_open) return 0;
    ImGui_ImplSDL2_ProcessEvent(e);
    return 1;
}

uint32_t ui_input_mask(void) { return g_open ? 0 : input_poll(); }

}  // extern "C"

static const char *k_socd_names[] = {"Last input wins", "Neutral", "First input wins", "Up wins (hitbox)", "Off"};

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
    if (!g_open) return;
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("tkdr settings (F1)", nullptr)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Input")) {
                input_tab();
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
    ImGui::Render();
    // HiDPI: ImGui works in window points, the renderer in pixels
    ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    SDL_RenderSetScale(g_ren, scale.x, scale.y);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_ren);
    SDL_RenderSetScale(g_ren, 1.f, 1.f);
}
