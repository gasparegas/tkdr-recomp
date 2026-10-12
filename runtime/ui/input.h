// Input mapping: any keyboard, gamepad, arcade stick, hitbox... to PSP buttons.
#pragma once
#include <SDL.h>

#include <string>
#include <vector>

enum Action {
    A_UP, A_DOWN, A_LEFT, A_RIGHT, A_CROSS, A_CIRCLE, A_SQUARE, A_TRIANGLE,
    A_L, A_R, A_START, A_SELECT, A_COUNT
};

enum BindKind {
    B_NONE,
    B_KEY,         // keyboard scancode
    B_PAD_BUTTON,  // SDL_GameController button (standard layout)
    B_PAD_AXIS,    // SDL_GameController axis, dir = +1 / -1
    B_JOY_BUTTON,  // raw joystick button (devices SDL has no mapping for)
    B_JOY_AXIS,    // raw joystick axis, dir = +1 / -1
    B_JOY_HAT,     // raw joystick hat, dir = SDL_HAT_* bit
};

struct Binding {
    BindKind kind = B_NONE;
    int code = 0, dir = 0;
    std::string device;  // "keyboard" or the joystick GUID
};

// What to do when opposite directions are held together (hitbox / mixbox).
enum Socd { SOCD_LAST_WINS, SOCD_NEUTRAL, SOCD_FIRST_WINS, SOCD_UP_WINS, SOCD_OFF };

struct InputConfig {
    std::vector<Binding> binds[A_COUNT];
    std::string active_device = "any";  // "any", "keyboard" or a GUID
    Socd socd_lr = SOCD_LAST_WINS, socd_ud = SOCD_UP_WINS;
    int axis_deadzone = 12000;
};

struct Device {
    std::string guid, name;
    SDL_JoystickID id;
    SDL_Joystick *joy;
    SDL_GameController *pad;  // null when SDL has no standard mapping
};

extern InputConfig g_input;
extern std::vector<Device> g_devices;

const char *action_name(int a);
std::string binding_label(const Binding &b);
void input_defaults();
void input_load();
void input_save();
void input_device_added(int index);
void input_device_removed(SDL_JoystickID id);
uint32_t input_poll();  // PSP button mask with SOCD applied
// Binding capture: returns true and fills `out` once the user presses something.
bool input_capture(Binding *out);
