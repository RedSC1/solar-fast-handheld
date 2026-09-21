/* ==================================================================
 * sim_frame.c —— 窗口画面的合成（屏幕 + 按键面板）
 *
 * 两条后端共用。这样换后端时面板长得一样，也省得把「K1 是哪个键」这种
 * 事实写两遍、改一处忘一处。
 * ================================================================== */
#include <string.h>

#include "host.h"
#include "ui.h"

/* 面板配色：固定深灰，模拟硬件外壳，不跟日夜主题走 —— 面板是「设备」，
 * 屏幕内容才是「界面」。 */
#define PC_BEZEL   UI_RGB(34, 38, 46)
#define PC_FACE    UI_RGB(62, 70, 84)
#define PC_EDGE    UI_RGB(96, 106, 124)
#define PC_TEXT    UI_RGB(206, 214, 228)
#define PC_DIM     UI_RGB(126, 136, 156)
#define PC_DOWN    UI_RGB(240, 176, 72)
#define PC_DOWN_TX UI_RGB(26, 22, 12)

/* 每个按钮的键位提示。第一行是功能名，第二行是模拟器的键。
 * ⚠️ 按钮只有 38 个屏幕像素宽、而半角字是 7 像素，所以**最多 5 个字符**。
 * 这里刻意不写 "Enter" 而写 "Ret"、不写箭头 —— 宽度是硬约束，不是偷懒。 */
static const char *const KEY_HINT[UI_BTN_COUNT] = {
    "1 w", "2 s", "3 a", "4 d", "5 Ret", "6 Esc", "7 Tab", "8 z"
};

int sim_frame_width(int scale)  { return UI_W * scale; }
int sim_frame_height(int scale) { return (UI_H + SIM_PANEL_UNIT) * scale; }

static void blit_lcd(uint16_t *win, int scale, const uint16_t *lcd)
{
    const int ww = UI_W * scale;
    for (int y = 0; y < UI_H * scale; y++) {
        const uint16_t *src = lcd + (y / scale) * UI_W;
        uint16_t *dst = win + (size_t)y * ww;
        for (int x = 0; x < ww; x++) dst[x] = src[x / scale];
    }
}

static void draw_button(uint16_t *win, int ww, int wh, int scale,
                        int bx, int by, int bw, int bh, int idx, int down)
{
    /* 坐标都是「屏幕像素」，画之前统一乘 scale */
    const int x = bx * scale, y = by * scale;
    const int w = bw * scale, h = bh * scale;

    for (int yy = y; yy < y + h && yy < wh; yy++) {
        if (yy < 0) continue;
        uint16_t *row = win + (size_t)yy * ww;
        for (int xx = x; xx < x + w && xx < ww; xx++) {
            if (xx < 0) continue;
            const int edge = (xx < x + scale) || (xx >= x + w - scale) ||
                             (yy < y + scale) || (yy >= y + h - scale);
            row[xx] = down ? PC_DOWN : (edge ? PC_EDGE : PC_FACE);
        }
    }

    const uint16_t fg = down ? PC_DOWN_TX : PC_TEXT;
    const uint16_t fg2 = down ? PC_DOWN_TX : PC_DIM;
    const struct ui_face *f = ui_font_s.latin;

    /* 第一行：功能名。第二行：键位提示。 */
    const char *line1 = ui_btn_names[idx];
    const char *line2 = KEY_HINT[idx];

    int tw = ui_canvas_ascii_w(scale, f, line1);
    ui_canvas_ascii(win, ww, wh, x + (w - tw) / 2, y + 2 * scale, scale, f, fg, line1);

    tw = ui_canvas_ascii_w(scale, f, line2);
    ui_canvas_ascii(win, ww, wh, x + (w - tw) / 2, y + 16 * scale, scale, f, fg2, line2);
}

void sim_frame_compose(uint16_t *win, int scale, const uint16_t *lcd,
                       uint32_t pressed)
{
    const int ww = sim_frame_width(scale);
    const int wh = sim_frame_height(scale);

    for (int i = 0; i < ww * wh; i++) win[i] = PC_BEZEL;
    blit_lcd(win, scale, lcd);

    /* --- 2x4 按钮，和模块的丝印排布一致 --- */
    const int bw = 38, bh = 28, gap = 2;
    const int x0 = (UI_W - (4 * bw + 3 * gap)) / 2;
    const int y0 = UI_H + 5;
    for (int i = 0; i < UI_BTN_COUNT; i++) {
        const int col = i % 4, row = i / 4;
        draw_button(win, ww, wh, scale, x0 + col * (bw + gap),
                    y0 + row * (bh + gap), bw, bh, i,
                    (pressed >> i) & 1u);
    }

    /* --- 帮助行。整行只有 22 个半角字宽（160/7），写不下更多了；
     *     `\` 复位时间那条挪进了 sim/README.md。 --- */
    {
        const char *help = "[ ] time   -/= zoom";
        const int tw = ui_canvas_ascii_w(scale, ui_font_s.latin, help);
        ui_canvas_ascii(win, ww, wh, (ww - tw) / 2, (UI_H + 66) * scale,
                        scale, ui_font_s.latin, PC_DIM, help);
    }
}
