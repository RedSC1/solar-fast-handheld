/* ==================================================================
 * ui_app.c —— 页面状态机 + 派生数据（节气表 / 朔望表 / 太阳量）
 *
 * 便携层，只依赖 UI 头 + solar_fast.h + libm。
 * 不含任何时间来源：现在的时刻由宿主通过 ui_app_tick(a, jd_tt) 喂进来。
 * ================================================================== */
#include <math.h>
#include <string.h>

#include "ui.h"
#include "solar_fast.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define UI_TWO_PI (2.0 * M_PI)

/* ------------------------------------------------------------------
 * 时间换算（Meeus 第 7 章）
 *
 * 只做**日历字段 ↔ JD** 这一步。TT / UT1 / UTC 之间的换算一律用库的
 * （sf_utc_to_tt / sf_tt_to_utc / sf_tt_to_ut）—— 这一层以前自带一份
 * Espenak–Meeus ΔT，跟库的 IERS 年表差 5.9 秒，屏上的钟会快 6 秒。
 * 一份实现，两处别再各写一遍。
 *
 * UI 的整年气朔支持 SF_QISHUO_RANGE_START_YEAR..END_YEAR，因此这里统一使用
 * 前推格里历（astronomical year numbering，含 0 年）。正反换算必须同口径：
 * 不能一边全程格里历、另一边在 1582 年以前偷偷切成儒略历。
 * ------------------------------------------------------------------ */
static double jd_round_to_second(double jd)
{
    return floor(jd * 86400.0 + 0.5) / 86400.0;
}

/* 事件时刻（节气/朔望/出没）要**四舍五入到分**再显示。
 *
 * 为什么不能像钟表那样截断：历书（紫金山天文台那类）给的节气时刻是
 * **四舍五入到分**的。2026 年 24 个节气里，有 13 个落在 :30 秒之后 ——
 * 截断就整体早显示一分钟。实测（experimental/ 里的探针对过 PMO 那张表）：
 *   大寒 9:44:55.8  → 历书 9:45，截断显示 9:44
 *   清明 2:39:59.6  → 历书 2:40，截断显示 2:39
 * 当年就是「夏至 16:24:30.5」这种恰好压在 :30 上的最脆 —— 只差 0.55 秒。
 *
 * ⚠️ 只给**事件列表**用。屏上那个走动的钟（今页的 HH:MM）必须截断：
 * 16:24:40 的钟面读数就是 16:24，读成 16:25 是错的。 */
double ui_jd_round_to_minute(double jd)
{
    return floor(jd * 1440.0 + 0.5) / 1440.0;
}

double ui_jd_from_calendar(int y, int mo, int d, int h, int mi, double s)
{
    int yy = y, mm = mo;
    if (mm <= 2) { yy -= 1; mm += 12; }
    const int A = (int)floor(yy / 100.0);
    const int B = 2 - A + (int)floor(A / 4.0);      /* 格里历 */
    const double day = d + (h + mi / 60.0 + s / 3600.0) / 24.0;
    return floor(365.25 * (yy + 4716)) + floor(30.6001 * (mm + 1))
         + day + B - 1524.5;
}

void ui_calendar_from_jd(double jd, int *y, int *mo, int *d,
                         int *h, int *mi, int *s)
{
    /* 先整秒对齐，后面分解就不会出现 60 秒进位那种烂事 */
    const double j = jd_round_to_second(jd) + 0.5;
    const double z = floor(j);
    const double f = j - z;

    const double g = floor((z - 1867216.25) / 36524.25);
    const double a = z + 1 + g - floor(g / 4);
    const double b = a + 1524;
    const double c = floor((b - 122.1) / 365.25);
    const double dd = floor(365.25 * c);
    const double e = floor((b - dd) / 30.6001);

    const double dayf = b - dd - floor(30.6001 * e) + f;
    const int day = (int)floor(dayf);
    const int month = (int)(e < 14 ? e - 1 : e - 13);
    const int year = (int)(month > 2 ? c - 4716 : c - 4715);

    double secs = (dayf - day) * 86400.0;
    long isec = lround(secs);
    if (isec >= 86400) isec = 86399;      /* 只可能差 1 秒，不值得为它做进位 */

    if (y)  *y = year;
    if (mo) *mo = month;
    if (d)  *d = day;
    if (h)  *h = (int)(isec / 3600);
    if (mi) *mi = (int)((isec % 3600) / 60);
    if (s)  *s = (int)(isec % 60);
}

/* ------------------------------------------------------------------
 * 月相盘朝向
 *
 * 屏上的月相要画成**观测者抬头看到的那个样子**：赤道附近月牙会躺着
 * （亮边朝上），南半球跟北半球大致反着。老版本固定「终线竖直、盈在右」，
 * 那是把北天极当成了「上」。
 *
 * 做法是**地平坐标直接投**，不是教科书那套「亮边位置角 χ − 视差角 q」：
 * 后者两个 atan2 加一堆符号约定，镜像方向极易搞反，而且这个库和上游
 * tools 里都没有现成约定可对。投法等价但不会搞反。
 *
 * 屏幕约定：**上 = 天顶、右 = 方位角增大的方向**（面朝某方位时右手边
 * 就是方位角 +90°，北→东→南→西 全对得上）。
 *
 * ⚠️ 这套东西在 experimental/moon_probe.c 里跟 χ−q 独立对照过：
 * 1392 个样本（6 纬度 × 4 经度 × 约一年）上两条路径吻合到 **0.0000°**，
 * 而且 8 种符号组合里只有唯一一种成立。要动这里的公式，先回去跑那个探针。
 * ------------------------------------------------------------------ */

/* 赤道 → 地平：方位角自北起、向东为正 */
static void radec_to_alt_az(double ra, double dec, double lst, double phi,
                            double *alt, double *az)
{
    double H = lst - ra;
    H = fmod(H, UI_TWO_PI);
    if (H > M_PI) H -= UI_TWO_PI;
    if (H < -M_PI) H += UI_TWO_PI;

    const double sp = sin(phi), cp = cos(phi);
    const double sd = sin(dec), cd = cos(dec);
    *alt = asin(sp * sd + cp * cd * cos(H));
    *az  = atan2(-cd * sin(H), sd * cp - cd * sp * cos(H));
}

/* 从 (alt1,az1) 出发、指向 (alt2,az2) 的方位，投影到前者的切平面，
 * 再化成屏幕坐标。返回未归一化的 (右, 下)。 */
static void bearing_screen(double alt1, double az1, double alt2, double az2,
                           double *out_x, double *out_y)
{
    const double m[3]  = { cos(alt1)*cos(az1), cos(alt1)*sin(az1), sin(alt1) };
    const double t[3]  = { cos(alt2)*cos(az2), cos(alt2)*sin(az2), sin(alt2) };
    const double up[3] = { -sin(alt1)*cos(az1), -sin(alt1)*sin(az1), cos(alt1) };
    const double ea[3] = { -sin(az1), cos(az1), 0.0 };

    const double d = m[0]*t[0] + m[1]*t[1] + m[2]*t[2];
    const double p[3] = { t[0] - d*m[0], t[1] - d*m[1], t[2] - d*m[2] };

    const double c_up = p[0]*up[0] + p[1]*up[1] + p[2]*up[2];   /* 屏上 */
    const double c_ea = p[0]*ea[0] + p[1]*ea[1] + p[2]*ea[2];   /* 屏右 */
    *out_x = c_ea;
    *out_y = -c_up;      /* 像素 y 向下 */
}

static void moon_orientation(ui_app *a)
{
    const double jd_tt = a->jd;
    double rm, dm, km;
    sf_moon_ra_dec(jd_tt, &rm, &dm, &km);

    /* 太阳的 ra/dec 由主页懒加载入口在调用本函数前准备好，直接用。 */
    const double jd_ut = sf_tt_to_ut(jd_tt);
    const double lst   = sf_last(jd_ut, jd_tt, a->observer_lon);
    const double phi   = a->observer_lat * (M_PI / 180.0);

    double alts, azs, altm, azm;
    radec_to_alt_az(a->ra, a->dec, lst, phi, &alts, &azs);
    radec_to_alt_az(rm, dm, lst, phi, &altm, &azm);

    double bx = 1.0, by = 0.0;
    if (isfinite(alts) && isfinite(altm)) {
        double ux, uy;
        bearing_screen(altm, azm, alts, azs, &ux, &uy);
        const double n = hypot(ux, uy);
        if (n > 1e-12) { bx = ux / n; by = uy / n; }
    }
    a->moon_bx = bx;
    a->moon_by = by;

    /* 亮面比例用**真角距**（大圆），不是黄经差。
     * a->elong 是黄经差（sf_elongation 是帧黄经之差），月亮有黄纬时
     * 两者最多差 5°，亮面比例跟着差约 4% —— r=20 时约 1px。既然为了
     * 朝向本来就要取两边赤经赤纬，换成真角距是免费的。 */
    a->moon_cos_sep = sin(a->dec) * sin(dm) + cos(a->dec) * cos(dm)
                    * cos(a->ra - rm);
}

/* ------------------------------------------------------------------
 * 派生数据
 * ------------------------------------------------------------------ */
static double norm2pi(double x)
{
    x = fmod(x, UI_TWO_PI);
    return x < 0.0 ? x + UI_TWO_PI : x;
}

/* 太阳此刻在哪个节气区间里。返回 0..23，i 对应视黄经 i·15°。 */
static int term_index_of(double lon)
{
    int i = (int)floor(norm2pi(lon) / (M_PI / 12.0));
    if (i < 0) i = 0;
    if (i > 23) i = 23;
    return i;
}

/* 主页只读当前与下一个节气。旧实现还算未来 24 气、13 朔、13 望，但气/朔
 * 年表早已迁到 year_fill，那 26 个月相没有任何消费者，是纯死计算。 */
static void rebuild_today_events(ui_app *a)
{
    const double step = M_PI / 12.0;
    a->term_jd[0] = sf_solve_solar_term(a->cur_term * step, a->jd);
    a->term_jd[1] = sf_solve_solar_term(((a->cur_term + 1) % 24) * step,
                                         a->jd + 15.0);
    a->today_events_dirty = 0;
}

/* ------------------------------------------------------------------
 * 整年表（气 / 朔两页）
 *
 * 走 sf_qishuo_year_run —— 也就是 JS 的 `getQiShuoYear(civilYear, options)`：
 * 给一个**公历年**和一组选项，一次拿到这一年的全部节气与指定相位角的月相。
 * **这是库里唯一按公历年的整年入口。**
 *
 * 这一段从前是自己拼的：拿 sf_cal_get_specific_term 一个个求节气、再用
 * MONTH_ORDER 把「春分起算」的索引轮转成 1–12 月，朔望还得自己滚动种子
 * 从 1 月 1 日滚到年底。整段现在已经删掉 —— 事件出来就是时间序，名字直接用
 * sf_qishuo_event_name()，没有任何索引推算，也就没有「跳过一个元素就整体错位」
 * 那种隐患。
 *
 * 「年」的口径仍跟 tools 的气朔工具一致（按所选时区的 1 月 1 日到 12 月 31 日），
 * 而且现在是真的按**当地**时区切窗口了：老代码那个边界用的是 UT 的 1 月 1 日，
 * 时区根本没进去（+8 区跨年那 8 小时里的事件会被算到隔壁年份）。
 * ------------------------------------------------------------------ */

/* sf_qishuo_year_run 的暂存区。
 *
 * ⚠️ 它要的容量是**上界**，不是实际条数：算完再发现装不下就只能丢掉一部分，
 * 而丢掉的是**发射顺序**而不是时间顺序的子集 —— 那种「部分结果」在时间序上
 * 是错的，所以库宁可整年不给（返回 SF_QISHUO_ERR_CAPACITY），并提醒「先拿
 * NULL/0 问一次大小」。本 sim 的选项组合是固定的，上界因此是常数：
 *
 *     节气页 24×2 = 48；朔望页 2×14 = 28（见 sf_qishuo.c 的 qs_upper_bound）
 *
 * 实际每页写入 24–25 条，多出来的是 API 上界余量。**改选项就要改这个数**
 * （若以后把两页重新合并并开七十二候，上界会变成 176；再往上就用库给的
 * SF_QISHUO_MAX_EVENTS = 256）；数给少了不会静默错，是整年页**整片空掉**。
 *
 * 放文件级 static 而不是塞进 ui_app：48 条 × 160 字节约 7.5 KB，而
 * ui_app 是栈上的。这里投影完就不留了，真正要显示的东西在 ui_year_row 里。 */
#define YEAR_SCRATCH_CAP 48
static sf_qishuo_event g_year_scratch[YEAR_SCRATCH_CAP];

/* 选项从 cal_cfg 投影过去 —— 字段一一对应（见 sf_calendar.h 的 sf_cal_config）。
 *
 * ⚠️ 经线**只在「按当地经度定日界」时才给**。API 会校验这一对组合
 * （按固定时区却给了经线 → SF_QISHUO_ERR_MERIDIAN_UNEXPECTED），是照 JS 抄的。
 * 好在 apply_boundary 已经把两者绑好了：非「当地天文」一律压成固定偏移，
 * 所以这里直接照着生效值判就行，不用再推一遍。
 *
 * 失败一律 `return -1`、表留空 —— 别在里面直接 return，外层还要夹滚动位置。 */
static int year_fill(ui_app *a, int want_terms)
{
    sf_qishuo_options o;
    sf_qishuo_options_init(&o);            /* ⚠️ 不能 memset：meridian 的「未给」是 NAN */
    o.mode               = a->cal_cfg.mode;
    o.day_boundary_mode  = a->cal_cfg.day_boundary_mode;
    o.utc_offset_minutes = a->cal_cfg.utc_offset_minutes;
    if (o.day_boundary_mode == SF_CAL_MEAN_SOLAR_MERIDIAN)
        o.meridian_deg = a->cal_cfg.calendar_meridian_deg;

    o.event_accuracy      = a->accuracy;          /* 设置页「气朔精度」，默认 HIGH */
    o.include_solar_terms = want_terms;
    o.include_pentads     = 0;                    /* 屏上不显示七十二候 */

    /* 朔望页才请求朔 + 望；节气页把相位角清空，不能暗中算另一张表。 */
    static const double PHASE_ANGLES[2] = { 0.0, 180.0 };
    if (sf_qishuo_set_phase_angles(&o, want_terms ? NULL : PHASE_ANGLES,
                                   want_terms ? 0 : 2) != SF_QISHUO_OK) return -1;

    sf_qishuo_year y;
    if (sf_qishuo_year_run(&o, a->view_year, g_year_scratch, YEAR_SCRATCH_CAP, &y)
            != SF_QISHUO_OK)
        return -1;                     /* 越界/容量不足 → 整年页留空，见上面的说明 */

    /* 先数一遍，装不下就整年不给。
     * 跟库对容量的态度一致：宁可整片空白，也不要一个「时间序上少了尾巴」的表
     * —— 那种表看起来是好的，但年尾几行凭空没了。两个循环都只跑 ≤76 次。 */
    int nt = 0, np = 0;
    for (int i = 0; i < y.count; i++) {
        const int k = g_year_scratch[i].kind;
        if (k == SF_QISHUO_KIND_SOLAR_TERM)       nt++;
        else if (k == SF_QISHUO_KIND_LUNAR_PHASE) np++;
    }
    if (nt > UI_YEAR_TERMS || np > UI_YEAR_PHASES) return -1;

    /* 投影：事件按时间混排，这里按 kind 分到两张表里 —— 各自的顺序就是时间序，
     * 屏上直接按行号取，不需要任何索引推算（从前那套 MONTH_ORDER 已经删了）。 */
    for (int i = 0; i < y.count; i++) {
        const sf_qishuo_event *e = &g_year_scratch[i];
        ui_year_row *r;
        if (e->kind == SF_QISHUO_KIND_SOLAR_TERM) {
            r = &a->year_term[a->year_term_n++];
            r->term_index = (uint8_t)e->term_index;   /* 高亮当前节气用 */
            r->full = 0;
        } else if (e->kind == SF_QISHUO_KIND_LUNAR_PHASE) {
            r = &a->year_phase[a->year_phase_n++];
            r->term_index = 0xFF;
            /* 望比朔醒目（「一个亮、一个暗」）。判据就是相位角本身 ——
             * 改相位角表的时候记得颜色也跟着这条走。 */
            r->full = (e->phase_angle_deg == 180.0) ? 1 : 0;
        } else {
            continue;
        }
        r->jd_tt = e->jd_tt;
        sf_qishuo_event_name(e, r->name, sizeof r->name);
    }
    return 0;
}

static void rebuild_year_terms(ui_app *a)
{
    a->year_term_n = 0;
    (void)year_fill(a, 1);

    const int mt = (a->year_term_n  > UI_LIST_ROWS) ? a->year_term_n  - UI_LIST_ROWS : 0;
    if (a->scroll[UI_PAGE_TERMS]  > mt) a->scroll[UI_PAGE_TERMS]  = mt;
}

static void rebuild_year_phases(ui_app *a)
{
    a->year_phase_n = 0;
    (void)year_fill(a, 0);

    /* 月相一年是 24 还是 25 行，换年后把滚动位置夹回新范围。 */
    const int mp = (a->year_phase_n > UI_LIST_ROWS) ? a->year_phase_n - UI_LIST_ROWS : 0;
    if (a->scroll[UI_PAGE_PHASES] > mp) a->scroll[UI_PAGE_PHASES] = mp;
}

/* 定义在文件后半（要先用到 local_fields 等），这里前置声明 */
static void rebuild_lunar_today(ui_app *a);
static void rebuild_lunar_grid(ui_app *a);
static void refresh_pillars(ui_app *a, int force);
static void bazi_rebuild(ui_app *a);
/* 同理：ui_app_init 就要用它们把时区和日界推导出来 */
static void apply_utc_lock(ui_app *a);
static void apply_boundary(ui_app *a);
static void bazi_apply_cfg(ui_app *a);
static int32_t bazi_offset(const ui_app *a);
/* 运限链（模型在 bazi_rebuild 后面那一大段）—— bazi_rebuild 结尾要重置它 */
static void luck_reset(ui_app *a);
static void luck_clamp(ui_app *a);
static void luck_days_rebuild(ui_app *a);
static void rebuild_rise(ui_app *a);
static double tidy(double v);            /* 经度取值收口，定义在设置页那一段 */

static void invalidate_calendar_pages(ui_app *a, int annual)
{
    a->lunar_dirty = 1;
    a->grid_dirty = 1;
    a->pillars_ok = 0;
    a->local_day_key = INT64_MIN;
    a->local_year_key = INT32_MIN;
    if (annual) a->year_term_dirty = a->year_phase_dirty = 1;
}

static void update_current_term(ui_app *a)
{
    a->sunlon = sf_sun_apparent_longitude(a->jd);
    const int ct = term_index_of(a->sunlon);
    if (a->cur_term != ct) {
        a->cur_term = ct;
        a->today_events_dirty = 1;
    }
}

/* `sf_cal_from_instant_ut` 落“今天”时用的同一条日界。当地天文模式必须按
 * 历法经度，不能偷换成屏幕时区；其它模式经 apply_boundary 固定为 UTC 偏移。 */
static double calendar_day_offset(const ui_app *a)
{
    return a->cal_cfg.day_boundary_mode == SF_CAL_MEAN_SOLAR_MERIDIAN
         ? a->cal_cfg.calendar_meridian_deg / 360.0
         : a->cal_cfg.utc_offset_minutes / 1440.0;
}

void ui_app_tick(ui_app *a, double jd_tt)
{
    a->jd = jd_tt;

    /* 跨日只做失效标记。这个“日”与 sf_cal_from_instant_ut 一致：固定模式按
     * UTC 偏移，当地天文模式按历法经度；不在 tick 里顺手重建数据。 */
    const double jd_ut = sf_tt_to_ut(jd_tt);
    const int64_t day = sf_cal_civil_day_number(jd_ut, calendar_day_offset(a));
    if (day != a->local_day_key) {
        const int32_t old_year = a->local_year_key;
        int32_t year = 0;
        sf_solar_date_from_day_number(day, &year, NULL, NULL);
        a->local_day_key = day;
        a->local_year_key = year;
        a->lunar_dirty = 1;
        a->moon_dirty = 1;

        /* 默认跟随“今年”；用户若手动选了别的年份，不强行跳回去。 */
        if (old_year != INT32_MIN && year != old_year && a->view_year == old_year) {
            a->view_year = year;
            a->year_term_dirty = a->year_phase_dirty = 1;
        }
    }

    /* 出没按观测经度的当地日缓存，日界可能与上面的显示民用日不同。这里只
     * 比较整数键并置失效，不做昂贵求根；太阳页打开时再加载。 */
    if (a->rise_cached) {
        const int64_t rise_day = sf_cal_civil_day_number(
            jd_ut, a->observer_lon / 360.0);
        if (rise_day != a->rise_day) a->rise_cached = 0;
    }
    /* 八字的盘**跟「现在」无关**，只在出生信息 / 口径变了之后重算。
     * 所以它挂在自己的脏标志上，不跟上面那道日粒度节流走。 */
    if (a->chart_dirty) bazi_rebuild(a);
    /* 流日那一层要解两个「节」，比别的层贵得多 —— 只在下标变了（按键里置位）
     * 或盘变了（bazi_rebuild 里置位）之后重算一次。 */
    if (a->luck_dirty) luck_days_rebuild(a);

    /* 只准备当前页面；其余 dirty 留到真正打开页面时。 */
    ui_app_prepare_visible(a);
}

void ui_app_init(ui_app *a, double jd_tt)
{
    for (unsigned i = 0; i < sizeof(*a); i++) ((char *)a)[i] = 0;
    a->cur_term = -1;
    a->local_day_key = INT64_MIN;
    a->local_year_key = INT32_MIN;
    a->today_events_dirty = 1;
    a->lunar_dirty = 1;
    a->grid_dirty = 1;
    a->moon_dirty = 1;
    a->prepared_jd = NAN;
    a->prepared_feature = -2;
    a->prepared_page = -1;

    /* ⚠️ 这一步不能省。上面整个 memset 会把 cal_cfg 清零，而「模式 0 +
     * 偏移 0」既不是默认的历史模式、时区也是格林尼治 —— 太阳页会显示 UT
     * 时刻，农历的 instant→日期 那一步也会差 8 小时。必须显式初始化。 */
    sf_cal_config_init_china_standard_historical(&a->cal_cfg, 480);
    a->observer_lat = 39.9042;      /* 缺省北京，跟这个项目的语境一致 */
    a->observer_lon = 116.4074;

    /* 四柱口径。默认跟上游一致：钟表时间、早晚子时不分开（23:00 起算次日，
     * 也就是 tools 的 `next-day`）。 */
    a->time_basis = UI_TIME_CLOCK;
    a->rat_hour   = SF_GANZHI_RAT_HOUR_NO_SPLIT;
    a->set_row    = 0;

    /* ⚠️ 上面 memset 会把它清零 = LOW（最省那档）。气朔精度默认要**最大档**：
     * 库那边 HIGH 就是原来的默认预算，文档里的精度数字也全是这一档的。 */
    a->accuracy   = SF_QISHUO_ACC_HIGH;

    /* 步长默认取最粗那档（各行档位表的 [0]）、方向默认「加」——
     * 先粗后细，跟「先翻到大致位置再微调」的用法一致。 */
    a->step_idx   = 0;
    a->step_plus  = 1;

    /* 显示时区：用户值 +480。上面 init 出来的历法是「中国古历」，所以
     * apply_utc_lock 会把生效值也钉在 +480 —— 两条路殊途同归。 */
    a->user_offset_minutes = 480;

    /* 定气日界。默认那档是固定偏移，但历法不是「当地天文」时 apply_boundary
     * 会把它压成固定偏移 —— 同样殊途同归。
     * 当地经度先给东经 120°（中国标准经线），用户动过之后才不再被播种。 */
    a->boundary_choice = SF_CAL_FIXED_UTC_OFFSET;
    a->meridian_set    = 0;
    a->cal_cfg.calendar_meridian_deg = 120.0;

    apply_utc_lock(a);
    apply_boundary(a);

    /* 气/朔的默认年份 = **当前这一年**（跟 tools 气朔工具一致：那边是在
     * 加载时从系统钟取 new Date().getFullYear()，不是写死的）。
     * 之后年份就是个独立选择，不再跟着 jd 走 —— 今页照旧显示「现在」。 */
    {
        int y = 0, mo = 0, d = 0;
        /* 先转 UTC 再拆 —— jd_tt 直接拆的话，跨年那 69 秒里会取到下一年的数 */
        ui_calendar_from_jd(sf_tt_to_utc(jd_tt), &y, &mo, &d, NULL, NULL, NULL);
        a->view_year = (y >= SF_QISHUO_RANGE_START_YEAR
                     && y <= SF_QISHUO_RANGE_END_YEAR) ? y : 2026;
    }
    a->year_term_dirty = a->year_phase_dirty = 1;

    /* --- 两级页面：开机停在**功能选择页**（feature = -1）--- */
    a->feature = -1;
    a->pick    = 0;
    a->page    = UI_PAGE_TODAY;    /* 进去之后落在哪一页，在这里定 */

    /* --- 八字 ---
     * 出生信息给个**显眼的假值**（2000-01-01 12:00）而不是「现在」：
     * 「现在」会让人以为那是默认的盘，而这个功能是要你填自己的。
     * 性别默认 NONE（必须自己选）—— 顺逆判据要靠它，猜一个不如让它空着
     * 并在盘面上明说。 */
    a->birth.year = 2000; a->birth.month = 1; a->birth.day = 1;
    a->birth.hour = 12;   a->birth.minute = 0; a->birth.second = 0.0;
    a->birth_offset = 480;
    a->birth_row    = UI_BIRTH_YEAR;
    a->gender       = SF_BAZI_GENDER_NONE;
    a->chart_dirty  = 1;
    a->luck_open    = 0;      /* 默认不展开：柱盘就是四柱 */
    a->luck_dirty   = 1;      /* 盘一算出来就把链和流日缓存重置（bazi_rebuild 里） */

    /* 八字自己那一套口径：**开局跟万年历同值，此后互不相干**
     * （历法=中国古历、经度=120°、日界=固定偏移、早晚子时=23点换日）。
     * 开局不给个跟万年历不一样的值，是为了「进去就能算」；真要不一样，
     * 用户在八字设置页那一屏里改。 */
    sf_cal_config_init_china_standard_historical(&a->bazi_cal, 480);
    a->bazi_cal.calendar_meridian_deg = 120.0;
    a->bazi_boundary_choice = SF_CAL_FIXED_UTC_OFFSET;
    a->bazi_meridian_set    = 0;
    a->bazi_time_basis      = UI_TIME_CLOCK;
    a->bazi_rat_hour        = SF_GANZHI_RAT_HOUR_NO_SPLIT;
    a->bset_row             = 0;
    bazi_apply_cfg(a);

    ui_app_tick(a, jd_tt);
}

/* ------------------------------------------------------------------
 * 八字：把出生信息算成一张盘
 *
 * 口径是**八字自己那一套**（`a->bazi_cal` / `bazi_rat_hour`），**不是**
 * 从 `cal_cfg` 投影来的 —— 那是上一版的写法，问题是每张盘的出生地不一样，
 * 共用一份「当地经度」时排完一张乌鲁木齐的盘，万年历那边给日出日落用的
 * 「当地经度」也跟着变了。上游那三个工具页（八字/紫微/气朔）本来就各自
 * 一份表单、互不共享，这里跟它对齐。
 *
 * ⚠️ 出生瞬间按 sf_calendar.h「钟表读数 → 四柱」那份配方现算：
 *   jd_utc = sf_julian_day_ut(读数字段) − 生效偏移／1440
 *   jd_ut1 = sf_utc_to_ut1(jd_utc)      ← 喂给库的必须是 **UT1**，不是 UTC
 * 两个参数（instant_jd_ut1 与 virtual_time）必须分开给：年柱月柱锚节气、
 * 只认物理时刻；日柱时柱认虚拟时刻 —— 也正是「排盘钟表」那个旋钮的着力点。 */
static void bazi_rebuild_inner(ui_app *a)
{
    sf_bazi_options o;
    sf_bazi_options_init(&o);
    o.cal = a->bazi_cal;
    o.cal.utc_offset_minutes = bazi_offset(a);   /* 生效值：锁着时是 +480 */
    o.rat_hour_mode  = a->bazi_rat_hour;
    o.gender         = a->gender;
    o.dayun_count    = SF_BAZI_DEFAULT_DAYUN_COUNT;

    a->chart_dirty = 0;
    a->chart_ok    = 0;
    a->dayun_n     = 0;
    /* 先问库这个读数存不存在（2 月 30 日、1582-10-10 那种）。不合法就
     * 到此为止 —— 盘面会说「出生信息有误」，比给一张算错的盘强。 */
    a->birth_valid = sf_bazi_valid_civil_time(&a->birth);
    if (!a->birth_valid) return;

    const double jd_utc = sf_julian_day_ut(a->birth.year, a->birth.month,
                                           a->birth.day, a->birth.hour,
                                           a->birth.minute, a->birth.second)
                        - (double)bazi_offset(a) / 1440.0;
    const double jd_ut1 = sf_utc_to_ut1(jd_utc);

    /* 排盘钟表：**读数是哪套时**。库把这件事留给调用方（sf_bazi.h 头注
     * 「钟表口径不在这层做」），所以在这里按 sf_calendar.h「钟表读数 → 四柱」
     * 配方里那句「要真太阳时那一派的：字段换成真太阳时的日期 + 时刻」办：
     * 先按物理时刻定出 instant，再把虚读时段落换成那套时的读数。
     *
     * ⚠️ 跟主页 civil_jd() 的**区别**：那个用**显示时区 + 观测经度**（屏上
     * 那个钟读什么），这个用**出生地偏移 + 出生地经度**（一个盘一个地方），
     * 而且不经过任何显示时区。两处都叫「平太阳时」，量的却不是同一条经线。 */
    sf_cal_datetime vt = a->birth;      /* 钟表时间：读数原样 */
    if (a->bazi_time_basis != UI_TIME_CLOCK) {
        const double lon = a->bazi_cal.calendar_meridian_deg;
        double jd_v = jd_ut1 + lon / 360.0;                 /* 平太阳时 */
        if (a->bazi_time_basis == UI_TIME_TRUE_SOLAR)
            jd_v += sf_equation_of_time_minutes(sf_ut_to_tt(jd_ut1)) / 1440.0;
        sf_cal_datetime_from_jd(jd_v, &vt);
    }

    if (sf_bazi_chart_run(&o, jd_ut1, &vt, &a->chart) != SF_BAZI_OK)
        return;

    /* 起运要性别；没给就到此为止（盘面照常显示四柱，大运那一档留空） */
    if (a->gender != SF_BAZI_GENDER_FEMALE && a->gender != SF_BAZI_GENDER_MALE)
        { a->chart_ok = 1; return; }
    if (sf_bazi_qi_yun(&o, jd_ut1, &vt, a->chart.pillars.primary.year,
                       &a->qiyun) != SF_BAZI_OK)
        { a->chart_ok = 1; return; }

    const int n = sf_bazi_da_yun(&o, &vt, a->chart.pillars.primary.month,
                                 &a->qiyun, a->dayun,
                                 SF_BAZI_DEFAULT_DAYUN_COUNT);
    a->dayun_n  = (n == SF_BAZI_OK) ? o.dayun_count : 0;
    a->chart_ok = 1;
}

/* 换过盘之后**一律**重算运限链。
 *
 * ⚠️ 分两层是必须的：里面那层有**四条提前 return**（出生日期不合法、盘算不出来、
 * 没选性别、起运算不出来）。收拾链的动作要是写在那四条上，早晚有人加第五条忘了补
 * —— 实测就是这么炸的：选好性别、把五层展开，再把出生日期改成 2 月 30 日，
 * `dayun_n` 归 0 而 `luck_open` 还停在上一次的 5，运限页顶栏于是显示 `1/0`
 * （第 1 张 / 共 0 张）。放在唯一的出口上，这条路就没了。 */
static void bazi_rebuild(ui_app *a)
{
    bazi_rebuild_inner(a);
    a->luck_dirty = 1;
    luck_reset(a);
}

/* ------------------------------------------------------------------
 * 运限链：大运 → 流年 → 流月 → 流日 → 流时
 *
 * 模型全在这里，ui_view.c 只负责画（它调 ui_app_luck_* 那几个口子）。
 *
 * ⚠️ 三处口径照抄上游 `tool-src/scripts/bazi-page.js`：
 *   1. **童限**（:550）不是库函数，是 UI 拼的：起运前那一张，柱取「1 岁的小运」；
 *      它存在的前提是第一步大运的起始虚岁 > 1。
 *   2. **流年卡的柱永远是按年号取的流年柱**（:614 `calculateFlowYear(year)`），
 *      童限下也一样 —— 小运只是那行**底注文字**（`:617`「N岁 · 小运名」）。
 *      童限真正改变的是**柱盘**：那一格从大运柱换成**小运柱**（`:643-645`）。
 *   3. 流年卡的年份/岁数：普通 = `大运起始年 + i` / `起始虚岁 + i`；
 *      童限下 = `出生年 + i` / `i + 1`（`:611-612`）。
 * ------------------------------------------------------------------ */

/* 童限的岁数上端 = 第一步大运的起始虚岁 − 1；< 1 就是没有童限。 */
static int luck_childhood_end(const ui_app *a)
{
    if (!a->chart_ok || a->dayun_n <= 0) return 0;
    const int32_t e = a->dayun[0].start_virtual_age - 1;
    return (e >= 1) ? (int)e : 0;
}

static int luck_has_childhood(const ui_app *a) { return luck_childhood_end(a) > 0; }

/* 该层有几张卡。⚠️ 第 1 层的张数取决于第 0 层选中了谁（童限下只有 N 张）。 */
static int luck_level_count(const ui_app *a, int level)
{
    if (!a->chart_ok || a->dayun_n <= 0) return 0;
    const int ch = luck_has_childhood(a);
    switch (level) {
    case UI_LUCK_DAYUN: return a->dayun_n + (ch ? 1 : 0);
    case UI_LUCK_YEAR:  return (ch && a->luck_sel[UI_LUCK_DAYUN] == 0)
                             ? luck_childhood_end(a) : 10;
    case UI_LUCK_MONTH: return 12;
    case UI_LUCK_DAY:   return a->luck_day_n;
    /* ⚠️ 流时要吃**流日柱**，没有流日就没有流时 —— 不然这一层会报 12 张
     * 却每张都算不出来（按下去像是卡住）。 */
    case UI_LUCK_HOUR:  return (a->luck_day_n > 0) ? 12 : 0;
    default:            return 0;
    }
}

/* 取一张卡。各层填的字段不同（其余留 0）：
 *   0 大运   pillar / civil_year / age（童限卡另有 age_end、is_childhood）
 *   1 流年   pillar（流年柱）/ xiao_yun（底注那根）/ civil_year / age
 *   2 流月   pillar
 *   3 流日   pillar / jdn
 *   4 流时   pillar
 * ⚠️ 第 2/3 层要往上取一层的结果（流月吃流年柱、流时吃流日柱），所以这里
 * 递归——深度最多 5，且都是纯整数，不用担心。 */
static int luck_get(const ui_app *a, int level, int i, ui_luck_entry *out)
{
    memset(out, 0, sizeof *out);
    if (level < 0 || level >= UI_LUCK_LEVELS) return 0;
    if (i < 0 || i >= luck_level_count(a, level)) return 0;

    const int ch  = luck_has_childhood(a);
    const int dir = a->qiyun.direction;
    const sf_ganzhi hour = a->chart.pillars.primary.hour;

    switch (level) {
    case UI_LUCK_DAYUN:
        if (ch && i == 0) {                       /* 童限那张 */
            out->is_childhood = 1;
            out->civil_year   = a->birth.year;
            out->age          = 1;
            out->age_end      = luck_childhood_end(a);
            return sf_bazi_xiao_yun(hour, dir, 1, &out->pillar) == SF_BAZI_OK;
        }
        {
            const sf_bazi_dayun_entry *d = &a->dayun[i - (ch ? 1 : 0)];
            out->pillar     = d->pillar;
            out->civil_year = d->start_civil_time.year;
            out->age        = d->start_virtual_age;
            out->age_end    = d->end_virtual_age;
            return 1;
        }

    case UI_LUCK_YEAR: {
        const int childhood = (ch && a->luck_sel[UI_LUCK_DAYUN] == 0);
        if (childhood) {
            out->civil_year = a->birth.year + i;
            out->age        = i + 1;
        } else {
            const sf_bazi_dayun_entry *d =
                &a->dayun[a->luck_sel[UI_LUCK_DAYUN] - (ch ? 1 : 0)];
            out->civil_year = d->start_civil_time.year + i;
            out->age        = d->start_virtual_age + i;
        }
        out->age_end = out->age;
        /* 柱**恒按年号**取（童限下也一样）；小运只进底注 */
        if (sf_bazi_flow_year(out->civil_year, &out->pillar) != SF_BAZI_OK) return 0;
        return sf_bazi_xiao_yun(hour, dir, out->age, &out->xiao_yun) == SF_BAZI_OK;
    }

    case UI_LUCK_MONTH: {
        ui_luck_entry y;
        if (!luck_get(a, UI_LUCK_YEAR, a->luck_sel[UI_LUCK_YEAR], &y)) return 0;
        return sf_bazi_flow_month(y.pillar, (uint8_t)((i + 2) % 12),
                                  &out->pillar) == SF_BAZI_OK;
    }

    case UI_LUCK_DAY: {
        sf_cal_datetime cd;
        int32_t yy = 0, mo = 0, dd = 0;
        out->jdn = a->luck_day_jdn + i;
        sf_solar_date_from_day_number(out->jdn, &yy, &mo, &dd);
        memset(&cd, 0, sizeof cd);
        cd.year = yy; cd.month = (uint8_t)mo; cd.day = (uint8_t)dd;
        return sf_bazi_flow_day(&cd, &out->pillar) == SF_BAZI_OK;
    }

    default: {                                    /* UI_LUCK_HOUR */
        ui_luck_entry d;
        if (!luck_get(a, UI_LUCK_DAY, a->luck_sel[UI_LUCK_DAY], &d)) return 0;
        return sf_bazi_flow_hour(d.pillar, (uint8_t)i, &out->pillar) == SF_BAZI_OK;
    }
    }
}

int ui_app_luck_count(const ui_app *a, int level) { return luck_level_count(a, level); }

int ui_app_luck_get(const ui_app *a, int level, int i, ui_luck_entry *out)
{
    if (!out) return 0;
    return luck_get(a, level, i, out);
}

int ui_app_luck_active(const ui_app *a)
{
    return (a->luck_open > 0) ? a->luck_open - 1 : -1;
}

/* 柱盘左边那两根。**从深到浅**：slot 0 = 最深的展开层，slot 1 = 次深。
 * 只有两格是因为 6 列是硬上限（7 列 = 22.8px/列，十神两个字要 24px 就撞了），
 * 所以流月/流日/流时**不上柱盘**，它们只在运限页那几行里。 */
int ui_app_luck_board(const ui_app *a, int slot, int *level, ui_luck_entry *out)
{
    if (!level || !out) return 0;
    const int lv = a->luck_open - 1 - slot;
    *level = lv;
    if (lv < 0 || a->luck_open <= slot) return 0;
    if (!luck_get(a, lv, a->luck_sel[lv], out)) return 0;

    /* ⚠️ 童限选中时，第 0 层那一格要换成**小运柱**（上游 :643-645 的
     * `selected.decade = {packed: yearEntry.xiaoYun}`）：选了流年就用那一年的
     * 小运，还没选到流年就用童限自己的（1 岁那根）。 */
    if (lv == UI_LUCK_DAYUN && out->is_childhood) {
        if (a->luck_open > UI_LUCK_YEAR) {
            ui_luck_entry y;
            if (luck_get(a, UI_LUCK_YEAR, a->luck_sel[UI_LUCK_YEAR], &y)) {
                out->pillar = y.xiao_yun;
                return 1;
            }
        }
        /* 没展开流年 → 童限自己那根（out->pillar 就是 1 岁小运） */
    }
    return 1;
}

/* 链的下标夹回合法范围。**盘变了之后必须调**（步数/童限都可能变），
 * 另外每次改链也要过一遍 —— 调用方改的是下标，合法性由这里兜底。 */
static void luck_clamp(ui_app *a)
{
    for (int lv = 0; lv < UI_LUCK_LEVELS; lv++) {
        const int n = luck_level_count(a, lv);
        if (n <= 0) {                 /* 这一层没法展开 → 链就到这里 */
            if (a->luck_open > lv) a->luck_open = lv;
            continue;
        }
        if (a->luck_sel[lv] < 0)     a->luck_sel[lv] = 0;
        if (a->luck_sel[lv] >= n)    a->luck_sel[lv] = n - 1;
    }
    if (a->luck_open < 0) a->luck_open = 0;
    if (a->luck_open > UI_LUCK_LEVELS) a->luck_open = UI_LUCK_LEVELS;
}

/* 换了一张盘（或改了口径）之后，把链**默认选到「含现在的那一步」** ——
 * 照上游 `bazi-page.js:768-770`（`nowJd >= startJdUT1 && < endJdUT1`）：
 * 今天落在哪一步大运就选哪一步；落在起运之前就选**童限**；都不含
 * （盘在未来）就退回第一步。这样打开运限页就是「我现在在哪一步」。 */
static void luck_reset(ui_app *a)
{
    a->luck_open = 0;                 /* 默认不展开：柱盘就是四柱 */
    for (int lv = 0; lv < UI_LUCK_LEVELS; lv++) a->luck_sel[lv] = 0;
    if (!a->chart_ok || a->dayun_n <= 0) return;

    const int ch = luck_has_childhood(a);
    const double now = sf_tt_to_ut(a->jd);
    a->luck_sel[UI_LUCK_DAYUN] = 0;   /* 兜底：第一步（或童限） */
    if (now < a->dayun[0].start_jd_ut1) {
        if (ch) a->luck_sel[UI_LUCK_DAYUN] = 0;         /* 童限 */
    } else {
        for (int k = 0; k < a->dayun_n; k++) {
            if (now >= a->dayun[k].start_jd_ut1 && now < a->dayun[k].end_jd_ut1) {
                a->luck_sel[UI_LUCK_DAYUN] = k + (ch ? 1 : 0);
                break;
            }
        }
    }
    luck_clamp(a);
}

/* 流日那层的取点：该**节气月**的整数天区间（上游 `bazi-page.js:666`
 * `monthCivilDates`）。⚠️ 不是公历月！两端是**两个「节」**（这个月的节 + 下个
 * 月的节），所以这一层要解两次节气 —— 只在下标变了或盘变了时算一次。 */
static const uint8_t kJieIndex[12] = { 21, 23, 1, 3, 5, 7, 9, 11, 13, 15, 17, 19 };

static void luck_days_rebuild(ui_app *a)
{
    a->luck_dirty = 0;
    a->luck_day_n = 0;
    if (!a->chart_ok || a->dayun_n <= 0) return;
    /* ⚠️ 条件是 `>= UI_LUCK_DAY`（流月开着就算），**不是**「流日已经开着」——
     * 那是先有鸡先有蛋：按键那边要看这一层的张数才敢展开（张数 = 0 不许展开），
     * 而张数正是这里算出来的。写成 `> UI_LUCK_DAY` 的话流日永远打不开。 */
    if (a->luck_open < UI_LUCK_DAY) return;

    ui_luck_entry y;
    if (!luck_get(a, UI_LUCK_YEAR, a->luck_sel[UI_LUCK_YEAR], &y)) return;
    const int32_t year = y.civil_year;
    const int mi = a->luck_sel[UI_LUCK_MONTH];    /* 月序：0 = 寅 */

    sf_cal_term_event ev;
    /* ⚠️ 丑月的节（小寒）落在**下一个公历年**，子月的下一个月节同理 ——
     * 上游那两处 +1（`year + (monthIndex===11?1:0)` / `(monthIndex>=10?1:0)`）。 */
    if (sf_cal_get_specific_term(&a->bazi_cal, year + (mi == 11 ? 1 : 0),
                                 kJieIndex[mi], &ev) != 0) return;
    const double off = (double)bazi_offset(a) / 1440.0;
    const int64_t first = (int64_t)floor(ev.jd_ut + off + 0.5);
    if (sf_cal_get_specific_term(&a->bazi_cal, year + (mi >= 10 ? 1 : 0),
                                 kJieIndex[(mi + 1) % 12], &ev) != 0) return;
    const int64_t last = (int64_t)floor(ev.jd_ut + off + 0.5);

    a->luck_day_jdn = first;
    a->luck_day_n   = (int)(last - first);
    if (a->luck_day_n < 0) a->luck_day_n = 0;
}

/* ------------------------------------------------------------------
 * 按钮
 * ------------------------------------------------------------------ */
static int scroll_max(const ui_app *a, int page)
{
    /* ⚠️ 只有万年历那两页有滚动列表。八字的子页号（0/1）会跟
     * UI_PAGE_TODAY(0) / UI_PAGE_LUNAR(1) 撞，所以**必须先看 feature** ——
     * 不然在八字盘上按下键会去读 year_term_n 那个跟它无关的量。 */
    if (a->feature != UI_FEATURE_CALENDAR) return 0;
    /* 整年表两页的行数**不是常数**：闰年节气 25 条（年初那个在年末再现），
     * 朔望 24~26 条。所以读实际条数，再夹住 ≥ 0 —— 不足一屏时不该滚。 */
    int n = 0;
    if (page == UI_PAGE_TERMS)       n = a->year_term_n;
    else if (page == UI_PAGE_PHASES) n = a->year_phase_n;
    return n > UI_LIST_ROWS ? n - UI_LIST_ROWS : 0;
}

/* 显示时区锁。照抄 tools 的 setupHistoricalUtcLock：
 *
 *     const historicalCalendar = form.elements.calendarMode.value === 'historical';
 *     const historicalTerms    = form.elements.pillarHistoricalMode?.value === 'on';
 *     const locked = historicalCalendar || historicalTerms;
 *     ...
 *     note.textContent = historicalCalendar
 *         ? '中国历史历法仅按 UTC+8 生效，时区已锁定'
 *         : historicalTerms ? '历史定气仅按 UTC+8 生效，时区已锁定' : '不自动处理夏令时';
 *
 * 道理：古历归日表是按中国民用日编的，历史定气也锚在 UTC+8 —— 换个时区
 * 读它没有意义。八字页那边同样是 `calendarMode === HISTORICAL ||
 * pillarHistoricalMode === ON ? 480 : readUtcOffset(data)`。
 *
 * 存的是**用户值**（user_offset_minutes），生效值每次推一遍 —— 比上游那套
 * 「进锁时存当前值、出锁时恢复」少一个状态位，解锁后也一定回到用户选的
 * 那一档，而不是回到「进锁前那一刻恰好是什么」。
 *
 * ------------------------------------------------------------------
 * 口径的**推导**：万年历一套（a->cal_cfg）、八字一套（a->bazi_cal），
 * 两套彼此完全独立（上游也是每个工具页各自一份表单、互不共享）。
 * 所以下面这几个实现都收「一个 sf_cal_config + 它的那几个用户意图位」、
 * 不认 ui_app —— `*_cfg` 才是实体，`ui_app` 那几个是薄包装。
 * ⚠️ 加推导规则时改 `*_cfg`，别改包装。
 * ------------------------------------------------------------------ */

/* 时区锁的判据。**只读 config**，所以万年历和八字能共用一条。 */
static int cfg_utc_locked(const sf_cal_config *c)
{
    return c->mode == SF_CAL_CHINA_STANDARD_HISTORICAL
        || c->pillar_historical_mode == SF_PILLAR_HISTORICAL_ON;
}

static void cfg_apply_utc_lock(sf_cal_config *c, int32_t user_offset)
{
    c->utc_offset_minutes = cfg_utc_locked(c) ? 480 : user_offset;
}

static int utc_locked(const ui_app *a)
{
    return cfg_utc_locked(&a->cal_cfg);
}

static void apply_utc_lock(ui_app *a)
{
    cfg_apply_utc_lock(&a->cal_cfg, a->user_offset_minutes);
}

/* 定气日界：**由历法模式推导**，不是独立可选的。
 *
 * 照抄 tools/public/shared/calendar-boundary.js 的 resolveCalendarBoundary：
 *
 *     if (calendarMode !== 'local-astronomical')
 *         return { dayBoundaryMode: FIXED_UTC_OFFSET, meridianDeg: undefined };
 *
 * 也就是「中国古历 / 中国天文」这两档下，日界一律按固定时区偏移，选了
 * 「按当地经度」也不作数 —— 界面把这行变暗，值的来源仍是 utc_offset_minutes。
 * 只有「当地天文」才轮到用户选。
 *
 * 多出来的只有一条播种：tools 那边 meridianDeg 是表单里用户填好的定值，
 * 而这里如果直接跳到一个默认经线，切一下历法初一就可能换天。所以用户没
 * 动过当地经度时，按当前偏移播种（offset/4 = 等价经度），日界一动不动。 */
static void cfg_apply_boundary(sf_cal_config *c, int choice, int meridian_set,
                               int32_t user_offset)
{
    if (c->mode != SF_CAL_LOCAL_ASTRONOMICAL) {
        c->day_boundary_mode = SF_CAL_FIXED_UTC_OFFSET;
        return;
    }
    /* 播种取的是**用户值**而不是生效值：当地经度描述的是「哪个地方」，
     * 时区被锁成 +8 不该把一个用户选好的地方挪走 —— 那会让农历结构跟着跳。 */
    if (choice == SF_CAL_MEAN_SOLAR_MERIDIAN && !meridian_set)
        c->calendar_meridian_deg = (double)user_offset / 4.0;
    c->day_boundary_mode = choice;
}

static void apply_boundary(ui_app *a)
{
    cfg_apply_boundary(&a->cal_cfg, a->boundary_choice, a->meridian_set,
                       a->user_offset_minutes);
}

/* 按 mode 重建配置。**必须走 init 函数**：别的字段也靠它们设默认值，
 * 手动改 mode 一个字段会留下上一个模式的值。重建完再让 boundary 把
 * 日界按新模式推导一遍。 */
static void cfg_apply_cal_mode(sf_cal_config *c, int mode, int32_t user_offset)
{
    const double mer = c->calendar_meridian_deg;

    switch (mode) {
    case SF_CAL_LOCAL_ASTRONOMICAL:
        sf_cal_config_init_local_astronomical_utc_offset(c, user_offset);
        break;
    case SF_CAL_CHINA_STANDARD_ASTRONOMICAL:
        sf_cal_config_init_china_standard_astronomical(c, user_offset);
        break;
    default:
        sf_cal_config_init_china_standard_historical(c, user_offset);
        break;
    }

    c->calendar_meridian_deg = mer;
}

static void apply_cal_mode(ui_app *a, int mode)
{
    const int32_t off = a->cal_cfg.utc_offset_minutes;
    cfg_apply_cal_mode(&a->cal_cfg, mode, off);
    apply_utc_lock(a);
    apply_boundary(a);
}

/* ------------------------------------------------------------------
 * 八字那一套口径（a->bazi_cal）+ 它的推导
 *
 * 两条推导跟万年历**共用同一份实现**（cfg_*），但输入是各自的那一份状态：
 * 历法/节气边界/日界/经度都是两个独立的值。
 * ------------------------------------------------------------------ */

/* 出生地时区的**生效值**。两处用它：出生信息页的值列、bazi_rebuild 的 instant。
 *
 * ⚠️ 锁的判据是**八字自己的**历法/节气边界，不是万年历那套 —— 两个功能的
 * 口径独立，锁也跟着独立。
 * ⚠️ 锁上时值列显示 480 而不是用户存的那个（跟万年历「显示时区」行同一个做法，
 * 那一行在锁上时也显示 +8:00），标签同时换成「时区锁定」。
 *
 * ⚠️ 连带影响（README 里要点名）：默认历法就是「中国古历」，所以**默认状态
 * 下这一行就是灰的 +480**。要排外国的盘，先把八字的历法改成「当地天文」。 */
static int32_t bazi_offset(const ui_app *a)
{
    return cfg_utc_locked(&a->bazi_cal) ? 480 : a->birth_offset;
}

static void bazi_apply_cfg(ui_app *a)
{
    cfg_apply_utc_lock(&a->bazi_cal, a->birth_offset);
    cfg_apply_boundary(&a->bazi_cal, a->bazi_boundary_choice,
                       a->bazi_meridian_set, a->birth_offset);
}

void ui_app_set_view_year(ui_app *a, int year)
{
    if (year < SF_QISHUO_RANGE_START_YEAR) year = SF_QISHUO_RANGE_START_YEAR;
    if (year > SF_QISHUO_RANGE_END_YEAR) year = SF_QISHUO_RANGE_END_YEAR;
    if (year == a->view_year) return;
    a->view_year = year;
    a->year_term_dirty = a->year_phase_dirty = 1;
}

void ui_app_set_utc_offset(ui_app *a, int minutes)
{
    if (minutes < -720) minutes = -720;
    if (minutes >  840) minutes =  840;
    a->user_offset_minutes = minutes;
    apply_utc_lock(a);
    apply_boundary(a);
    invalidate_calendar_pages(a, 1);
}

int ui_app_setting_inert(const ui_app *a, int row)
{
    if (row == UI_SET_TZ)
        return utc_locked(a);
    if (row == UI_SET_BOUNDARY)
        return a->cal_cfg.mode != SF_CAL_LOCAL_ASTRONOMICAL;
    if (row == UI_SET_MERIDIAN)
        return a->cal_cfg.day_boundary_mode != SF_CAL_MEAN_SOLAR_MERIDIAN;
    return 0;
}

int32_t ui_app_bazi_offset(const ui_app *a)
{
    return bazi_offset(a);
}

/* 出生信息页第 row 行现在按不动。两行会被锁，判据都是**八字自己的**
 * 历法/节气边界（不是万年历那套）。
 *
 * ⚠️ row 这个参数不能省成「整页一个开关」：时区锁只关那一行，其余七行
 * （年/月/日/时/分/性别/经度）跟历法口径**无关或另有判据**。第一版就是没带
 * row、拿 `cfg_utc_locked()` 当整页的闸门 —— 而默认历法正是「中国古历」，
 * 于是那一页**所有行都按不动了**，屏上却只把时区那一行画暗，是个
 * 「界面说能改、按键说不能」的分歧。 */
int ui_app_birth_inert(const ui_app *a, int row)
{
    switch (row) {
    case UI_BIRTH_OFFSET:
        return cfg_utc_locked(&a->bazi_cal);
    case UI_BIRTH_MERIDIAN:
        /* ⚠️ 跟万年历设置页那条判据**不一样**，是故意的：出生地经度在这边有
         * **两个**消费者 —— 日界（`calendar_meridian_deg` 当日界的经线）和
         * 排盘钟表（平/真太阳时拿它换经线）。**只要还有一个活着的就不该灰**；
         * 上游那个 longitude 也就是个普通输入框，压根不 disable。
         * （这一条是从八字设置页整段搬过来的 —— 判据一个字没改。） */
        return a->bazi_cal.day_boundary_mode != SF_CAL_MEAN_SOLAR_MERIDIAN
            && a->bazi_time_basis == UI_TIME_CLOCK;
    default:
        return 0;
    }
}

int ui_app_bset_inert(const ui_app *a, int row)
{
    if (row == UI_BSET_BOUNDARY)
        /* 日界由历法推导，不是独立可选的 —— 跟万年历那条逐字相同 */
        return a->bazi_cal.mode != SF_CAL_LOCAL_ASTRONOMICAL;
    return 0;
}

/* 设置页的步长档位。**每行一套** —— 单位不一样的行走同一套会按错：
 * 时区差 30 分和经度差 30° 完全不是一回事。
 *
 * 只有一档的行（枚举类）「换步长」这个键不起作用，「调一下」就是往后/往前
 * 翻一格。所以枚举行不需要单独写分支，走同一套代码即可。 */
static const double STEP_TZ[]  = { 60, 30, 15, 5 };       /* 分钟 */
/* ⚠️ 这一档的**值列必须显示两位小数**（ui_view.c 的 fmt_deg）—— 不然 0.01°
 * 那一档按十下屏上一个字都不动，看着像按键坏了。0.01° 的实义：纬度 1.1 km、
 * 经度在赤道上 2.4 秒时差，是拿来跟别的工具对齐到小数点后两位的，
 * 不是拿来肉眼调日出的。 */
static const double STEP_DEG[] = { 10, 1, 0.1, 0.01 };    /* 度 */
static const double STEP_ONE[] = { 1 };                   /* 枚举：一格 */

static const double *setting_steps(int row, int *n)
{
    switch (row) {
    case UI_SET_TZ:
        *n = (int)(sizeof STEP_TZ / sizeof STEP_TZ[0]);
        return STEP_TZ;
    case UI_SET_MERIDIAN:
    case UI_SET_LAT:
    case UI_SET_LON:
        *n = (int)(sizeof STEP_DEG / sizeof STEP_DEG[0]);
        return STEP_DEG;
    default:
        *n = 1;
        return STEP_ONE;
    }
}

/* 气/朔两页调的是**年份**。1 年一档够用，10 年一档用来跨远 ——
 * 默认就是今年，10 年档 4 下以内能回到 1990 年。 */
static const double YEAR_STEPS[] = { 1, 10 };

/* 「调一下」动的东西在**三处**：
 *   设置页 —— 光标那一行的设置项（setting_steps）
 *   气/朔   —— 选中的年份            （YEAR_STEPS）
 *   出生信息 —— 光标那一格读数        （birth_steps）
 * K5/K7/K8 三键在这三处是**同一个意思**，只是目标不同 —— 学一次到处用。 */

/* 出生信息页各行的档位。跟设置页**同一套约定**：
 *
 *   1. 单位不一样的行走不同的表；
 *   2. 单位**一样**的行直接共用同一张表 —— 出生地时区跟「显示时区」都是分钟，
 *      所以这里指 STEP_TZ，不再抄一份。（抄一份就会像原来那样少一档，
 *      两个屏上同一件事的档位还不一样，换个页就得重新学。）
 *
 * 每行都有表 —— 「只能 +1」不是设计，是原来漏写了这几张表、并且 birth_apply
 * 里把步长整个丢掉了（那几个 `wrap_i(x + d, N)` 只加了 d）。 */
static const double BIRTH_STEP_YEAR[]   = { 1, 10 };            /* 年 */
static const double BIRTH_STEP_MONTH[]  = { 1, 3, 6 };          /* 月，12 回环 */
static const double BIRTH_STEP_DAY[]    = { 1, 5 };             /* 日，夹在 1..31 */
static const double BIRTH_STEP_HOUR[]   = { 1, 6, 12 };         /* 时，24 回环 */
static const double BIRTH_STEP_MINUTE[] = { 1, 5, 15, 30 };     /* 分，60 回环 */

static const double *birth_steps(int row, int *n)
{
#define BIRTH_TABLE(t) do { *n = (int)(sizeof t / sizeof t[0]); return t; } while (0)
    switch (row) {
    case UI_BIRTH_YEAR:   BIRTH_TABLE(BIRTH_STEP_YEAR);
    case UI_BIRTH_MONTH:  BIRTH_TABLE(BIRTH_STEP_MONTH);
    case UI_BIRTH_DAY:    BIRTH_TABLE(BIRTH_STEP_DAY);
    case UI_BIRTH_HOUR:   BIRTH_TABLE(BIRTH_STEP_HOUR);
    case UI_BIRTH_MINUTE: BIRTH_TABLE(BIRTH_STEP_MINUTE);
    case UI_BIRTH_OFFSET: BIRTH_TABLE(STEP_TZ);      /* 跟设置页的显示时区共用 */
    /* 经度用设置页那张 10/1/0.1/0.01 的度表（同一件事在两处是同一组档位）。
     * 值行要按 2 位小数显示 —— 0.01 那一档才看得见动了。 */
    case UI_BIRTH_MERIDIAN: BIRTH_TABLE(STEP_DEG);
    default:              *n = 1; return STEP_ONE;   /* 性别：枚举，一格 */
    }
#undef BIRTH_TABLE
}

/* 八字设置页各行的档位。**整页都是枚举行**（钟表/历法/日界/节气边界/早晚子时），
 * 所以一律 STEP_ONE —— 「换步长」在这一页没有意义。
 * ⚠️ 挪走「出生地经度」之前这里有过一行度表；那一行现在在出生信息页上，
 * 用同一张 STEP_DEG。 */
static const double *bset_steps(int row, int *n)
{
    (void)row;
    *n = 1;
    return STEP_ONE;                 /* 枚举：一格 */
}

static const double *active_steps(const ui_app *a, int *n)
{
    if (a->feature == UI_FEATURE_BAZI) {
        if (a->page == UI_BAZI_PAGE_SETTINGS) return bset_steps(a->bset_row, n);
        return birth_steps(a->birth_row, n);
    }
    if (a->feature == UI_FEATURE_CALENDAR
        && (a->page == UI_PAGE_TERMS || a->page == UI_PAGE_PHASES)) {
        *n = (int)(sizeof YEAR_STEPS / sizeof YEAR_STEPS[0]);
        return YEAR_STEPS;
    }
    return setting_steps(a->set_row, n);
}

/* 哪一页在用自己的档位下标。**判据只有一条**（出生信息页的表跟别处完全不同：
 * 年/月/日/时/分），下面两个函数共用它 —— 分成两处写迟早会漂。
 *
 * 出生信息页**自己记一份**（`birth_step_idx`）：它跟设置页的表毫无关系，
 * 共用一个下标就是互相踩 —— 在设置页调到 0.01° 那一档，走进出生页按一下
 * K7 换档，再退回来 0.01° 就没了（而 K7 的反馈只在顶栏那一小段字上）。
 *
 * 八字设置页则**共用** `step_idx`：它跟万年历设置页的行同型（经度→STEP_DEG、
 * 其余枚举），共用正是想要的 —— 在那边调好的 0.01° 走过来还在。 */
static int bazi_birth_slot(const ui_app *a)
{
    return a->feature == UI_FEATURE_BAZI && a->page == UI_BAZI_PAGE_BIRTH;
}

static int step_index(const ui_app *a)
{
    return bazi_birth_slot(a) ? a->birth_step_idx : a->step_idx;
}

static void step_index_set(ui_app *a, int k)
{
    if (bazi_birth_slot(a)) a->birth_step_idx = k;
    else                    a->step_idx        = k;
}

static double magnitude_of(const ui_app *a, const double *st, int n)
{
    int k = step_index(a);
    if (k < 0) k = 0;
    if (k >= n) k = n - 1;
    return st[k];
}

/* 当前功能有几个子页。⚠️ 通用分支里所有 `% N` 都必须走这里 ——
 * `page` 是两个功能共用的裸 int，UI_BAZI_PAGE_CHART(1) 会撞上
 * UI_PAGE_LUNAR(1)，拿 UI_PAGE_COUNT 去模会翻出界。 */
static int subpage_count(const ui_app *a)
{
    return (a->feature == UI_FEATURE_BAZI) ? UI_BAZI_PAGE_COUNT : UI_PAGE_COUNT;
}

static int wrap_i(int v, int n)
{
    const int r = v % n;
    return r < 0 ? r + n : r;
}

/* 出生信息页：「调一下」。
 *
 * ⚠️ 日期**不做「自动夹到月末」**：2 月 30 日就让它留在那儿、顶栏明说
 * 「无此日期」。用户手滑打到 30 就说明他要改，悄悄改成 28 反而更坏 ——
 * 而且那也是库的 valid_civil_time 的态度（回环校验失败就拒绝，不是修正）。
 *
 * 回环 vs 夹住，按**是不是真周期**分：
 *   月 12 / 时 24 / 分 60 是真周期 —— 到顶了绕回去（23 时 + 6 → 5 时）；
 *   年 / 日 / 偏移是**有界量** —— 越界就停住。31 日再往上不是「回到 1 日」，
 *   那是 1..31 这个周期本身就不成立（本 UI 不做月长表，理由见下）；
 *   而偏移停住是 UTC±14 的硬边界。
 *
 * ⚠️ 日为什么不做月长表：这个库的「某月有没有 30 日」是**回环查一遍历法**
 * 得到的（见 sf_bazi_valid_civil_time），而历法模式不同答案就不同
 * （「中国古历」下 1582-10-05..14 根本不存在）。在 UI 里再写一张公历闰年表
 * = 第二处真相，两边一旦不一致，用户看到的是「表格说合法、顶栏说无此日期」。
 * 所以：**日子只夹在 1..31，合不合法由库那一句话说了算**，顶栏照实转述。 */
static void birth_apply(ui_app *a)
{
    int n = 0;
    const double *st = active_steps(a, &n);
    /* ⚠️ **不能在这里就把步长整数化**：经度那一档有 0.01°，`lround(0.01) = 0`，
     * 那一档就永远按不动了 —— 这正是「出生地经度」还在设置页时踩过的坑
     * （`bset_apply` 里那段注还在）。行搬到这一页，坑也跟着搬过来了。
     * 所以：**double 的留一份给经度**，整数化的那份只发给下面几个整数行。 */
    const double magd = magnitude_of(a, st, n);
    const int mag = (int)lround(magd);
    const int d = a->step_plus ? +1 : -1;

    switch (a->birth_row) {
    case UI_BIRTH_YEAR: {
        int y = a->birth.year + d * mag;
        /* 夹在库的有效范围内（±8000 年那一条）—— 跑出去四柱就解不出来，
         * 而用户只看到一片空白，比「按不动」更费解 */
        if (y < -6000) y = -6000;
        if (y >  10000) y =  10000;
        a->birth.year = y;
        break;
    }
    case UI_BIRTH_MONTH:  a->birth.month  = wrap_i(a->birth.month  - 1 + d * mag, 12) + 1; break;
    case UI_BIRTH_DAY: {
        int v = a->birth.day + d * mag;
        if (v < 1)  v = 1;
        if (v > 31) v = 31;
        a->birth.day = v;
        break;
    }
    case UI_BIRTH_HOUR:   a->birth.hour   = wrap_i(a->birth.hour   + d * mag, 24); break;
    case UI_BIRTH_MINUTE: a->birth.minute = wrap_i(a->birth.minute + d * mag, 60); break;
    case UI_BIRTH_GENDER:
        /* 未选 → 先给**男**（乾造在前，跟上游表单 radio 的顺序一致）；
         * 选中之后在 男/女 之间翻。不这么做的话 -1+1=0 会先蹦到女。 */
        if (a->gender == SF_BAZI_GENDER_NONE)
            a->gender = (d > 0) ? SF_BAZI_GENDER_MALE : SF_BAZI_GENDER_FEMALE;
        else
            a->gender = wrap_i(a->gender + d, 2);
        break;
    case UI_BIRTH_OFFSET: {
        int m = a->birth_offset + d * mag;
        if (m < -720) m = -720;            /* UTC−12:00 */
        if (m >  840) m =  840;            /* UTC+14:00 */
        a->birth_offset = m;
        break;
    }
    case UI_BIRTH_MERIDIAN: {
        /* 跟万年历设置页那条逐字一样（含 180 的硬边界和 `tidy`）—— 同一个
         * 旋钮，两处同一组档位、同一个手感，改的只是**八字这一份**配置。
         * ⚠️ 不需要跟着调 bazi_apply_cfg：经线是**解算时读**的（日界和排盘
         * 钟表都是当场拿 `calendar_meridian_deg` 算），没有缓存要刷新。 */
        double v = tidy(a->bazi_cal.calendar_meridian_deg + d * magd);
        if (v < -180.0) v = -180.0;
        if (v >  180.0) v =  180.0;
        a->bazi_cal.calendar_meridian_deg = v;
        a->bazi_meridian_set = 1;   /* 用户动过了，别再拿偏移去播种 */
        break;
    }
    default: break;
    }
    a->chart_dirty = 1;
}

static double setting_magnitude(const ui_app *a)
{
    int n = 0;
    return magnitude_of(a, setting_steps(a->set_row, &n), n);
}

double ui_app_step_magnitude(const ui_app *a)
{
    int n = 0;
    const double *st = active_steps(a, &n);
    /* ⚠️ 判据是**档位数**，不是数值 —— 经纬度的档位表里就有 1° 这一档，
     * 拿「mag == 1」当「没有步长」会让 1° 那一档的顶栏退回显示时刻。 */
    if (n <= 1) return 0.0;                  /* 枚举：没有「步长」可言 */
    /* ⚠️ feature 那一半不能省：`page` 是两个功能共用的裸 int，
     * UI_BAZI_PAGE_BIRTH(0) / _CHART(1) 会撞上 UI_PAGE_TODAY / _LUNAR。
     * 现在恰好撞不到 UI_PAGE_SETTINGS(5)，但那是**巧合**，不是推理。 */
    if (a->feature == UI_FEATURE_CALENDAR && a->page == UI_PAGE_SETTINGS
        && ui_app_setting_inert(a, a->set_row))
        return 0.0;
    /* 失效的行没有步长可言 —— 顶栏该显示别的（出生页显示日期） */
    if (a->feature == UI_FEATURE_BAZI) {
        if (a->page == UI_BAZI_PAGE_SETTINGS
            && ui_app_bset_inert(a, a->bset_row)) return 0.0;
        if (a->page == UI_BAZI_PAGE_BIRTH
            && ui_app_birth_inert(a, a->birth_row)) return 0.0;
    }
    return magnitude_of(a, st, n);
}

/* 「调一下」在气/朔两页上做的事：换年份。 */
static void year_step(ui_app *a)
{
    int n = 0;
    const double *st = active_steps(a, &n);
    const int mag = (int)lround(magnitude_of(a, st, n));
    int y = a->view_year + (a->step_plus ? mag : -mag);
    if (y < SF_QISHUO_RANGE_START_YEAR) y = SF_QISHUO_RANGE_START_YEAR;
    if (y > SF_QISHUO_RANGE_END_YEAR) y = SF_QISHUO_RANGE_END_YEAR;
    if (y != a->view_year) {
        a->view_year = y;
        a->year_term_dirty = a->year_phase_dirty = 1;
    }
    a->flash = 6;        /* 跟别的页按 OK 一样闪一下，确认按到了 */
}

/* 0.1° 按 32 下之后会攒出 116.79999999 这种尾巴，显示虽然四舍五入看不出来，
 * 但下次播种/换算就会露出来。每步归整到 4 位小数 —— 观测经纬度的默认值
 * （116.4074）本来就是 4 位，归整不会动它。 */
static double tidy(double v)
{
    return floor(v * 10000.0 + 0.5) / 10000.0;
}

/* 「调一下」：按当前步长和方向改一格。 */
static void setting_apply(ui_app *a)
{
    const double mag = setting_magnitude(a);
    const int d = a->step_plus ? +1 : -1;

    /* 整年表要不要跟着重建。**只有真正喂进 sf_qishuo_options 的那几行才置位** ——
     * 一次重建是一整轮 sf_qishuo_year_run（HIGH 档约 50 次全量求解），
     * 按着观测经纬度也重建纯属浪费。
     *
     * ⚠️ 往下面这个 switch 里**加行时记得看一眼**：改的东西只要进了
     * sf_qishuo_options（见 year_fill 的投影表），就得在这里置位。漏了就是
     * 「设置改了、气/朔两页还是老样子」，而且只有换年份或重启才刷得掉，
     * 极难归因。 */
    int year_cfg_changed = 0;

    switch (a->set_row) {
    case UI_SET_TZ: {
        /* 改的是**用户值**；生效值由 apply_utc_lock 推。
         * （锁着的时候这行是变暗的，根本走不到这儿。） */
        int v = a->user_offset_minutes + (int)lround(d * mag);
        if (v < -720) v = -720;          /* UTC−12:00 */
        if (v >  840) v =  840;          /* config_valid 的上限是 ±840 */
        a->user_offset_minutes = v;
        apply_utc_lock(a);
        /* 偏移也是「按当地经度」那档的播种来源，改完重推一次 */
        apply_boundary(a);
        year_cfg_changed = 1;            /* 窗口边界按它切 */
        break;
    }
    case UI_SET_BASIS:
        a->time_basis = (a->time_basis + d + 3) % 3;
        break;
    case UI_SET_RAT_HOUR:
        a->rat_hour = (a->rat_hour + d + 3) % 3;
        break;
    case UI_SET_CAL_MODE:
        apply_cal_mode(a, (a->cal_cfg.mode + d + 3) % 3);
        year_cfg_changed = 1;
        break;
    case UI_SET_BOUNDARY:
        a->boundary_choice = (a->boundary_choice == SF_CAL_FIXED_UTC_OFFSET)
                               ? SF_CAL_MEAN_SOLAR_MERIDIAN
                               : SF_CAL_FIXED_UTC_OFFSET;
        apply_boundary(a);
        year_cfg_changed = 1;
        break;
    case UI_SET_MERIDIAN: {
        double v = tidy(a->cal_cfg.calendar_meridian_deg + d * mag);
        if (v < -180.0) v = -180.0;
        if (v >  180.0) v =  180.0;
        a->cal_cfg.calendar_meridian_deg = v;
        a->meridian_set = 1;     /* 用户动过了，别再拿偏移去播种 */
        year_cfg_changed = 1;
        break;
    }
    case UI_SET_PILLAR_HIST:
        a->cal_cfg.pillar_historical_mode =
            (a->cal_cfg.pillar_historical_mode + d + 3) % 3;
        /* 「历史定气」也锁时区 —— 跟历法那条路是同一把锁 */
        apply_utc_lock(a);
        apply_boundary(a);
        year_cfg_changed = 1;            /* 经由上面那把锁改了生效时区 */
        break;
    case UI_SET_ACCURACY:
        /* 精度档：0/1/2 = LOW/MED/HIGH，**方向键的「加」就是提高精度**。
         * 枚举值本来就是低→高排的，所以不用像别的枚举行那样倒着走。 */
        a->accuracy = (a->accuracy + d + 3) % 3;
        year_cfg_changed = 1;
        break;
    case UI_SET_LAT: {
        double v = tidy(a->observer_lat + d * mag);
        if (v < -90.0) v = -90.0;
        if (v >  90.0) v =  90.0;
        a->observer_lat = v;
        break;
    }
    case UI_SET_LON: {
        double v = tidy(a->observer_lon + d * mag);
        if (v < -180.0) v = -180.0;
        if (v >  180.0) v =  180.0;
        a->observer_lon = v;
        break;
    }
    default:
        return;
    }
    switch (a->set_row) {
    case UI_SET_BASIS:
    case UI_SET_RAT_HOUR:
        a->pillars_ok = 0;
        break;
    case UI_SET_ACCURACY:
        a->year_term_dirty = a->year_phase_dirty = 1;
        break;
    case UI_SET_LAT:
        a->rise_cached = 0;
        a->moon_dirty = 1;
        break;
    case UI_SET_LON:
        a->rise_cached = 0;
        a->moon_dirty = 1;
        /* 地方平时 / 真太阳时的当前四柱也吃观测经度。 */
        if (a->time_basis != UI_TIME_CLOCK) a->pillars_ok = 0;
        break;
    default:
        invalidate_calendar_pages(a, year_cfg_changed);
        break;
    }
}

/* 八字设置页：「调一下」。
 *
 * 跟 setting_apply 是**两套状态**（bazi_cal vs cal_cfg），所以不能共用函数；
 * 但每一行的处理逐行对齐 —— 改这里时顺手看一眼那边，两页的行是一一对应的。
 *
 * ⚠️ 这一页**没有数值行**（整页枚举），所以用不到步长 —— 「换步长」那个键
 * 在这一页是空转的。经度那一行搬去出生信息页之前，这里算过一次 `magnitude_of`，
 * 而且**必须留成 double**（0.01° 那一档 lround 成 0 就永远按不动）；现在
 * 那个陷阱跟着那一行搬到了 `birth_apply` 里，那边的 `mag` 也是 double 起的、
 * 只在最后才 `lround`。 */
static void bset_apply(ui_app *a)
{
    const int d = a->step_plus ? +1 : -1;

    switch (a->bset_row) {
    case UI_BSET_TIME_BASIS:
        /* 枚举排的是 钟表时间 / 平太阳时 / 真太阳时，跟 UI_TIME_* 同序，
         * 方向键的「加」就是往真太阳时那头走。 */
        a->bazi_time_basis = (a->bazi_time_basis + d + 3) % 3;
        break;
    case UI_BSET_CAL_MODE:
        /* 走 cfg_apply_cal_mode（它会保住经度），再让 bazi_apply_cfg 把
         * 时区锁和日界按新模式推一遍 —— 跟万年历那条路同一个次序。 */
        cfg_apply_cal_mode(&a->bazi_cal, (a->bazi_cal.mode + d + 3) % 3,
                           a->birth_offset);
        bazi_apply_cfg(a);
        break;
    case UI_BSET_BOUNDARY:
        a->bazi_boundary_choice =
            (a->bazi_boundary_choice == SF_CAL_FIXED_UTC_OFFSET)
                ? SF_CAL_MEAN_SOLAR_MERIDIAN : SF_CAL_FIXED_UTC_OFFSET;
        bazi_apply_cfg(a);
        break;
    case UI_BSET_PILLAR_HIST:
        /* 「历史定气」也锁时区 —— 跟万年历那条路是同一把锁（cfg_utc_locked） */
        a->bazi_cal.pillar_historical_mode =
            (a->bazi_cal.pillar_historical_mode + d + 3) % 3;
        bazi_apply_cfg(a);
        break;
    case UI_BSET_RAT_HOUR:
        a->bazi_rat_hour = (a->bazi_rat_hour + d + 3) % 3;
        break;
    default:
        return;
    }
    a->chart_dirty = 1;      /* 口径变了 → 下次 tick 重排这张盘 */
}

/* 「换步长」：在**当前这一页**的档位表里往后滚一格。
 * ⚠️ 必须走 active_steps，不能直接拿 setting_steps(a->set_row) —— 在气/朔
 * 两页上 set_row 是设置页残留的光标位置，跟这里的档位表根本不是一个东西：
 * 轻则按了没反应（撞上枚举行 n=1），重则滚出一个大于 2 的下标，
 * 再被 magnitude_of 夹到 10 年那一档，看着像「步长键时灵时不灵」。
 *
 * ⚠️ 写回也要走 step_index —— 出生信息页那份下标是**另一个变量**
 * （见 step_index 的注）。设置页、气朔页、出生信息页共用这一个函数。 */
static void cycle_step(ui_app *a)
{
    int n = 0;
    (void)active_steps(a, &n);
    if (n <= 1) return;
    step_index_set(a, (step_index(a) + 1) % n);
}

/* 光标到哪，设置页的窗口就跟到哪。十行放不下六行的屏，但**不画滚动条**：
 * 光标自己就是指示器（它永远可见），而且 10 行只滚 4 步，不像「气」那页要
 * 滚 18 步。真要画也画得下，只是这一页的值都右对齐到 0.95，边上没地方了。 */
static void settings_keep_visible(ui_app *a)
{
    int *s = &a->scroll[UI_PAGE_SETTINGS];
    if (a->set_row < *s) *s = a->set_row;
    if (a->set_row >= *s + UI_SET_ROWS) *s = a->set_row - UI_SET_ROWS + 1;
    if (*s > UI_SET_COUNT - UI_SET_ROWS) *s = UI_SET_COUNT - UI_SET_ROWS;
    if (*s < 0) *s = 0;
}

void ui_app_button(ui_app *a, ui_btn b, int pressed)
{
    if (!pressed) return;                 /* 目前只吃按下沿 */

    /* ------------------------------------------------------------------
     * 功能选择页（feature == -1）
     *
     * 上下左右**都**移动高亮（用户要的是「上下左右滑」），OK 进去。
     * 只有两个功能，所以左右与上下等价 —— 但两对键都认，省得猜。
     * ACT 照旧切主题；BACK 在这一页没有上一级，不响应。
     * ------------------------------------------------------------------ */
    if (a->feature < 0) {
        if (b == UI_BTN_UP || b == UI_BTN_LEFT) {
            a->pick = (a->pick + UI_FEATURE_COUNT - 1) % UI_FEATURE_COUNT;
        } else if (b == UI_BTN_DOWN || b == UI_BTN_RIGHT || b == UI_BTN_MENU) {
            a->pick = (a->pick + 1) % UI_FEATURE_COUNT;
        } else if (b == UI_BTN_OK) {
            a->feature = a->pick;
            a->page    = UI_PAGE_TODAY;
            a->flash   = 6;
        } else if (b == UI_BTN_ACT) {
            a->night = !a->night;
        }
        return;                            /* 这一页把**所有**键都截住 */
    }

    /* ------------------------------------------------------------------
     * 八字：出生信息页 —— 与设置页**完全同一套三键**
     * （上下列、OK 调值、MENU 换步长、ACT 切 ±），左右键切子页、
     * BACK 逐级退回。所以这里只拦四个，其余落到通用分支。
     * ------------------------------------------------------------------ */
    if (a->feature == UI_FEATURE_BAZI && a->page == UI_BAZI_PAGE_BIRTH) {
        if (b == UI_BTN_UP || b == UI_BTN_DOWN) {
            a->birth_row += (b == UI_BTN_UP) ? -1 : +1;
            if (a->birth_row < 0) a->birth_row = 0;
            if (a->birth_row >= UI_BIRTH_ROW_COUNT)
                a->birth_row = UI_BIRTH_ROW_COUNT - 1;
            return;
        }
        if (b == UI_BTN_ACT) { a->step_plus = !a->step_plus; return; }
        /* ⚠️ 变暗的行**不吃按键** —— 照抄万年历设置页那套（上游的
         * `controls.forEach(c => c.disabled = locked)`）。不挡的话按下去
         * 没反应、内部值却已经翻了：时区被锁着时用户以为改的是 +8，
         * 一解锁蹦出来一个他从来没选过的偏移。 */
        if (b == UI_BTN_OK) {
            if (!ui_app_birth_inert(a, a->birth_row)) birth_apply(a);
            return;
        }
        if (b == UI_BTN_MENU){
            if (!ui_app_birth_inert(a, a->birth_row)) cycle_step(a);
            return;
        }
    }

    /* ------------------------------------------------------------------
     * 八字：设置页 —— 六行，跟万年历设置页**同一套编辑三键**。
     * 变暗的行同样不吃按键。K3/K4 翻页、K6 退一级落到通用分支。
     * ------------------------------------------------------------------ */
    if (a->feature == UI_FEATURE_BAZI && a->page == UI_BAZI_PAGE_SETTINGS) {
        if (b == UI_BTN_UP || b == UI_BTN_DOWN) {
            a->bset_row += (b == UI_BTN_UP) ? -1 : +1;
            if (a->bset_row < 0) a->bset_row = 0;
            if (a->bset_row >= UI_BSET_COUNT) a->bset_row = UI_BSET_COUNT - 1;
            return;
        }
        if (b == UI_BTN_ACT) { a->step_plus = !a->step_plus; return; }
        if (b == UI_BTN_OK) {
            if (!ui_app_bset_inert(a, a->bset_row)) bset_apply(a);
            return;
        }
        if (b == UI_BTN_MENU) {
            if (!ui_app_bset_inert(a, a->bset_row)) cycle_step(a);
            return;
        }
    }

    /* ------------------------------------------------------------------
     * 八字：排盘页
     *
     * OK **递进**：四柱 → +大运 → +流年 → 回到四柱（列数 4/5/6 循环）。
     * 上下换第几步大运（只在显示大运时才有意义）。
     * 左右切子页、BACK 退一级 —— 落到通用分支。
     * ------------------------------------------------------------------ */
    if (a->feature == UI_FEATURE_BAZI && a->page == UI_BAZI_PAGE_CHART) {
        /* OK / DOWN = 往下**加**一级，UP = **退**一级。
         * ⚠️ 这里原来是 `chart_mode = (chart_mode+1) % 3` —— 从 6 列想回 5 列
         * 得绕一圈（6→4→5），用户问的就是这个「怎么取消流年」。现在退一级
         * 就是退一级（夹在 0..5，不回环）。 */
        if (b == UI_BTN_OK || b == UI_BTN_DOWN) {
            /* ⚠️ 没选性别时**别装作递进**：顺逆判据要靠性别，`dayun_n` 是 0，
             * 按下去什么都不会多出来 —— 用户会以为「流年大运没写」。
             * 与其静默不动，不如把人送到该去的那一行。 */
            if (a->gender != SF_BAZI_GENDER_MALE
                && a->gender != SF_BAZI_GENDER_FEMALE) {
                a->page      = UI_BAZI_PAGE_BIRTH;
                a->birth_row = UI_BIRTH_GENDER;
                a->flash     = 6;
                return;
            }
            if (a->luck_open < UI_LUCK_LEVELS
                && luck_level_count(a, a->luck_open) > 0) {
                a->luck_sel[a->luck_open] = 0;
                a->luck_open++;
                a->luck_dirty = 1;      /* 新展开那层可能要吃节气（流日） */
            }
            a->flash = 6;
            return;
        }
        if (b == UI_BTN_UP) {
            if (a->luck_open > 0) {
                a->luck_open--;
                a->luck_dirty = 1;
            }
            return;
        }
        /* LEFT / RIGHT / BACK 落到通用分支：翻页、退一级页面。 */
    }

    /* ------------------------------------------------------------------
     * 八字：运限页 —— 五级联动
     *
     * 手势**就是全站那套编辑手势**（设置页 / 出生信息页 / 主页选日期）：
     *   K1/K2  选「层」—— 光标往下就是展开一层，往上就是收一层
     *   K5     **动**当前这一层：换一张卡（往前还是往后由 K8 的 ＋/− 定）
     *   K8     切 ＋/−            → 顶栏右上角就显示这个符号
     *   K3/K4  翻页 —— **回归全站惯例**（K7 跟 K4 同义，也都是翻页）
     *   K6     退一级页面
     *
     * ⚠️ 这里原来是我把「左右能选中看」直接压在 K3/K4 上，代价是把全站唯一
     * 一处「左右不翻页」的例外做进了这一页 —— 用户在别的页左右翻惯了，到了
     * 这里左右一跳就换了大运。**「选中」得是显式的一步**：K5 动，K8 定方向。
     * 位置本身不丢：每行右端那个副标签就写着 `3/8 33-42`。
     * ------------------------------------------------------------------ */
    if (a->feature == UI_FEATURE_BAZI && a->page == UI_BAZI_PAGE_LUCK) {
        if (b == UI_BTN_DOWN) {
            if (a->luck_open < UI_LUCK_LEVELS
                && luck_level_count(a, a->luck_open) > 0) {
                a->luck_sel[a->luck_open] = 0;
                a->luck_open++;
                a->luck_dirty = 1;
                a->flash = 6;
            }
            return;
        }
        if (b == UI_BTN_UP) {
            if (a->luck_open > 0) {
                a->luck_open--;
                a->luck_dirty = 1;
            }
            return;
        }
        if (b == UI_BTN_OK) {
            const int lv = ui_app_luck_active(a);
            if (lv >= 0) {
                const int n = luck_level_count(a, lv);
                if (n <= 0) return;     /* 空层：动了也是白动，别把下标弄成 −1 */
                const int d = a->step_plus ? +1 : -1;
                int v = a->luck_sel[lv] + d;
                if (v < 0) v = 0;       /* 夹住**不回环**：大运到头就是到头 */
                if (v >= n) v = n - 1;
                if (v != a->luck_sel[lv]) {
                    a->luck_sel[lv] = v;
                    a->luck_dirty = 1;  /* 第 2 层变了 → 流日那一段要重取 */
                    a->flash = 6;
                }
            }
            return;
        }
        if (b == UI_BTN_ACT) {          /* z：切 ＋/− */
            a->step_plus = !a->step_plus;
            return;
        }
        /* LEFT / RIGHT / MENU / BACK 落到通用分支：翻页、退一级页面。 */
        /* BACK 落到通用分支（逐级退回 → 排盘页） */
    }

    /* 设置页的按键：调值挪到「确定」上，**左右键还给翻页** ——
     * 这一页不再是特殊页，K3/K4 跟其它页一个意思。
     * （K7 也不翻页了，改成换步长；往后走用 K4，回主页用 K6。） */
    if (a->feature == UI_FEATURE_CALENDAR && a->page == UI_PAGE_SETTINGS) {
        if (b == UI_BTN_UP || b == UI_BTN_DOWN) {
            a->set_row += (b == UI_BTN_UP) ? -1 : +1;
            if (a->set_row < 0) a->set_row = 0;
            if (a->set_row >= UI_SET_COUNT) a->set_row = UI_SET_COUNT - 1;
            settings_keep_visible(a);
            return;
        }
        if (b == UI_BTN_ACT) {          /* 切 ＋/－ */
            a->step_plus = !a->step_plus;
            return;
        }
        /* ⚠️ 下面两个对变暗的行**不响应** —— 照抄 tools 的
         * `controls.forEach(c => c.disabled = locked)`。
         * 不挡的话，按下没反应但内部值已经翻了，等切到「当地天文」时
         * 定气日界会莫名其妙变成「按当地经度」—— 一个纯隐形的状态。 */
        if (b == UI_BTN_OK) {
            if (!ui_app_setting_inert(a, a->set_row)) setting_apply(a);
            return;
        }
        if (b == UI_BTN_MENU) {
            if (!ui_app_setting_inert(a, a->set_row)) cycle_step(a);
            return;
        }
        /* LEFT / RIGHT / BACK 落到下面的通用分支：翻页、回主页。
         * OK 的「回主页」没了 —— 但那本来就是 K6 的事，K6 就在旁边。 */
    }

    /* 气/朔两页：K5/K7/K8 也是**同一套编辑三键**，只是目标换成**年份**。
     * 上下键照旧滚动、左右键照旧翻页 —— 这两页不重载任何导航键。
     * 代价（写进 sim/README）：这两页 K5 不再是「回主页」（K6 照旧）、
     * K8 不再是「日夜主题」。 */
    /* ⚠️ feature 那一半**不能省** —— `page` 是两个功能共用的裸 int，
     * 八字的子页号会跟万年历的撞：`UI_BAZI_PAGE_SETTINGS`(2) == `UI_PAGE_TERMS`(2)、
     * `UI_BAZI_PAGE_LUCK`(3) == `UI_PAGE_PHASES`(3)。少了这道闸门，
     * 八字那两页按 K5 会去改**气/朔的年份**（`year_step`），而屏上是另一页 ——
     * 一个纯隐形的状态改动。八字的按键块排在这段之前，所以只有「那几页没截住
     * 的键」才会漏下来，正是最难发现的那一类。 */
    if (a->feature == UI_FEATURE_CALENDAR
        && (a->page == UI_PAGE_TERMS || a->page == UI_PAGE_PHASES)) {
        if (b == UI_BTN_ACT)  { a->step_plus = !a->step_plus; return; }
        if (b == UI_BTN_MENU) { cycle_step(a);        return; }
        if (b == UI_BTN_OK)   { year_step(a);                 return; }
    }

    switch (b) {
    case UI_BTN_MENU:
    case UI_BTN_RIGHT:
        a->scroll[a->page] = 0;
        a->page = (a->page + 1) % subpage_count(a);
        break;

    case UI_BTN_LEFT:
        a->scroll[a->page] = 0;
        a->page = (a->page + subpage_count(a) - 1) % subpage_count(a);
        break;

    case UI_BTN_UP:
    case UI_BTN_DOWN: {
        const int d = (b == UI_BTN_UP) ? -1 : +1;
        const int m = scroll_max(a, a->page);
        int s = a->scroll[a->page] + d;
        if (s < 0) s = 0;
        if (s > m) s = m;
        a->scroll[a->page] = s;
        break;
    }

    case UI_BTN_OK:
        /* 主页 → 节气表；其它页 → 回主页。列表页里 OK 相当于「回顶」 */
        if (a->page == UI_PAGE_TODAY) {
            a->page = UI_PAGE_TERMS;
        } else {
            a->scroll[a->page] = 0;
            a->page = UI_PAGE_TODAY;
        }
        a->flash = 6;
        break;

    case UI_BTN_BACK:
        /* **逐级退回**：子页 → 主页 → 选择页。两个功能走同一条规矩。
         *
         * ⚠️ 万年历这边原来是「主页按 BACK 还是主页」—— 于是功能选择页
         * **除了开机之外没有第二条路能进去**（得先进八字、再按两次 BACK 绕
         * 回来）。两级模型的第二级必须退得出去，这是那个 bug 的根。 */
        if (a->feature == UI_FEATURE_BAZI) {
            /* 按**子页号**退（跟页码点的顺序一致）：设置 → 运限 → 排盘 →
             * 出生信息 → 选择页。加了「运限」这一页之后，写死「退到出生信息」
             * 那种写法就得每加一页改一次。 */
            if (a->page > UI_BAZI_PAGE_BIRTH) a->page--;
            else                               a->feature = -1;
            a->pick = UI_FEATURE_BAZI;         /* 回去时高亮停在刚用的那个 */
            break;
        }
        if (a->page == UI_PAGE_TODAY) {
            /* 已经在这一级的顶上了 —— 再退一级就是选择页。
             * ⚠️ 别让控制流落到下面那句 `page = TODAY`（那是个空操作，
             * 但会让人以为这儿有「回主页」的意思）。 */
            a->feature = -1;
            a->pick    = UI_FEATURE_CALENDAR;
            a->scroll[UI_PAGE_TERMS]  = 0;
            a->scroll[UI_PAGE_PHASES] = 0;
            break;
        }
        a->scroll[UI_PAGE_TERMS] = 0;
        a->scroll[UI_PAGE_PHASES] = 0;
        a->page = UI_PAGE_TODAY;
        break;

    case UI_BTN_ACT:
        a->night = !a->night;
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------
 * 农历 / 月历网格 / 出没
 *
 * ⚠️ 库那一层收 JD(UT)，而 a->jd 是 TT。每处都要转，漏一处就会差 ΔT
 * （今天约 69 秒）—— 对「哪一天是初一」在午夜附近是致命的。
 * ------------------------------------------------------------------ */
void ui_app_set_observer(ui_app *a, double lat_deg, double lon_east_deg)
{
    a->observer_lat = lat_deg;
    a->observer_lon = lon_east_deg;
    a->rise_cached = 0;
    a->moon_dirty = 1;
    if (a->time_basis != UI_TIME_CLOCK) a->pillars_ok = 0;
}

/* 民用时刻：按 time_basis 把 TT 瞬时落成用户会读的那套钟。
 *
 * 库的 jd_ut 参数全是 **UT1**，民用读数另算 —— 三个口径的区别只在于
 * 「哪个瞬时算作当地那一刻」：
 *     钟表时     UTC + 时区偏移     （真民用时，闰秒表 + DUT1 那条路）
 *     真太阳时   UT1 + 东经/15 + 均时差
 *     地方平时   UT1 + 东经/15
 * 完整配方见 sf_calendar.h「钟表读数 → 四柱」。 */
static double civil_jd(const ui_app *a, double jd_tt)
{
    if (a->time_basis == UI_TIME_TRUE_SOLAR)
        return sf_tt_to_ut(jd_tt) + a->observer_lon / 360.0
             + sf_equation_of_time_minutes(jd_tt) / 1440.0;
    if (a->time_basis == UI_TIME_LOCAL_MEAN)
        return sf_tt_to_ut(jd_tt) + a->observer_lon / 360.0;
    return sf_tt_to_utc(jd_tt) + a->cal_cfg.utc_offset_minutes / 1440.0;
}

static void local_fields(const ui_app *a, sf_cal_datetime *out)
{
    /* ⚠️ 先整秒归整再拆。jd 在 2.46e6 量级，1 秒 ≈ 1.2e-5 天，末位本来就在
     * 5e-5 秒上下抖；直接对小数逐级截断的话 22:53:00 会显示成 22:52:59
     * （实测踩到过）。ui_calendar_from_jd 也是先 round 再拆的。
     *
     * 秒要留着：时柱在 23:00 换，差一秒就是另一个柱子。 */
    const double shifted = civil_jd(a, a->jd) + 0.5;
    const double day = floor(shifted);

    int32_t y = 0, mo = 0, d = 0;
    sf_solar_date_from_day_number((int64_t)day, &y, &mo, &d);
    long secs = lround((shifted - day) * 86400.0);
    if (secs >= 86400) secs = 86399;   /* 只可能多 1 秒，不值得为它做进位 */

    out->year = y; out->month = mo; out->day = d;
    out->hour   = (int32_t)(secs / 3600);
    out->minute = (int32_t)((secs % 3600) / 60);
    out->second = (double)(secs % 60);
}

/* 四柱 + 虚拟时刻。
 *
 * 主页可见时每分钟最多真算一次，不能绑在农历日缓存上：时柱两小时就会变。
 * 其它页面不消费当前四柱，因此不在后台刷新。
 *
 * 代价用**缓存键**压住：只有虚拟时刻的字段真的变了才去求解。四柱里最贵的
 * 是月柱（要一次 sf_cal_find_term），所以「每分钟一次」和「每帧一次」差着
 * 三个数量级。local_fields 本身很便宜，值得每帧算。
 *
 * force=1 是给「改口径」那条路用的：换历法模式 / 早晚子时 / 钟表口径时
 * 虚拟时刻的**字段可能一个字都没变**（比如改的是历法模式，它只影响月柱的
 * 定界），光靠缓存键判断会漏掉。 */
static void refresh_pillars(ui_app *a, int force)
{
    sf_cal_datetime vt;
    local_fields(a, &vt);

    if (!force && a->pillars_ok
        && vt.year == a->virtual_time.year && vt.month == a->virtual_time.month
        && vt.day  == a->virtual_time.day  && vt.hour  == a->virtual_time.hour
        && vt.minute == a->virtual_time.minute)
        return;

    a->virtual_time = vt;
    a->pillars_ok = (sf_ganzhi_four_pillars(&a->cal_cfg, sf_tt_to_ut(a->jd),
                                            &a->virtual_time, a->rat_hour,
                                            &a->pillars) == 0);
}

static void rebuild_lunar_today(ui_app *a)
{
    const double jd_ut = sf_tt_to_ut(a->jd);
    const int old_ok = a->lunar_ok;
    const int32_t old_year = a->lunar.year;
    const uint8_t old_month = a->lunar.month;
    const uint8_t old_leap = a->lunar.is_leap;

    a->lunar_ok = (sf_cal_from_instant_ut(&a->cal_cfg, jd_ut, &a->lunar) == 0);
    a->lunar_dirty = 0;

    /* 月历网格只在农历月份真的变化时失效；普通跨日不重建整个月。 */
    if (!old_ok || !a->lunar_ok || old_year != a->lunar.year
        || old_month != a->lunar.month || old_leap != a->lunar.is_leap)
        a->grid_dirty = 1;
}

static void rebuild_lunar_grid(ui_app *a)
{
    memset(a->grid_term, 0, sizeof a->grid_term);
    a->grid_days = 0;
    a->grid_wday0 = 0;
    a->grid_first_jdn = 0;
    if (!a->lunar_ok) { a->grid_dirty = 0; return; }

    /* 本月初一的公历日号 */
    sf_lunar_date first = a->lunar;
    first.day = 1;
    sf_solar_date sd;
    if (sf_cal_from_lunar(&a->cal_cfg, &first, &sd) != 0)
        { a->grid_dirty = 0; return; }
    a->grid_first_jdn = sf_solar_day_number(sd.year, sd.month, sd.day);
    a->grid_days = a->lunar.month_days;
    a->grid_wday0 = (int)(((a->grid_first_jdn + 1) % 7 + 7) % 7);   /* 0 = 日 */

    /* 标出本月的节气日。往前多退一天，保证月初当天就落进来的也能被找到。 */
    double cursor = (double)a->grid_first_jdn - 0.5 - 480.0 / 1440.0 - 1.0;
    for (int k = 0; k < 4; k++) {
        sf_cal_term_event ev;
        if (sf_cal_find_term(&a->cal_cfg, cursor, 1, SF_TERM_ANY, &ev) != 0) break;
        const int off = (int)(ev.civil_day_number - a->grid_first_jdn);
        if (off >= a->grid_days) break;
        if (off >= 0 && off + 1 < 31) a->grid_term[off + 1] = 1;
        cursor = ev.jd_ut;
    }
    a->grid_dirty = 0;
}

/* 出没 / 昼长 / 太阳高度 / 均时差。
 *
 * ⚠️ **这四个不是一回事，成本差两个数量级**，从前一律每帧重算，那是这一页
 * 最大的浪费（实测出没一项就占了每帧开销的九成以上）：
 *
 *   每帧都要 ─ 太阳高度  一次求值，最便宜。高度每秒都在变，不缓存是对的。
 *   每帧都要 ─ 均时差    一次求值（约两次视黄经的量）。天内变化是分钟级，
 *                        但值本身就便宜，缓存省不下什么，还得担误差。
 *   按天缓存 ─ 出没+昼长 同一次 `sf_sun_rise_set_day_length` 求根；昼长直接
 *                        取升落两根之差，不再为它把整天重解第二遍。
 *
 * 缓存是按「当地日号 + 观者位置」做的：整个当地日之内 day0 不变，库解出来的
 * 就是同一天同一对穿越点，所以**加上缓存输出完全不变**，只是不再重算。
 * 跨日、改观测地时键变了，自动重解。 */
static void rebuild_rise(ui_app *a)
{
    const double jd_ut = sf_tt_to_ut(a->jd);

    /* 每帧这两个 */
    double az = 0.0;
    a->alt_deg = sf_sun_altitude(jd_ut, a->observer_lat, a->observer_lon, &az)
               * 180.0 / M_PI;
    a->eot_min = sf_equation_of_time_minutes(a->jd);

    /* 「jd_ut 落在哪个当地日」—— 跟库内部 local_day_start 的口径一致
     * （都是 offset 走观测经度，不是走显示时区）。 */
    const int64_t day = sf_cal_civil_day_number(jd_ut, a->observer_lon / 360.0);
    if (a->rise_cached && a->rise_day == day
        && a->rise_lat == a->observer_lat && a->rise_lon == a->observer_lon)
        return;

    a->rise_ok = sf_sun_rise_set_day_length(
        jd_ut, a->observer_lat, a->observer_lon,
        &a->rise_jd, &a->set_jd, &a->day_len_h);
    a->rise_day    = day;
    a->rise_lat    = a->observer_lat;
    a->rise_lon    = a->observer_lon;
    a->rise_cached = 1;
}

void ui_app_prepare_visible(ui_app *a)
{
    if (!a || a->feature != UI_FEATURE_CALENDAR) return;

    int dirty = 0;
    switch (a->page) {
    case UI_PAGE_TODAY:
        dirty = a->today_events_dirty || a->lunar_dirty || a->moon_dirty
             || !a->pillars_ok;
        break;
    case UI_PAGE_LUNAR:  dirty = a->lunar_dirty || a->grid_dirty; break;
    case UI_PAGE_TERMS:  dirty = a->year_term_dirty; break;
    case UI_PAGE_PHASES: dirty = a->year_phase_dirty; break;
    case UI_PAGE_SUN:    dirty = !a->rise_cached; break;
    default: break;
    }
    if (!dirty && a->prepared_jd == a->jd
        && a->prepared_feature == a->feature && a->prepared_page == a->page)
        return;

    switch (a->page) {
    case UI_PAGE_TODAY:
        update_current_term(a);
        a->elong = sf_elongation(a->jd);
        if (a->today_events_dirty) rebuild_today_events(a);
        if (a->lunar_dirty) rebuild_lunar_today(a);
        if (a->moon_dirty) {
            sf_sun_ra_dec(a->jd, &a->ra, &a->dec);
            moon_orientation(a);
            a->moon_dirty = 0;
        }
        refresh_pillars(a, 0);
        break;

    case UI_PAGE_LUNAR:
        if (a->lunar_dirty) rebuild_lunar_today(a);
        if (a->grid_dirty) rebuild_lunar_grid(a);
        break;

    case UI_PAGE_TERMS:
        update_current_term(a);
        if (a->year_term_dirty) {
            rebuild_year_terms(a);
            a->year_term_dirty = 0;
        }
        break;

    case UI_PAGE_PHASES:
        if (a->year_phase_dirty) {
            rebuild_year_phases(a);
            a->year_phase_dirty = 0;
        }
        break;

    case UI_PAGE_SUN:
        a->sunlon = sf_sun_apparent_longitude(a->jd);
        a->sunlat = sf_sun_apparent_latitude(a->jd);
        a->dist = sf_sun_distance(a->jd);
        rebuild_rise(a);       /* 出没本身仍按当地日 + 经纬度命中缓存 */
        break;

    default:
        break;
    }
    a->prepared_jd = a->jd;
    a->prepared_feature = a->feature;
    a->prepared_page = a->page;
}
