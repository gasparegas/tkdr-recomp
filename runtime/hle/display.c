// sceDisplay, sceGe_user (list bookkeeping), sceCtrl, sceUmdUser, sceRtc.
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "hle.h"
#include "../gpu/gpu.h"

// ---------------------------------------------------------------- display

DisplayState g_display = {.fb_addr = 0x04000000u, .stride = 512, .format = 3};

void sceDisplaySetMode(Ctx *c) { RET(0); }

uint32_t g_frames_presented;

void sceDisplaySetFrameBuf(Ctx *c) {
    g_frames_presented++;
    gpu_tex_invalidate();
    if (ARG(0)) {
        g_display.fb_addr = ARG(0);
        g_display.stride = ARG(1);
        g_display.format = ARG(2);
    }
    RET(0);
}

void sceDisplayWaitVblankStart(Ctx *c) { hle_wait_vblank(c, 0); }
void sceDisplayWaitVblankStartCB(Ctx *c) { hle_wait_vblank(c, 1); }
void sceDisplayGetVcount(Ctx *c) { RET((uint32_t)(hle_time_us() / 16683u)); }

// ---------------------------------------------------------------- GE

void sceGeEdramGetAddr(Ctx *c) { RET(0x04000000u); }
void sceGeEdramGetSize(Ctx *c) { RET(0x00200000u); }

void sceGeSetCallback(Ctx *c) {
    gpu_set_callbacks(mem_r32(ARG(0)), mem_r32(ARG(0) + 4), mem_r32(ARG(0) + 8),
                      mem_r32(ARG(0) + 12));
    RET(1);
}

void sceGeListEnQueue(Ctx *c) { RET(gpu_enqueue(c, ARG(0), ARG(1), (int)ARG(2), ARG(3), 0)); }
void sceGeListEnQueueHead(Ctx *c) { RET(gpu_enqueue(c, ARG(0), ARG(1), (int)ARG(2), ARG(3), 1)); }
void sceGeListUpdateStallAddr(Ctx *c) { RET(gpu_update_stall(c, (int)ARG(0), ARG(1))); }
void sceGeListSync(Ctx *c) { RET(gpu_sync(c, (int)ARG(0), (int)ARG(1))); }
void sceGeDrawSync(Ctx *c) { RET(gpu_sync(c, -1, (int)ARG(0))); }

// ---------------------------------------------------------------- controller

uint32_t g_buttons;  // PSP button bits, set by the frontend
uint8_t g_analog_x = 128, g_analog_y = 128;

void sceCtrlSetSamplingCycle(Ctx *c) { RET(0); }
void sceCtrlSetSamplingMode(Ctx *c) { RET(0); }

void sceCtrlReadBufferPositive(Ctx *c) {
    uint32_t p = ARG(0), n = ARG(1) ? ARG(1) : 1;
    for (uint32_t k = 0; k < n; k++, p += 16) {
        memset(host(p), 0, 16);
        mem_w32(p, (uint32_t)hle_time_us());
        mem_w32(p + 4, g_buttons);
        mem_w8(p + 8, g_analog_x);
        mem_w8(p + 9, g_analog_y);
    }
    RET(n);
}

// ---------------------------------------------------------------- UMD

void sceUmdCheckMedium(Ctx *c) { RET(1); }
void sceUmdActivate(Ctx *c) { RET(0); }
void sceUmdDeactivate(Ctx *c) { RET(0); }
void sceUmdWaitDriveStatCB(Ctx *c) { RET(0); }
void sceUmdRegisterUMDCallBack(Ctx *c) { RET(0); }

// ---------------------------------------------------------------- RTC
// Ticks are microseconds since 0001-01-01.

#define RTC_UNIX_EPOCH 62135596800000000ull

static uint64_t now_tick(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return RTC_UNIX_EPOCH + (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

static void write_dt(uint32_t p, uint64_t tick, int local) {
    uint64_t us = tick - RTC_UNIX_EPOCH;
    time_t t = (time_t)(us / 1000000u);
    struct tm tm;
    if (local) localtime_r(&t, &tm);
    else gmtime_r(&t, &tm);
    mem_w16(p, (uint16_t)(tm.tm_year + 1900));
    mem_w16(p + 2, (uint16_t)(tm.tm_mon + 1));
    mem_w16(p + 4, (uint16_t)tm.tm_mday);
    mem_w16(p + 6, (uint16_t)tm.tm_hour);
    mem_w16(p + 8, (uint16_t)tm.tm_min);
    mem_w16(p + 10, (uint16_t)tm.tm_sec);
    mem_w32(p + 12, (uint32_t)(us % 1000000u));
}

static uint64_t read_dt(uint32_t p) {
    struct tm tm = {0};
    tm.tm_year = mem_r16(p) - 1900;
    tm.tm_mon = mem_r16(p + 2) - 1;
    tm.tm_mday = mem_r16(p + 4);
    tm.tm_hour = mem_r16(p + 6);
    tm.tm_min = mem_r16(p + 8);
    tm.tm_sec = mem_r16(p + 10);
    return RTC_UNIX_EPOCH + (uint64_t)timegm(&tm) * 1000000u + mem_r32(p + 12);
}

static void w64(uint32_t p, uint64_t v) {
    mem_w32(p, (uint32_t)v);
    mem_w32(p + 4, (uint32_t)(v >> 32));
}
static uint64_t r64(uint32_t p) { return mem_r32(p) | ((uint64_t)mem_r32(p + 4) << 32); }

void sceRtcGetCurrentTick(Ctx *c) { w64(ARG(0), now_tick()); RET(0); }
void sceRtcGetCurrentClock(Ctx *c) {
    write_dt(ARG(0), now_tick() + (uint64_t)(int64_t)(int32_t)ARG(1) * 60000000ll, 0);
    RET(0);
}
void sceRtcGetCurrentClockLocalTime(Ctx *c) { write_dt(ARG(0), now_tick(), 1); RET(0); }
void sceRtcGetTick(Ctx *c) { w64(ARG(1), read_dt(ARG(0))); RET(0); }
void sceRtcSetTick(Ctx *c) { write_dt(ARG(0), r64(ARG(1)), 0); RET(0); }
void sceRtcGetTime_t(Ctx *c) {
    mem_w32(ARG(1), (uint32_t)((read_dt(ARG(0)) - RTC_UNIX_EPOCH) / 1000000u));
    RET(0);
}
void sceRtcSetTime_t(Ctx *c) {
    write_dt(ARG(0), RTC_UNIX_EPOCH + (uint64_t)ARG(1) * 1000000u, 0);
    RET(0);
}
void sceRtcConvertUtcToLocalTime(Ctx *c) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    w64(ARG(1), r64(ARG(0)) + (uint64_t)(int64_t)lt.tm_gmtoff * 1000000);
    RET(0);
}
