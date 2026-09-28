# solar-fast-handheld —— 1.8" ST7735S 掌上终端与模拟器
#
#   make                  使用 Cocoa 后端（macOS 自带，零外部依赖）
#   make BACKEND=sdl      使用 SDL2 后端（需要 brew install sdl2）
#   make font             重新生成 ui_font.h（更改 UI 汉字后执行）
#   make font-preview     生成字库预览图到 /tmp/ui_font_preview.png
#
# 依赖说明：
#   依赖上一级目录中的 solar-fast 核心库（../solar-fast）。
#   若放置在不同路径，可通过 SF_DIR 指定，如：make SF_DIR=/path/to/solar-fast

SF_DIR ?= ../solar-fast

CC     ?= cc
OPT    ?= -O2
WARN   := -Wall -Wextra
CFLAGS := -std=c99 $(OPT) $(WARN) -D_GNU_SOURCE -I. -I$(SF_DIR) \
          -I$(SF_DIR)/include -I$(SF_DIR)/core -I$(SF_DIR)/rise \
          -I$(SF_DIR)/qishuo -I$(SF_DIR)/calendar -I$(SF_DIR)/bazi -I$(SF_DIR)/yijing
LDLIBS := -lm

BACKEND ?= cocoa

# 便携 UI 层
UI_SRC := ui_gfx.c ui_app.c ui_view.c sim_frame.c main.c

# solar-fast 核心源文件
LIBSRC := $(SF_DIR)/core/sf_fixed.c $(SF_DIR)/core/sf_series.c $(SF_DIR)/core/sf_solve.c \
          $(SF_DIR)/calendar/sf_calendar.c $(SF_DIR)/calendar/sf_ganzhi.c \
          $(SF_DIR)/rise/sf_rise.c $(SF_DIR)/qishuo/sf_qishuo.c \
          $(SF_DIR)/bazi/sf_bazi_rules.c $(SF_DIR)/bazi/sf_bazi.c \
          $(SF_DIR)/yijing/sf_yijing.c
LIBOBJ := $(notdir $(LIBSRC:.c=.o))

ifeq ($(BACKEND),cocoa)
  HOST_SRC := host_cocoa.m
  LDLIBS   += -framework Cocoa -framework CoreGraphics
else ifeq ($(BACKEND),sdl)
  ifeq ($(wildcard host_sdl.c),)
    $(error host_sdl.c 尚未提供 —— 当前默认支持 cocoa 后端)
  endif
  ifeq ($(shell command -v sdl2-config),)
    $(error 找不到 sdl2-config —— 请先执行 brew install sdl2)
  endif
  HOST_SRC := host_sdl.c
  CFLAGS   += -DSIM_HAVE_SDL $(shell sdl2-config --cflags)
  LDLIBS   += $(shell sdl2-config --libs)
else
  $(error BACKEND 只能是 cocoa 或 sdl)
endif

BIN := sim

all: $(BIN)

$(BIN): $(UI_SRC) $(HOST_SRC) $(LIBOBJ) ui.h host.h ui_font.h
	$(CC) $(CFLAGS) -o $@ $(UI_SRC) $(HOST_SRC) $(LIBOBJ) $(LDLIBS)

# 编译各个模块的目标文件
%.o: $(SF_DIR)/core/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(SF_DIR)/calendar/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(SF_DIR)/rise/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(SF_DIR)/qishuo/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(SF_DIR)/bazi/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(SF_DIR)/yijing/%.c
	$(CC) $(CFLAGS) -c $< -o $@

font:
	SF_DIR="$(SF_DIR)" python3 gen_font.py
	@rm -f $(BIN)

font-preview:
	SF_DIR="$(SF_DIR)" python3 gen_font.py --preview /tmp/ui_font_preview.png
	@echo "预览图已生成至 /tmp/ui_font_preview.png"

clean:
	rm -f $(BIN) $(LIBOBJ)

.PHONY: all font font-preview clean
