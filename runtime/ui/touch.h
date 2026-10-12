// On-screen touch controls (see touch.cpp).
#pragma once
#include <SDL.h>
#include <stdint.h>

struct ImDrawList;

enum TouchMode { TOUCH_AUTO, TOUCH_ON, TOUCH_OFF };
extern int g_touch_mode;      // TouchMode
extern float g_touch_opacity;

extern "C" int touch_visible(void);
extern "C" uint32_t touch_mask(void);  // PSP buttons held on screen
extern "C" void touch_reset(void);
int touch_event(const SDL_Event *e, float window_w, float window_h);  // 1 if handled
void touch_draw(ImDrawList *dl, float window_w, float window_h);
void touch_load();
void touch_save();
