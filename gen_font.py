#!/usr/bin/env python3
"""生成 sim/ui_font.h —— 小尺寸 1bpp 点阵字库。

为什么要生成
    1.8" 的屏只有 160x128，汉字得用点阵。手写点阵不现实（光节气名就有 27
    个不同的字）。这里用系统矢量字体在 4 倍分辨率下渲染、降采样、阈值化，
    得到接近 BDF 的 1bpp 位图。

字符集是**扫源码扫出来的**，不是手写的
    只扫**字符串字面量**（正则只匹配单行 "..."，不碰注释 —— 否则三行中文
    注释就能把字库撑爆），并且把 sf_solve.c 里的 SF_SOLAR_TERM_NAMES 一起
    扫进来。所以字库永远不会和 UI 脱节：改了 UI 里的字，重跑一次就有。

用法
    python3 sim/gen_font.py                 # 写 sim/ui_font.h
    python3 sim/gen_font.py --preview /tmp/p.png   # 顺便出一张字表预览图

缺字会怎样
    运行时画不出字形会画一个**空心方框**，所以漏字一眼就能看见，不会静默
    变成空白。
"""

import argparse
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- 字体来源
LATIN_CANDIDATES = [
    ("/System/Library/Fonts/Menlo.ttc", 0),
    ("/System/Library/Fonts/SFNSMono.ttf", 0),
    ("/System/Library/Fonts/Monaco.ttf", 0),
    ("/System/Library/Fonts/Supplemental/Andale Mono.ttf", 0),
]
# W6 是粗体。12px 下细体阈值化后会断笔画，粗体才立得住。
CJK_CANDIDATES = [
    ("/System/Library/Fonts/Hiragino Sans GB.ttc", 2),
    ("/System/Library/Fonts/STHeiti Medium.ttc", 1),
    ("/System/Library/Fonts/Supplemental/Songti.ttc", 1),
]

SS = 4   # 超采样倍数

# 阈值化比例。**逐面不同**，因为最优点取决于字号：小字号笔画细，阈值太低
# 会把笔画糊成一块（12px 的「惊蛰」「霜露」在 0.38 下完全认不出），太高又
# 会把横画打断。下面这几组是拿 /tmp 的对比图挑出来的。
# 见 README 的「点阵字库」一节，`make -C sim font-preview` 可以复现对比。

# 窄符号：塞进 latin 面（全角位会撑出难看的空隙）
NARROW_EXTRA = "°′″±·×÷"

# 这些都是"宽"字符，走 cjk 面
def is_wide(cp):
    return (0x2E80 <= cp <= 0x9FFF or      # 部首扩展 + CJK
            0xF900 <= cp <= 0xFAFF or      # 兼容汉字
            0xFF00 <= cp <= 0xFF60 or      # 全角
            0x3000 <= cp <= 0x303F)        # CJK 标点


def pick(cands):
    for path, idx in cands:
        if not os.path.exists(path):
            continue
        try:
            f = ImageFont.truetype(path, 24, index=idx)
            return path, idx, f.getname()
        except Exception:
            continue
    sys.exit("找不到可用的系统字体，候选全试过了：%r" % (cands,))


# ---------------------------------------------------------------- 收集字符
def scan_literals(src):
    """走一遍 C 词法，只吐出**字符串字面量**的内容。

    不能直接用正则找引号对：注释里也常有 ASCII 引号（本仓库的注释就写了
    「/* ... 传 "最后一步" ... */」），正则会把这些注释当字面量收进来，
    于是字库被注释里的字撑爆、真正的 UI 字符反而漏掉。
    """
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j + 1
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c in "\"'":
            quote, i = c, i + 1
            buf = []
            while i < n and src[i] != quote:
                if src[i] == "\\":
                    i += 1
                    if i < n:
                        buf.append(src[i])
                        i += 1
                    continue
                buf.append(src[i])
                i += 1
            i += 1
            if quote == '"':
                out.append("".join(buf))
        else:
            i += 1
    return out


def collect(paths):
    """只从字符串字面量里收非 ASCII 字符。"""
    wide, narrow = set(), set()
    for p in paths:
        if not os.path.exists(p):
            print("  (跳过不存在的 %s)" % p)
            continue
        for lit in scan_literals(open(p, encoding="utf-8").read()):
            for ch in lit:
                cp = ord(ch)
                if cp < 128:
                    continue
                (wide if is_wide(cp) else narrow).add(ch)
    for ch in NARROW_EXTRA:
        narrow.add(ch)
    return wide, narrow


# ---------------------------------------------------------------- 渲染
def build_face(chars, ttf, index, cell_w, cell_h, font_px, center_h, thresh):
    """把一个字符集渲染成 1bpp 位图集合。

    垂直方向按 em 盒居中（不是按墨迹居中）—— 否则「一」和「露」的基线会
    上下乱跳。水平方向 latin 居中、cjk 顶格。
    """
    f = ImageFont.truetype(ttf, int(font_px * SS), index=index)
    ascent, descent = f.getmetrics()
    dy = (cell_h * SS - (ascent + descent)) // 2
    baseline = dy + ascent

    stride = (cell_w + 7) // 8
    blob = bytearray()
    tab = []
    for ch in sorted(chars, key=ord):
        img = Image.new("L", (cell_w * SS, cell_h * SS), 0)
        d = ImageDraw.Draw(img)
        dx = int((cell_w * SS - f.getlength(ch)) / 2) if center_h else 0
        d.text((dx, baseline), ch, font=f, fill=255, anchor="ls")
        px = img.resize((cell_w, cell_h), Image.LANCZOS).load()
        off = len(blob)
        for y in range(cell_h):
            row = bytearray(stride)
            for x in range(cell_w):
                if px[x, y] / 255.0 >= thresh:
                    row[x >> 3] |= 0x80 >> (x & 7)
            blob += row
        tab.append((ord(ch), cell_w, off))
    return tab, bytes(blob), stride


# ---------------------------------------------------------------- 预览图
def preview(faces, path, scale=4):
    """把每个面摊成一张图，好肉眼检查阈值化有没有把笔画打断。"""
    tiles = []
    for name, tab, blob, cw, ch, stride in faces:
        cols = 16
        rows = (len(tab) + cols - 1) // cols
        img = Image.new("L", (cols * (cw + 2), rows * (ch + 2)), 40)
        d = ImageDraw.Draw(img)
        for i, (cp, adv, off) in enumerate(tab):
            gx = (i % cols) * (cw + 2) + 1
            gy = (i // cols) * (ch + 2) + 1
            for y in range(ch):
                for x in range(cw):
                    if blob[off + y * stride + (x >> 3)] & (0x80 >> (x & 7)):
                        d.point((gx + x, gy + y), fill=255)
        tiles.append((name, img))
    W = max(t.width for _, t in tiles)
    H = sum(t.height + 14 for _, t in tiles)
    out = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(out)
    y = 0
    for name, t in tiles:
        d.text((0, y), name, fill=255)
        y += 14
        out.paste(t, (0, y))
        y += t.height
    out = out.resize((out.width * scale // 2, out.height * scale // 2), Image.NEAREST)
    out.save(path)
    print("  预览图 → %s" % path)


# ---------------------------------------------------------------- 输出 C
def emit_bytes(f, name, blob, per=16):
    f.write("static const uint8_t %s[] = {\n" % name)
    for i in range(0, len(blob), per):
        f.write("    " + " ".join("0x%02x," % b for b in blob[i:i + per]) + "\n")
    f.write("};\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", default=None, help="顺便输出字表预览 PNG")
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "ui_font.h"))
    a = ap.parse_args()

    latin_path, latin_idx, latin_name = pick(LATIN_CANDIDATES)
    cjk_path, cjk_idx, cjk_name = pick(CJK_CANDIDATES)
    print("latin = %s (index %d, %s)" % (latin_path, latin_idx, latin_name))
    print("cjk   = %s (index %d, %s)" % (cjk_path, cjk_idx, cjk_name))

    # 只扫**往屏上画**的文件。main.c 不扫 —— 它的中文是控制台提示，
    # LCD 上一个字都用不到，扫进来白白占 flash。
    srcs = [
        os.path.join(HERE, "ui_view.c"),
        os.path.join(HERE, "ui_app.c"),
        os.path.join(HERE, "ui_gfx.c"),           # ui_btn_names 在这（目前是 ASCII）
        os.path.join(HERE, "..", "sf_ganzhi.c"),  # 天干地支、纳音名等汉字
    # ⚠️ 八字那两张名字表（十神 / 十二长生 / 五行）在 sf_bazi_rules.c 里 ——
    # **新功能的字放到新建的 .c 里，就必须把它加到这里**，否则屏上全是空心框，
    # 而 `--dump` 只会报一个漏字数，第一反应总是怀疑字库本身。
    os.path.join(HERE, "..", "sf_bazi_rules.c"),
        os.path.join(HERE, "..", "sf_series.c"),  # SF_SOLAR_TERM_NAMES 在这
    ]
    wide, narrow = collect(srcs)

    latin_chars = set(chr(c) for c in range(0x20, 0x7F)) | narrow
    # 窄符号里如果有落在 wide 区间的（比如全角逗号），让 wide 面去管
    latin_chars = set(c for c in latin_chars if not is_wide(ord(c)))
    wide = set(c for c in wide if ord(c) > 127)

    print("latin 字符数 %d，cjk 字符数 %d" % (len(latin_chars), len(wide)))
    if wide:
        print("  cjk: %s" % "".join(sorted(wide, key=ord)))

    # (名字, 字符集, 字体, cell_w, cell_h, 渲染字号, 水平居中?, 阈值)
    specs = [
        ("latin12", latin_chars, latin_path, latin_idx,  7, 12, 11, True,  0.40),
        ("latin16", latin_chars, latin_path, latin_idx,  9, 16, 14, True,  0.40),
        # cjk8：**只给藏干那一行用**（八字盘 6 列时每列 26 px，三个 12px 的
        # 藏干要 36 px 放不下）。8x8 的汉字能不能认得出只有渲出来才知道 ——
        # 认不出的退路是「只显示主气一个字 @12px」，见 sim/README。
        ("cjk8",    wide,        cjk_path,   cjk_idx,    8,  8,  8, False, 0.45),
        ("cjk12",   wide,        cjk_path,   cjk_idx,   12, 12, 12, False, 0.50),
        ("cjk16",   wide,        cjk_path,   cjk_idx,   16, 16, 16, False, 0.52),
        ("cjk24",   wide,        cjk_path,   cjk_idx,   24, 24, 24, False, 0.42),
    ]

    faces = []
    for name, chars, path, idx, cw, ch, fpx, ctr, th in specs:
        tab, blob, stride = build_face(chars, path, idx, cw, ch, fpx, ctr, th)
        faces.append((name, tab, blob, cw, ch, stride))
        print("  %-8s cell %2dx%-2d  t=%.2f  %3d 字形  %5d 字节位图"
              % (name, cw, ch, th, len(tab), len(blob)))

    with open(a.out, "w", encoding="utf-8") as f:
        f.write("/* 自动生成，勿手改 —— 由 sim/gen_font.py 产出。\n"
                " * 字体：latin = %s / cjk = %s\n"
                " * 重新生成：make -C sim font\n"
                " */\n" % (latin_name, cjk_name))
        f.write("#ifndef UI_FONT_H\n#define UI_FONT_H\n\n#include <stdint.h>\n\n")
        f.write("typedef struct { uint16_t cp; uint8_t adv; uint16_t off; } ui_glyph;\n")
        # 带 struct 标签，这样 ui.h 里只要一个前置声明就能引用它，
        # 不必把这张几十 KB 的表 include 进每个编译单元。
        f.write("""typedef struct ui_face {
    const ui_glyph *tab;
    int             n;
    const uint8_t  *bits;
    int cell_w, cell_h, stride, line;
} ui_face;

""")
        for name, tab, blob, cw, ch, stride in faces:
            f.write("/* ---- %s：%dx%d 单元，%d 个字形 ---- */\n" % (name, cw, ch, len(tab)))
            emit_bytes(f, "ui_bits_" + name, blob)
            f.write("static const ui_glyph ui_tab_%s[] = {\n" % name)
            for cp, adv, off in tab:
                f.write("    {0x%04x,%2d,%4d},\n" % (cp, adv, off))
            f.write("};\n")
            f.write("static const ui_face ui_face_%s = { ui_tab_%s, %d, ui_bits_%s, %d, %d, %d, %d };\n\n"
                    % (name, name, len(tab), name, cw, ch, stride, ch + 1))
        f.write("#endif /* UI_FONT_H */\n")

    print("→ %s" % a.out)
    if a.preview:
        preview(faces, a.preview)


if __name__ == "__main__":
    main()
