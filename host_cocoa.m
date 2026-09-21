/* ==================================================================
 * host_cocoa.m —— macOS 窗口后端（零依赖）
 *
 * 只用系统自带的 AppKit / CoreGraphics，不需要 brew 装 SDL2。键盘事件在
 * SimView 里收，转成 8 个逻辑按钮 + 几个宿主专用键，塞进队列让 main.c 取。
 *
 * 自建 runloop：main.c 控制节奏（30 fps），所以这里不用 [NSApp run]，
 * 而是每次 poll 用 nextEventMatchingMask + distantPast 抽干事件队列。
 * ================================================================== */
#import <Cocoa/Cocoa.h>

#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "ui.h"

/* ------------------------------------------------------------------
 * 全局
 * ------------------------------------------------------------------ */
static NSWindow       *g_win;
static NSView         *g_view;
static CGContextRef g_bmp;            /* 位图上下文，直接引用 g_rgba，不复制 */

static uint16_t *g_winbuf;            /* 合成缓冲，RGB565 */
static unsigned char *g_rgba;         /* 给 CGImage，R G B X */
static int       g_scale = 3;

static sim_ev    g_q[128];
static int       g_qh, g_qt;
static uint32_t  g_down;              /* 当前按住的按钮位掩码 */

static void push(sim_ev e)
{
    if (g_qh - g_qt >= 128) return;
    g_q[g_qh++ % 128] = e;
}

/* ------------------------------------------------------------------
 * 键盘
 *
 * 数字键 1..8 永远等于模块丝印 K1..K8；另外给一套更顺手的别名。
 * 方向键走 keyCode（charactersIgnoringModifiers 对功能键给的是私有区
 * 码点，靠不住），可打印字符走 charactersIgnoringModifiers（不受键盘
 * 布局影响）。
 * ------------------------------------------------------------------ */
static int key_to_btn(unsigned short kc, NSString *s)
{
    switch (kc) {
    case 126: return UI_BTN_UP;
    case 125: return UI_BTN_DOWN;
    case 123: return UI_BTN_LEFT;
    case 124: return UI_BTN_RIGHT;
    case 36: case 76: return UI_BTN_OK;      /* Return / 小键盘 Enter */
    case 53: return UI_BTN_BACK;             /* Esc */
    case 51: return UI_BTN_BACK;             /* Delete */
    case 48: return UI_BTN_MENU;             /* Tab */
    case 49: return UI_BTN_OK;               /* 空格 */
    default: break;
    }
    if ([s length] == 0) return -1;
    switch ([s characterAtIndex:0]) {
    case '1': return UI_BTN_UP;
    case '2': return UI_BTN_DOWN;
    case '3': return UI_BTN_LEFT;
    case '4': return UI_BTN_RIGHT;
    case '5': return UI_BTN_OK;
    case '6': return UI_BTN_BACK;
    case '7': return UI_BTN_MENU;
    case '8': return UI_BTN_ACT;
    case 'w': case 'W': return UI_BTN_UP;
    case 's': case 'S': return UI_BTN_DOWN;
    case 'a': case 'A': return UI_BTN_LEFT;
    case 'd': case 'D': return UI_BTN_RIGHT;
    case 'z': case 'Z': return UI_BTN_ACT;
    case 'm': case 'M': return UI_BTN_MENU;
    default: return -1;
    }
}

static void handle_key(NSEvent *e, int down)
{
    const int b = key_to_btn([e keyCode], [e charactersIgnoringModifiers]);

    if (b >= 0) {
        const uint32_t bit = 1u << b;
        /* 去抖：按住不放时系统会一直发 keyDown，不滤掉的话翻页会疯跑 */
        if (down) { if (g_down & bit) return; g_down |= bit; }
        else      { if (!(g_down & bit)) return; g_down &= ~bit; }
        sim_ev ev = { SIM_EV_BUTTON, b, down };
        push(ev);
        return;
    }

    if (!down) return;
    NSString *s = [e charactersIgnoringModifiers];
    if ([s length] == 0) return;

    /* 宿主专用键（不属于那 8 个按钮）。这里**故意**允许自动重复，
     * 因为按住 [ 一直往回倒时间正是想要的。 */
    sim_ev ev = { SIM_EV_NONE, 0, 0 };
    switch ([s characterAtIndex:0]) {
    case '[':  ev.kind = SIM_EV_TIME;  ev.val = -1; push(ev); break;
    case ']':  ev.kind = SIM_EV_TIME;  ev.val = +1; push(ev); break;
    case '\\': ev.kind = SIM_EV_RESET; push(ev); break;
    case '-':  ev.kind = SIM_EV_ZOOM;  ev.val = -1; push(ev); break;
    case '=': case '+': ev.kind = SIM_EV_ZOOM; ev.val = +1; push(ev); break;
    default: break;
    }
}

/* ------------------------------------------------------------------
 * 视图 / 窗口
 * ------------------------------------------------------------------ */
@interface SimView : NSView
@end

@implementation SimView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque              { return YES; }

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    if (!g_bmp) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGImageRef img = CGBitmapContextCreateImage(g_bmp);
    if (!img) return;
    CGContextDrawImage(ctx, CGRectMake(0, 0, sim_frame_width(g_scale),
                                             sim_frame_height(g_scale)), img);
    CGImageRelease(img);
}

- (void)keyDown:(NSEvent *)e { handle_key(e, 1); }
- (void)keyUp:(NSEvent *)e   { handle_key(e, 0); }

/* 别让系统发出警告响 —— 我们不处理的键安静吞掉 */
- (void)flagsChanged:(NSEvent *)e { (void)e; }
@end

@interface SimDelegate : NSObject <NSWindowDelegate>
@end

@implementation SimDelegate
- (void)windowWillClose:(NSNotification *)n
{
    (void)n;
    sim_ev ev = { SIM_EV_QUIT, 0, 0 };
    push(ev);
}
@end

/* ------------------------------------------------------------------
 * 缓冲
 * ------------------------------------------------------------------ */
static void pack_rgba(void)
{
    const int n = sim_frame_width(g_scale) * sim_frame_height(g_scale);
    for (int i = 0; i < n; i++) {
        const uint16_t c = g_winbuf[i];
        const unsigned r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
        g_rgba[i * 4 + 0] = (unsigned char)((r << 3) | (r >> 2));
        g_rgba[i * 4 + 1] = (unsigned char)((g << 2) | (g >> 4));
        g_rgba[i * 4 + 2] = (unsigned char)((b << 3) | (b >> 2));
        g_rgba[i * 4 + 3] = 0;
    }
}

static void alloc_buffers(int scale)
{
    const int w = sim_frame_width(scale), h = sim_frame_height(scale);

    free(g_winbuf); free(g_rgba);
    g_winbuf = calloc((size_t)w * h, sizeof(uint16_t));
    g_rgba   = calloc((size_t)w * h, 4);
    if (g_bmp) CGContextRelease(g_bmp);

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    g_bmp = CGBitmapContextCreate(g_rgba, w, h, 8, (size_t)w * 4, cs,
                                  (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    CGColorSpaceRelease(cs);
}

static void set_scale(int s)
{
    if (s < 1) s = 1;
    if (s > 8) s = 8;
    if (s == g_scale) return;
    g_scale = s;
    alloc_buffers(s);

    NSRect f = g_win.frame;
    NSRect c = [g_win contentRectForFrameRect:f];
    const CGFloat dx = sim_frame_width(s)  - c.size.width;
    const CGFloat dy = sim_frame_height(s) - c.size.height;
    f.origin.y -= dy;                      /* 锚住左上角，别让窗口乱跳 */
    f.size.width += dx;
    f.size.height += dy;
    [g_win setFrame:f display:YES];
    [g_view setFrame:NSMakeRect(0, 0, sim_frame_width(s), sim_frame_height(s))];
}

/* ------------------------------------------------------------------
 * 后端接口
 * ------------------------------------------------------------------ */
static int cocoa_init(const char *title, int scale)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSMenu *mb = [[NSMenu alloc] init];
        NSMenuItem *appItem = [[NSMenuItem alloc] init];
        [mb addItem:appItem];
        NSMenu *appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Quit"
                          action:@selector(terminate:)
                   keyEquivalent:@"q"];
        [appItem setSubmenu:appMenu];
        [NSApp setMainMenu:mb];

        g_scale = scale;
        alloc_buffers(scale);

        NSRect frame = NSMakeRect(0, 0, sim_frame_width(scale),
                                        sim_frame_height(scale));
        g_win = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled |
                                 NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [g_win setTitle:[NSString stringWithUTF8String:title]];
        [g_win setDelegate:[[SimDelegate alloc] init]];
        g_view = [[SimView alloc] initWithFrame:frame];
        [g_win setContentView:g_view];
        [g_win makeFirstResponder:g_view];
        [g_win center];
        [g_win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        /* 真的起来了才算成功。没有这个检查，在无头环境（比如 ssh 进去跑）
         * 里窗口建不出来也不报错，--selftest 会假阳性。 */
        if (!g_win || ![g_win isVisible]) return 1;
    }
    return 0;
}

static void cocoa_shutdown(void)
{
    @autoreleasepool {
        if (g_bmp) { CGContextRelease(g_bmp); g_bmp = NULL; }
        free(g_winbuf); g_winbuf = NULL;
        free(g_rgba);   g_rgba = NULL;
    }
}

static void cocoa_present(const uint16_t *lcd, uint32_t pressed)
{
    if (!g_winbuf) return;
    sim_frame_compose(g_winbuf, g_scale, lcd, pressed);
    pack_rgba();
    @autoreleasepool {
        [g_view setNeedsDisplay:YES];
        [g_view displayIfNeeded];   /* 自建 runloop，不主动刷就不会画 */
    }
}

static int cocoa_poll(sim_ev *ev)
{
    if (g_qh != g_qt) { *ev = g_q[g_qt++ % 128]; return 1; }

    @autoreleasepool {
        NSEvent *e;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES]) != nil) {
            [NSApp sendEvent:e];
            if (g_qh != g_qt) { *ev = g_q[g_qt++ % 128]; return 1; }
        }
    }
    return 0;
}

static void cocoa_zoom(int delta) { set_scale(g_scale + delta); }
static int  cocoa_scale(void)     { return g_scale; }

const sim_host sim_host_cocoa = {
    "cocoa", cocoa_init, cocoa_shutdown, cocoa_present, cocoa_poll,
    cocoa_zoom, cocoa_scale
};
