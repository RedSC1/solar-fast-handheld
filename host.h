/* ==================================================================
 * host.h —— 宿主后端接口
 *
 * 后端只做两件事：把 160x128 的 RGB565 缓冲显示出来，把键盘映射成
 * 8 个逻辑按钮。所有「画什么」都在 sim_frame.c 里，两条后端共用 ——
 * 否则 Cocoa 和 SDL 各画一套面板，迟早长得不一样。
 * ================================================================== */
#ifndef SIM_HOST_H
#define SIM_HOST_H

#include <stdint.h>
#include "ui.h"

/* 按键面板的高度，单位是**屏幕像素**（还须乘放大倍数）。
 * 8 个按钮排成 2x4，每个按钮两行字（功能名 + 键位），下面一行帮助。 */
#define SIM_PANEL_UNIT 80

/* 窗口总高（屏幕像素） */
#define SIM_WIN_H_UNIT (UI_H + SIM_PANEL_UNIT)

typedef enum {
    SIM_EV_NONE = 0,
    SIM_EV_BUTTON,   /* id = ui_btn, val = 1 按下 / 0 抬起 */
    SIM_EV_ZOOM,     /* val = +1 / -1 */
    SIM_EV_TIME,     /* val = 日偏移（±1 天） */
    SIM_EV_RESET,    /* 回到现在 */
    SIM_EV_QUIT
} sim_ev_kind;

typedef struct { int kind, id, val; } sim_ev;

typedef struct {
    const char *name;
    int  (*init)(const char *title, int scale);
    void (*shutdown)(void);
    /* 把屏幕缓冲交出去显示。pressed 是 8 个按钮的位掩码（bit i = UI_BTN_i）。 */
    void (*present)(const uint16_t *lcd, uint32_t pressed);
    /* 取一个事件。返回 0 = 没有。 */
    int  (*poll)(sim_ev *ev);
    /* 整数放大倍数调整（-1 / +1）。窗口会跟着改大小。 */
    void (*zoom)(int delta);
    int  (*scale)(void);
} sim_host;

extern const sim_host sim_host_cocoa;
#if defined(SIM_HAVE_SDL)
extern const sim_host sim_host_sdl;
#endif

/* ------------------------------------------------------------------
 * 帧合成（两条后端共用）
 *
 * 把 160x128 的屏幕缓冲按 scale 最近邻放大，下面接一条按键面板，
 * 一起写进 win（win 是 ww x wh 的 RGB565）。
 * ------------------------------------------------------------------ */
int  sim_frame_width(int scale);
int  sim_frame_height(int scale);
void sim_frame_compose(uint16_t *win, int scale, const uint16_t *lcd,
                       uint32_t pressed);

#endif /* SIM_HOST_H */
