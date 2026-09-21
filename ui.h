/* ==================================================================
 * ui.h —— 小屏 UI：屏幕抽象 / 比例坐标 / 绘制原语 / 页面状态
 *
 * 这一层（ui.h + ui_gfx.c + ui_view.c + ui_app.c）是**便携的**：
 *   纯 C99 + libm，不使用 malloc，不依赖平台头文件，绘制到调用方提供的
 *   RGB565 缓冲 —— 即 ST7735S 的像素格式，DMA 输出即可。
 *   移植到 ESP32 时这四个文件原样使用，只替换 host_*.c。
 *
 * 屏幕
 *   ST7735S 1.8"，原生 128x160 竖屏，此处按**横屏** 160x128 使用。
 *
 * 布局用比例，不用像素
 *   所有版式写成 0..1 的 ui_box，落屏那一刻才乘上 UI_W / UI_H。
 *   换 240x240 或 320x240 的屏时版式代码不用改，只需改「字号档位」
 *   那张表（点阵字体的尺寸是像素量）。
 * ================================================================== */
#ifndef SIM_UI_H
#define SIM_UI_H

#include <stdint.h>

/* 这一层直接使用库的农历 / 出没 / 气朔 / 八字类型，不再包一层 —— 包装会使两边定义漂移。 */
#include "sf_bazi.h"
#include "sf_calendar.h"
#include "sf_qishuo.h"
#include "sf_rise.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 屏幕
 * ------------------------------------------------------------------ */
#define UI_W 160
#define UI_H 128

/* 颜色就是 ST7735S 的像素格式（RGB565），不用转 */
#define UI_RGB(r, g, b) \
    ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | (((b) & 0xFFu) >> 3)))

/* ------------------------------------------------------------------
 * 比例坐标
 * ------------------------------------------------------------------ */
typedef struct { float x, y, w, h; } ui_box;   /* 全部 0..1 */
typedef struct { int x, y, w, h; }   ui_rect;  /* 落屏后的像素 */

#define UI_BOX(x, y, w, h) ((ui_box){ (x), (y), (w), (h) })

int     ui_px_x(float f);            /* 0..1 → 0..UI_W（四舍五入） */
int     ui_px_y(float f);
ui_rect ui_box_to_rect(ui_box b);

/* 对齐 */
enum { UI_LEFT = 0, UI_HCENTER, UI_RIGHT };
enum { UI_TOP = 0, UI_VCENTER, UI_BOTTOM };

/* ------------------------------------------------------------------
 * 主题
 *
 * 两套：白天（纸感）/ 夜（暗场，便于观月）。都只在高对比色里挑，
 * 因为 1.8" 的 TN 屏视角一变就发灰。
 * ------------------------------------------------------------------ */
typedef struct {
    uint16_t bg, panel, fg, dim, accent, on_accent, rule;
    uint16_t moon_lit, moon_dark, moon_edge;
    /* 五行色，**按 SF_WUXING_\* 索引**（水 0 木 1 金 2 土 3 火 4）。
     *
     * 注意：两套主题的取法不同，不是同一组色换个底：网页那五个 hex
     * （木 #2E7D32 / 火 #C62828 / 土 #795548 / 金 #A47700 / 水 #1565C0）
     * 是**按白底调的**，置于近黑的夜间主题上会糊成一片。故日间那套照抄
     * 网页（只把土稍微提亮，原色偏浑），夜间那套是按对比度另配的亮版。 */
    uint16_t wuxing[5];
} ui_theme;

extern const ui_theme ui_theme_day, ui_theme_night;

/* ------------------------------------------------------------------
 * 绘制原语
 * ------------------------------------------------------------------ */
/* 只做前置声明，不 typedef —— 生成的 ui_font.h 里会 typedef，C99 下同一
 * 个 typedef 出现两次是违规的（C11 才放开）。这里一律写 struct ui_face。 */
struct ui_face;

typedef struct {
    const struct ui_face *latin;     /* 半角 */
    const struct ui_face *cjk;       /* 全角（汉字 + 全角标点） */
    int line;                        /* 行高，= 较高那个面的 cell_h + 1 */
} ui_font;

extern const ui_font ui_font_xs;     /* latin12 + cjk8  —— 只给藏干那一行用 */
extern const ui_font ui_font_s;      /* latin12 + cjk12 */
extern const ui_font ui_font_m;      /* latin16 + cjk16 */
extern const ui_font ui_font_l;      /* latin16 + cjk24 */

void ui_init(uint16_t *fb);          /* 调用方给缓冲（MCU 上就是 DMA 缓冲） */
const uint16_t *ui_fb(void);

void ui_clear(uint16_t c);
void ui_px(int x, int y, uint16_t c);
void ui_fill(ui_rect r, uint16_t c);
void ui_frame(ui_rect r, uint16_t c);
void ui_hline(int x0, int x1, int y, uint16_t c);
void ui_vline(int x, int y0, int y1, uint16_t c);
void ui_disc(int cx, int cy, int r, uint16_t c);
void ui_ring(int cx, int cy, int r, uint16_t c);

/* 月相：亮边方向由单位向量 (bx, by) 给出（屏幕坐标，y 向下），
 * 亮面比例由 cos_sep = cos(日月**真角距**) 给出：+1 全暗、0 半、−1 全亮。
 * 「上」= 天顶，「右」= 方位角增大的方向 —— 即**观测者抬头所见的方向**。
 * (bx, by) 的算法见 ui_app.c 的 moon_orientation()。 */
void ui_moon_dir(int cx, int cy, int r, double bx, double by, double cos_sep,
                 uint16_t lit, uint16_t dark, uint16_t edge);

/* 旧接口：固定「亮面朝右」，即北半球面朝南看时的样子。
 * 实际使用的是 ui_moon_dir；此接口保留供对照（--dump 的月相序列、单测），
 * 调用方不必自行计算亮边朝向。 */
void ui_moon(int cx, int cy, int r, double elongation_rad,
             uint16_t lit, uint16_t dark, uint16_t edge);

/* ------------------------------------------------------------------
 * 文字
 *
 * 自动按字符选面：<128 走 latin，否则走 cjk。找不到字形时画**空心方框**，
 * 漏字可一眼看见，不会静默变空白。
 * ------------------------------------------------------------------ */
int  ui_text_w(const ui_font *f, const char *utf8);
void ui_text(int x, int y, const ui_font *f, uint16_t c, const char *utf8);   /* y = 顶 */
int  ui_text_c(int cx, int y, const ui_font *f, uint16_t c, const char *utf8); /* 水平居中 */

/* 摆进比例矩形。返回基线矩形，方便接着往下排。 */
ui_rect ui_text_in(ui_box b, int ha, int va, const ui_font *f, uint16_t c,
                   const char *utf8);

/* 通用画布：往**任意** RGB565 缓冲写放大的 ASCII，供宿主窗口的按键面板使用。
 * 面板不是设备 UI，所以允许整数放大；共用同一套点阵，宿主端无需再引入
 * 一份系统字体（Cocoa 和 SDL 两条后端因此外观一致）。 */
void ui_canvas_ascii(uint16_t *dst, int dw, int dh, int x, int y, int scale,
                     const struct ui_face *f, uint16_t c, const char *ascii);
int  ui_canvas_ascii_w(int scale, const struct ui_face *f, const char *ascii);

/* 字面的像素高度。ui_font.line 给的是**较高**那个面，手动像素排版时
 * 需要单独知道半角面的高度（如右对齐一列数值），故单独提供。 */
int  ui_font_latin_h(const ui_font *f);

/* 画不出来的字形（会画成空心方框）的累计次数。
 * 使「字库是否够用」成为可断言的数字而非目视检查 —— `sim --dump` 结束时报出。 */
int  ui_missing_glyphs(void);
void ui_missing_reset(void);

/* ------------------------------------------------------------------
 * 按钮：8 个
 *
 * 「8 按键模块」的物理型号尚未确定（AD 分压单线 / 8 根独立 IO /
 * TTP226 电容触摸，均通称「8 按键模块」），因此这里只定义**逻辑**
 * 按钮，物理映射留给 host 层和真机固件。
 *
 * 主机模拟器的键位见 README.md。
 * ------------------------------------------------------------------ */
typedef enum {
    UI_BTN_UP = 0, UI_BTN_DOWN, UI_BTN_LEFT, UI_BTN_RIGHT,
    UI_BTN_OK, UI_BTN_BACK, UI_BTN_MENU, UI_BTN_ACT,
    UI_BTN_COUNT
} ui_btn;

extern const char *const ui_btn_names[UI_BTN_COUNT];

/* ------------------------------------------------------------------
 * 应用状态
 * ------------------------------------------------------------------ */
/* ------------------------------------------------------------------
 * 两级页面：**功能 → 子页**
 *
 * 开机停在**功能选择页**（feature == -1），选一个功能进去；在功能里按
 * BACK 逐级退回。`page` 是**当前功能内**的子页号，两个功能的子页枚举
 * 各自从 0 起，靠 `feature` 区分。
 * ------------------------------------------------------------------ */
typedef enum {
    UI_FEATURE_CALENDAR = 0,   /* 万年历：照旧那六页 */
    UI_FEATURE_BAZI,           /* 八字：出生信息 → 排盘 */
    UI_FEATURE_COUNT
} ui_feature;

typedef enum {
    UI_PAGE_TODAY = 0,   /* 今：当前节气 + 农历 + 月相 + 倒计时 */
    UI_PAGE_LUNAR,       /* 历：农历月历网格 */
    UI_PAGE_TERMS,       /* 气：选中年份的 24 个节气（默认今年） */
    UI_PAGE_PHASES,      /* 朔：选中年份的 12~13 朔 + 同样多的望 */
    UI_PAGE_SUN,         /* 阳：太阳量 + 日出日落 */
    UI_PAGE_SETTINGS,    /* 设：时区 / 口径 / 早晚子时 / 历法 / 观者 */
    UI_PAGE_COUNT
} ui_page;

enum {
    UI_BAZI_PAGE_BIRTH = 0,    /* 出生信息：七个旋钮 */
    UI_BAZI_PAGE_CHART,        /* 排盘 */
    UI_BAZI_PAGE_LUCK,         /* 运限：大运→流年→流月→流日→流时 */
    UI_BAZI_PAGE_SETTINGS,     /* 八字自己的口径（跟万年历那套独立） */
    UI_BAZI_PAGE_COUNT
};

/* 运限链的层。注意：顺序即链的方向（浅→深），数组下标直接使用。 */
enum {
    UI_LUCK_DAYUN = 0,   /* 大运（含最前面那张「童限」） */
    UI_LUCK_YEAR,        /* 流年（童限下是**流年小运**：小运柱） */
    UI_LUCK_MONTH,       /* 流月（寅月起，12 张） */
    UI_LUCK_DAY,         /* 流日（该**节气月**的整数天，29~32 张，要解两个节） */
    UI_LUCK_HOUR,        /* 流时（子时起，12 张） */
    UI_LUCK_LEVELS
};

/* 运限里的一张卡。**只存数字，不拼字符串** —— 排版由 ui_view 负责。
 * 各层用到的字段不同，见 ui_app.c 里 luck_get 的注。 */
typedef struct {
    sf_ganzhi pillar;       /* 这张卡对应的柱 */
    sf_ganzhi xiao_yun;     /* 流年卡的**底注**那根小运（其余层无意义） */
    int32_t   civil_year;   /* 大运/流年/小运：该步起始公历年；其余层 0 */
    int32_t   age;          /* 虚岁 */
    int32_t   age_end;      /* 童限卡：区间的上端（其余层 == age） */
    int64_t   jdn;          /* 流日：这一天的日号；其余层 0 */
    int       is_childhood; /* 童限那张（第 0 层的 index 0） */
} ui_luck_entry;

/* 八字设置页的行。顺序即屏上的顺序，也是**上游表单的字段顺序**
 * （clockMode → calendarMode → calendarDayBoundary → pillarHistoricalMode
 * → ratHourMode，经度插在日界之后，因为它是日界的参数）。
 * 注意：跟出生信息页一样是「三处同步」的：标签表 / 取值 / 改值分支。 */
enum {
    UI_BSET_TIME_BASIS = 0,   /* 排盘钟表：钟表时间 / 平太阳时 / 真太阳时 */
    UI_BSET_CAL_MODE,         /* 历法 */
    UI_BSET_BOUNDARY,         /* 定气日界 */
    /* 注意：「出生地经度」不在此行 —— 屏上位置按**用户怎么找它**划分，
     * 而非上游表单的字段序：「出生地经度」属于「这个盘生在哪儿」，
     * 与出生信息归为一处。上游字段序（dayBoundary 之后）在 160px 页面上不构成约束。 */
    UI_BSET_PILLAR_HIST,      /* 节气边界 */
    UI_BSET_RAT_HOUR,         /* 早晚子时 */
    UI_BSET_COUNT
};

/* `scroll[]` 按**子页**索引，两个功能的子页共用一张表，所以维度取较大者。
 * 注意：加子页时这里会自动跟着长，但**不得超过** UI_PAGE_COUNT，
 * 否则日历那边会空出一段永远用不到的表项（不致命，只是浪费）。 */
/* 注意：两个枚举都必须显式转 int 再比较：GCC（ESP-IDF 的编译器）将
 * `-Wenum-compare` **视为错误**，两个匿名枚举直接比较大小会编译失败。
 * Mac 上的 clang 只在 -Wextra 下给出警告，故本地不会暴露。 */
#define UI_SUBPAGE_MAX \
    (((int)UI_PAGE_COUNT > (int)UI_BAZI_PAGE_COUNT) \
        ? (int)UI_PAGE_COUNT : (int)UI_BAZI_PAGE_COUNT)

/* 出生信息页的行。顺序即屏上的顺序 —— 跟设置页一样是「三处同步」的
 * （标签表 / 取值 / 改值分支），加一行须同时改三处。 */
enum {
    UI_BIRTH_YEAR = 0,
    UI_BIRTH_MONTH,
    UI_BIRTH_DAY,
    UI_BIRTH_HOUR,
    UI_BIRTH_MINUTE,
    UI_BIRTH_GENDER,
    /* 出生地时区偏移。注意：**不得复用设置页那个显示时区** —— 后者在
     * 「中国古历」下被 apply_utc_lock 钉死在 +480，用它排一张纽约出生的
     * 盘会整整差 13 小时。两者是**不同的量**：显示时区管屏上的钟，
     * 出生地偏移管「那组读数是哪个时区的」。 */
    UI_BIRTH_OFFSET,
    /* 出生地经度。**与出生地时区相邻，同属「这个盘生在哪儿」** ——
     * 它是**独立的一份**（`bazi_cal.calendar_meridian_deg`，与万年历的
     * `cal_cfg` 互不相干），放在这一页（「一个盘一份」）而非「八字设置」页。
     * 注意：它是**两个**消费者的入参：日界（`day_boundary_mode`）和排盘钟表
     * （平/真太阳时的经线）—— 故仅当两者都失效时才变灰（见
     * ui_app_birth_inert）。 */
    UI_BIRTH_MERIDIAN,
    UI_BIRTH_ROW_COUNT
};
#define UI_BIRTH_ROWS 8               /* 一屏可见行数，= ROW_COUNT */
typedef char ui_birth_rows_must_fit[(UI_BIRTH_ROW_COUNT == UI_BIRTH_ROWS) ? 1 : -1];

/* 注意：「四柱」并进主页两行，不单独占页。八字是第二个功能，排盘属其自身
 * 职责；主页那两行表示「此刻的干支记法」。 */

/* 四柱「虚拟时刻」按哪套时计算。叫法照抄 tools 的 `clockMode`
 * （钟表时间 / 平太阳时 / 真太阳时）。
 *
 * 注意：三套只替换 **virtual_time**；喂给库的 instant_jd_ut 永远是真实
 * UT1 瞬时 —— 年柱月柱锚的是节气，换成真太阳时会一起偏掉。
 * tools 在 bazi/ziwei 的表单上也写着同一句话：「这里影响农历结构，不等同于
 * 排盘钟表的太阳时修正」—— 两者是**两条独立的轴**，不得合并。
 * 库那边的完整配方见 sf_calendar.h「钟表读数 → 四柱」。 */
enum {
    UI_TIME_CLOCK = 0,       /* 钟表时间：用户读数那套，走 UTC + 时区偏移 */
    UI_TIME_TRUE_SOLAR = 1,  /* 真太阳时：平太阳时 + 均时差（八字里那派） */
    UI_TIME_LOCAL_MEAN = 2   /* 平太阳时：UT1 + 东经/15，不带均时差 */
};

/* 设置页的行。**顺序即屏上的顺序** —— ui_view.c 的排版和 ui_app.c 的
 * 改值分支都照这个顺序写，加一行须同时改三处（标签表 SET_LABELS、
 * 取值表 setting_value、改值分支 setting_apply）。
 *
 * 标签照抄 tools/public/tools/qishuo/index.html 和 locales/zh-CN.js：
 *   历法归日 / 显示时区 / 当地经度 / 定气定朔日界 / 节气边界 / 观测经度。
 * 160 px 放不下全称的用短名（见 ui_view.c 的表），全称写进 README.md。
 *
 * 注意：最后那行「气朔精度」是**本 sim 独有的**，tools 那边没有对应控件 ——
 * 上游的气朔精度是另一个选项（`accuracy` 的 fast/mid/accurate，三个不同
 * 算法），本 sim 的三档是同一个求解器的三种截断。不得当作 tools 那一项的复刻。
 *
 * 注意：「时区」和「定气日界」「当地经度」不是一回事，虽然都在描述经度：
 *   时区      = 屏上的钟怎么读          （sf_cal_config.utc_offset_minutes）
 *   定气日界  = 「此刻算哪一天」按什么定（sf_cal_config.day_boundary_mode）
 *   当地经度  = 上面选「按当地经度」时的经线（calendar_meridian_deg）
 * 两者可以不一致 —— 设备设成纽约时间、日界仍按东经 120°，这是有意的。
 *
 * 按键分工（K1..K8 = 上 下 左 右 确定 返回 菜单 功能）：
 *
 *     K1/K2  换行（光标到哪，六行窗口跟到哪）
 *     K3/K4  翻页 —— **跟其它页一样**，这一页左右键不重载
 *     K5     调一下（按当前步长和方向）
 *     K6     回主页
 *     K7     换步长（10/5/1/0.1 之类；枚举行只有一档，按了不动）
 *     K8     切换 ＋/－
 *
 * 方向和步长分开，因为**单位不同的行走同一套步长会按错**：时区差 30 分
 * 和经度差 30° 完全不是一回事。故档位表每行一套（见 ui_app.c 的
 * setting_steps），调值也变成「先定方向、再按确定」，比左右各一个键多一下，
 * 换来的是左右键能翻页、以及 0.1° 这种细档够得着（7.5° 一整格到不了 87.6°E）。 */
enum {
    UI_SET_TZ = 0,       /* 显示时区，分钟 —— 钟面 + 钟表时间口径 */
    UI_SET_BASIS,        /* 钟表口径：四柱虚拟时刻用哪套时 */
    UI_SET_RAT_HOUR,     /* 早晚子时 */

    UI_SET_CAL_MODE,     /* sf_cal_config.mode —— 历法归日 */
    UI_SET_BOUNDARY,     /* sf_cal_config.day_boundary_mode —— 定气日界 */
    UI_SET_MERIDIAN,     /* sf_cal_config.calendar_meridian_deg —— 当地经度 */
    UI_SET_PILLAR_HIST,  /* sf_cal_config.pillar_historical_mode —— 节气边界 */
    UI_SET_ACCURACY,     /* sf_qishuo_options.event_accuracy —— 气朔精度档 */

    UI_SET_LAT,          /* 观测纬度 */
    UI_SET_LON,          /* 观测经度 */
    UI_SET_COUNT
};

/* 设置页一屏能放几行。放不下则滚动（光标到哪窗口跟到哪），见 ui_app.c 的
 * settings_keep_visible。行高用列表页那套像素行高，故这个数不得超过
 * (0.88 - UI_LIST_TOP) / UI_LIST_ROW_H —— 6 行正好卡住。 */
#define UI_SET_ROWS 6

/* C99 没有 _Static_assert，用数组长度做同样的检查 */
typedef char ui_set_rows_must_fit[(UI_SET_COUNT >= UI_SET_ROWS) ? 1 : -1];

/* 整年表（气 / 朔两页）的行数，由实测得出：
 *
 *   UI 与 API 都允许 −6000..10000，逐年实测节气为 23–25 条。
 *   容量必须按 25 留，不得把现代范围恒为 24 的观察当作全定义域性质。
 *
 *   月相 **最多 25**（不是 26）：每个相位角一年 12~13 条，两个角合计最多 25
 *   （实测 -6000..10000 全是 24 或 25，未出现 26）。
 *
 * 故格数只需 24 / 25，取 26 是留给「相位角表将来改动」的余量。
 * 实际条数一律读 year_term_n / year_phase_n，**不得把 24 当常数**：
 * 真装不下时 ui_app.c 会让整年页整片空掉，而不是少几行。 */
#define UI_YEAR_TERMS  26
#define UI_YEAR_PHASES 26

/* 整年表的一行。
 *
 * 只保留屏上需要的东西 —— 完整的 sf_qishuo_event 是 160 字节，一年五十条
 * 即 8 KB，而 ui_app 是**栈上的**（`ui_app app;`），真机固件中也只会
 * 这样存放，无需把 8 KB 顶在栈上。投影在 ui_app.c 的 rebuild_year 里完成，
 * 事件本身不进入这一层。 */
typedef struct {
    double  jd_tt;        /* 时刻（TT，跟今页那两个表一个口径） */
    uint8_t term_index;   /* 节气 0..23（i ↔ i·15°，0 = 春分）；月相 = 0xFF */
    uint8_t full;         /* 月相：望 = 1（只用来选颜色）；节气恒 0 */
    /* 事件名，sf_qishuo_event_name() 现拼（JS 的 event.name 在 C 里没法用
     * 静态指针表示）。最长的「冬至」6 字节、「135°月相」11 字节，16 够。 */
    char    name[16];
} ui_year_row;

/* 列表页版式。ui_view.c 用它排版、ui_app.c 用它算滚动上限，故放在公共头里 ——
 * 两处必须一致，若分散在两个文件中迟早会对不上。 */
#define UI_LIST_TOP    0.10f                    /* 顶栏之下 */
#define UI_LIST_ROWS   6                        /* 可见行数 */
#define UI_LIST_ROW_H  0.135f                   /* 比例行高 → 17 px */

typedef struct {
    double jd;                 /* 当前显示的 JD(TT) */

    /* --- 两级页面（见上面 ui_feature 那一段） --- */
    int    feature;            /* ui_feature；**-1 = 停在功能选择页** */
    int    pick;               /* 选择页上高亮的卡片 0..UI_FEATURE_COUNT-1 */
    int    page;
    int    scroll[UI_SUBPAGE_MAX];
    int    night;              /* 主题开关 */
    int    flash;              /* 按下 OK 后闪几帧，做视觉反馈 */

    /* --- 主页派生数据（按页懒加载，渲染只读） --- */
    int    cur_term;           /* 太阳此刻所在的节气，0..23 */
    int    today_events_dirty; /* 当前/下一个节气需要重算 */
    double term_jd[2];         /* 当前节气、下一个节气；主页只消费这两个 */

    /* 失效键：tick 只比较日期并置 dirty，不在后台并发运行星历。
     * 真正的计算由 ui_app_prepare_visible 在对应页面首次显示时完成。 */
    int64_t local_day_key;     /* 历法输入日；INT64_MIN = 尚未建立 */
    int32_t local_year_key;    /* 该日所属公历年；INT32_MIN = 尚未建立 */
    int     lunar_dirty;       /* 今日农历 */
    int     grid_dirty;        /* 农历月历网格 */
    int     moon_dirty;        /* 主页月相朝向（日粒度） */
    double  prepared_jd;       /* 避免 tick 后 render 对同页同瞬时重复准备 */
    int     prepared_feature;
    int     prepared_page;

    /* --- 气/朔两页的「整年表」---
     * 两页各自调用 sf_qishuo_year_run，只请求自己显示的事件，再投影到对应表。
     * **结果即时间序**，名字直接在行里，不需要任何索引推算。
     *
     * 注意：**独立数组，不得复用上面那两个**：今页的进度条读 term_jd[0]、
     * 倒计时读 term_jd[1]、节气名靠 SF_SOLAR_TERM_NAMES[(cur_term+k)%24]
     * 位置推导 —— 整年表一旦塞进去，那三处全部静默出错。
     *
     * 且这组**不随「现在」变**，只在 view_year / 配置改动时分别置 dirty；
     * 进入哪一页才重建哪一张表。 */
    int    view_year;                  /* 选中年份（公历），默认当前年 */
    int    year_term_dirty;            /* 置 1：节气页首次进入时重建 */
    int    year_phase_dirty;           /* 置 1：朔望页首次进入时重建 */
    /* 气朔精度档（sf_qishuo_accuracy，默认 HIGH = 库原来的全量预算）。
     * **只作用于整年表** —— 今页的倒计时/进度条仍走 sf_solve_* 默认档，
     * 不受此项影响。见 sf_qishuo.h 头注「与 JS 的差异 1」。 */
    int    accuracy;
    int    year_term_n;                /* 节气条数（UI 年份范围内为 24） */
    int    year_phase_n;               /* 月相条数（[0,180] → 24 或 25） */
    ui_year_row year_term[UI_YEAR_TERMS];
    ui_year_row year_phase[UI_YEAR_PHASES];

    /* 太阳量 */
    double sunlon, sunlat, dist, ra, dec, elong;

    /* 月相盘朝向。bx/by 是亮边单位向量（屏幕坐标 y 向下），
     * cos_sep = cos(日月真角距)：+1 全暗、0 半、−1 全亮。
     * 注意：这三个**按 rebuild 的节奏限流**，不是每帧计算 —— 详见 ui_app.c。 */
    double moon_bx, moon_by, moon_cos_sep;

    /* --- 农历 ---
     * 库那一层收的是 JD(UT)，所以这里存 cal_cfg 和观者位置，
     * 每帧用 jd_ut 去查。 */
    sf_cal_config cal_cfg;
    double  observer_lat, observer_lon;   /* 度，东经为正 */
    int     lunar_ok;
    sf_lunar_date lunar;
    int     pillars_ok;
    sf_ganzhi_pillars pillars;
    /* 喂给四柱的「虚拟时刻」—— 按 time_basis 算出来的那组民用字段。
     *
     * 注意：它同时是 refresh_pillars() 的**缓存键**：字段没变就不重算四柱。
     * 故不得绕过该函数直接写 pillars —— 那样缓存键不更新，下次会漏算。 */
    sf_cal_datetime virtual_time;

    /* 四柱的口径（设置页可改）。历法模式不另存 —— 它就在 cal_cfg.mode 里。 */
    int     time_basis;        /* UI_TIME_* */
    int     rat_hour;          /* SF_GANZHI_RAT_HOUR_* */
    int     set_row;           /* 设置页光标落在第几行（UI_SET_*） */

    /* 下面这几个都是「**用户选的**和**实际生效的**分开存」：
     *   user_offset_minutes / boundary_choice / meridian_set  —— 用户那份
     *   cal_cfg.utc_offset_minutes / .day_boundary_mode       —— 实际生效的
     * 生效值一律由 apply_utc_lock / apply_boundary 推导，不得绕过它们直接
     * 写 cal_cfg —— 那等于把锁和推导一起绕过。
     * 照抄 tools/public/shared/calendar-boundary.js 的 resolveCalendarBoundary
     * 和 setupHistoricalUtcLock。 */

    /* 显示时区（分钟）。注意：**用户值，不是生效值** ——
     * 「中国古历」或「历史定气」下生效值会被锁成 +480。 */
    int     user_offset_minutes;
    int     boundary_choice;   /* SF_CAL_*，同上 */
    int     meridian_set;      /* 用户动过当地经度没有 —— 没动过就按上面那个播种 */

    /* 设置页的调值方式：步长档 + 方向分开。
     * 注意：档位表**每行一套**（时区按分钟、经纬度按度），step_idx 只是一个
     * 下标，各行自行夹取 —— 故它在行与行之间没有可比性，不得当全局状态存。 */
    int     step_idx;          /* 当前步长档，进该行的档位表，越界就夹住 */
    int     step_plus;         /* 1 = 加，0 = 减（ACT 键切换） */

    /* 月历网格：当前这个农历月 */
    int     grid_days;        /* 29 或 30 */
    int     grid_wday0;       /* 初一是星期几，0 = 日 */
    int64_t grid_first_jdn;   /* 初一那天的民用日号 */
    uint8_t grid_term[31];    /* 该月第 i 天（1..30）是不是节气日 */

    /* 日出日落 */
    int     rise_ok;          /* 0 失败 / SF_RISE_OK / SF_RISE_POLAR_* */
    double  rise_jd, set_jd;  /* UT */
    double  day_len_h, alt_deg, eot_min;

    /* 出没 / 昼长的缓存键 —— 见 ui_app.c 的 rebuild_rise。
     *
     * 那两组数字是**一整天解一次根**的价格（一次出没 ≈ 90 次求节气，是这一页
     * 最贵的运算），且整个当地日之内是常数；而 rebuild_rise 挂在每帧的 tick
     * 上，不缓存即每帧重解一遍。昼长和出没已合并为同一次求解。
     * 键 = 当地日号 + 观者位置，任一项变了就重解。
     *
     * 注意：该缓存**不改变任何输出**：库内部按「jd_ut 所在的那个当地日」
     * 取 day0，再解那一天的穿越点，故一天之内恒定。 */
    int64_t rise_day;         /* 缓存对应的当地日号 */
    double  rise_lat, rise_lon;
    int     rise_cached;      /* 0 = 还没有缓存（ui_app_init 的清零就是它） */

    /* ------------------------------------------------------------------
     * 八字（第二个功能）
     *
     * 注意：**不在这里另存一份 sf_bazi_options**：日历那部分是 cal_cfg 的投影
     * （跟 year_fill 投影整年表同一做法），存两份即有两处真相。
     * 下面这几个是**八字独有的**，cal_cfg 里没有对应物。
     *
     * 注意：这几块合计约 1.1 KB（大运表 768 B 是大头），而 ui_app 是**栈上**的
     * （main.c 与 dump 的 shot() 都是）。加字段前先核算栈。
     * ------------------------------------------------------------------ */
    sf_cal_datetime   birth;        /* 出生钟表读数（用户填），默认 2000-01-01 12:00 */
    int               birth_offset; /* 出生地时区偏移（分钟）—— 见 UI_BIRTH_OFFSET 的注 */
    int               birth_row;    /* 出生信息页光标行 0..UI_BIRTH_ROW_COUNT-1 */
    /* 出生信息页的**步长档位下标**，与设置页那个 step_idx 是两份。
     * 两页的档位表互不相关（年/月/日/时/分 vs 时区/经度/枚举），共用一个
     * 下标会互相干扰 —— 见 ui_app.c 里 step_index() 的注。 */
    int               birth_step_idx;

    /* ------------------------------------------------------------------
     * 八字**自己那一套口径**。跟万年历的 cal_cfg / user_offset_minutes /
     * boundary_choice / meridian_set **三个都独立** —— 上游那三个工具页
     * （八字 / 紫微 / 气朔）本来就是各自一份表单、互不共享。
     *
     * 必须分开的原因：**每张盘的出生地不一样**。共用「当地经度」时，
     * 排完一张乌鲁木齐的盘，万年历的「当地经度」也会变成 87.6° —— 而那一
     * 行是给日出日落用的。
     * ------------------------------------------------------------------ */
    sf_cal_config     bazi_cal;          /* 八字的历法口径（含出生地经度） */
    int               bazi_time_basis;   /* 排盘钟表：UI_TIME_* —— 出生读数是哪套时 */
    int               bazi_rat_hour;     /* 早晚子时（跟主页那个分开） */
    int               bazi_boundary_choice;  /* 定气日界的用户意图（推导前的值） */
    int               bazi_meridian_set;     /* 用户动过出生地经度没有 */
    int               bset_row;          /* 八字设置页光标行 0..UI_BSET_COUNT-1 */
    int               gender;       /* SF_BAZI_GENDER_*，默认 NONE（必须显式选） */
    sf_bazi_chart     chart;        /* 派生：排盘 */
    sf_bazi_qiyun     qiyun;        /* 派生：起运 */
    sf_bazi_dayun_entry dayun[SF_BAZI_DEFAULT_DAYUN_COUNT];
    int               dayun_n;      /* 实际算出来的步数 */
    int               chart_ok;     /* 上面三块算出来了没有 */
    int               birth_valid;  /* 出生读数是不是一个真实存在的时刻 */
    int               chart_dirty;  /* 出生信息 / 口径改了 → 下次 tick 重算 */
    /* ------------------------------------------------------------------
     * 运限链：大运 → 流年 → 流月 → 流日 → 流时
     *
     * `luck_open` 同时即**排盘页的列数**
     * （0 → 四柱 4 列、1 → 5 列、≥2 → 6 列 —— 6 列是 160px 的硬上限），
     * `luck_sel[]` 同时即「选中的第几步大运」。
     * ------------------------------------------------------------------ */
    int               luck_open;      /* 展开到几层：0..UI_LUCK_LEVELS */
    int               luck_sel[UI_LUCK_LEVELS];  /* 每层选中第几张 */
    /* 流日那层要解两个「节」，只在选择变化时算一次。dirty 由 bazi_rebuild
     * 与「改了链上第 1/2 层」两处置位，tick 里重算。 */
    int               luck_dirty;
    int               luck_day_n;     /* 该节气月的天数 */
    int64_t           luck_day_jdn;   /* 第一天（日号） */
} ui_app;

void ui_app_init(ui_app *a, double jd_tt);
/* 时间推进时调用：更新时刻并标记过期数据；只准备当前可见页面。 */
void ui_app_tick(ui_app *a, double jd_tt);
/* 按当前 feature/page 懒加载所需数据。ui_render 会自动调用；单独驱动模型的
 * 宿主也可显式调用。不会加载其它页面的月历、年表或日出日落。 */
void ui_app_prepare_visible(ui_app *a);
/* 按钮事件。pressed=1 按下，0 抬起。 */
void ui_app_button(ui_app *a, ui_btn b, int pressed);

/* 观者位置（度，东经为正）。影响日出日落与农历的显示时区。
 * 真机上会是设置页；模拟器用命令行参数。 */
void ui_app_set_observer(ui_app *a, double lat_deg, double lon_east_deg);

/* 设置页第 row 行当前是否可用。不可用的行绘制时**变暗**：隐藏会使行号
 * 跳来跳去、用户误以为按错，变暗则表明「它仍在，只是当前不适用」。
 * 变暗的行也**不接收按键**。实现在 ui_app.c —— 它需要读 cal_cfg，渲染层
 * 不自行推导。 */
int ui_app_setting_inert(const ui_app *a, int row);

/* 八字那两页的「这行当前是否可用」。与上面那个分开，因为**判据来自另一份
 * config**（bazi_cal）—— 同样是「时区锁定」，两页锁的是各自那套历法。
 * 出生信息页**只有时区那一行**会失效，故不带 row 参数；八字设置页见下。 */
/* ------------------------------------------------------------------
 * 运限链（ui_app.c 里的模型，ui_view.c 只负责画）
 * ------------------------------------------------------------------ */
/* 第 level 层有几张卡。没有盘 / 没有大运时是 0（整条链都不存在）。 */
int ui_app_luck_count(const ui_app *a, int level);
/* 取第 level 层第 i 张。越界返回 0。 */
int ui_app_luck_get(const ui_app *a, int level, int i, ui_luck_entry *out);
/* 活动层 = 最深的那个展开层（没展开时返回 -1）。 */
int ui_app_luck_active(const ui_app *a);

/* 柱盘左边那两根流运柱。注意：只有两格（6 列是硬上限），取链上**最深的两层**，
 * 从深到浅：slot 0 = 最深、slot 1 = 次深。返回 0 表示这一格是空的。
 * `*level` 给出这一格是哪一层（调用方据此选标签），`out` 是那一层选中的卡 ——
 * 注意：童限那张返回的是**小运柱**（照上游柱盘的做法），看 `out->is_childhood`。 */
int ui_app_luck_board(const ui_app *a, int slot, int *level, ui_luck_entry *out);

int ui_app_birth_inert(const ui_app *a, int row);
int ui_app_bset_inert(const ui_app *a, int row);

/* 出生地时区的**生效值**（分钟）。锁上时是 480，与值列显示一致。
 * 注意：bazi_rebuild 算 instant 用的也必须是这个，不是 birth_offset ——
 * 否则屏上写 +8:00、算出却按 −5 排，是一处难以察觉的分歧。
 * 注意：类型必须是 `int32_t`，与实现一致：Mac 上 `int32_t` 即 `int`，
 * 写 `int` 也能编译；**Xtensa 上 int32_t 是 `long`**，两处不一致即
 * 「conflicting types」硬错误。 */
int32_t ui_app_bazi_offset(const ui_app *a);

/* 用户设的显示时区（分钟）。
 * 注意：**不得直接写 `cal_cfg.utc_offset_minutes`** —— 「中国古历」或「历史定气」
 * 下生效值被锁成 +480，直接写等于绕过锁，且解锁时无法恢复。
 * 真机从 NVS 恢复设置时也走这个入口。 */
void ui_app_set_utc_offset(ui_app *a, int minutes);

/* 「按一下确定动多少」，供设置页 / 气朔页的顶栏显示。返回 0 表示当前无
 * 步长（枚举行，或该行当前失效）—— 此时顶栏应显示别的（虚拟时刻）。
 * 单位由调用方按行决定：设置页时区是分钟、经纬度是度，气朔页是年 ——
 * 那是排版的事，不在此拼字符串。符号看 a->step_plus。 */
double ui_app_step_magnitude(const ui_app *a);

/* 气/朔两页显示哪一年。真机从 NVS 恢复设置时走这个入口。 */
void ui_app_set_view_year(ui_app *a, int year);

/* 画一帧。非 const：会把 flash 计数往下走。 */
void ui_render(ui_app *a);

/* ------------------------------------------------------------------
 * 时间工具（纯数学，便携）
 *
 * 这里**不提供**「取当前时间」—— 那属于宿主：sim 用 time()，MCU 上换成
 * RTC，故刻意不放进这一层。见 main.c 的 sim_now_jd_tt()。
 *
 * 也**不提供** ΔT —— 使用库的 sf_utc_to_tt / sf_tt_to_utc（sf_calendar.h）。
 * 本层自带 ΔT 会与库的 IERS 年表差 5.9 秒，屏上的钟会快约 6 秒；
 * 两份 ΔT 迟早会对不上，只保留库那份。
 * ------------------------------------------------------------------ */
/* 事件时刻**四舍五入到分**再交给 ui_calendar_from_jd —— 历书给的节气/朔望
 * 时刻是四舍五入到分的，截断会整体早显示一分钟（2026 年 24 个节气里 13 个
 * 受影响）。走动的钟**不得**用它。 */
double ui_jd_round_to_minute(double jd);

double ui_jd_from_calendar(int y, int mo, int d, int h, int mi, double s);
void   ui_calendar_from_jd(double jd, int *y, int *mo, int *d,
                           int *h, int *mi, int *s);

#ifdef __cplusplus
}
#endif
#endif /* SIM_UI_H */
