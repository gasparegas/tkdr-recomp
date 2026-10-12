// SDL2 frontend: shows the PSP framebuffer, routes input through the
// configurable mapping (runtime/ui), and draws the settings overlay (F1).
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "gpu/gpu.h"
#include "hle/hle.h"

extern uint32_t g_buttons;

void ui_init(SDL_Window *win, SDL_Renderer *ren);
int ui_event(SDL_Event *e);
uint32_t ui_input_mask(void);
void ui_draw(void);

// Convert the displayed framebuffer to ABGR8888 (byte order R,G,B,A).
void frontend_scanout(uint32_t *out) {
    uint32_t base = g_display.fb_addr, stride = g_display.stride;
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++) {
            uint32_t p;
            if (g_display.format == 3) {
                p = mem_r32(base + (uint32_t)(y * stride + x) * 4) | 0xFF000000u;
            } else {
                uint32_t v = mem_r16(base + (uint32_t)(y * stride + x) * 2), r, g, b;
                if (g_display.format == 0) {
                    r = (v & 31) << 3; g = ((v >> 5) & 63) << 2; b = ((v >> 11) & 31) << 3;
                } else if (g_display.format == 1) {
                    r = (v & 31) << 3; g = ((v >> 5) & 31) << 3; b = ((v >> 10) & 31) << 3;
                } else {
                    r = (v & 15) << 4; g = ((v >> 4) & 15) << 4; b = ((v >> 8) & 15) << 4;
                }
                p = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
            out[y * 480 + x] = p;
        }
}

void audio_pull(int16_t *out, int frames);

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    audio_pull((int16_t *)stream, len / 4);
}

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tex_scale;  // size of `tex`: 480x272 times this
static uint32_t frames, last;

void script_tick(void);

static void frame(void) {
    static uint32_t pixels[480 * 4 * 272 * 4];
    script_tick();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) exit(0);
        if (ui_event(&e)) continue;
#ifndef __EMSCRIPTEN__
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) exit(0);
#endif
    }
    if (!getenv("INPUT")) g_buttons = ui_input_mask();

    int scale = gpu_scanout_scaled(g_display.fb_addr, g_display.stride, (uint32_t)g_display.format, pixels, 4);
    if (!scale) {
        frontend_scanout(pixels);
        scale = 1;
    }
    if (scale != tex_scale) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 480 * scale, 272 * scale);
        SDL_SetTextureScaleMode(tex, scale > 1 ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        tex_scale = scale;
    }
    SDL_UpdateTexture(tex, NULL, pixels, 480 * scale * 4);
    // letterbox the 480x272 picture into the window
    int ww, wh;
    SDL_GetRendererOutputSize(ren, &ww, &wh);
    SDL_Rect dst = {0, 0, ww, ww * 272 / 480};
    if (dst.h > wh) dst = (SDL_Rect){0, 0, wh * 480 / 272, wh};
    dst.x = (ww - dst.w) / 2;
    dst.y = (wh - dst.h) / 2;
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, &dst);
    ui_draw();
    SDL_RenderPresent(ren);

    frames++;
    if (SDL_GetTicks() - last >= 1000) {
        extern uint32_t g_frames_presented;
        static uint32_t last_presented;
        char title[96];
        snprintf(title, sizeof title, "tkdr - game %u fps (display %u) - F1 settings", g_frames_presented - last_presented,
                 frames);
        last_presented = g_frames_presented;
        SDL_SetWindowTitle(win, title);
        frames = 0;
        last = SDL_GetTicks();
    }
}

#ifdef __EMSCRIPTEN__
// Debug hooks callable from the page: present a frame now (works even when
// requestAnimationFrame is paused) and read the guest frame counter.
EMSCRIPTEN_KEEPALIVE void web_present(void) { frame(); }
EMSCRIPTEN_KEEPALIVE double web_time(void) { return (double)hle_time_us() / 1e6; }
EMSCRIPTEN_KEEPALIVE uint32_t web_frames(void) {
    extern uint32_t g_frames_presented;
    return g_frames_presented;
}
#endif

void frontend_run(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (getenv("UI_PREVIEW")) SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
    win = SDL_CreateWindow("tkdr", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 544,
                           SDL_WINDOW_RESIZABLE | (getenv("UI_PREVIEW") ? 0 : SDL_WINDOW_ALLOW_HIGHDPI));
    ren = SDL_CreateRenderer(win, -1, getenv("UI_PREVIEW") ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_PRESENTVSYNC);
    ui_init(win, ren);
    SDL_AudioSpec want = {.freq = 44100, .format = AUDIO_S16SYS, .channels = 2, .samples = 1024, .callback = audio_cb};
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);
    last = SDL_GetTicks();
#ifndef __EMSCRIPTEN__
    // UI_PREVIEW=file.bmp: render game + settings overlay offscreen after a few seconds
    const char *preview = getenv("UI_PREVIEW");
    if (preview) {
        void ui_set_open(int open);
        SDL_Delay(1000 * (getenv("UI_PREVIEW_DELAY") ? (Uint32)atoi(getenv("UI_PREVIEW_DELAY")) : 5));
        ui_set_open(1);
        for (int i = 0; i < 4; i++) frame();
        int w, h;
        SDL_GetRendererOutputSize(ren, &w, &h);
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
        SDL_SaveBMP(s, preview);
        exit(0);
    }
#endif
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(frame, 0, 1);
#else
    for (;;) frame();
#endif
}
