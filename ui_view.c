/* ==================================================================
 * ui_view.c —— 每一页怎么画
 *
 * 版式**全部用比例**（ui_box 的 0..1），落屏那一刻才乘 UI_W/UI_H。
 * 唯一的例外是字号：点阵字体的尺寸天生是像素量，见 ui.h 的说明。
 *
 * 竖着切四条（比例）：
 *     0.00 – 0.10   顶栏      标题 / 日期
 *     0.10 – 0.88   主体      每页自己排
 *     0.88 – 1.00   底栏      页码点
 * ================================================================== */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "solar_fast.h"

/* ------------------------------------------------------------------
 * ⚠️ 整个文件关掉 `-Wformat-truncation`。**这不是图省事**，理由是这个警告
 *    对渲染层的 API 形态**固有地**是假阳性，改代码改不掉：
 *
 *   1. 渲染层的每个格式化函数都是 `f(char *b, size_t n, ...)` 这个形态
 *      （`fmt_date` / `fmt_deg` / `setting_value` / `birth_value` / `bset_value`
 *      …），`n` 是**调用方给的**。GCC 对 `snprintf(b, n, ...)` 一律按
 *      **n = 1** 的最坏情况算 —— 除非把 n 写成编译期常量，否则永远报。
 *      而「缓冲小就截断」正是 snprintf 的契约：n = 1 时它写 0 字节，
 *      绝不溢出。
 *   2. 浮点那一类（`%.4f°` / `%.5f AU` / `%.2f°E`）：GCC 按 double 的
 *      **全量程**算最坏情况（`%.2f` → 313 个字符，DBL_MAX 有 309 位），
 *      而调用点上的值是经度、纬度、日地距离、黄经 —— 定义域早就有界。
 *      它的浮点值域分析不做这种跨语句推导。
 *
 * **放弃了什么（诚实说）**：以后有人往这个文件里写一个**真的**太小的缓冲，
 * 编译器不会再提醒。兜底是像素回归 —— `sim --dump` 那 120 张帧里任何一处
 * 被截断的标签都会直接画出来看得见（`make check` 的那批帧比对也在）。
 * 库那一侧没有关整个文件：`sf_qishuo.c` 是**逐函数**夹的，因为库要在别人
 * 的工程里用未知的编译选项编，能保的检查要尽量保住。
 * ------------------------------------------------------------------ */
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

/* ------------------------------------------------------------------
 * int32_t → int，只为喂给 %d。
 *
 * ⚠️ 这不是多此一举：**Xtensa（ESP-IDF）的 newlib 里 `int32_t` 是 `long`**，
 * 而 `%d` 要的是 `int`。Mac 上 `int32_t` 恰好就是 `int`，所以本地怎么编都
 * 干净；换到 IDF 就是一片 `-Werror=format=`（GCC 把格式串不匹配当错误）。
 *
 * 屏上要显示的年份 / 岁数 / 天数 / 时分数都远小于 INT_MAX（最大的是流日的
 * 日号，±8000 年那一档也才 500 万），收成 int 不丢信息。
 * 每一处都显式写出来，而不是靠「反正一样大」—— 那个假设只在某些 ABI 上成立。
 * ------------------------------------------------------------------ */
static int I32(int32_t v) { return (int)v; }

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 显示时区偏移，分钟。渲染层只读 —— 由 ui_render 每帧从 app 里取一次塞进来，
 * 免得每个要显示时刻的地方都把 cal_cfg 一路传下去。 */
static int g_utc_offset_min = 480;

#define UI_BAR_TOP    UI_BOX(0.00f, 0.00f, 1.00f, 0.10f)
#define UI_BAR_BOTTOM UI_BOX(0.00f, 0.88f, 1.00f, 0.12f)

/* ------------------------------------------------------------------
 * 时间格式化
 *
 * 库里全是 JD(TT)；屏上要的是**用户钟面上的读数**，所以两步都要走：
 * 先减成 UTC，再加显示时区。两道都收在这一个函数里，页面只管调它。
 *
 * ⚠️ 减的是 **UTC**（sf_tt_to_utc，走闰秒表），不是 UT1。两者差一个 DUT1
 * （现在约 −0.1 s，历史上到过 ±0.9 s）—— 秒级，而屏上最快也就到分钟，
 * 看不出来。但别以为它们是同一个东西：恒星时 / 出没时刻要的是 UT1。
 * ------------------------------------------------------------------ */
static double to_local(double jd_tt)
{
    return sf_tt_to_utc(jd_tt) + g_utc_offset_min / 1440.0;
}

static void split_jd(double jd, int *mo, int *d, int *h, int *mi)
{
    int y, s;
    ui_calendar_from_jd(jd, &y, mo, d, h, mi, &s);
}

static void fmt_date(char *b, size_t n, double jd)      /* 9-07 */
{
    int mo, d, h, mi;
    split_jd(jd, &mo, &d, &h, &mi);
    snprintf(b, n, "%d-%02d", mo, d);
}

static void fmt_time(char *b, size_t n, double jd)      /* 18:41 */
{
    int mo, d, h, mi;
    split_jd(jd, &mo, &d, &h, &mi);
    snprintf(b, n, "%02d:%02d", h, mi);
}

static void fmt_ymd(char *b, size_t n, double jd)       /* 2026-09-19 */
{
    int y, mo, d, h, mi, s;
    ui_calendar_from_jd(jd, &y, &mo, &d, &h, &mi, &s);
    snprintf(b, n, "%04d-%02d-%02d", y, mo, d);
}

/* JD(UT) + 显示时区 → HH:MM。日出日落用。
 * 出没也是「事件」，同节气/朔望按历书口径**四舍五入到分**。
 * ⚠️ 喂进来的是**出没时刻（UT1）**，加显示偏移只是把它落到钟面上，
 * 不是严格意义的真民用时 —— 差一个 DUT1（≤0.9 s）。原先写的是「分钟级
 * 看不见」，但取整点就在 :30 秒附近，0.9 秒足够翻边，所以这句别当成
 * 永远成立；真要对齐历书得先把 UT1 换成 UTC 再取整（见 to_local）。 */
static void fmt_hm_local(char *b, size_t n, double jd_ut)
{
    const double j = ui_jd_round_to_minute(jd_ut + g_utc_offset_min / 1440.0);
    const double frac = j + 0.5 - floor(j + 0.5);
    const int hh = (int)(frac * 24.0);
    const int mm = (int)((frac * 24.0 - hh) * 60.0);
    snprintf(b, n, "%02d:%02d", hh, mm);
}

static void fmt_countdown(char *b, size_t n, double ddays)
{
    if (!(ddays > 0.0)) ddays = 0.0;
    const int d = (int)ddays;
    const int h = (int)((ddays - d) * 24.0);
    const int mi = (int)(((ddays - d) * 24.0 - h) * 60.0);
    if (d > 0)      snprintf(b, n, "%d天%d时", d, h);
    else if (h > 0) snprintf(b, n, "%d时%d分", h, mi);
    else            snprintf(b, n, "%d分", mi);
}

/* ------------------------------------------------------------------
 * 农历的字符串
 *
 * 月名用**口语**：十一月叫冬月、十二月叫腊月。但改历窗口里「同名后月」
 * 那种特例上游是用书面的十一/十二写的（见 sf_calendar.h 的 MONTH_NAME），
 * 所以那一种要单独走书面名 —— 不然两个不同的月会显示成同一个样子。
 * ------------------------------------------------------------------ */
static const char *lunar_month_name(const sf_lunar_date *l)
{
    if (l->month == 13) return "十三";
    if (l->month_name == SF_MONTH_NAME_LATER_SAME_NAME) {
        static const char *const written[12] = {
            "正", "二", "三", "四", "五", "六",
            "七", "八", "九", "十", "十一", "十二"
        };
        return (l->month >= 1 && l->month <= 12) ? written[l->month - 1] : "?";
    }
    static const char *const spoken[12] = {
        "正", "二", "三", "四", "五", "六",
        "七", "八", "九", "十", "冬", "腊"
    };
    return (l->month >= 1 && l->month <= 12) ? spoken[l->month - 1] : "?";
}

static const char *lunar_day_name(int d)
{
    static const char *const n[31] = {
        "", "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八",
        "初九", "初十", "十一", "十二", "十三", "十四", "十五", "十六",
        "十七", "十八", "十九", "二十", "廿一", "廿二", "廿三", "廿四",
        "廿五", "廿六", "廿七", "廿八", "廿九", "三十"
    };
    return (d >= 1 && d <= 30) ? n[d] : "";
}

/* 「丙午」这种四字串。干支是打包的，天干在 8 位的高 4 位。 */
static void ganzhi_str(char *b, size_t n, sf_ganzhi g)
{
    const int stem = (g >> 4) & 0x0f, branch = g & 0x0f;
    snprintf(b, n, "%s%s",
             stem < 10 ? SF_HEAVENLY_STEMS[stem] : "?",
             branch < 12 ? SF_EARTHLY_BRANCHES[branch] : "?");
}

/* 农历年的干支 —— 给它旁边那些**农历**月日配的年。
 *
 * ⚠️ 用的是 sf_ganzhi_year_of(lunar.historical_year)，**不是 a->pillars.year**。
 * 后者是年柱，锚**立春**；农历年跟**正月初一**换。两者在立春到春节之间差整整
 * 一年：2026 年立春 2/4、春节 2/17，2/10 那天年柱已经是丙午，农历还在乙巳年
 * 腊月 —— 配错了屏上就是「丙午年 腊月廿三」，而丙午年根本没有腊月。
 * 详见 sf_calendar.h 里 sf_ganzhi_year_of 的注释。 */
static void lunar_year_ganzhi(const ui_app *a, char *b, size_t n)
{
    sf_ganzhi g;
    if (!a->lunar_ok || sf_ganzhi_year_of(a->lunar.historical_year, &g) != 0) {
        snprintf(b, n, "?");
        return;
    }
    ganzhi_str(b, n, g);
}

static double norm2pi(double x)
{
    x = fmod(x, 2.0 * M_PI);
    return x < 0.0 ? x + 2.0 * M_PI : x;
}

/* 八相。按距角每 45° 一档 —— 这是通行的分法，不是严格的天文定义。 */
static const char *phase_name(double elong)
{
    static const char *const names[8] = {
        "朔", "蛾眉", "上弦", "盈凸", "望", "亏凸", "下弦", "残月"
    };
    int i = (int)lround(norm2pi(elong) / (M_PI / 4.0)) % 8;
    return names[(i + 8) % 8];
}

/* ------------------------------------------------------------------
 * 公共零件
 * ------------------------------------------------------------------ */
static void draw_topbar(const ui_app *a, const ui_theme *t,
                        const char *title, const char *right)
{
    ui_rect bar = ui_box_to_rect(UI_BAR_TOP);
    /* flash：按 OK 时整条反色几帧，做「按到了」的反馈 */
    ui_fill(bar, a->flash > 0 ? t->accent : t->panel);
    const uint16_t c = a->flash > 0 ? t->on_accent : t->fg;

    ui_text_in(UI_BOX(0.03f, 0.00f, 0.60f, 0.10f), UI_LEFT, UI_VCENTER,
               &ui_font_s, c, title);
    /* ⚠️ ui_box 是 (x, y, w, h)，不是「左右边缘」。想右对齐到 0.97 就得写
     * x = 0.67, w = 0.30 —— 直接写 x=0.97 会让文字整个跑到屏外。 */
    if (right && *right)
        ui_text_in(UI_BOX(0.67f, 0.00f, 0.30f, 0.10f), UI_RIGHT, UI_VCENTER,
                   &ui_font_s, a->flash > 0 ? t->on_accent : t->dim, right);
}

/* 页码点。**点始终反映当前这一级**：
 *   选择页 → UI_FEATURE_COUNT 个（选的是哪个功能）
 *   功能内 → 该功能的子页数（选的是哪一页）
 * 所以它会跟着 feature 换长度 —— 这正是「两级」在屏上的表达。 */
static void draw_page_dots(const ui_app *a, const ui_theme *t)
{
    int n, cur;
    if (a->feature < 0) {
        n = UI_FEATURE_COUNT;  cur = a->pick;
    } else if (a->feature == UI_FEATURE_BAZI) {
        n = UI_BAZI_PAGE_COUNT; cur = a->page;
    } else {
        n = UI_PAGE_COUNT;     cur = a->page;
    }

    const int dw = 7, gap = 4;
    const int total = n * dw + (n - 1) * gap;
    int x = (UI_W - total) / 2;
    const int y = ui_px_y(0.945f);
    for (int i = 0; i < n; i++) {
        ui_rect r = { x, y, dw, 3 };
        ui_fill(r, i == cur ? t->accent : t->rule);
        x += dw + gap;
    }
}

/* 一条列表行：左名称 + 右两栏（日期 / 时刻）。
 *
 * 这里用「像素 y + 像素行高」而不是 ui_box，因为列表要的是**固定行距**：
 * 行高取整后逐行累加，用比例的话 0.135*128=17.28 会逐行漂，最后一行能差
 * 出小半行，行与行之间的留白就不齐了。 */
static void row_px(const ui_theme *t, int y, int h, const char *name,
                   const char *date, const char *time,
                   uint16_t bg, uint16_t name_c, uint16_t sub_c,
                   const ui_font *nf)
{
    (void)t;
    if (bg) { ui_rect r = { 0, y, UI_W, h }; ui_fill(r, bg); }

    const int pad = ui_px_x(0.04f);

    /* 名称：左对齐，垂直居中 */
    ui_text(pad, y + (h - nf->line) / 2, nf, name_c, name);

    /* 日期右对齐到 0.60，时刻右对齐到 0.96 —— 固定列才好扫 */
    const int th = ui_font_s.line;
    ui_text(ui_px_x(0.60f) - ui_text_w(&ui_font_s, date),
            y + (h - th) / 2, &ui_font_s, sub_c, date);
    ui_text(ui_px_x(0.96f) - ui_text_w(&ui_font_s, time),
            y + (h - th) / 2, &ui_font_s, sub_c, time);
}

/* ------------------------------------------------------------------
 * 页 1：今
 * ------------------------------------------------------------------ */
static void page_today(const ui_app *a, const ui_theme *t)
{
    char buf[64], l[32];

    fmt_ymd(buf, sizeof buf, to_local(a->jd));
    fmt_time(l, sizeof l, to_local(a->jd));
    draw_topbar(a, t, buf, l);

    /* --- 当前节气名 --- */
    const char *term = SF_SOLAR_TERM_NAMES[a->cur_term];
    ui_text_in(UI_BOX(0.0f, 0.10f, 1.0f, 0.20f), UI_HCENTER, UI_VCENTER,
               &ui_font_l, t->accent, term);

    /* --- 两行日期：农历一行、干支一行 ---
     *
     * 农历「丙午年 闰八月廿九」= 8 个全角字 = 96 px；
     * 干支「丁酉月 丁酉日 丙午时」= 9 个全角 + 2 空格 = 120 px。都放得下。
     *
     * 年柱只在第一行出现（农历那行的「丙午年」就是它），第二行不重复 ——
     * 两行加起来正好是完整的年/月/日/时，也就是原来那个「八」页的内容。
     * 那页已经删了，理由见 ui.h 里 ui_page 的注释。 */
    if (a->lunar_ok) {
        char gz[16];
        lunar_year_ganzhi(a, gz, sizeof gz);
        snprintf(buf, sizeof buf, "%s年 %s%s月%s", gz,
                 a->lunar.is_leap ? "闰" : "",
                 lunar_month_name(&a->lunar), lunar_day_name(a->lunar.day));
        ui_text_in(UI_BOX(0.0f, 0.30f, 1.0f, 0.10f), UI_HCENTER, UI_VCENTER,
                   &ui_font_s, t->fg, buf);
    }
    if (a->pillars_ok) {
        /* ⚠️ 8 字节够：ganzhi_str 只写「干」+「支」= 2 个全角字 = 6 B + NUL。
         *    写成 16 本身没错，但**下面拼整行的 buf[64] 就会报截断** ——
         *    GCC 按声明尺寸算最坏情况：4×15+3 个全角字 = 72 > 64。
         *    声明成实际契约的大小，编译器就能证明放得下（4×7+12 = 40）。 */
        char yz[8], mz[8], dz[8], hz[8];
        ganzhi_str(yz, sizeof yz, a->pillars.year);
        ganzhi_str(mz, sizeof mz, a->pillars.month);
        ganzhi_str(dz, sizeof dz, a->pillars.day);
        ganzhi_str(hz, sizeof hz, a->pillars.hour);
        /* 12 个全角字不空格 = 144 px，160 宽塞得下（加空格就 162，超了）。
         * 没有分隔符，靠固定的年/月/日/时顺序读 —— 上面那行已经把年月日
         * 以农历的形式排过一遍，读法是一致的。 */
        snprintf(buf, sizeof buf, "%s年%s月%s日%s时", yz, mz, dz, hz);
        ui_text_in(UI_BOX(0.0f, 0.409f, 1.0f, 0.10f), UI_HCENTER, UI_VCENTER,
                   &ui_font_s, t->dim, buf);
    }

    /* 视黄经不在这页显示 —— 它和农历行挤在一起，而且太阳页已经有了。
     * 1.8" 的屏上，「少一行、留出呼吸」比「多一个数」重要。
     *
     * ⚠️ 这一页现在的纵向账（128 px）：顶栏 13 + 白露 20 + 农历/干支 27 +
     * 进度条 3 + 月相块 43 ≈ 106，只剩 ~22 px 分给五处留白。所以下面这些
     * 0.0x 的数是**互相咬着**的：动一个就得把这条链上的其它几个一起挪，
     * 改完务必渲出来看一眼（`./sim --dump`），别只看代码。 */

    /* --- 本节气已过多少 --- */
    {
        const double t0 = a->term_jd[0], t1 = a->term_jd[1];
        if (isfinite(t0) && isfinite(t1) && t1 > t0) {
            double p = (a->jd - t0) / (t1 - t0);
            if (p < 0.0) p = 0.0;
            if (p > 1.0) p = 1.0;
            const int x0 = ui_px_x(0.10f), x1 = ui_px_x(0.90f);
            const int y  = ui_px_y(0.531f);
            ui_fill((ui_rect){ x0, y, x1 - x0, 3 }, t->rule);
            ui_fill((ui_rect){ x0, y, (int)((x1 - x0) * p), 3 }, t->accent);
        }
    }

    /* --- 月相 --- */
    {
        const int cx = ui_px_x(0.25f), cy = ui_px_y(0.715f);
        const int r  = ui_px_y(0.147f);
        ui_moon_dir(cx, cy, r, a->moon_bx, a->moon_by, a->moon_cos_sep,
                    t->moon_lit, t->moon_dark, t->moon_edge);

        const int tx = ui_px_x(0.42f);
        const int lh = ui_font_s.line;

        snprintf(l, sizeof l, "距 %s", SF_SOLAR_TERM_NAMES[(a->cur_term + 1) % 24]);
        ui_text(tx, ui_px_y(0.565f), &ui_font_s, t->dim, l);

        fmt_countdown(l, sizeof l, isfinite(a->term_jd[1]) ? a->term_jd[1] - a->jd : 0.0);
        ui_text(tx, ui_px_y(0.675f), &ui_font_m, t->fg, l);

        /* 比例用 cos_sep（真角距），跟盘上画的是同一个量；八相名仍用
         * elong —— 那个是粗分档，而且靠它的符号分盈亏。 */
        snprintf(l, sizeof l, "%s %.0f%%", phase_name(a->elong),
                 50.0 * (1.0 - a->moon_cos_sep));
        ui_text(tx, ui_px_y(0.80f), &ui_font_s, t->accent, l);
        (void)lh;
    }
}

/* ------------------------------------------------------------------
 * 页 2：历（农历月历）
 *
 * 7 列 × 6 行的网格。160 px 宽 → 每格 22 px，只放得下**数字**：
 * 「廿九」这种两字写法要 24 px，超了。所以格子里是 1–30 的阿拉伯数字，
 * 想要「初一/廿九」的全称看主页那一行。
 * ------------------------------------------------------------------ */
static void page_lunar(const ui_app *a, const ui_theme *t)
{
    char title[48], right[32];

    if (!a->lunar_ok) {
        draw_topbar(a, t, "农历", "");
        ui_text_in(UI_BOX(0.0f, 0.4f, 1.0f, 0.2f), UI_HCENTER, UI_VCENTER,
                   &ui_font_m, t->dim, "超出农历有效范围");
        return;
    }

    /* 顶栏：本月 + 干支年。右上是「大/小」—— 30 天叫大月、29 天叫小月。
     * ⚠️ 这一页整页都是农历，所以年要用**农历年**那个干支，不是年柱
     * （见 lunar_year_ganzhi 上面那段）。 */
    snprintf(title, sizeof title, "农历 %s%s月（%s）",
             a->lunar.is_leap ? "闰" : "", lunar_month_name(&a->lunar),
             a->grid_days == 30 ? "大" : "小");
    {
        char gz[16];
        lunar_year_ganzhi(a, gz, sizeof gz);
        snprintf(right, sizeof right, "%s年", gz);
    }
    draw_topbar(a, t, title, right);

    /* --- 星期表头 --- */
    {
        static const char *const wd[7] = { "日", "一", "二", "三", "四", "五", "六" };
        const int cw = UI_W / 7;
        const int y  = ui_px_y(0.105f);
        for (int i = 0; i < 7; i++) {
            const uint16_t c = (i == 0 || i == 6) ? t->accent : t->dim;
            ui_text_c(i * cw + cw / 2, y, &ui_font_s, c, wd[i]);
        }
        ui_hline(0, UI_W - 1, y + ui_font_s.line, t->rule);
    }

    /* --- 网格 --- */
    {
        const int cw = UI_W / 7;                       /* 22 px */
        const int gh = ui_px_y(0.135f);                /* ~17 px */
        const int top = ui_px_y(0.21f);

        /* 当天是本月第几天（1-based）；不在本月就是 0。
         *
         * ⚠️ 这里**故意不用 civil_jd()**：网格本身是库按 `jd_ut + offset`
         * 归日算出来的（sf_cal_from_instant_ut），高亮必须跟库用同一个口径，
         * 否则午夜前后 DUT1 那 0.9 秒里高亮会和网格错开一格。
         * 页面顶栏的钟表读数反过来该用真民用时 —— 两处的基准本来就不一样。 */
        int today = 0;
        {
            int32_t y = 0, mo = 0, d = 0;
            const double jl = sf_tt_to_ut(a->jd)
                            + a->cal_cfg.utc_offset_minutes / 1440.0;
            sf_solar_date_from_day_number((int64_t)floor(jl + 0.5), &y, &mo, &d);
            const int64_t jdn = sf_solar_day_number(y, mo, d);
            const int64_t off = jdn - a->grid_first_jdn;
            if (off >= 0 && off < a->grid_days) today = (int)off + 1;
        }

        const int cells = 6 * 7;
        for (int i = 0; i < cells; i++) {
            const int day = i - a->grid_wday0 + 1;
            if (day < 1 || day > a->grid_days) continue;
            const int cx = (i % 7) * cw;
            const int cy = top + (i / 7) * gh;

            const int is_today = (day == today);
            const int holy = (day == 1) || a->grid_term[day];   /* 初一或节气 */

            if (is_today) ui_fill((ui_rect){ cx, cy, cw, gh }, t->accent);

            /* ⚠️ 12 不是 4：`%d` 的最坏情况（11 位 + NUL）要 12 字节才装得下，
             * 而 GCC 看不到 day 的定义域。实际内容最多两位。 */
            char num[12];
            snprintf(num, sizeof num, "%d", day);
            const uint16_t c = is_today ? t->on_accent
                             : (holy ? t->accent : t->fg);
            ui_text_c(cx + cw / 2, cy + (gh - ui_font_s.line) / 2,
                      &ui_font_s, c, num);

            /* 初一画一道底线，节气的日子在数字下点一点 —— 两种语义要分开，
             * 都用强调色的话会分不清哪个是哪个 */
            if (!is_today && day == 1)
                ui_hline(cx + 4, cx + cw - 5, cy + gh - 2, t->accent);
            else if (!is_today && a->grid_term[day])
                ui_fill((ui_rect){ cx + cw / 2 - 1, cy + gh - 3, 2, 2 }, t->accent);
        }
    }
}

/* ------------------------------------------------------------------
 * 页 3：气（往后 24 个节气）
 * ------------------------------------------------------------------ */
/* 本机民用时的年份 —— 用来判断「选中的这一年是不是就是今年」 */
static int local_year(const ui_app *a)
{
    int y = 0;
    ui_calendar_from_jd(to_local(a->jd), &y, NULL, NULL, NULL, NULL, NULL);
    return y;
}

/* 气/朔两页共用的顶栏：左边写年份、右边写方向+步长（跟设置页同一套编辑三键）。
 * 步长放右边是必须的：K8 切方向在别处没有任何反馈。 */
static void draw_year_topbar(const ui_app *a, const ui_theme *t,
                             const char *kind, char *title, size_t tn)
{
    char r[24];
    snprintf(title, tn, "%d 年%s", a->view_year, kind);
    snprintf(r, sizeof r, "%c%g年", a->step_plus ? '+' : '-',
             ui_app_step_magnitude(a));
    draw_topbar(a, t, title, r);
}

/* ------------------------------------------------------------------
 * 页 3：气（选中年份的 24 个节气）
 *
 * 按**公历年** 1–12 月排，不是「从此刻往后数 24 个」——
 * 数据在 ui_app.c 的 rebuild_year 里备好（一次 sf_qishuo_year_run），
 * 出来就是时间序，名字直接从事件里取，没有任何索引推算。
 * ------------------------------------------------------------------ */
static void page_terms(const ui_app *a, const ui_theme *t)
{
    char title[32], d[16], tm[16];
    draw_year_topbar(a, t, "节气", title, sizeof title);

    /* 「现在落在哪个节气」只在选中的就是今年时才标 —— 看别的年份时
     * 高亮一个跟屏幕无关的行只会让人困惑。
     *
     * 取**第一个**匹配：闰年那个重复的节气出现在年末，而「现在」若真在那个
     * 节气里，命中的必然是年初那一条。 */
    int cur_row = -1;
    if (a->view_year == local_year(a)) {
        for (int k = 0; k < a->year_term_n; k++)
            if (a->year_term[k].term_index == a->cur_term) { cur_row = k; break; }
    }

    const int row_h = ui_px_y(UI_LIST_ROW_H);
    const int top   = ui_px_y(UI_LIST_TOP);

    for (int i = 0; i < UI_LIST_ROWS; i++) {
        const int k = a->scroll[UI_PAGE_TERMS] + i;
        if (k >= a->year_term_n) break;
        const int ry = top + i * row_h;
        const ui_year_row *r = &a->year_term[k];

        if (!isfinite(r->jd_tt)) {
            row_px(t, ry, row_h, r->name, "--", "--", 0, t->dim, t->dim, &ui_font_m);
            continue;
        }
        /* ⚠️ 事件时刻要先**四舍五入到分**再拆 —— 历书给的节气时刻是四舍五入
         * 到分的，直接截断会让 2026 年 24 个节气里有 13 个早显示一分钟
         * （大寒 9:44:55.8 会显示成 9:44，历书是 9:45）。理由见
         * ui_jd_round_to_minute 的注释。日期也跟着一起用取整后的值，
         * 免得 23:59:40 这种进位时日期和时刻对不上。 */
        const double lt = ui_jd_round_to_minute(to_local(r->jd_tt));
        fmt_date(d, sizeof d, lt);
        fmt_time(tm, sizeof tm, lt);

        /* ⚠️ 两个比较都要先判 cur_row >= 0。cur_row 是 -1 表示「这一年不是今年，
         * 没有当前节气」—— 而 k == cur_row + 1 在 cur_row = -1 时正好是 k == 0，
         * 于是**每一年**的第一行（小寒）都会被误标成「下一个」。 */
        if (cur_row >= 0 && k == cur_row) {
            row_px(t, ry, row_h, r->name, d, tm, t->accent, t->on_accent,
                   t->on_accent, &ui_font_m);
        } else if (cur_row >= 0 && k == cur_row + 1) {
            row_px(t, ry, row_h, r->name, d, tm, 0, t->accent, t->dim, &ui_font_m);
        } else {
            row_px(t, ry, row_h, r->name, d, tm, 0, t->fg, t->dim, &ui_font_m);
        }
    }
}

/* ------------------------------------------------------------------
 * 页 3：朔（选中年份的朔 + 望，按时间交错）
 *
 * 一年的行数**不是常数**：每个相位角各 12~13 条，两个角就是 24~26 行。
 * ------------------------------------------------------------------ */
static void page_phases(const ui_app *a, const ui_theme *t)
{
    char title[32], d[16], tm[16];
    draw_year_topbar(a, t, "朔望", title, sizeof title);

    /* 标出「下一个还没到的」—— 一整年 20 多行，没有这个锚点不好找现在在哪 */
    int cur_row = -1;
    if (a->view_year == local_year(a)) {
        /* ⚠️ 跟 a->jd 比（两边都是 TT）。原来写成 sf_tt_to_ut(a->jd) 是拿
         * UT1 比 TT，差 ΔT ≈ 69 秒 —— 事件正好落在「现在」前后一分钟时选错行。
         * 表是时间序，所以第一个「还没到」的就是下一个。 */
        for (int k = 0; k < a->year_phase_n; k++)
            if (a->year_phase[k].jd_tt > a->jd) { cur_row = k; break; }
    }

    const int row_h = ui_px_y(UI_LIST_ROW_H);
    const int top   = ui_px_y(UI_LIST_TOP);

    for (int i = 0; i < UI_LIST_ROWS; i++) {
        const int k = a->scroll[UI_PAGE_PHASES] + i;
        if (k >= a->year_phase_n) break;
        const int ry = top + i * row_h;
        const ui_year_row *r = &a->year_phase[k];

        if (!isfinite(r->jd_tt)) {
            row_px(t, ry, row_h, "--", "", "", 0, t->dim, t->dim, &ui_font_m);
            continue;
        }
        /* 同节气页：朔望时刻也按历书口径四舍五入到分 */
        const double lt = ui_jd_round_to_minute(to_local(r->jd_tt));
        fmt_date(d, sizeof d, lt);
        fmt_time(tm, sizeof tm, lt);

        if (k == cur_row) {
            row_px(t, ry, row_h, r->name, d, tm, t->accent,
                   t->on_accent, t->on_accent, &ui_font_m);
        } else {
            /* 望比朔醒目：一个亮、一个暗 */
            row_px(t, ry, row_h, r->name, d, tm, 0,
                   r->full ? t->accent : t->fg, t->dim, &ui_font_m);
        }
    }
}

/* ------------------------------------------------------------------
 * 页 4：阳（此刻的太阳量）
 * ------------------------------------------------------------------ */
static void page_sun(const ui_app *a, const ui_theme *t)
{
    char r[24];
    int y, mo, d, h, mi, s;
    ui_calendar_from_jd(to_local(a->jd), &y, &mo, &d, &h, &mi, &s);
    snprintf(r, sizeof r, "%02d:%02d", h, mi);
    draw_topbar(a, t, "太阳", r);

    static const char *const labels[6] = {
        "视黄经", "视黄纬", "日地距离", "日出", "日落", "昼长"
    };

    const int row_h = ui_px_y(0.12f);
    const int top   = ui_px_y(0.105f);

    for (int i = 0; i < 6; i++) {
        const int ry = top + i * row_h;
        switch (i) {
        case 0: snprintf(r, sizeof r, "%.4f°", norm2pi(a->sunlon) * 180.0 / M_PI); break;
        case 1: snprintf(r, sizeof r, "%+.4f°", a->sunlat * 180.0 / M_PI);         break;
        /* 5 位小数（≈1500 km），不是 6 位：6 位时 "1.004595 AU" 有 11 个
         * 半角字宽 = 99 px，右对齐到 0.96 后左端落在 54.6，正好撞上
         * "日地距离" 的尾巴（48 px 宽、从 6.4 起 = 到 54.4）。库自身精度
         * 是 90 km 量级，第 6 位本来就是噪声。 */
        case 2: snprintf(r, sizeof r, "%.5f AU", a->dist);                      break;

        /* 出没：库给的是 JD(UT)，要按显示时区落成时刻 */
        case 3:
        case 4: {
            if (a->rise_ok == SF_RISE_POLAR_DAY)        snprintf(r, sizeof r, "极昼");
            else if (a->rise_ok == SF_RISE_POLAR_NIGHT) snprintf(r, sizeof r, "极夜");
            else if (a->rise_ok != SF_RISE_OK)          snprintf(r, sizeof r, "--");
            else fmt_hm_local(r, sizeof r,
                              i == 3 ? a->rise_jd : a->set_jd);
            break;
        }
        default:
            if (isfinite(a->day_len_h)) {
                const int H = (int)a->day_len_h;
                const int M = (int)lround((a->day_len_h - H) * 60.0);
                snprintf(r, sizeof r, "%d时%02d分", H, M);
            } else snprintf(r, sizeof r, "--");
            break;
        }

        const int lth = ui_font_s.line;
        const int rth = ui_font_latin_h(&ui_font_m);
        ui_text(ui_px_x(0.04f), ry + (row_h - lth) / 2,
                &ui_font_s, t->dim, labels[i]);
        ui_text(ui_px_x(0.96f) - ui_text_w(&ui_font_m, r),
                ry + (row_h - rth) / 2, &ui_font_m, t->fg, r);

        if (i) ui_hline(ui_px_x(0.04f), ui_px_x(0.96f), ry, t->rule);
    }

    /* 观者位置放在最下面一行 —— 不然算出来的出没时刻没有参照 */
    {
        /* ⚠️ 一行里塞「位置 + 均时差」会超过 160 px 被右边裁掉
         * （实测 162 px，第一个字符整个没了）。拆成左右两段各自对齐，
         * 长度就互不叠加了。 */
        /* ⚠️ 这里是**一位小数**，跟设置页那个两位小数的编辑器不同步 —— 是
         * 故意的，不是漏改。拆成两段之后留给这一段的钱只有约 103 px
         * （右段最宽 `-16.4分` = 47 px，从 155 往左到 108），而实测宽度：
         *   `39.90N 116.41E` =  98 px  ✓（北京，还差 5 px）
         *   `-39.90S -116.41W` = 112 px ✗（南北美那种负纬负经，压到均时差上）
         * 一位小数两个都放得下。要改两位的话，先想好这行怎么排两行。 */
        char pos[32];
        snprintf(pos, sizeof pos, "%.1f%s %.1f%s",
                 fabs(a->observer_lat), a->observer_lat >= 0 ? "N" : "S",
                 fabs(a->observer_lon), a->observer_lon >= 0 ? "E" : "W");
        snprintf(r, sizeof r, "%+.1f分", a->eot_min);
        const int ybase = ui_px_y(0.842f);
        ui_text(ui_px_x(0.03f), ybase, &ui_font_s, t->dim, pos);
        ui_text(ui_px_x(0.97f) - ui_text_w(&ui_font_s, r), ybase,
                &ui_font_s, t->dim, r);
    }
}

/* ⚠️ 这里原来有「页 6：八（四柱）」：pillar_chars / kv_line / page_pillars
 * 三块。整页搬进主页第一行下面了（见 page_today），三个函数一并删掉 ——
 * kv_line 和 pillar_chars 只被那一页用，留着就是 -Wunused-function。 */

/* 名字照抄 tools 项目里气朔那一套（见 ui.h 的注释），160 px 放不下全称的
 * 用短名。对照表：
 *
 *   BASIS_NAMES   clockMode      civil / mean-solar / true-solar
 *                               钟表时间 / 平太阳时 / 真太阳时
 *   RAT_NAMES     ratHourMode    next-day / current-day /
 *                               current-day-tomorrow-stem
 *                               （ziwei 全称：不分早晚子 / 晚子当天＋今日天干 /
 *                                 晚子当天＋明日天干）
 *   CALMODE_NAMES calendarMode   historical / china-astronomical /
 *                               local-astronomical
 *                               （气朔全称：中国历史历法 / 现代中国天文历法 /
 *                                 当地定气定朔）
 *   BOUNDARY      dayBoundaryMode fixed-utc-offset / mean-solar-meridian
 *                               （八字全称：按当地标准时间（标准经线）/
 *                                 按出生地经度（地方平太阳时））
 *   PILLAR_HIST   pillarHistoricalMode
 *                               follow-calendar / off / on
 *                               （全称：跟随历法 / 精确定气时刻 /
 *                                 历史定气 · UTC+8 日界） */
static const char *const BASIS_NAMES[3]   = { "钟表时间", "平太阳时", "真太阳时" };
static const char *const RAT_NAMES[3]     = { "23点换日", "晚子用当日", "时干随次日" };
/* ⚠️ 这三张表是**按枚举值索引**的，不是按屏幕上的顺序 —— 库的 mode 是
 * HISTORICAL=0 / LOCAL_ASTRONOMICAL=1 / CHINA_STANDARD_ASTRONOMICAL=2，
 * 所以左右翻的时候是「中国古历 → 当地天文 → 中国天文」。别照直觉排。 */
static const char *const CALMODE_NAMES[3] = { "中国古历", "当地天文", "中国天文" };
static const char *const BOUNDARY_NAMES[2] = { "按标准经线", "按当地经度" };
static const char *const PILLAR_HIST_NAMES[3] = { "跟历法", "精确定气", "历史定气" };

/* ------------------------------------------------------------------
 * 页 6：设（设置）
 *
 * 这一页是**唯一**左右键改成调值的页（ui_app_button 里有说明）。每行都是
 * 在一串取值里滚动，没有需要键入的东西 —— 1.8" 屏上做多级编辑菜单不划算。
 *
 * 顶栏右上是**虚拟时刻**：改时区或口径时，这里立刻动 —— 不然改完得退回
 * 主页，对着那行干支才知道改没改上（这几个旋钮**只**影响主页的干支/农历，
 * 太阳量、节气、月相都不受它们影响）。枚举行没有步长，右上自然回到时刻，
 * 所以「换个口径看时柱变没变」这件事在屏上一直是看得见的。
 * ------------------------------------------------------------------ */
static const char *const SET_LABELS[UI_SET_COUNT] = {
    "显示时区", "钟表口径", "早晚子时",
    "历法", "定气日界", "当地经度", "节气边界", "气朔精度",
    "观测纬度", "观测经度"
};

/* 精度档的名字**按枚举值索引**（LOW=0 / MED=1 / HIGH=2，跟 sf_qishuo_accuracy 同序）。
 *
 * 这里刻意不借 JS 的 fast/mid/accurate：上游那三个是**三个不同算法**，我们这三档
 * 是同一个求解器的三种截断（取预算表前 N 项），借名字会让人以为「我们的 low
 * 就是上游的 fast」。实测正好相反 —— 上游 fast 对紫金山那张历书表 24/24 全对，
 * 我们截到 LOW 只有 9/24。见 sf_qishuo.h 头注「与 JS 的差异 1」。
 *
 * 屏上只写一个字：行标签「气朔精度」已经说清它是什么，再挂个「全量/截断」
 * 在 96 px 的值列里只会挤。各档的实际误差数字（低 27 s / 中 3.2 s / 高 0）
 * 见 sf_qishuo.h 和 sim/README。 */
static const char *const ACC_NAMES[3] = { "低", "中", "高" };

/* ⚠️ 度数是**两位小数**，跟档位表对齐：0.01° 那一档按一下值列必须动一个字
 * （ui_app.c 的 STEP_DEG）。写成一位小数的话那一档看着完全没反应。
 * 两位的宽度：`116.41°E` = 8 个半角 ≈ 56 px，右对齐到 0.95 时左边缘在
 * 约 96 px 处，而左边标签最多 4 个汉字（「当地经度」）到 56 px 就完了。 */
static void fmt_deg(char *b, size_t n, double deg)
{
    snprintf(b, n, "%.2f%s", fabs(deg), deg >= 0 ? "°E" : "°W");
}

static void setting_value(const ui_app *a, int row, char *b, size_t n)
{
    switch (row) {
    case UI_SET_TZ: {
        const int m = a->cal_cfg.utc_offset_minutes;
        snprintf(b, n, "UTC%c%d:%02d", m < 0 ? '-' : '+',
                 (m < 0 ? -m : m) / 60, (m < 0 ? -m : m) % 60);
        break;
    }
    case UI_SET_BASIS:
        snprintf(b, n, "%s", BASIS_NAMES[a->time_basis]);
        break;
    case UI_SET_RAT_HOUR:
        snprintf(b, n, "%s", RAT_NAMES[a->rat_hour]);
        break;
    case UI_SET_CAL_MODE:
        snprintf(b, n, "%s", CALMODE_NAMES[a->cal_cfg.mode]);
        break;
    case UI_SET_BOUNDARY:
        snprintf(b, n, "%s", BOUNDARY_NAMES[a->cal_cfg.day_boundary_mode]);
        break;
    case UI_SET_MERIDIAN:
        fmt_deg(b, n, a->cal_cfg.calendar_meridian_deg);
        break;
    case UI_SET_PILLAR_HIST:
        snprintf(b, n, "%s", PILLAR_HIST_NAMES[a->cal_cfg.pillar_historical_mode]);
        break;
    case UI_SET_ACCURACY:
        snprintf(b, n, "%s", ACC_NAMES[a->accuracy]);
        break;
    case UI_SET_LAT:
        /* 两位小数，跟 fmt_deg 同一个理由（0.01° 那一档要看得见） */
        snprintf(b, n, "%.2f%s", fabs(a->observer_lat),
                 a->observer_lat >= 0 ? "°N" : "°S");
        break;
    case UI_SET_LON:
        fmt_deg(b, n, a->observer_lon);
        break;
    default:
        b[0] = '\0';
        break;
    }
}

static void page_settings(const ui_app *a, const ui_theme *t)
{
    /* 顶栏右上：光标这一行有步长可言时显示**「方向 + 步长」**，否则显示
     * 虚拟时刻。
     *
     * 为什么必须显示：方向和步长现在是两个独立按键，不显示的话你根本不知道
     * 按下去会动多少、往哪边动。而方向键（K8）按下去除了这里没有任何反馈。
     *
     * 为什么可以顶掉时刻：数值行自己的值就是反馈（时区那行的值就是偏移本身）；
     * 而枚举行没有步长，右上自然回到时刻 —— 那正是「换个口径看时柱变没变」
     * 唯一的地方。两边各得其所。 */
    /* 24 不是 16：`%02d:%02d` 最坏 11×2+1 = 23（GCC 看不到时分是 0..59）。
     * 实际内容「08:00」5 个字符。 */
    char right[24];
    const double mag = ui_app_step_magnitude(a);
    if (mag > 0.0) {
        const char unit = (a->set_row == UI_SET_TZ) ? 'm' : 'd';
        if (unit == 'm')
            snprintf(right, sizeof right, "%c%d分", a->step_plus ? '+' : '-',
                     (int)mag);
        else
            snprintf(right, sizeof right, "%c%g°", a->step_plus ? '+' : '-', mag);
    } else {
        snprintf(right, sizeof right, "%02d:%02d",
                 I32(a->virtual_time.hour), I32(a->virtual_time.minute));
    }
    draw_topbar(a, t, "设置", right);

    const int row_h = ui_px_y(UI_LIST_ROW_H);
    const int top   = ui_px_y(UI_LIST_TOP);
    const int xl    = ui_px_x(0.05f);
    const int xr    = ui_px_x(0.95f);
    const int first = a->scroll[UI_PAGE_SETTINGS];

    for (int i = 0; i < UI_SET_ROWS; i++) {
        const int row = first + i;
        if (row >= UI_SET_COUNT) break;

        char val[32];
        const int ry = top + i * row_h;
        const int inert = ui_app_setting_inert(a, row);
        const int sel   = (row == a->set_row);
        /* 三种状态三种画法，别混：
         *   普通行        什么都不画
         *   光标行        整行反色 —— 「左右键改的是这一行」
         *   光标 + 失效行  只描个框、值保持暗色 —— 「你在这儿，但这行现在不归你管」
         * 光标压在失效行上时若照样反色，就完全看不出它按不动了。 */
        const int solid = sel && !inert;

        if (solid)    ui_fill ((ui_rect){ 0, ry, UI_W, row_h }, t->accent);
        else if (sel) ui_frame((ui_rect){ 0, ry, UI_W, row_h }, t->accent);

        uint16_t lc, vc;
        if (solid)    { lc = t->on_accent; vc = t->on_accent; }
        else if (sel) { lc = t->fg;        vc = t->rule; }
        else          { lc = t->dim;       vc = inert ? t->rule : t->fg; }

        setting_value(a, row, val, sizeof val);

        /* 时区被锁时**换标签**：「显示时区」→「时区锁定」。
         * 上游那边是在表单底下印一行注（「中国历史历法仅按 UTC+8 生效，
         * 时区已锁定」），160 px 没地方印注 —— 把这层意思塞进标签，
         * 宽度一个字没变，而且一眼就知道这行为什么按不动。 */
        const char *label = (row == UI_SET_TZ
                             && ui_app_setting_inert(a, UI_SET_TZ))
                              ? "时区锁定" : SET_LABELS[row];

        const int th = ui_font_s.line;
        const int yy = ry + (row_h - th) / 2;
        ui_text(xl, yy, &ui_font_s, lc, label);
        ui_text(xr - ui_text_w(&ui_font_s, val), yy, &ui_font_s, vc, val);
    }
}

/* ==================================================================
 * 功能选择页（feature == -1 时显示）
 *
 * 两级页面的**第一级**：一张卡片一个功能。上下左右**都**移动高亮 ——
 * 只有两个功能，两对键等价，但都认省得猜（用户要的就是「上下左右滑」）。
 * ================================================================== */
static void page_pick(const ui_app *a, const ui_theme *t)
{
    char buf[64], l[32];
    fmt_ymd(buf, sizeof buf, to_local(a->jd));
    fmt_time(l, sizeof l, to_local(a->jd));
    draw_topbar(a, t, buf, l);

    static const char *const NAMES[UI_FEATURE_COUNT] = { "万年历", "八字" };
    static const char *const SUBS[UI_FEATURE_COUNT]  = {
        "节气 · 农历 · 日月", "四柱 · 大运 · 流年"
    };

    for (int i = 0; i < UI_FEATURE_COUNT; i++) {
        const float y = 0.235f + 0.30f * (float)i;
        const int sel = (i == a->pick);
        const ui_rect r = ui_box_to_rect(UI_BOX(0.12f, y, 0.76f, 0.245f));
        /* 选中整块反色；未选中只描细框。描边两边都画 —— 反色块在纸白底上
         * 本来就够醒目，框是给夜间主题补轮廓的，顺手统一了。 */
        if (sel) ui_fill(r, t->accent);
        ui_frame(r, sel ? t->accent : t->rule);

        ui_text_in(UI_BOX(0.12f, y, 0.76f, 0.13f), UI_HCENTER, UI_VCENTER,
                   &ui_font_m, sel ? t->on_accent : t->fg, NAMES[i]);
        ui_text_in(UI_BOX(0.12f, y + 0.125f, 0.76f, 0.11f), UI_HCENTER, UI_VCENTER,
                   &ui_font_s, sel ? t->on_accent : t->dim, SUBS[i]);
    }
}

/* ==================================================================
 * 八字 · 出生信息页
 *
 * **完全复用设置页那套三键**（上下列 / OK 调值 / MENU 换步长 / ACT 切 ±）。
 * 不新造输入控件 —— 这个 UI 里没有任何光标/软键盘的基础设施，见 sim/README。
 * ================================================================== */
static const char *const BIRTH_LABELS[UI_BIRTH_ROW_COUNT] = {
    "出生年", "月", "日", "时", "分", "性别", "出生地时区", "出生地经度"
};

/* 步长的单位。跟设置页同一个做法：**单位不同的行给不同的后缀**。
 * ⚠️「分」和「m」必须分开 —— 两个都是分钟，但一个是钟面读数、一个是时区
 * 偏移，混用就分不清顶栏那个 `+15?` 改的是哪一行。偏移的 m 跟设置页
 * 「显示时区」那个 m 是同一个人。（性别那行没有步长，空着；
 * 经度是 `°`，跟设置页「经度」那个同一个。） */
static const char *const BIRTH_STEP_UNIT[UI_BIRTH_ROW_COUNT] = {
    "年", "月", "日", "时", "分", "", "m", "°"
};

static void birth_value(const ui_app *a, int row, char *b, size_t n)
{
    switch (row) {
    case UI_BIRTH_YEAR:   snprintf(b, n, "%d", I32(a->birth.year)); break;
    case UI_BIRTH_MONTH:  snprintf(b, n, "%d", I32(a->birth.month)); break;
    case UI_BIRTH_DAY:    snprintf(b, n, "%d", I32(a->birth.day)); break;
    case UI_BIRTH_HOUR:   snprintf(b, n, "%d 时", I32(a->birth.hour)); break;
    case UI_BIRTH_MINUTE: snprintf(b, n, "%02d 分", I32(a->birth.minute)); break;
    /* 「未选」是个**显式状态**，不是「默认男」—— 顺逆判据要靠它，
     * 猜一个会静默把整张盘排反。所以这里如实说没选，盘面也照实说。 */
    case UI_BIRTH_GENDER:
        snprintf(b, n, "%s", a->gender == SF_BAZI_GENDER_MALE ? "男"
                          : a->gender == SF_BAZI_GENDER_FEMALE ? "女" : "未选");
        break;
    case UI_BIRTH_OFFSET: {
        /* ⚠️ 显示**生效值**（`ui_app_bazi_offset`），不是用户存的那个：
         * 时区被八字历法锁住时生效值是 +480，值列照实说 —— 跟万年历
         * 「显示时区」行锁上时显示 +8:00 是同一个做法。 */
        const int m = ui_app_bazi_offset(a);
        snprintf(b, n, "UTC%c%d:%02d", m < 0 ? '-' : '+',
                 (m < 0 ? -m : m) / 60, (m < 0 ? -m : m) % 60);
        break;
    }
    case UI_BIRTH_MERIDIAN:
        /* 显示**生效值**，跟设置页「经度」那一行同一个格式化（2 位小数）——
         * 0.01° 那一档不动小数点后两位就等于没动。（这一行的 `bazi_meridian_set
         * 为 0` 时是播种值 `偏移/4`，照实显示它，不装作"没设过"。） */
        fmt_deg(b, n, a->bazi_cal.calendar_meridian_deg);
        break;
    default: snprintf(b, n, "?"); break;
    }
}

static void page_birth(const ui_app *a, const ui_theme *t)
{
    /* ⚠️ 这一页有 **8 行**（全站最长的一页），直接借用 UI_LIST_ROW_H（17 px）
     * 会算成 13 + 8×17 = 149 ≫ 128，**最后两行压到页码点上**。
     * 13 px 是 8 行放得下的最大整数行高：(128 − 13 − 10) / 8 = 13.1。
     * 字号是 ui_font_s（行高 12）—— 13 的框装 12 的字，上下各剩 0.5 px，
     * 是「刚好不挤」的极限；再往里加行就得动滚动条了（这一页没有滚动）。 */
    const int row_h = 13;
    const int top   = ui_px_y(UI_LIST_TOP);

    /* 顶栏右上：三件事抢这一格，**优先级从高到低**：
     *
     *   1. 「无此日期」—— 警告必须有地方说，而这一页 8 行已经把纵向占满，
     *      塞在行间会压到页码点。而且这个警告**恰恰是在日/月那两行调值时
     *      造出来的**，那正是最需要看见它的一刻，不能让给步长。
     *   2. 「方向 + 步长」（如 `+5日`）—— 跟设置页同一个理由：方向和步长是
     *      两个独立按键（K8 / K7），不显示的话你不知道按 K5 会动多少、往哪
     *      边动，而 K7/K8 按下去除了这里没有任何反馈。
     *   3. 完整的出生日期 —— 光标落在**没有步长的行**（性别）上时才轮到它。
     *      平时它其实是冗余的：年/月/日三个分量就在下面三行里各占一行。 */
    char right[32];
    const double mag = ui_app_step_magnitude(a);
    if (!a->birth_valid)
        snprintf(right, sizeof right, "无此日期");
    else if (mag > 0.0)
        /* %g 而不是 %d：步长表的类型是 double，将来真要出现小数档
         * （像设置页的 0.01° 那样）也不会被截成 0 */
        snprintf(right, sizeof right, "%c%g%s", a->step_plus ? '+' : '-',
                 mag, BIRTH_STEP_UNIT[a->birth_row]);
    else
        snprintf(right, sizeof right, "%04d-%02d-%02d",
                 I32(a->birth.year), I32(a->birth.month), I32(a->birth.day));
    draw_topbar(a, t, "出生信息", right);

    const int xl = ui_px_x(0.05f);
    const int xr = ui_px_x(0.95f);

    /* 时区被八字历法锁住时，这一行按万年历设置页那套画法：**只描框不反色** +
     * 标签换成「时区锁定」。光标压在失效行上时若照样反色，就完全看不出它
     * 按不动了（见 page_settings 里那段三种状态的说明）。 */
    for (int row = 0; row < UI_BIRTH_ROW_COUNT; row++) {
        const int ry = top + row * row_h;
        const int sel = (row == a->birth_row);
        const int inert = ui_app_birth_inert(a, row);
        const int solid = sel && !inert;
        char val[32];
        char label[16];

        if (solid)    ui_fill ((ui_rect){ 0, ry, UI_W, row_h }, t->accent);
        else if (sel) ui_frame((ui_rect){ 0, ry, UI_W, row_h }, t->accent);

        /* 时区锁定时换标签：「出生地时区」→「时区锁定」。上游那边是在表单
         * 底下印一行注（「中国历史历法仅按 UTC+8 生效，时区已锁定」），
         * 160 px 没地方印注 —— 把意思塞进标签，宽度一个字没变。
         * ⚠️ 这跟万年历「显示时区」→「时区锁定」是**两处不同的锁**：
         * 判据来自两套独立的历法口径。 */
        snprintf(label, sizeof label, "%s",
                 (inert && row == UI_BIRTH_OFFSET) ? "时区锁定"
                                                   : BIRTH_LABELS[row]);

        birth_value(a, row, val, sizeof val);

        uint16_t lc, vc;
        if (solid)    { lc = t->on_accent; vc = t->on_accent; }
        else if (sel) { lc = t->fg;        vc = t->rule; }
        else          { lc = t->dim;       vc = inert ? t->rule : t->fg; }

        const int th = ui_font_s.line;
        const int yy = ry + (row_h - th) / 2;
        ui_text(xl, yy, &ui_font_s, lc, label);
        ui_text(xr - ui_text_w(&ui_font_s, val), yy, &ui_font_s, vc, val);
    }
}

/* ==================================================================
 * 八字 · 设置（六行）
 *
 * **跟万年历的设置页是两套状态**：这一页改的是 `a->bazi_cal` / `bazi_rat_hour`，
 * 那一页改的是 `a->cal_cfg` / `rat_hour`。屏幕长得几乎一样是刻意的 ——
 * 同一个概念在两处是同一个操作，学一次到处用；不同的只是**改的是谁的**。
 *
 * 行序 = 上游八字表单的字段顺序（clockMode → calendarMode → dayBoundary →
 * pillarHistoricalMode → ratHourMode，经度跟在日界后面因为它是它的参数）。
 * ================================================================== */
static const char *const BSET_LABELS[UI_BSET_COUNT] = {
    "排盘钟表", "历法", "定气日界", "节气边界", "早晚子时"
};

static void bset_value(const ui_app *a, int row, char *b, size_t n)
{
    switch (row) {
    case UI_BSET_TIME_BASIS:
        snprintf(b, n, "%s", BASIS_NAMES[a->bazi_time_basis]);
        break;
    case UI_BSET_CAL_MODE:
        snprintf(b, n, "%s", CALMODE_NAMES[a->bazi_cal.mode]);
        break;
    case UI_BSET_BOUNDARY:
        snprintf(b, n, "%s", BOUNDARY_NAMES[a->bazi_cal.day_boundary_mode]);
        break;
    case UI_BSET_PILLAR_HIST:
        snprintf(b, n, "%s", PILLAR_HIST_NAMES[a->bazi_cal.pillar_historical_mode]);
        break;
    case UI_BSET_RAT_HOUR:
        snprintf(b, n, "%s", RAT_NAMES[a->bazi_rat_hour]);
        break;
    default:
        b[0] = '\0';
        break;
    }
}

/* 步长的单位：**这一页整页都是枚举**（没有数值行），所以全是空串 ——
 * 顶栏那一格于是总落到「显示虚拟时刻」那一支。经度搬走之前这里是
 * `"", "", "", "°", "", ""`。 */
static const char *const BSET_UNIT[UI_BSET_COUNT] = {
    "", "", "", "", ""
};

static void page_bset(const ui_app *a, const ui_theme *t)
{
    /* 顶栏右上跟万年历设置页同一条规矩：光标这一行有步长就显示
     * 「方向 + 步长」（`+0.01°`），否则回到显示虚拟时刻。
     * ⚠️ 虚拟时刻在这一页是**万年历那个钟**（八字这边没有「现在」的概念 ——
     * 出生读数跟此刻无关），所以它显示的是「现在几点」，不是「排的是哪一刻」。
     * 有点串味，但这一格不显示步长时总得有东西填，而它是全局唯一的每秒读数。 */
    /* 24 不是 16：`%02d:%02d` 最坏 11×2+1 = 23（GCC 看不到时分是 0..59）。
     * 实际内容「08:00」5 个字符。 */
    char right[24];
    const double mag = ui_app_step_magnitude(a);
    if (mag > 0.0)
        snprintf(right, sizeof right, "%c%g%s", a->step_plus ? '+' : '-',
                 mag, BSET_UNIT[a->bset_row]);
    else
        snprintf(right, sizeof right, "%02d:%02d",
                 I32(a->virtual_time.hour), I32(a->virtual_time.minute));
    draw_topbar(a, t, "八字设置", right);

    const int row_h = ui_px_y(UI_LIST_ROW_H);
    const int top   = ui_px_y(UI_LIST_TOP);
    const int xl    = ui_px_x(0.05f);
    const int xr    = ui_px_x(0.95f);

    for (int row = 0; row < UI_BSET_COUNT; row++) {
        char val[32];
        const int ry = top + row * row_h;
        const int inert = ui_app_bset_inert(a, row);
        const int sel   = (row == a->bset_row);
        const int solid = sel && !inert;

        if (solid)    ui_fill ((ui_rect){ 0, ry, UI_W, row_h }, t->accent);
        else if (sel) ui_frame((ui_rect){ 0, ry, UI_W, row_h }, t->accent);

        uint16_t lc, vc;
        if (solid)    { lc = t->on_accent; vc = t->on_accent; }
        else if (sel) { lc = t->fg;        vc = t->rule; }
        else          { lc = t->dim;       vc = inert ? t->rule : t->fg; }

        bset_value(a, row, val, sizeof val);

        const int th = ui_font_s.line;
        const int yy = ry + (row_h - th) / 2;
        ui_text(xl, yy, &ui_font_s, lc, BSET_LABELS[row]);
        ui_text(xr - ui_text_w(&ui_font_s, val), yy, &ui_font_s, vc, val);
    }
}

/* ==================================================================
 * 八字 · 运限（五级联动，一行一层）
 *
 * 五层 × 19 px + 顶栏 13 = 108，页码点在 121 —— 放得下，不用砍行。
 *
 * 每行三段（宽度都是实测的，ui_font_s = latin12/cjk12）：
 *   层名 24 │ 干支 24 │ …右对齐… 副标签
 * ⚠️ 「流年小运」是 5 个汉字 = 48 px，那行的副标签就只写年份（28 px），
 * 不然 4+48+8+24+69 = 153 会顶到边（152 是可用宽度）。
 * ================================================================== */
static const char *const LUCK_LEVEL_NM[UI_LUCK_LEVELS] = {
    "大运", "流年", "流月", "流日", "流时"
};
/* 流时那层的十二时辰名。⚠️ 0 = 子时（**23 时起**，不是 0 时起）。 */
static const char *const HOUR_NM[12] = {
    "子时", "丑时", "寅时", "卯时", "辰时", "巳时",
    "午时", "未时", "申时", "酉时", "戌时", "亥时"
};

static void page_luck(const ui_app *a, const ui_theme *t)
{
    /* ------------------------------------------------------------------
     * 两处「这一页根本没东西可给你看」——**必须先说清楚再说别的**。
     *
     * 不拦的话这一页就是五行 `—`：八个键只有一个有反应，看着像死机，
     * 而真正的原因（性别没选 / 出生日期不合法）一个字都没写在屏上。
     * 跟排盘页 `page_chart` 开头那段是同一个写法、**同一套措辞**：
     * 居中的大字说「缺什么」，小字说「去哪补」。
     * ------------------------------------------------------------------ */
    if (!a->chart_ok) {                /* 盘本身就算不出来 */
        draw_topbar(a, t, "运限", "");
        ui_text_in(UI_BOX(0.0f, 0.32f, 1.0f, 0.20f), UI_HCENTER, UI_VCENTER,
                   &ui_font_m, t->dim,
                   a->birth_valid ? "超出有效范围" : "出生信息有误");
        ui_text_in(UI_BOX(0.0f, 0.50f, 1.0f, 0.16f), UI_HCENTER, UI_VCENTER,
                   &ui_font_s, t->dim,
                   a->birth_valid ? "（约 ±8000 年）" : "回出生信息页改一下");
        return;
    }
    if (a->dayun_n <= 0) {             /* 有盘，但大运排不出来 → 链无从谈起 */
        const int nog = (a->gender != SF_BAZI_GENDER_MALE
                         && a->gender != SF_BAZI_GENDER_FEMALE);
        draw_topbar(a, t, "运限", "");
        ui_text_in(UI_BOX(0.0f, 0.32f, 1.0f, 0.20f), UI_HCENTER, UI_VCENTER,
                   &ui_font_m, t->dim, nog ? "先选性别" : "大运不可用");
        ui_text_in(UI_BOX(0.0f, 0.50f, 1.0f, 0.16f), UI_HCENTER, UI_VCENTER,
                   &ui_font_s, t->dim,
                   nog ? "造化顺逆要按它定" : "起运那一步没解出来");
        return;
    }

    /* 顶栏右上：**K5 会往哪边走**（＋/−，跟设置页一个写法）＋ 当前是这一层的
     * 第几张。符号在前面是因为它跟设置页的 `+1°` 同一个位置、同一个意思 ——
     * K8（z）一按就翻过来。没展开任何层时就只剩一个符号（K5 这会儿没得动）。 */
    char right[16] = "";
    const int act = ui_app_luck_active(a);
    if (act >= 0)
        snprintf(right, sizeof right, "%c%d/%d", a->step_plus ? '+' : '-',
                 a->luck_sel[act] + 1, ui_app_luck_count(a, act));
    else
        snprintf(right, sizeof right, "%c", a->step_plus ? '+' : '-');

    char title[32];
    snprintf(title, sizeof title, "运限 · %s",
             a->gender == SF_BAZI_GENDER_MALE ? "乾造"
           : a->gender == SF_BAZI_GENDER_FEMALE ? "坤造" : "未选");
    draw_topbar(a, t, title, right);

    const int row_h = 19;
    const int top   = ui_px_y(0.10f);
    const int xl    = ui_px_x(0.025f);
    const int xr    = ui_px_x(0.975f);

    for (int lv = 0; lv < UI_LUCK_LEVELS; lv++) {
        const int ry = top + lv * row_h;
        const int n  = ui_app_luck_count(a, lv);

        /* 「这一层开着没有」由 luck_open 说了算：展开到第 k 层才有它 */
        const int open = (lv < a->luck_open) && n > 0;
        const int sel  = open && (lv == act);
        if (sel) ui_fill((ui_rect){ 0, ry, UI_W, row_h }, t->accent);

        const uint16_t lc = sel ? t->on_accent : t->dim;
        const int th = ui_font_s.line;
        const int yy = ry + (row_h - th) / 2;

        /* 层名。⚠️ 童限选中时第 1 层叫「流年小运」（上游 :607 也是这么改的） */
        const int childhood0 = (n > 0
            && ui_app_luck_count(a, UI_LUCK_DAYUN) > a->dayun_n
            && a->luck_sel[UI_LUCK_DAYUN] == 0);
        const char *ln = LUCK_LEVEL_NM[lv];
        if (lv == UI_LUCK_YEAR && childhood0) ln = "流年小运";
        ui_text(xl, yy, &ui_font_s, lc, ln);

        if (!open) {                       /* 没展开的层：一横了事 */
            char dash[8];
            snprintf(dash, sizeof dash, "—");
            ui_text(xl + ui_text_w(&ui_font_s, ln) + 8, yy, &ui_font_s, t->rule, dash);
            ui_text(xr - ui_text_w(&ui_font_s, dash), yy, &ui_font_s, t->rule, dash);
            continue;
        }

        ui_luck_entry e;
        if (!ui_app_luck_get(a, lv, a->luck_sel[lv], &e)) continue;

        /* 干支（童限那张写「童限」）。染色规则跟柱盘一致：按本气五行。
         * ⚠️ 起点**按层名的实际宽度算**：两字层名是 24，而「流年小运」是 48 ——
         * 写死 `xl + 32` 的话童限那一行的「流年小运」会直接压在干支上
         * （渲出来才看见：`流年小庚辰`）。 */
        {
            const int x = xl + ui_text_w(&ui_font_s, ln) + 8;
            if (e.is_childhood) {
                ui_text(x, yy, &ui_font_s, sel ? t->on_accent : t->fg, "童限");
            } else {
                const uint8_t st = (uint8_t)(e.pillar >> 4);
                const uint8_t br = (uint8_t)(e.pillar & 0x0f);
                uint8_t el = 0;
                if (!sel) (void)sf_bazi_stem_element(st, &el);
                ui_text(x, yy, &ui_font_s,
                        sel ? t->on_accent : t->wuxing[el],
                        SF_HEAVENLY_STEMS[st]);
                if (!sel) (void)sf_bazi_branch_element(br, &el);
                ui_text(x + 12, yy, &ui_font_s,
                        sel ? t->on_accent : t->wuxing[el],
                        SF_EARTHLY_BRANCHES[br]);
            }
        }

        /* 副标签（右对齐）：各层说各层的话 */
        /* ⚠️ 48 不是 24：`%d` 单个最坏 11 字节，四个 11×4+3 = 47。
         *    实际内容最长是「3/8 19-28」10 个字符，但 GCC 看不到定义域。 */
        char sub[48] = "";
        switch (lv) {
        case UI_LUCK_DAYUN:
            if (e.is_childhood) snprintf(sub, sizeof sub, "1～%d岁", I32(e.age_end));
            else snprintf(sub, sizeof sub, "%d/%d %d-%d",
                          a->luck_sel[lv] - (childhood0 ? 1 : 0) + 1,
                          a->dayun_n, I32(e.age), I32(e.age_end));
            break;
        case UI_LUCK_YEAR:
            /* 童限那层只写年份（层名占掉了 48 px），普通那层写年份 + 岁数 */
            if (childhood0) snprintf(sub, sizeof sub, "%d", I32(e.civil_year));
            else snprintf(sub, sizeof sub, "%d %d岁",
                          I32(e.civil_year), I32(e.age));
            break;
        case UI_LUCK_MONTH:
            snprintf(sub, sizeof sub, "%s %d/%d",
                     SF_EARTHLY_BRANCHES[(a->luck_sel[lv] + 2) % 12],
                     a->luck_sel[lv] + 1, 12);
            break;
        case UI_LUCK_DAY: {
            int32_t yy2 = 0, mo2 = 0, dd2 = 0;
            sf_solar_date_from_day_number(e.jdn, &yy2, &mo2, &dd2);
            snprintf(sub, sizeof sub, "%d/%d %d/%d",
                     I32(mo2), I32(dd2), a->luck_sel[lv] + 1, n);
            break;
        }
        default: {                         /* 流时：0 = 子时 = 23 时起 */
            snprintf(sub, sizeof sub, "%s %d时起",
                     HOUR_NM[a->luck_sel[lv]], (a->luck_sel[lv] * 2 + 23) % 24);
            break;
        }
        }
        ui_text(xr - ui_text_w(&ui_font_s, sub), yy, &ui_font_s,
                sel ? t->on_accent : t->rule, sub);
    }
}

/* ==================================================================
 * 八字 · 盘
 *
 * 列数随 OK **递进**：四柱(4) → +大运(5) → +流年(6)。
 *
 * ⚠️ **列宽是这一页的硬约束**（已算过）：
 *    4 列 40 px / 5 列 32 px / 6 列 26.6 px
 * 6 列时 12 px 的字只放得下**两个** —— 「十神」刚好（24 px），而藏干
 * 三个字要 36 px 放不下，所以藏干那行走 8 px 的 ui_font_xs（3×8 = 24 px）。
 * ================================================================== */
typedef struct {
    /* ⚠️ 柱名缓冲**必须放得下 6 字节 + NUL**：「大运」是 2 个汉字 = 6 字节。
     * 给 6 会把它切在半路，剩半个 UTF-8 序列解码成 U+8FC0 那种垃圾 ——
     * 而 `--dump` 只会报 12 个漏字、屏幕上是一个空心框，很难联想到是
     * 「缓冲小了一字节」。流年那列放年份（4 位数字），这个尺寸也够。 */
    char      label[12];
    sf_ganzhi pillar;
    uint8_t   ten_god;
    uint8_t   is_day_master;   /* 日柱显示「日主」而不是「比肩」 */
    uint8_t   n_hidden;
    uint8_t   hidden[SF_BAZI_HIDDEN_STEM_CAPACITY];
    uint8_t   hidden_god[SF_BAZI_HIDDEN_STEM_CAPACITY];
    uint8_t   life_stage;
    uint8_t   stem_el, branch_el;
} chart_col;

static void col_fill(chart_col *c, sf_ganzhi pillar, const sf_bazi_chart *ch,
                     int is_day, const char *label)
{
    const uint8_t dm  = ch->day_master;
    const uint8_t st  = (uint8_t)(pillar >> 4);
    const uint8_t br  = (uint8_t)(pillar & 0x0f);
    int n = 0;

    snprintf(c->label, sizeof c->label, "%s", label);
    c->pillar = pillar;
    (void)sf_bazi_ten_god(dm, st, &c->ten_god);
    c->is_day_master = (uint8_t)is_day;

    if (sf_bazi_hidden_stems(br, c->hidden, &n) != 0) n = 0;
    c->n_hidden = (uint8_t)n;
    for (int i = 0; i < n; i++)
        (void)sf_bazi_ten_god(dm, c->hidden[i], &c->hidden_god[i]);

    (void)sf_bazi_life_stage(dm, br, ch->earth_palace_mode, &c->life_stage);
    (void)sf_bazi_stem_element(st, &c->stem_el);
    (void)sf_bazi_branch_element(br, &c->branch_el);
}

static void page_chart(const ui_app *a, const ui_theme *t)
{
    /* ⚠️ 48 不是 32：`%d/%d %d-%d` 四个 %d 的最坏情况是 11×4+3 = 47，
     * GCC 看不到定义域，IDF 下按错误算。实际内容最长「3/8 19-28」。 */
    char right[48] = "";

    if (!a->chart_ok) {
        draw_topbar(a, t, "八字", "");
        ui_text_in(UI_BOX(0.0f, 0.32f, 1.0f, 0.20f), UI_HCENTER, UI_VCENTER,
                   &ui_font_m, t->dim,
                   a->birth_valid ? "超出有效范围" : "出生信息有误");
        ui_text_in(UI_BOX(0.0f, 0.50f, 1.0f, 0.16f), UI_HCENTER, UI_VCENTER,
                   &ui_font_s, t->dim,
                   a->birth_valid ? "（约 ±8000 年）" : "回出生信息页改一下");
        return;
    }

    /* --- 组列：左边是**链上最深的两层流运柱**（从深到浅），然后本命四柱 ---
     * 与网页的 #flow-pillars 排在 #natal-pillars **之前**同一个顺序。
     *
     * ⚠️ 只有两格：6 列是 160px 的硬上限（7 列 = 22.8px/列，而「十神」两个字
     * 要 24px、藏干三个字也要 24px，都会撞）。所以流月/流日/流时**不上柱盘**，
     * 它们只在「运限」页那几行里各占一行。 */
    chart_col cols[6];
    int ncol = 0;
    static const char *const LUCK_NM[UI_LUCK_LEVELS] = {
        "大运", "流年", "流月", "流日", "流时"
    };

    for (int slot = 0; slot < 2; slot++) {     /* slot 0 = 最深 */
        int lv = -1;
        ui_luck_entry e;
        if (!ui_app_luck_board(a, slot, &lv, &e)) continue;
        char lb[12];
        if (lv == UI_LUCK_YEAR)      snprintf(lb, sizeof lb, "%d", I32(e.civil_year));
        else if (lv == UI_LUCK_DAYUN && e.is_childhood) snprintf(lb, sizeof lb, "小运");
        else snprintf(lb, sizeof lb, "%s", LUCK_NM[lv]);
        col_fill(&cols[ncol++], e.pillar, &a->chart, 0, lb);
    }

    {
        static const char *const NM[4] = { "年", "月", "日", "时" };
        const sf_ganzhi natal[4] = {
            a->chart.pillars.primary.year,  a->chart.pillars.primary.month,
            a->chart.pillars.primary.day,   a->chart.pillars.primary.hour,
        };
        for (int i = 0; i < 4; i++)
            col_fill(&cols[ncol++], natal[i], &a->chart, i == 2, NM[i]);
    }

    /* --- 顶栏：左「八字 · 乾造/坤造」，右给当前大运的进度 ---
     *
     * ⚠️ 没选性别时**如实说「未选」**，右边提示去选。原来这里是
     * `gender == MALE ? 乾造 : 坤造` —— -1 掉进 else，于是没选性别的盘
     * 被写成**坤造**，而大运/流年那两列又因为算不出顺逆而上不来：屏上
     * 是一张假的坤造四柱。当初把性别默认成「未选」要防的就是这个，
     * 结果在盘面这头漏了。 */
    const int no_gender = (a->gender != SF_BAZI_GENDER_MALE
                           && a->gender != SF_BAZI_GENDER_FEMALE);
    if (!no_gender && a->dayun_n > 0) {
        /* 第几步 · 虚岁区间。⚠️ 童限选中时第 0 层的柱换成了小运，
         * 但进度说的还是**那一步大运**（上游的「/8」也是这么数的）。 */
        const int ch = (a->luck_sel[UI_LUCK_DAYUN] == 0
                        && ui_app_luck_count(a, UI_LUCK_DAYUN) > a->dayun_n);
        const int k  = a->luck_sel[UI_LUCK_DAYUN] - (ch ? 1 : 0);
        const int ki = (k < 0) ? 0 : (k >= a->dayun_n ? a->dayun_n - 1 : k);
        const sf_bazi_dayun_entry *d = &a->dayun[ki];
        if (ch && k < 0)
            snprintf(right, sizeof right, "童限 %d岁前",
                     I32(d->start_virtual_age) - 1);
        else
            snprintf(right, sizeof right, "%d/%d %d-%d",
                     ki + 1, a->dayun_n, I32(d->start_virtual_age),
                     I32(d->end_virtual_age));
    } else if (no_gender) {
        snprintf(right, sizeof right, "先选性别");
    } else if (a->luck_open >= 1) {
        /* 有性别却还是没有大运 → 起运/大运那一步解不出来（出生时刻落在
         * 解算范围外那类）。同样是「按了 K5 什么都不多」，也得说一声。 */
        snprintf(right, sizeof right, "大运不可用");
    }
    {
        char title[32];
        snprintf(title, sizeof title, "八字 · %s",
                 a->gender == SF_BAZI_GENDER_MALE ? "乾造"
               : a->gender == SF_BAZI_GENDER_FEMALE ? "坤造" : "未选");
        draw_topbar(a, t, title, right);
    }

    /* --- 七行。行高不一律：干支那两行用 16 px 的字，占 17 px --- */
    static const int ROWH[7] = { 13, 13, 17, 17, 11, 13, 13 };
    int ry = ui_px_y(0.10f);
    const int cw = UI_W / ncol;

    for (int r = 0; r < 7; r++) {
        const int th = ROWH[r];
        const int ty = ry;
        for (int i = 0; i < ncol; i++) {
            const chart_col *c = &cols[i];
            const int cx = i * cw + cw / 2;

            switch (r) {
            case 0:   /* 柱名 */
                ui_text_c(cx, ty, &ui_font_s, t->dim, c->label);
                break;
            case 1:   /* 十神 —— 日柱显示「日主」 */
                if (c->is_day_master) ui_text_c(cx, ty, &ui_font_s, t->accent, "日主");
                else {
                    const char *nm = sf_bazi_ten_god_name(c->ten_god);
                    ui_text_c(cx, ty, &ui_font_s, t->fg, nm ? nm : "?");
                }
                break;
            case 2:   /* 干：按**本气五行**染色，16 px */
            case 3: { /* 支 */
                const uint8_t k = (r == 2) ? (uint8_t)(c->pillar >> 4)
                                           : (uint8_t)(c->pillar & 0x0f);
                const uint8_t el = (r == 2) ? c->stem_el : c->branch_el;
                const char *nm = (r == 2) ? SF_HEAVENLY_STEMS[k] : SF_EARTHLY_BRANCHES[k];
                ui_text_c(cx, ty, &ui_font_m,
                          (el <= SF_WUXING_FIRE) ? t->wuxing[el] : t->fg, nm);
                break;
            }
            case 4: {  /* 藏干：8 px，只画**实际个数**那几个 */
                /* ⚠️ 缓冲按**字节**算：一个汉字 3 字节，最多 3 个藏干 = 9 字节
                 * + NUL。给 16 是留余量，不是为了好看。 */
                char s[16] = "";
                for (int j = 0; j < c->n_hidden; j++) {
                    const char *nm = SF_HEAVENLY_STEMS[c->hidden[j]];
                    const size_t l = strlen(s);
                    /* ⚠️ 余量要**显式判**：写成 snprintf(s+l, sizeof s - l, ...)
                     * 在 l 顶到 sizeof s 时是往 0 字节的区域写字，GCC 报
                     * 「可能在最后一个格式符之前截断」（IDF 下是错误）。
                     * CAPACITY 是 3、每个藏干 3 字节，本来就装得下 ——
                     * 这一句是防以后 CAPACITY 变大，不是现在的需要。 */
                    if (nm && l + strlen(nm) < sizeof s) strcpy(s + l, nm);
                }
                ui_text_c(cx, ty, &ui_font_xs, t->dim, s);
                break;
            }
            case 5: { /* 星运（十二长生） */
                const char *nm = sf_bazi_life_stage_name(c->life_stage);
                ui_text_c(cx, ty, &ui_font_s, t->dim, nm ? nm : "?");
                break;
            }
            default: break;
            }
        }
        ry += th;
    }

    /* --- 最后一行：五行统计，**整行居中、逐段染色** ---
     * ⚠️ 口径：只数四柱的**天干 + 地支八个本气**，不含藏干 —— 含藏干的话
     * 同一个字会被数好几遍，各家口径也不一。这条写进 sim/README。 */
    {
        int cnt[5] = { 0, 0, 0, 0, 0 };
        const sf_ganzhi natal[4] = {
            a->chart.pillars.primary.year,  a->chart.pillars.primary.month,
            a->chart.pillars.primary.day,   a->chart.pillars.primary.hour,
        };
        for (int i = 0; i < 4; i++) {
            uint8_t e = 0;
            if (sf_bazi_stem_element((uint8_t)(natal[i] >> 4), &e) == 0) cnt[e]++;
            if (sf_bazi_branch_element((uint8_t)(natal[i] & 0x0f), &e) == 0) cnt[e]++;
        }
        /* 逐段量宽度再居中 —— ui_text 不支持一段一色，只能自己排 */
        char seg[5][12];
        int w[5], total = 0;
        for (int e = 0; e < 5; e++) {
            snprintf(seg[e], sizeof seg[e], "%s%d", sf_bazi_wuxing_name((uint8_t)e), cnt[e]);
            w[e] = ui_text_w(&ui_font_s, seg[e]);
            total += w[e];
        }
        int x = (UI_W - total) / 2;
        for (int e = 0; e < 5; e++) {
            ui_text(x, ry, &ui_font_s, t->wuxing[e], seg[e]);
            x += w[e];
        }
    }
}

/* ------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------ */
void ui_render(ui_app *a)
{
    /* 页面数据按需准备。tick 只置失效标记；直接 render 的宿主也不会看到旧数据。 */
    ui_app_prepare_visible(a);
    const ui_theme *t = a->night ? &ui_theme_night : &ui_theme_day;
    g_utc_offset_min = a->cal_cfg.utc_offset_minutes;

    ui_clear(t->bg);

    /* ⚠️ **先看 feature 再看 page** —— page 是两个功能共用的裸 int，
     * UI_BAZI_PAGE_CHART(1) 跟 UI_PAGE_LUNAR(1) 是同一个值。 */
    if (a->feature < 0) {
        page_pick(a, t);
    } else if (a->feature == UI_FEATURE_BAZI) {
        switch (a->page) {
        case UI_BAZI_PAGE_BIRTH:    page_birth(a, t); break;
        case UI_BAZI_PAGE_CHART:    page_chart(a, t); break;
        case UI_BAZI_PAGE_LUCK:     page_luck(a, t);  break;
        case UI_BAZI_PAGE_SETTINGS: page_bset(a, t);  break;
        default: break;
        }
    } else {
        switch (a->page) {
        case UI_PAGE_TODAY:    page_today(a, t);    break;
        case UI_PAGE_LUNAR:    page_lunar(a, t);    break;
        case UI_PAGE_TERMS:    page_terms(a, t);    break;
        case UI_PAGE_PHASES:   page_phases(a, t);   break;
        case UI_PAGE_SUN:      page_sun(a, t);      break;
        case UI_PAGE_SETTINGS: page_settings(a, t); break;
        default: break;
        }
    }
    draw_page_dots(a, t);

    if (a->flash > 0) a->flash--;
}
