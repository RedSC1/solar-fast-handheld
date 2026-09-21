/* ==================================================================
 * ui_gfx.c —— 帧缓冲 + 绘制原语 + 点阵排版
 *
 * 便携层。不使用 malloc、不依赖平台头文件，只依赖 <math.h>。
 * ================================================================== */
#include <math.h>
#include <string.h>

#include "ui.h"
#include "ui_font.h"   /* 生成的 */

/* ------------------------------------------------------------------
 * 帧缓冲
 * ------------------------------------------------------------------ */
static uint16_t *s_fb;

static int s_missing;      /* 画不出来的字形次数，见 ui_missing_glyphs() */

void ui_init(uint16_t *fb) { s_fb = fb; s_missing = 0; }
const uint16_t *ui_fb(void) { return s_fb; }

int  ui_missing_glyphs(void) { return s_missing; }
void ui_missing_reset(void)  { s_missing = 0; }

void ui_px(int x, int y, uint16_t c)
{
    if ((unsigned)x < (unsigned)UI_W && (unsigned)y < (unsigned)UI_H)
        s_fb[y * UI_W + x] = c;
}

void ui_clear(uint16_t c)
{
    for (int i = 0; i < UI_W * UI_H; i++) s_fb[i] = c;
}

void ui_hline(int x0, int x1, int y, uint16_t c)
{
    if ((unsigned)y >= (unsigned)UI_H) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= UI_W) x1 = UI_W - 1;
    for (int x = x0; x <= x1; x++) s_fb[y * UI_W + x] = c;
}

void ui_vline(int x, int y0, int y1, uint16_t c)
{
    if ((unsigned)x >= (unsigned)UI_W) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 >= UI_H) y1 = UI_H - 1;
    for (int y = y0; y <= y1; y++) s_fb[y * UI_W + x] = c;
}

void ui_fill(ui_rect r, uint16_t c)
{
    for (int y = r.y; y < r.y + r.h; y++) ui_hline(r.x, r.x + r.w - 1, y, c);
}

void ui_frame(ui_rect r, uint16_t c)
{
    if (r.w <= 0 || r.h <= 0) return;
    ui_hline(r.x, r.x + r.w - 1, r.y, c);
    ui_hline(r.x, r.x + r.w - 1, r.y + r.h - 1, c);
    ui_vline(r.x, r.y, r.y + r.h - 1, c);
    ui_vline(r.x + r.w - 1, r.y, r.y + r.h - 1, c);
}

void ui_disc(int cx, int cy, int r, uint16_t c)
{
    if (r < 0) return;
    for (int dy = -r; dy <= r; dy++) {
        int hl = (int)(sqrt((double)r * r - (double)dy * dy) + 0.5);
        ui_hline(cx - hl, cx + hl, cy + dy, c);
    }
}

void ui_ring(int cx, int cy, int r, uint16_t c)
{
    if (r < 0) return;
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        ui_px(cx + x, cy + y, c); ui_px(cx - x, cy + y, c);
        ui_px(cx + x, cy - y, c); ui_px(cx - x, cy - y, c);
        ui_px(cx + y, cy + x, c); ui_px(cx - y, cy + x, c);
        ui_px(cx + y, cy - x, c); ui_px(cx - y, cy - x, c);
        y++;
        if (d <= 0) d += 2 * y + 1;
        else { x--; d += 2 * (y - x) + 1; }
    }
}

/* ------------------------------------------------------------------
 * 月相
 *
 * 明暗界线是半个椭圆：半长轴 = 视半径，半短轴 = r·|cos(日月角距)|，
 * 半长轴的方向**指向太阳**。故判据为
 *
 *     u >= cos(角距) · sqrt(r² − v²)
 *
 * 其中 (u, v) 是相对盘心的坐标、u 沿亮边方向、v 垂直于它。
 * 验算：角距 π（望）→ cos = −1 → u ≥ −sqrt(…) 恒真 → 全亮；
 *       角距 0（朔）→ cos = +1 → 恒假 → 全暗；90° → 右半边。
 *
 * 注意：此处**不分盈亏分支**，也不需要 —— 亮边方向是个向量，
 * 盈亏已含在其指向中；分盈亏分支会引入「望月画成全黑」一类错误。
 *
 * 代价：逐像素而非逐行。r≈20 时约 1600 个点、每个一次 sqrt，
 * 相对帧预算里那几次星历求值可以忽略。
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------
 * 圆盘缓存
 *
 * 注意：上面那段「逐像素、每个一次 sqrt …… 相对帧预算里那几次星历求值可以
 * 忽略」**只在 Mac 上成立**。真机上正好相反：ESP32-S3（LX7）只有
 * 单精度 FPU，一次 double sqrt 需 **4.5 µs**，r=19 的盘约 1134 个点 →
 * **约 9 ms**，而整页 render 实测才 11.9 ms（见 test/esp32_bench）。
 * 在 Mac 上可忽略的开销在真机上成为整页的大头。
 *
 * 但亮边方向（bx, by, cos_sep）**一天才变一次**（ui_app 的 moon_orientation
 * 挂在「跨日 / 跨节气」那道节流上），r 又是版式常量。故把算好的图案留一份：
 * 朝向不变时只做纯整数搬运，边界那圈仍由 ui_ring 画。
 *
 * **算法不变** —— 缓存由原来那段循环生成（判据写法相同），
 * 出图逐字节相同（像素回归验过）。重建一次仍需 ~9 ms，但它落在本来就
 * 几百毫秒的「跨日那一帧」里；省下的是**每一帧**。
 *
 * 三态：0 = 盘外（原来被 continue 掉的那些）、1 = 亮、2 = 暗。
 * 把「盘外」也存下来的意义是**连圆判据一起缓存** —— 否则搬运那圈仍须逐像素
 * 算 dx²+dy² 与 r² 比较，那又是几次 double 乘加（实测只能省一半）。
 * ------------------------------------------------------------------ */
#define MOON_R_MAX 24
static uint8_t s_moon[MOON_R_MAX * 2 + 1][MOON_R_MAX * 2 + 1];
static int    s_moon_r  = -1;
static double s_moon_bx = 0.0, s_moon_by = 0.0, s_moon_cs = 0.0;

/* 亮边方向给成单位向量（bx, by，屏幕坐标 y 向下）；cos_sep = cos(日月角距) */
void ui_moon_dir(int cx, int cy, int r, double bx, double by, double cos_sep,
                 uint16_t lit, uint16_t dark, uint16_t edge)
{
    if (r > MOON_R_MAX) {                 /* 超过缓存尺寸：走原路，不缓存 */
        const double rr = (double)r * (double)r;
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if ((double)dx * dx + (double)dy * dy > rr) continue;
                const double u =  dx * bx + dy * by;
                const double v = -dx * by + dy * bx;
                ui_px(cx + dx, cy + dy,
                      u >= cos_sep * sqrt(rr - v * v) ? lit : dark);
            }
        ui_ring(cx, cy, r, edge);
        return;
    }

    if (r != s_moon_r || bx != s_moon_bx || by != s_moon_by
        || cos_sep != s_moon_cs) {
        const double rr = (double)r * (double)r;
        memset(s_moon, 0, sizeof s_moon);
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if ((double)dx * dx + (double)dy * dy > rr) continue;
                const double u =  dx * bx + dy * by;
                const double v = -dx * by + dy * bx;
                s_moon[dy + MOON_R_MAX][dx + MOON_R_MAX] =
                    (u >= cos_sep * sqrt(rr - v * v)) ? 1u : 2u;
            }
        s_moon_r = r; s_moon_bx = bx; s_moon_by = by; s_moon_cs = cos_sep;
    }

    for (int dy = -r; dy <= r; dy++)       /* 快路径：纯整数 */
        for (int dx = -r; dx <= r; dx++) {
            const uint8_t st = s_moon[dy + MOON_R_MAX][dx + MOON_R_MAX];
            if (st) ui_px(cx + dx, cy + dy, st == 1u ? lit : dark);
        }
    ui_ring(cx, cy, r, edge);
}

/* 旧接口：固定「亮面朝右」，即北半球面朝南看时的样子。
 * 实际使用的是 ui_moon_dir；此接口保留供对照图和单测，
 * 调用方不必自行计算亮边朝向。 */
void ui_moon(int cx, int cy, int r, double elong,
             uint16_t lit, uint16_t dark, uint16_t edge)
{
    ui_moon_dir(cx, cy, r, 1.0, 0.0, cos(elong), lit, dark, edge);
}

/* ------------------------------------------------------------------
 * 点阵文字
 * ------------------------------------------------------------------ */
static int utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    int cp;
    if (s[0] < 0x80)             { cp = s[0];                       *p += 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);                          *p += 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);   *p += 3; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
                                            ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);                          *p += 4; }
    else                         { cp = '?';                        *p += 1; }
    return cp;
}

/* 字形表按码点升序（gen_font.py 里 sorted(..., key=ord) 保证的） */
static const ui_glyph *face_find(const ui_face *f, int cp)
{
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (f->tab[mid].cp == cp) return &f->tab[mid];
        if (f->tab[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static const ui_face *face_of(const ui_font *f, int cp)
{
    return (cp < 128 && f->latin) ? f->latin : f->cjk;
}

/* 先按码点猜一个面，猜不中再试另一个。
 *
 * 注意：必须有这个兜底：gen_font.py 按「是否全角」分面（窄符号如 ° ′ ″ 归
 * 半角面），而这里只能按「码点 < 128」猜 —— 两条规则不等价，° (U+00B0)
 * 落在半角面里却 ≥128，会猜错。在 C 里另抄一份 is_wide 也可行，
 * 但那份表迟早与 Python 那边对不上；试两次的代价为零。 */
static const ui_glyph *find_any(const ui_font *f, int cp, const ui_face **out)
{
    const ui_face *p = face_of(f, cp);
    const ui_glyph *g = face_find(p, cp);
    if (!g && p == f->latin) {
        g = face_find(f->cjk, cp);
        if (g) p = f->cjk;
    } else if (!g && p == f->cjk && f->latin) {
        g = face_find(f->latin, cp);
        if (g) p = f->latin;
    }
    if (out) *out = p;
    return g;
}

static int glyph_adv(const ui_font *f, int cp)
{
    const ui_face *fa;
    const ui_glyph *g = find_any(f, cp, &fa);
    return g ? g->adv : fa->cell_w;
}

/* ------------------------------------------------------------------
 * 通用画布写字。scale 是整数放大倍数（1 = 直接落屏）。
 *
 * 宿主窗口的「按键面板」也用它 —— 面板不是设备 UI，但同样只需要
 * ASCII；在 host_cocoa.m / host_sdl.c 里各写一套文字渲染（并各引入
 * 一份系统字体）不如共用这一套点阵。
 * ------------------------------------------------------------------ */
static void canvas_blit(uint16_t *dst, int dw, int dh, const ui_face *f,
                        const ui_glyph *g, int x, int y, int scale, uint16_t c)
{
    for (int ry = 0; ry < f->cell_h; ry++) {
        const uint8_t *row = f->bits + g->off + (size_t)ry * f->stride;
        for (int rx = 0; rx < f->cell_w; rx++) {
            if (!(row[rx >> 3] & (0x80 >> (rx & 7)))) continue;
            int px0 = x + rx * scale, py0 = y + ry * scale;
            for (int sy = 0; sy < scale; sy++) {
                int py = py0 + sy;
                if ((unsigned)py >= (unsigned)dh) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px = px0 + sx;
                    if ((unsigned)px < (unsigned)dw) dst[py * dw + px] = c;
                }
            }
        }
    }
}

void ui_canvas_ascii(uint16_t *dst, int dw, int dh, int x, int y, int scale,
                     const ui_face *f, uint16_t c, const char *s)
{
    if (!f) return;
    for (const char *p = s; *p; ) {
        int cp = utf8_next(&p);
        const ui_glyph *g = face_find(f, cp);
        if (g) {
            canvas_blit(dst, dw, dh, f, g, x, y, scale, c);
            x += g->adv * scale;
        } else {
            s_missing++;                 /* 面板上的字也要算 */
            x += f->cell_w * scale;
        }
    }
}

int ui_canvas_ascii_w(int scale, const ui_face *f, const char *s)
{
    if (!f) return 0;
    int w = 0;
    for (const char *p = s; *p; ) {
        const ui_glyph *g = face_find(f, utf8_next(&p));
        w += (g ? g->adv : f->cell_w) * scale;
    }
    return w;
}

/* ------------------------------------------------------------------
 * 落屏文字（scale = 1，直接走 ui_px）
 * ------------------------------------------------------------------ */
int ui_text_w(const ui_font *f, const char *s)
{
    int w = 0;
    for (const char *p = s; *p; ) w += glyph_adv(f, utf8_next(&p));
    return w;
}

void ui_text(int x, int y, const ui_font *f, uint16_t c, const char *s)
{
    int cx = x;
    for (const char *p = s; *p; ) {
        int cp = utf8_next(&p);
        const ui_face *fa;
        const ui_glyph *g = find_any(f, cp, &fa);
        if (g) {
            for (int ry = 0; ry < fa->cell_h; ry++) {
                const uint8_t *row = fa->bits + g->off + (size_t)ry * fa->stride;
                for (int rx = 0; rx < fa->cell_w; rx++)
                    if (row[rx >> 3] & (0x80 >> (rx & 7)))
                        ui_px(cx + rx, y + ry, c);
            }
            cx += g->adv;
        } else {
            /* 漏字：画一个空心方框，一眼可见；同时计数，
             * `sim --dump` 结束时会报出该数字（见 ui_missing_glyphs）。
             * gen_font.py 扫源码字面量生成字库，理论上不会漏 ——
             * 漏了即改了 UI 文字未重跑 `make font`，不得让它静默变空白。 */
            ui_frame((ui_rect){ cx + 1, y + 2, fa->cell_w - 2, fa->cell_h - 3 }, c);
            s_missing++;
            cx += fa->cell_w;
        }
    }
}

int ui_text_c(int cx, int y, const ui_font *f, uint16_t c, const char *s)
{
    int x = cx - ui_text_w(f, s) / 2;
    ui_text(x, y, f, c, s);
    return x;
}

ui_rect ui_text_in(ui_box b, int ha, int va, const ui_font *f, uint16_t c,
                   const char *s)
{
    ui_rect r = ui_box_to_rect(b);
    int tw = ui_text_w(f, s);
    int th = f->line;
    int x, y;
    switch (ha) {
        case UI_HCENTER: x = r.x + (r.w - tw) / 2; break;
        case UI_RIGHT:   x = r.x + r.w - tw;       break;
        default:         x = r.x;                  break;
    }
    switch (va) {
        case UI_VCENTER: y = r.y + (r.h - th) / 2; break;
        case UI_BOTTOM:  y = r.y + r.h - th;       break;
        default:         y = r.y;                  break;
    }
    ui_text(x, y, f, c, s);
    return (ui_rect){ x, y, tw, th };
}

/* ------------------------------------------------------------------
 * 比例坐标
 * ------------------------------------------------------------------ */
int ui_px_x(float f) { return (int)lround(f * UI_W); }
int ui_px_y(float f) { return (int)lround(f * UI_H); }

ui_rect ui_box_to_rect(ui_box b)
{
    int x0 = ui_px_x(b.x),          y0 = ui_px_y(b.y);
    int x1 = ui_px_x(b.x + b.w),    y1 = ui_px_y(b.y + b.h);
    return (ui_rect){ x0, y0, x1 - x0, y1 - y0 };
}

/* ------------------------------------------------------------------
 * 字体 / 主题 / 按钮名
 * ------------------------------------------------------------------ */
/* line 是**手写**的常数（= 较高那个面的 cell_h + 1）。不能写成
 * ui_face_cjk12.line —— 结构体成员不是 C 的常量表达式，初始化器里用不了。 */
/* 注意：cjk8 只配全角面 —— 它用于藏干那一行，那里只有汉字。
 * 半角借 latin12，行高取 9（8 + 1，与其它档同一个「cell_h + 1」约定）。 */
const ui_font ui_font_xs = { &ui_face_latin12, &ui_face_cjk8,  9 };
const ui_font ui_font_s = { &ui_face_latin12, &ui_face_cjk12, 13 };
const ui_font ui_font_m = { &ui_face_latin16, &ui_face_cjk16, 17 };
const ui_font ui_font_l = { &ui_face_latin16, &ui_face_cjk24, 25 };

int ui_font_latin_h(const ui_font *f) { return f->latin ? f->latin->cell_h : f->line; }

const ui_theme ui_theme_day = {
    .bg        = UI_RGB(252, 250, 245),   /* 纸白 */
    .panel     = UI_RGB(232, 226, 212),   /* 米灰，顶/底栏 */
    .fg        = UI_RGB(24, 22, 20),
    .dim       = UI_RGB(140, 132, 122),
    .accent    = UI_RGB(196, 74, 24),     /* 赭红 */
    .on_accent = UI_RGB(252, 250, 245),
    .rule      = UI_RGB(200, 192, 178),
    .moon_lit  = UI_RGB(248, 208, 88),
    .moon_dark = UI_RGB(206, 200, 190),
    .moon_edge = UI_RGB(150, 142, 132),
    /* 五行色，**索引顺序是 SF_WUXING_\*：水 0 木 1 金 2 土 3 火 4**，
     * 不是「金木水火土」的习惯顺序 —— 不得照习惯重排。
     *
     * 网页（opendestiny-web 的 bazi 页）那五个 hex 按白底调，日间这套照抄，
     * 只把土提亮一点（原值 #795548 在白底上偏浑，「土」与「金」
     * #A47700 也易看混，故拉开）。 */
    .wuxing    = {
        UI_RGB(0x15, 0x65, 0xC0),   /* 0 水 #1565C0 蓝 */
        UI_RGB(0x2E, 0x7D, 0x32),   /* 1 木 #2E7D32 绿 */
        UI_RGB(0xA4, 0x77, 0x00),   /* 2 金 #A47700 暗金 */
        UI_RGB(0x8D, 0x6E, 0x63),   /* 3 土 #8D6E63 棕（网页是 #795548） */
        UI_RGB(0xC6, 0x28, 0x28),   /* 4 火 #C62828 红 */
    },
};

const ui_theme ui_theme_night = {
    .bg        = UI_RGB(6, 8, 14),
    .panel     = UI_RGB(20, 26, 42),
    .fg        = UI_RGB(226, 232, 244),
    .dim       = UI_RGB(104, 118, 146),
    .accent    = UI_RGB(240, 176, 72),    /* 琥珀 */
    .on_accent = UI_RGB(6, 8, 14),
    .rule      = UI_RGB(38, 48, 72),
    .moon_lit  = UI_RGB(248, 240, 208),
    .moon_dark = UI_RGB(30, 36, 54),
    .moon_edge = UI_RGB(70, 82, 110),
    /* 夜间这套**不是**把日间那五个调暗 —— 网页的色按白底调，置于近黑
     * （bg = 6,8,14）上会糊成一片。这五个按「在近黑底上互不混淆」另配：
     * 都提到高亮度、并彼此拉开色相（蓝→青绿→黄→橙棕→红）。
     * 注意：五个色须在**同一行**里同时可辨（五行统计那行即五色并排），
     * 故亮度要齐，不能有明有暗。 */
    .wuxing    = {
        UI_RGB(110, 170, 255),      /* 0 水 亮蓝 */
        UI_RGB(110, 205, 125),      /* 1 木 亮绿 */
        UI_RGB(235, 195,  90),      /* 2 金 亮金 */
        UI_RGB(205, 150, 110),      /* 3 土 亮棕 */
        UI_RGB(255, 110, 100),      /* 4 火 亮红 */
    },
};

/* 只用 ASCII —— 面板是宿主窗口的一部分，不是设备 UI，不必为它再扩字库 */
const char *const ui_btn_names[UI_BTN_COUNT] = {
    "UP", "DOWN", "LEFT", "RIGHT", "OK", "BACK", "MENU", "ACT"
};
