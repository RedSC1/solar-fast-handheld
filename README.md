# solar-fast-handheld

[![License: PolyForm Noncommercial 1.0.0](https://img.shields.io/badge/License-PolyForm_NC_1.0.0-lightgrey.svg)](https://polyformproject.org/licenses/noncommercial/1.0.0)
[![Source Available](https://img.shields.io/badge/Source-Available-blue.svg)]()

`solar-fast-handheld` 是基于 [`solar-fast`](https://github.com/your-username/solar-fast) 核心计算引擎打造的**掌上天文历算仪终端与便携 UI 系统**。

项目面向 1.8" (128×160 ST7735S) 彩色液晶屏设计，包含完整的便携 UI 渲染引擎、按键状态机、汉字字模库以及 macOS Cocoa / SDL2 桌面模拟器，可直接编译并在电脑或嵌入式开发板上运行。

---

## 页面与功能

1. **主页（日月万年历）**：
   * 瞬时公历、农历、月相盘动态朝向与受光率、日出日落时刻。
2. **太阳与黄道坐标页**：
   * 实时视赤经、视赤纬、日地距离、视高度角、均时差、真恒星时。
3. **二十四节气年表页**：
   * 当前公历年全部 24 节气精确交节时刻（精确定气求解）。
4. **合朔望月年表页**：
   * 全年朔（新月）、望（满月）、两弦时刻表。
5. **八字排盘与神煞页**：
   * 瞬时或指定生辰四柱、藏干、十神、纳音、神煞格局。
6. **大运与流运限页**：
   * 精确起运时刻、十步大运、流年、流月、流日推算。

---

## 依赖关系

本终端应用依赖 `solar-fast` 核心计算库。

* **开发推荐**：克隆本仓库到与 `solar-fast` 相同的父级目录下：
  ```bash
  Developer/
  ├── solar-fast/           # 天文/历法核心计算库 (MPL-2.0)
  └── solar-fast-handheld/  # 掌上终端与 UI 系统 (PolyForm NC 1.0.0)
  ```
* 亦可通过 Git Submodule 或 CMake 引入。

---

## 编译与运行桌面模拟器

### macOS (Cocoa 原生零依赖)
```bash
make
./sim
```

### Linux / macOS (SDL2 后端)
```bash
make BACKEND=sdl
./sim
```

### 模拟器按键操作
* `W / A / S / D` 或 `方向键`：页面切换与光标导航
* `J / K`：数值调节 / 切换精度与模式
* `Space / Enter`：确认 / 返回主页

---

## 许可协议 (License)

本项目源码基于 **[PolyForm Noncommercial License 1.0.0](LICENSE)** 授权（Source Available）：

* **允许**：个人 DIY、学习、研究、非商业展示与修改。
* **禁止**：任何未经授权的商业使用、硬件套件捆绑销售或商业化分发。
* **商业授权**：如需在商业产品中使用本 UI 架构或定制固件，请联系作者获取商业授权。
