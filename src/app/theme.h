#pragma once

// Material 3（亮色）外观。
//
// 能做什么、不能做什么
// --------------------
// 这是 **Qt Widgets + QSS** 实现的"M3 风格"，不是真正的 Material 3。
// 能做到：配色（全套 M3 color tokens）、圆角、字体层级、
//         Filled / Outlined / Text 三种按钮、卡片、开关、菜单、滚动条。
// 做不到：水波纹、真正的 elevation 阴影、state layer 叠加、动效、自适应布局。
//
// 为什么不走 Qt Quick
// -------------------
// 本机 Qt 里确实有 Qt6QuickControls2Material，但它是 **Material 2**；
// 而且引入 Qt Quick 会让发布包从 82 MB 涨到 110+ MB
// （Qml/Quick/QuickTemplates2 一大串 DLL + QML 模块目录），
// 还要把整个设置对话框用 QML 重写。不值得。
//
// 为什么先把样式切成 Fusion
// -------------------------
// Qt 在 Windows 上默认用 windows11 / windowsvista 原生样式。原生样式里有一部分
// 是系统绘制的，QSS 盖不干净；而且 Win10 和 Win11 的原生样式不同，
// 会让"同一套 Material 主题"在两台机器上长得不一样。
// Fusion 是 Qt 自绘的，配色和形状完全可控，做主题更稳。
//
// 用法：QApplication 构造之后立刻调一次 applyMaterialTheme()。

namespace cm {

/// 套用 Material 3 亮色主题（全局：调色板 + QSS + 字体）。
void applyMaterialTheme();

/// 主题用到的色值，给非 QSS 的地方（例如自绘的托盘图标）取色。
namespace m3 {
// ---- 亮色 baseline 配色 ----
extern const char *kPrimary;            // #6750A4
extern const char *kOnPrimary;          // #FFFFFF
extern const char *kPrimaryContainer;   // #EADDFF
extern const char *kOnPrimaryContainer; // #21005D
extern const char *kSurface;            // #FFFBFE
extern const char *kOnSurface;          // #1C1B1F
extern const char *kSurfaceContainer;   // #F3EDF7
extern const char *kOnSurfaceVariant;   // #49454F
extern const char *kOutline;            // #79747E
extern const char *kOutlineVariant;     // #CAC4D0

/// 主色在底色上叠 state layer（8% / 12%）之后的近似实色。
/// QSS 里用 rgba() 各家实现有差异，直接算好更保险。
extern const char *kPrimaryState8;      // #F2EDF6
extern const char *kPrimaryState12;     // #ECE6F3
} // namespace m3

} // namespace cm
