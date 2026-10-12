#include "input.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void web_persist(void);

InputConfig g_input;
std::vector<Device> g_devices;

static const uint32_t k_psp_bits[A_COUNT] = {
    0x0010, 0x0040, 0x0080, 0x0020, 0x4000, 0x2000, 0x8000, 0x1000, 0x0100, 0x0200, 0x0008, 0x0001,
};
static const char *k_action_names[A_COUNT] = {
    "Up", "Down", "Left", "Right", "Cross", "Circle", "Square", "Triangle", "L", "R", "Start", "Select",
};
static const char *k_action_keys[A_COUNT] = {
    "up", "down", "left", "right", "cross", "circle", "square", "triangle", "l", "r", "start", "select",
};

const char *action_name(int a) { return k_action_names[a]; }

static const char *config_path() { return "ms0/tkdr-input.cfg"; }

std::string binding_label(const Binding &b) {
    char buf[96];
    switch (b.kind) {
    case B_KEY: return SDL_GetScancodeName((SDL_Scancode)b.code);
    case B_PAD_BUTTON: return std::string("Pad ") + SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b.code);
    case B_PAD_AXIS:
        snprintf(buf, sizeof buf, "Pad %s%c", SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)b.code),
                 b.dir > 0 ? '+' : '-');
        return buf;
    case B_JOY_BUTTON: snprintf(buf, sizeof buf, "Button %d", b.code); return buf;
    case B_JOY_AXIS: snprintf(buf, sizeof buf, "Axis %d%c", b.code, b.dir > 0 ? '+' : '-'); return buf;
    case B_JOY_HAT: {
        const char *d = b.dir == SDL_HAT_UP ? "Up" : b.dir == SDL_HAT_DOWN ? "Down" : b.dir == SDL_HAT_LEFT ? "Left" : "Right";
        snprintf(buf, sizeof buf, "Hat %d %s", b.code, d);
        return buf;
    }
    default: return "-";
    }
}

static Binding key(SDL_Scancode sc) { return Binding{B_KEY, (int)sc, 0, "keyboard"}; }
static Binding pad(SDL_GameControllerButton b) { return Binding{B_PAD_BUTTON, (int)b, 0, "*"}; }
static Binding pad_axis(SDL_GameControllerAxis a, int dir) { return Binding{B_PAD_AXIS, (int)a, dir, "*"}; }

// Keyboard layout plus the standard gamepad layout for any mapped controller
// (device "*" = every controller SDL knows the layout of).
void input_defaults() {
    InputConfig c;
    c.binds[A_UP] = {key(SDL_SCANCODE_UP), pad(SDL_CONTROLLER_BUTTON_DPAD_UP), pad_axis(SDL_CONTROLLER_AXIS_LEFTY, -1)};
    c.binds[A_DOWN] = {key(SDL_SCANCODE_DOWN), pad(SDL_CONTROLLER_BUTTON_DPAD_DOWN), pad_axis(SDL_CONTROLLER_AXIS_LEFTY, 1)};
    c.binds[A_LEFT] = {key(SDL_SCANCODE_LEFT), pad(SDL_CONTROLLER_BUTTON_DPAD_LEFT), pad_axis(SDL_CONTROLLER_AXIS_LEFTX, -1)};
    c.binds[A_RIGHT] = {key(SDL_SCANCODE_RIGHT), pad(SDL_CONTROLLER_BUTTON_DPAD_RIGHT), pad_axis(SDL_CONTROLLER_AXIS_LEFTX, 1)};
    c.binds[A_CROSS] = {key(SDL_SCANCODE_X), pad(SDL_CONTROLLER_BUTTON_A)};
    c.binds[A_CIRCLE] = {key(SDL_SCANCODE_C), pad(SDL_CONTROLLER_BUTTON_B)};
    c.binds[A_SQUARE] = {key(SDL_SCANCODE_Z), pad(SDL_CONTROLLER_BUTTON_X)};
    c.binds[A_TRIANGLE] = {key(SDL_SCANCODE_S), pad(SDL_CONTROLLER_BUTTON_Y)};
    c.binds[A_L] = {key(SDL_SCANCODE_Q), pad(SDL_CONTROLLER_BUTTON_LEFTSHOULDER), pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1)};
    c.binds[A_R] = {key(SDL_SCANCODE_W), pad(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1)};
    c.binds[A_START] = {key(SDL_SCANCODE_RETURN), pad(SDL_CONTROLLER_BUTTON_START)};
    c.binds[A_SELECT] = {key(SDL_SCANCODE_SPACE), pad(SDL_CONTROLLER_BUTTON_BACK)};
    g_input = c;
}

// ---------------------------------------------------------------- persistence
// Plain text, one setting per line:  bind <action> <kind> <code> <dir> <device>

void input_save() {
    FILE *f = fopen(config_path(), "w");
    if (!f) return;
    fprintf(f, "device %s\nsocd_lr %d\nsocd_ud %d\ndeadzone %d\n", g_input.active_device.c_str(), g_input.socd_lr,
            g_input.socd_ud, g_input.axis_deadzone);
    for (int a = 0; a < A_COUNT; a++)
        for (const Binding &b : g_input.binds[a])
            fprintf(f, "bind %s %d %d %d %s\n", k_action_keys[a], b.kind, b.code, b.dir, b.device.c_str());
    fclose(f);
    web_persist();
}

void input_load() {
    input_defaults();
    FILE *f = fopen(config_path(), "r");
    if (!f) return;
    InputConfig c;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char word[32], arg[128];
        int kind, code, dir;
        if (sscanf(line, "bind %31s %d %d %d %127s", word, &kind, &code, &dir, arg) == 5) {
            for (int a = 0; a < A_COUNT; a++)
                if (!strcmp(word, k_action_keys[a])) c.binds[a].push_back(Binding{(BindKind)kind, code, dir, arg});
        } else if (sscanf(line, "device %127s", arg) == 1) {
            c.active_device = arg;
        } else if (sscanf(line, "socd_lr %d", &code) == 1) {
            c.socd_lr = (Socd)code;
        } else if (sscanf(line, "socd_ud %d", &code) == 1) {
            c.socd_ud = (Socd)code;
        } else if (sscanf(line, "deadzone %d", &code) == 1) {
            c.axis_deadzone = code;
        }
    }
    fclose(f);
    g_input = c;
}

// ---------------------------------------------------------------- devices

void input_device_added(int index) {
    SDL_Joystick *joy = SDL_JoystickOpen(index);
    if (!joy) return;
    SDL_JoystickID id = SDL_JoystickInstanceID(joy);
    for (const Device &d : g_devices)
        if (d.id == id) return;
    Device d;
    char guid[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), guid, sizeof guid);
    d.guid = guid;
    d.id = id;
    d.joy = joy;
    d.pad = SDL_IsGameController(index) ? SDL_GameControllerOpen(index) : nullptr;
    const char *name = d.pad ? SDL_GameControllerName(d.pad) : SDL_JoystickName(joy);
    d.name = name ? name : "Unknown device";
    g_devices.push_back(d);
}

void input_device_removed(SDL_JoystickID id) {
    for (size_t k = 0; k < g_devices.size(); k++)
        if (g_devices[k].id == id) {
            if (g_devices[k].pad) SDL_GameControllerClose(g_devices[k].pad);
            else SDL_JoystickClose(g_devices[k].joy);
            g_devices.erase(g_devices.begin() + (long)k);
            return;
        }
}

static bool device_allowed(const std::string &guid) {
    const std::string &a = g_input.active_device;
    return a == "any" || a == guid;
}

// Is binding `b` held on device `d`?
static bool held_on(const Binding &b, const Device &d) {
    int dz = g_input.axis_deadzone;
    switch (b.kind) {
    case B_PAD_BUTTON: return d.pad && SDL_GameControllerGetButton(d.pad, (SDL_GameControllerButton)b.code);
    case B_PAD_AXIS: {
        if (!d.pad) return false;
        int v = SDL_GameControllerGetAxis(d.pad, (SDL_GameControllerAxis)b.code);
        return b.dir > 0 ? v > dz : v < -dz;
    }
    case B_JOY_BUTTON: return SDL_JoystickGetButton(d.joy, b.code) != 0;
    case B_JOY_AXIS: {
        int v = SDL_JoystickGetAxis(d.joy, b.code);
        return b.dir > 0 ? v > dz : v < -dz;
    }
    case B_JOY_HAT: return (SDL_JoystickGetHat(d.joy, b.code) & b.dir) != 0;
    default: return false;
    }
}

static bool held(const Binding &b) {
    if (b.kind == B_KEY) {
        if (!device_allowed("keyboard") && g_input.active_device != "any") return false;
        const Uint8 *ks = SDL_GetKeyboardState(nullptr);
        return ks[b.code] != 0;
    }
    for (const Device &d : g_devices) {
        if (!device_allowed(d.guid)) continue;
        if (b.device != "*" && b.device != d.guid) continue;
        if (held_on(b, d)) return true;
    }
    return false;
}

// SOCD resolution for one axis. `first`/`second` are the two opposite
// directions; the order they went down is tracked across calls.
static void socd(Socd mode, bool *a, bool *b, int *order, bool b_is_up) {
    // order: 0 none, 1 a pressed first, 2 b pressed first
    if (!*a && !*b) *order = 0;
    else if (*a && !*b) *order = 1;
    else if (!*a && *b) *order = 2;
    if (!(*a && *b) || mode == SOCD_OFF) return;
    switch (mode) {
    case SOCD_NEUTRAL: *a = *b = false; break;
    case SOCD_LAST_WINS:  // the direction pressed second wins
        if (*order == 1) *a = false; else *b = false;
        break;
    case SOCD_FIRST_WINS:
        if (*order == 1) *b = false; else *a = false;
        break;
    case SOCD_UP_WINS:
        if (b_is_up) *a = false; else *a = *b = false;
        break;
    default: break;
    }
}

uint32_t input_poll() {
    bool on[A_COUNT];
    for (int a = 0; a < A_COUNT; a++) {
        on[a] = false;
        for (const Binding &b : g_input.binds[a])
            if (held(b)) { on[a] = true; break; }
    }
    static int order_lr, order_ud;
    socd(g_input.socd_lr == SOCD_UP_WINS ? SOCD_NEUTRAL : g_input.socd_lr, &on[A_LEFT], &on[A_RIGHT], &order_lr, false);
    socd(g_input.socd_ud, &on[A_DOWN], &on[A_UP], &order_ud, true);
    uint32_t mask = 0;
    for (int a = 0; a < A_COUNT; a++)
        if (on[a]) mask |= k_psp_bits[a];
    return mask;
}

// ---------------------------------------------------------------- capture

bool input_capture(Binding *out) {
    // keyboard
    int n;
    const Uint8 *ks = SDL_GetKeyboardState(&n);
    for (int sc = 0; sc < n; sc++)
        if (ks[sc] && sc != SDL_SCANCODE_ESCAPE) {
            *out = key((SDL_Scancode)sc);
            return true;
        }
    int dz = 20000;  // generous, so resting sticks don't bind themselves
    for (const Device &d : g_devices) {
        if (d.pad) {
            for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
                if (SDL_GameControllerGetButton(d.pad, (SDL_GameControllerButton)b)) {
                    *out = Binding{B_PAD_BUTTON, b, 0, d.guid};
                    return true;
                }
            for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
                int v = SDL_GameControllerGetAxis(d.pad, (SDL_GameControllerAxis)a);
                if (v > dz || v < -dz) {
                    *out = Binding{B_PAD_AXIS, a, v > 0 ? 1 : -1, d.guid};
                    return true;
                }
            }
            continue;
        }
        for (int b = 0; b < SDL_JoystickNumButtons(d.joy); b++)
            if (SDL_JoystickGetButton(d.joy, b)) {
                *out = Binding{B_JOY_BUTTON, b, 0, d.guid};
                return true;
            }
        for (int h = 0; h < SDL_JoystickNumHats(d.joy); h++) {
            int v = SDL_JoystickGetHat(d.joy, h);
            for (int bit : {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT})
                if (v & bit) {
                    *out = Binding{B_JOY_HAT, h, bit, d.guid};
                    return true;
                }
        }
        for (int a = 0; a < SDL_JoystickNumAxes(d.joy); a++) {
            int v = SDL_JoystickGetAxis(d.joy, a);
            if (v > dz || v < -dz) {
                *out = Binding{B_JOY_AXIS, a, v > 0 ? 1 : -1, d.guid};
                return true;
            }
        }
    }
    return false;
}
