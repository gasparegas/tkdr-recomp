// SDL2 frontend: shows the PSP framebuffer, maps the keyboard to buttons.
#include <SDL.h>
#include <stdio.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "gpu/gpu.h"
#include "hle/hle.h"

extern uint32_t g_buttons;

enum {
    PSP_SELECT = 0x0001, PSP_START = 0x0008, PSP_UP = 0x0010, PSP_RIGHT = 0x0020,
    PSP_DOWN = 0x0040, PSP_LEFT = 0x0080, PSP_LTRIGGER = 0x0100, PSP_RTRIGGER = 0x0200,
    PSP_TRIANGLE = 0x1000, PSP_CIRCLE = 0x2000, PSP_CROSS = 0x4000, PSP_SQUARE = 0x8000,
};

static uint32_t key_to_button(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: return PSP_UP;
    case SDLK_DOWN: return PSP_DOWN;
    case SDLK_LEFT: return PSP_LEFT;
    case SDLK_RIGHT: return PSP_RIGHT;
    case SDLK_RETURN: return PSP_START;
    case SDLK_SPACE: return PSP_SELECT;
    case SDLK_x: return PSP_CROSS;      // Tekken: right kick
    case SDLK_c: return PSP_CIRCLE;     // right punch
    case SDLK_z: return PSP_SQUARE;     // left kick... see README for the real layout
    case SDLK_s: return PSP_TRIANGLE;
    case SDLK_q: return PSP_LTRIGGER;
    case SDLK_w: return PSP_RTRIGGER;
    default: return 0;
    }
}

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
static uint32_t frames, last;

void script_tick(void);

static void frame(void) {
    static uint32_t pixels[480 * 272];
    script_tick();
    {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) exit(0);
            if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                uint32_t b = key_to_button(e.key.keysym.sym);
                if (e.type == SDL_KEYDOWN) g_buttons |= b;
                else g_buttons &= ~b;
                if (e.key.keysym.sym == SDLK_ESCAPE) exit(0);
            }
        }
        frontend_scanout(pixels);
        SDL_UpdateTexture(tex, NULL, pixels, 480 * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        frames++;
        if (SDL_GetTicks() - last >= 1000) {
            extern uint32_t g_frames_presented;
            static uint32_t last_presented;
            char title[96];
            snprintf(title, sizeof title, "tkdr - game %u fps (display %u) - %u prims/s",
                     g_frames_presented - last_presented, frames, gpu_stat_prims());
            last_presented = g_frames_presented;
            SDL_SetWindowTitle(win, title);
            frames = 0;
            last = SDL_GetTicks();
        }
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
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return;
    }
    win = SDL_CreateWindow("tkdr", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 544, SDL_WINDOW_RESIZABLE);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 480, 272);
    SDL_RenderSetLogicalSize(ren, 480, 272);
    SDL_AudioSpec want = {.freq = 44100, .format = AUDIO_S16SYS, .channels = 2, .samples = 1024, .callback = audio_cb};
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);
    last = SDL_GetTicks();
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(frame, 0, 1);
#else
    for (;;) frame();
#endif
}
