/* ==================================================================
 * main.c —— 模拟器入口
 *
 * 节奏由这里控制：抽事件 → 推进时钟 → 画一帧 → 交给后端显示。
 * 时间来源也在这里（下面的 sim_now_jd_tt）—— ui_*.c 那一层刻意不含
 * 「取当前时间」，好让它原样搬去 MCU 换成 RTC。
 * ================================================================== */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "ui.h"
#include "solar_fast.h"             /* --dump 的月相取点要用 sf_solve_lunar_phase */

#define SIM_FRAME_MS 33                 /* ~30 fps */
#define SIM_DEFAULT_SCALE 3

static uint16_t g_fb[UI_H][UI_W];       /* 真机上这就是 DMA 出去的缓冲 */

static double g_offset_days;            /* 时间旅行偏移，只影响显示 */
static int    g_view_year;              /* --year：气/朔两页看哪一年，0 = 当前年 */

/* 当前时刻。系统钟给的是 **UTC**，转成 TT —— solar_fast 全部按 TT。
 *
 * ⚠️ 转换用库的 sf_utc_to_tt（闰秒表），不要自己拿 ΔT 多项式凑：这里从前
 * 用的是 sim 自带那份 Espenak–Meeus，跟库的 IERS 年表差 5.9 秒，于是屏上
 * 的钟一直快 6 秒 —— 因为显示那边减的是库的 ΔT，一加一减就对不上了。 */
static double sim_now_jd_tt(void)
{
    time_t t = time(NULL);
    struct tm g;
    gmtime_r(&t, &g);
    const double jd_utc = ui_jd_from_calendar(g.tm_year + 1900, g.tm_mon + 1,
                                              g.tm_mday, g.tm_hour, g.tm_min,
                                              (double)g.tm_sec);
    return sf_utc_to_tt(jd_utc) + g_offset_days;
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void usage(const char *argv0)
{
    printf("用法：%s [--scale N] [--day] [--sdl] [--at 日期] [--dump 目录]\n"
           "  --scale N   初始放大倍数 1..8（默认 %d，用 - / = 随时改）\n"
           "  --day       用白天主题启动（默认夜）\n"
           "  --sdl       用 SDL2 后端（需要 brew install sdl2）\n"
           "  --at 日期   从指定时刻开始，格式 2026-09-19 或 2026-09-19T06:41\n"
           "  --page N    开在哪一页。万年历 0..5（默认 0 = 主页；5 = 设置）；\n"
           "              八字 0 = 出生信息、1 = 盘、2 = 运限、3 = 八字设置\n"
           "  --feature N 开在哪个功能 -1 = 选择页、0 = 万年历、1 = 八字\n"
           "  --year N    气/朔两页看哪一年（默认当前年）\n"
           "  --dump 目录 不开窗口，渲染全部页面 + 8 个月相成 PPM 后退出\n"
           "  --lat 纬度   观者纬度，北正（默认 39.9042）\n"
           "  --lon 经度   观者经度，东正（默认 116.4074）\n"
           "  --utc 分钟   显示时区偏移，分钟（默认 480 = UTC+8）\n"
           "              ⚠️ 默认历法是「中国古历」，那把锁会把**生效值**钉在\n"
           "                 +480 —— 想真的换时区，先在设置页把历法改成别的。\n"
           "  --selftest  开窗出一帧就退出，用来验证后端能起来\n",
           argv0, SIM_DEFAULT_SCALE);
}

/* ------------------------------------------------------------------
 * 无头渲染：把画面写成 PPM，好在没有显示器（或不想开窗口）时检查版式。
 * PPM 是几行代码的事，不值得为截图引一个 PNG 库。
 * ------------------------------------------------------------------ */
static void write_ppm(const char *path, const uint16_t *buf, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "写不了 %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        const uint16_t c = buf[i];
        const unsigned r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
        fputc((int)((r << 3) | (r >> 2)), f);
        fputc((int)((g << 2) | (g >> 4)), f);
        fputc((int)((b << 3) | (b >> 2)), f);
    }
    fclose(f);
    printf("  %s\n", path);
}

/* 渲染一帧并写盘。pressed 用来点亮相应的按钮（默认点 OK，好看清新色）。
 * set_row 只对设置页有意义，-1 表示不动；live=1 先把设置页切到「所有行都
 * 活着」的那套（当地天文 + 按当地经度），好跟默认那套变暗的对照着看。 */
/* feature 与 page 是**两级**：feature < 0 是功能选择页。默认万年历，
 * 只有要拍八字那几页时才传别的 —— 多数调用点写 -2 表示「不变」。 */
#define SHOT_FEATURE_KEEP (-2)

static void shot(const char *path, uint16_t *win, int scale, double jd,
                 int night, int page, uint32_t pressed, int set_row, int live,
                 int feature)
{
    ui_app app;
    ui_app_init(&app, jd);
    if (night) ui_app_button(&app, UI_BTN_ACT, 1);
    if (feature != SHOT_FEATURE_KEEP) app.feature = feature;
    else                               app.feature = UI_FEATURE_CALENDAR;
    app.page = page;
    if (g_view_year) ui_app_set_view_year(&app, g_view_year);
    if (set_row >= 0 || live) {
        /* ⚠️ 别直接写 app.set_row —— 那样绕过了 settings_keep_visible，
         * 光标跑到屏外也不会滚，渲出来的图看着像「没滚动」。按真按钮走。 */
        app.page = UI_PAGE_SETTINGS;
        if (live) {
            /* ⚠️ 调值是 K5（确定），不是 K4 —— 设置页的左右键跟其它页一样
             * 是翻页，按下去会把 page 翻走。 */
            app.set_row = UI_SET_CAL_MODE;
            ui_app_button(&app, UI_BTN_OK, 1);           /* → 当地天文 */
            app.set_row = UI_SET_BOUNDARY;
            ui_app_button(&app, UI_BTN_OK, 1);           /* → 按当地经度 */
        }
        app.set_row = 0;
        for (int k = 0; k < set_row; k++) ui_app_button(&app, UI_BTN_DOWN, 1);
    }
    ui_app_tick(&app, jd);            /* init 那次 rebuild 的档位随 page 无关，重来一次 */
    ui_render(&app);
    sim_frame_compose(win, scale, ui_fb(), pressed);
    write_ppm(path, win, sim_frame_width(scale), sim_frame_height(scale));
}

/* 功能选择页。pick 是主还是副 —— 两张都要拍：反色块在纸白底上够醒目，
 * 但「未选中」那半边的细框在夜间主题下够不够看，只有渲出来才知道。 */
static void shot_pick(const char *path, uint16_t *win, int scale, double jd,
                      int night, int pick)
{
    ui_app app;
    ui_app_init(&app, jd);
    if (night) ui_app_button(&app, UI_BTN_ACT, 1);
    app.feature = -1;
    app.pick = 0;
    for (int k = 0; k < pick; k++) ui_app_button(&app, UI_BTN_DOWN, 1);
    ui_app_tick(&app, jd);
    ui_render(&app);
    sim_frame_compose(win, scale, ui_fb(), 1u << UI_BTN_OK);
    write_ppm(path, win, sim_frame_width(scale), sim_frame_height(scale));
}

/* 八字两页。出生信息要**先填进去**再拍，而且它跟 shot() 的参数集差得远 ——
 * 不往 shot() 上再挂三个参数，单独一个。 */
static void shot_bazi(const char *path, uint16_t *win, int scale, double jd,
                      int night, int page, int mode, int birth_row, int steps,
                      int gender, int basis)
{
    ui_app app;
    ui_app_init(&app, jd);
    if (night) ui_app_button(&app, UI_BTN_ACT, 1);
    app.feature = UI_FEATURE_BAZI;
    app.page    = page;
    /* 固定填 2000-01-01 12:00 +480 —— 与 --at 那个当下时刻无关，
     * 这样盘面上的四柱/大运/流年每次都是同一张，好逐像素比对。
     * gender 传 -1 时故意让「未选性别」那条提示也拍到。 */
    app.birth.year = 2000; app.birth.month = 1; app.birth.day = 1;
    app.birth.hour = 12;   app.birth.minute = 0; app.birth.second = 0.0;
    app.birth_offset = 480;
    app.gender = (gender < 0) ? SF_BAZI_GENDER_NONE
               : (gender == 0) ? SF_BAZI_GENDER_FEMALE : SF_BAZI_GENDER_MALE;
    /* basis < 0 = 不动（默认「钟表时间」）。要拍「排盘钟表=真太阳时」时
     * 得在这里直接设 —— 那是**八字设置页**上的字段，而这一趟拍的是出生页；
     * 走按键就得先翻到设置页再翻回来，拍出来的东西一样。 */
    if (basis >= 0) app.bazi_time_basis = basis;
    app.chart_dirty = 1;              /* 上面刚改过 birth，逼它重算 */

    if (page == UI_BAZI_PAGE_BIRTH) {
        for (int k = 0; k < birth_row; k++) ui_app_button(&app, UI_BTN_DOWN, 1);
        /* K7 = 换档。⚠️ 必须在**选好行之后**按：档位表是按当前光标行取的
         * （ui_app.c 的 active_steps），顺序反了就一直在拍第一行的档位。 */
        for (int k = 0; k < steps; k++) ui_app_button(&app, UI_BTN_MENU, 1);
    }
    if (page == UI_BAZI_PAGE_CHART)
        for (int k = 0; k < mode; k++) ui_app_button(&app, UI_BTN_OK, 1);

    ui_app_tick(&app, jd);
    ui_render(&app);
    sim_frame_compose(win, scale, ui_fb(), 0u);
    write_ppm(path, win, sim_frame_width(scale), sim_frame_height(scale));
}

/* 运限页：展开到第几层 + 每层选第几张。 */
static void shot_luck(const char *path, uint16_t *win, int scale, double jd,
                      int night, int page, int open, const int sel[UI_LUCK_LEVELS])
{
    ui_app app;
    ui_app_init(&app, jd);
    if (night) ui_app_button(&app, UI_BTN_ACT, 1);
    app.feature = UI_FEATURE_BAZI;
    app.page    = page;
    app.birth.year = 2000; app.birth.month = 1; app.birth.day = 1;
    app.birth.hour = 12;   app.birth.minute = 0; app.birth.second = 0.0;
    app.birth_offset = 480;
    app.gender = SF_BAZI_GENDER_MALE;
    app.chart_dirty = 1;
    ui_app_tick(&app, jd);               /* 先把盘算出来（链才有东西可选） */

    app.luck_open = 0;
    for (int lv = 0; lv < open; lv++) {
        app.luck_sel[lv] = sel[lv];
        app.luck_open++;
    }
    app.luck_dirty = 1;

    ui_app_tick(&app, jd);
    ui_render(&app);
    sim_frame_compose(win, scale, ui_fb(), 0u);
    write_ppm(path, win, sim_frame_width(scale), sim_frame_height(scale));
}

/* 八字设置页。跟 shot_bazi 差得远（要的是**行**和**那几行活没活着**），
 * 所以单独一个。
 *
 * `local_cal` = 先把「历法」按一下（中国古历 → 当地天文）再拍：这一下会让
 * **三行同时变活**（定气日界、出生地经度、还有出生信息页的出生地时区 ——
 * 时区锁的判据是 pillar_historical_mode 和 mode）。只拍默认那套就永远看不到
 * 它们活着的样子，而「默认全是灰的」恰恰是这一版最容易看懵的地方。
 *
 * ⚠️ 直接写 `app.bset_row` 而不是按 K1/K2 —— 光标行是纯状态，按方向键和
 * 赋值等价，但这里省四行代码。**改口径必须按键**（走 bset_apply 的推导），
 * 所以下面那两下是真的按 OK。 */
static void shot_bset(const char *path, uint16_t *win, int scale, double jd,
                      int night, int row, int local_cal, int solar_basis)
{
    ui_app app;
    ui_app_init(&app, jd);
    if (night) ui_app_button(&app, UI_BTN_ACT, 1);
    app.feature = UI_FEATURE_BAZI;
    app.page    = UI_BAZI_PAGE_SETTINGS;

    if (solar_basis) {                 /* 排盘钟表 → 真太阳时（两下） */
        app.bset_row = UI_BSET_TIME_BASIS;
        ui_app_button(&app, UI_BTN_OK, 1);
        ui_app_button(&app, UI_BTN_OK, 1);
    }
    if (local_cal) {                   /* 历法 → 当地天文（一下） */
        app.bset_row = UI_BSET_CAL_MODE;
        ui_app_button(&app, UI_BTN_OK, 1);
    }
    app.bset_row = row;

    ui_app_tick(&app, jd);
    ui_render(&app);
    sim_frame_compose(win, scale, ui_fb(), 0u);
    write_ppm(path, win, sim_frame_width(scale), sim_frame_height(scale));
}

static int dump_pages(const char *dir, double jd_start, int scale)
{
    uint16_t *win = malloc((size_t)sim_frame_width(scale) *
                           sim_frame_height(scale) * sizeof(uint16_t));
    if (!win) return 1;
    char path[512];

    /* --- 一、各页 x 2 主题，用当下时刻 --- */
    char tag[32];
    for (int night = 0; night < 2; night++) {
        for (int page = 0; page < UI_PAGE_COUNT; page++) {
            snprintf(tag, sizeof tag, "%s_p%d", night ? "night" : "day", page);
            snprintf(path, sizeof path, "%s/page_%s.ppm", dir, tag);
            shot(path, win, scale, jd_start, night, page,
                 page == 0 ? 1u << UI_BTN_OK : 0u, -1, 0,
                 SHOT_FEATURE_KEEP);
        }
    }

    /* --- 一之二、设置页的每一个光标位置 ---
     * 选中行是整行反色，行高是取整后的像素值逐行累加 —— 最后一行会不会
     * 压到底部页码点上，只有渲出来才知道。往 UI_SET_* 里加一项时看这几张。 */
    for (int r = 0; r < UI_SET_COUNT; r++) {
        snprintf(path, sizeof path, "%s/set_row%d.ppm", dir, r);
        shot(path, win, scale, jd_start, 1, UI_PAGE_SETTINGS, 0u, r, 0,
             SHOT_FEATURE_KEEP);
    }
    /* 再来一遍「所有行都活着」的那套 —— 默认两行是变暗的，只拍默认那套
     * 就永远看不到它们正常时的样子。 */
    for (int r = 0; r < UI_SET_COUNT; r++) {
        snprintf(path, sizeof path, "%s/set_live_row%d.ppm", dir, r);
        shot(path, win, scale, jd_start, 1, UI_PAGE_SETTINGS, 0u, r, 1,
             SHOT_FEATURE_KEEP);
    }

    /* --- 二、8 个月相 ---
     * 从下一个朔起每 3.69 天一张，正好把一个朔望月切成 8 个相位
     * （0/45/90/.../315°）。之前我用「now + 14.77 天」当满月，结果那天
     * 月亮其实是下弦 43% —— 满月压根没被拍到过。按真实朔取点才靠谱。 */
    double nm = sf_solve_lunar_phase(0.0, jd_start);
    if (!isfinite(nm) || nm < jd_start) nm = sf_solve_lunar_phase(0.0, jd_start + 29.53);
    if (isfinite(nm)) {
        for (int k = 0; k < 8; k++) {
            snprintf(path, sizeof path, "%s/moon_%d.ppm", dir, k);
            shot(path, win, scale, nm + k * 3.69, 1, UI_PAGE_TODAY, 0, -1, 0,
                 SHOT_FEATURE_KEEP);
        }
    }

    /* --- 三、功能选择页 + 八字两页 ---
     * ⚠️ 这两块**不在**上面那个 `page < UI_PAGE_COUNT` 的循环里：功能选择页
     * 的 feature 是 -1、八字是另一个 feature，那个循环按定义覆盖不到。
     * 加子页 / 加功能时，这一段的覆盖面要跟着长。 */
    for (int night = 0; night < 2; night++) {
        for (int pk = 0; pk < UI_FEATURE_COUNT; pk++) {
            snprintf(path, sizeof path, "%s/pick_%s_%d.ppm", dir,
                     night ? "night" : "day", pk);
            shot_pick(path, win, scale, jd_start, night, pk);
        }
        for (int m = 0; m < 3; m++) {          /* 盘面的三种列数：4/5/6 */
            snprintf(path, sizeof path, "%s/bazi_chart_%s_m%d.ppm", dir,
                     night ? "night" : "day", m);
            shot_bazi(path, win, scale, jd_start, night, UI_BAZI_PAGE_CHART, m, 0, 0, 1, -1);
        }
    }
    /* 出生信息页的每一个光标位（跟设置页同一个理由：最后一行会不会压到
     * 底下那行提示、提示会不会压到页码点，只有渲出来才知道） */
    for (int r = 0; r < UI_BIRTH_ROW_COUNT; r++) {
        snprintf(path, sizeof path, "%s/bazi_birth_row%d.ppm", dir, r);
        shot_bazi(path, win, scale, jd_start, 1, UI_BAZI_PAGE_BIRTH, 0, r, 0, 1, -1);
    }
    /* 顶栏那个「方向 + 步长」：每行的每一档都拍一张。后缀的字不一样
     * （年/月/日/时/分/m），右对齐之后会不会撞上左边的标题、会不会出屏，
     * 只有渲出来才知道（宽度能算，但字距是逐字累加的，算不如渲）。
     * 4 档是最大的那张表（偏移/分），多按了也只是绕回来。 */
    for (int r = 0; r < UI_BIRTH_ROW_COUNT; r++)
        for (int s = 0; s < 4; s++) {
            snprintf(path, sizeof path, "%s/bazi_birth_step%d_%d.ppm", dir, r, s);
            shot_bazi(path, win, scale, jd_start, 1, UI_BAZI_PAGE_BIRTH, 0, r, s, 1, -1);
        }
    /* 两条错误路径也要拍：未选性别、日期不存在 */
    snprintf(path, sizeof path, "%s/bazi_birth_nogender.ppm", dir);
    shot_bazi(path, win, scale, jd_start, 1, UI_BAZI_PAGE_BIRTH, 0, 5, 0, -1, -1);
    snprintf(path, sizeof path, "%s/bazi_bad_date.ppm", dir);
    shot_bazi(path, win, scale, jd_start, 1, UI_BAZI_PAGE_CHART, 0, 0, 0, 1, -1);
    /* 没选性别的盘面：标题必须是「未选」、右上角「先选性别」——
     * 原来这里会显示成「坤造」而大运那两列上不来（见 page_chart 的注）。
     *
     * ⚠️ 只拍 m0。m≥1 时那几下 K5 **会跳到出生信息页的性别行**（正是这一版
     * 修的行为），拍到的是出生页 —— 所以后两张单独命名，当「K5 跳转」的证据。 */
    snprintf(path, sizeof path, "%s/bazi_chart_nogender.ppm", dir);
    shot_bazi(path, win, scale, jd_start, 1, UI_BAZI_PAGE_CHART, 0, 0, 0, -1, -1);
    snprintf(path, sizeof path, "%s/bazi_nogender_k5.ppm", dir);
    shot_bazi(path, win, scale, jd_start, 1, UI_BAZI_PAGE_CHART, 1, 0, 0, -1, -1);
    snprintf(path, sizeof path, "%s/bazi_nogender_k5_again.ppm", dir);
    shot_bazi(path, win, scale, jd_start, 1, UI_BAZI_PAGE_CHART, 2, 0, 0, -1, -1);

    /* --- 三点五、运限页（五级联动）---
     * 逐层展开 × 选到不同的卡。⚠️ 这一页的**满屏**（五层全开）是最坏情况：
     * 五层 × 19 px + 顶栏 = 108，页码点在 121 —— 会不会压点只有渲出来才知道。 */
    for (int open = 1; open <= UI_LUCK_LEVELS; open++) {
        const int sel0[UI_LUCK_LEVELS] = { 0, 0, 0, 0, 0 };
        snprintf(path, sizeof path, "%s/bazi_luck_open%d.ppm", dir, open);
        shot_luck(path, win, scale, jd_start, 1, UI_BAZI_PAGE_LUCK, open, sel0);
    }
    {
        /* 选到中间那一步大运 + 中间那个流年/流月/流日/流时 */
        const int mid[UI_LUCK_LEVELS] = { 3, 4, 5, 6, 7 };
        snprintf(path, sizeof path, "%s/bazi_luck_mid.ppm", dir);
        shot_luck(path, win, scale, jd_start, 1, UI_BAZI_PAGE_LUCK, UI_LUCK_LEVELS, mid);
        /* 第 0 层选童限（index 0 且它有童限时）：第 1 层要变成「流年小运」 */
        const int ch[UI_LUCK_LEVELS] = { 0, 0, 0, 0, 0 };
        snprintf(path, sizeof path, "%s/bazi_luck_childhood.ppm", dir);
        shot_luck(path, win, scale, jd_start, 1, UI_BAZI_PAGE_LUCK, 2, ch);
    }
    /* 盘面本身：链展开之后柱盘左边那两根（四柱 → +大运 → +流年 → 更深） */
    for (int open = 0; open <= UI_LUCK_LEVELS; open++) {
        snprintf(path, sizeof path, "%s/bazi_chart_chain%d.ppm", dir, open);
        const int sel0[UI_LUCK_LEVELS] = { 0, 0, 0, 0, 0 };
        shot_luck(path, win, scale, jd_start, 1, UI_BAZI_PAGE_CHART, open, sel0);
    }

    /* --- 四、八字设置页 ---
     * 默认那套（六行里有三行**是灰的**）+ 「历法=当地天文」那套（它们活了）。
     * 不拍后面这套，就永远看不到活着的样子 —— 而上游的规矩本来就是
     * 「默认全灰，要排外国的盘先把历法改成当地天文」。 */
    for (int r = 0; r < UI_BSET_COUNT; r++) {
        snprintf(path, sizeof path, "%s/bazi_bset_row%d.ppm", dir, r);
        shot_bset(path, win, scale, jd_start, 1, r, 0, 0);
        snprintf(path, sizeof path, "%s/bazi_bset_live_row%d.ppm", dir, r);
        shot_bset(path, win, scale, jd_start, 1, r, 1, 0);
    }
    /* 排盘钟表=真太阳时：**出生地经度那行的失效判据与众不同**（它有两个
     * 消费者 —— 日界和排盘钟表），这一张就是那条规则的证据 —— 历法还是
     * 中国古历、日界还是固定偏移（日界那一行还是灰的），但经度已经活了。
     * ⚠️ 那一行 2026-09-20 从这一页搬到了**出生信息页**（「你出生在哪儿」
     * 跟年月日时是一伙的），所以证据也搬过去拍：光标压在经度行上，
     * 值色是亮的 = 按得动。 */
    snprintf(path, sizeof path, "%s/bazi_bset_solar_row%d.ppm", dir, UI_BSET_TIME_BASIS);
    shot_bset(path, win, scale, jd_start, 1, UI_BSET_TIME_BASIS, 0, 1);
    snprintf(path, sizeof path, "%s/bazi_birth_solar_meridian.ppm", dir);
    shot_bazi(path, win, scale, jd_start, 1, UI_BAZI_PAGE_BIRTH, 0,
              UI_BIRTH_MERIDIAN, 0, 1, UI_TIME_TRUE_SOLAR);

    free(win);

    /* 字库够不够，用数字说话：任何一次画不出来的字形都会计数 */
    const int miss = ui_missing_glyphs();
    printf("漏字 %d 个%s\n", miss,
           miss ? " —— 改了 UI 文字没重跑 `make font`？" : " ✓");
    return miss ? 1 : 0;
}

/* "2026-09-19" 或 "2026-09-19T06:41" → JD(UT)。失败返回 NaN。 */
static double parse_at(const char *s)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    const int n = sscanf(s, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi);
    if (n < 3) return NAN;
    if (n == 3) {                       /* 只给了日期，就取当天的 12:00 */
        if (sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) < 3) { h = 12; mi = 0; }
    }
    if (y < 1600 || y > 2400 || mo < 1 || mo > 12 || d < 1 || d > 31) return NAN;
    return ui_jd_from_calendar(y, mo, d, h, mi, 0.0);
}

int main(int argc, char **argv)
{
    int scale  = SIM_DEFAULT_SCALE;
    int page   = UI_PAGE_TODAY;
    int year   = 0;                 /* 0 = 用当前年 */
    int night  = 1;
    int selftest = 0;
    double at  = NAN;
    double lat = 39.9042, lon = 116.4074;
    int utc_min = 480;
    const char *dump = NULL;
    const sim_host *h = &sim_host_cocoa;

    setvbuf(stdout, NULL, _IOLBF, 0);   /* 重定向到文件时也看得见进度 */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            scale = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--day")) {
            night = 0;
        } else if (!strcmp(argv[i], "--at") && i + 1 < argc) {
            at = parse_at(argv[++i]);
            if (!isfinite(at)) { fprintf(stderr, "看不懂的日期：%s\n", argv[i]); return 2; }
        } else if (!strcmp(argv[i], "--page") && i + 1 < argc) {
            page = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--year") && i + 1 < argc) {
            year = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
            dump = argv[++i];
        } else if (!strcmp(argv[i], "--lat") && i + 1 < argc) {
            lat = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--lon") && i + 1 < argc) {
            lon = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--utc") && i + 1 < argc) {
            utc_min = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--selftest")) {
            selftest = 1;
        } else if (!strcmp(argv[i], "--sdl")) {
#if defined(SIM_HAVE_SDL)
            h = &sim_host_sdl;
#else
            fprintf(stderr, "这个版本没编进 SDL2 后端（make SIM_BACKEND=sdl）\n");
            return 2;
#endif
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    if (page < 0) page = 0;
    if (page >= UI_PAGE_COUNT) page = UI_PAGE_COUNT - 1;

    ui_init(&g_fb[0][0]);

    g_view_year = year;      /* ⚠️ 必须在下面 dump 那条 return **之前**赋值 */
    if (dump) {
        const double jd = isfinite(at) ? sf_utc_to_tt(at) : sim_now_jd_tt();
        printf("无头渲染 → %s\n", dump);
        return dump_pages(dump, jd, scale);
    }

    printf("solar-fast 模拟屏 —— ST7735S 1.8\" 128x160，横屏 160x128\n"
           "  后端 %s，放大 %dx\n\n"
           "  按钮（模块丝印 K1..K8，2x4 排布）\n"
           "    K1 上  K2 下  K3 左  K4 右   键：1 2 3 4，或方向键 / w a s d\n"
           "    K5 确定 K6 返回 K7 菜单 K8 功能   键：5 6 7 8，或 Enter Esc Tab z\n"
           "  宿主专用（不属于那 8 个按钮）\n"
           "    [ ]  时间 ∓1 天    \\  回到现在    - =  缩放    Cmd+Q 退出\n"
           "  K3/K4 翻页 · K1/K2 滚动列表 / 设置页选行 · K8 切换日夜主题\n"
           "  两个设置页 / 气·朔两页 / 八字出生信息页另有一套编辑三键（目标不同）\n"
           "    K5 调一下 · K7 换步长 · K8 切 ＋/－\n\n",
           h->name, scale);

    if (h->init("solar-fast — 1.8\" ST7735S 160x128", scale) != 0) {
        fprintf(stderr, "后端 %s 初始化失败\n", h->name);
        return 1;
    }

    if (selftest) {
        ui_app probe;
        ui_app_init(&probe, sim_now_jd_tt());
        ui_render(&probe);
        h->present(ui_fb(), 1u << UI_BTN_OK);
        sim_ev ev;
        int n = 0;
        while (h->poll(&ev)) n++;
        h->shutdown();
        printf("selftest ok：后端 %s，窗口 %dx%d，poll 到 %d 个待处理事件\n",
               h->name, sim_frame_width(scale), sim_frame_height(scale), n);
        return 0;
    }

    /* --at 换算成偏移量，这样时钟还是接着走的，不是冻在那儿。
     * ⚠️ at 是 UTC，sim_now_jd_tt() 是 TT —— 两边都得是 TT 才能相减。 */
    if (isfinite(at)) g_offset_days = sf_utc_to_tt(at) - sim_now_jd_tt();

    ui_app app;
    ui_app_init(&app, sim_now_jd_tt());
    /* ⚠️ 走 setter，别无脑写 cal_cfg.utc_offset_minutes：默认历法是「中国古历」，
     * 那把锁会把生效值钉在 +480，直接写等于绕过它。 */
    ui_app_set_utc_offset(&app, utc_min);
    ui_app_set_observer(&app, lat, lon);
    app.page = page;
    if (year) ui_app_set_view_year(&app, year);
    if (!night) {
        /* 用一次 ACT 把主题翻过来，免得另开一条设置路径 */
        ui_app_button(&app, UI_BTN_ACT, 1);
    }

    uint32_t pressed = 0;

    for (;;) {
        sim_ev ev;
        int got = 0;

        while (h->poll(&ev)) {
            got = 1;
            switch (ev.kind) {
            case SIM_EV_QUIT:
                h->shutdown();
                printf("退出。\n");
                return 0;

            case SIM_EV_BUTTON:
                if (ev.val) pressed |=  (1u << ev.id);
                else        pressed &= ~(1u << ev.id);
                ui_app_button(&app, (ui_btn)ev.id, ev.val);
                break;

            case SIM_EV_ZOOM:  h->zoom(ev.val);            break;
            case SIM_EV_TIME:  g_offset_days += ev.val;    break;
            case SIM_EV_RESET: g_offset_days = 0.0;        break;
            default: break;
            }
        }

        ui_app_tick(&app, sim_now_jd_tt());
        ui_render(&app);
        h->present(ui_fb(), pressed);

        if (!got) sleep_ms(SIM_FRAME_MS);
    }
}
