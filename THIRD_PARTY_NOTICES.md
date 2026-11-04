# 第三方组件与许可

BoardMark 自身以 **AGPL-3.0** 发布（见 [LICENSE](LICENSE)）。
发布包（`release/*.zip`）里除了 `BoardMark.exe`，还带着下面这些运行时库 —— 它们各有各的许可，
分发时请连同许可证文本一起给出。

## 主要依赖

| 组件 | 版本 | 许可 | 说明 |
|---|---|---|---|
| Qt 6（Core / Gui / Widgets / Network / Svg） | 6.11.2 | **LGPL-3.0**（或 GPL-3.0 商业许可） | 本项目**动态链接**（`Qt6*.dll`） |
| Qt 平台插件 `qwindows.dll` 等 | 6.11.2 | LGPL-3.0 | 位于 `platforms/`、`styles/`、`imageformats/` 等子目录 |
| MinGW-w64 运行时（`libgcc_s_seh-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`） | GCC 16.1 | GPL-3.0 + **GCC Runtime Library Exception** | 运行时例外允许随程序分发 |
| ICU（`libicu*.dll`，约 36 MB） | 78 | Unicode License | Qt 的本地化依赖 |
| zlib / libpng / libjpeg-turbo / libzstd / bzip2 | — | zlib / libpng / IJG+BSD / BSD / BSD-like | 图像与压缩 |
| FreeType / HarfBuzz / Graphite2 | — | FTL+GPLv2 / MIT / MIT+LGPL | 字形渲染 |
| GLib / GIO（`libglib-2.0-0.dll` 等） | 2.x | LGPL-2.1 | 经 GIO 间接依赖 |
| PCRE2 / double-conversion / md4c / Brotli / libiconv / libintl | — | BSD-3 / BSD-3 / MIT / MIT / LGPL / LGPL-2.1 | Qt 的间接依赖 |

TLS 走 Windows 自带的 Schannel（Qt Network 在 Windows 上的默认后端），**不打包 OpenSSL**。

## LGPL 合规说明（Qt）

Qt 以 LGPL-3.0 使用，满足以下条件：

1. **动态链接**：所有 Qt 库都以独立 DLL 形式随包提供，未静态链接进 `BoardMark.exe`；
2. **可替换**：用户可以将 `release/` 目录里的 `Qt6*.dll` 换成自己编译的版本，程序仍能运行；
3. **许可文本**：本文件与 [LICENSE](LICENSE) 随源码仓库提供，`Qt6*.dll` 的完整许可文本可从
   <https://www.qt.io/licensing/open-source-lgpl-obligations> 获取；
4. **源码获取**：Qt 6.11.2 源码见 <https://download.qt.io/official_releases/qt/6.11/>，
   本项目对 Qt 未做任何修改。

Qt 的版权声明：`Copyright (C) The Qt Company Ltd. and contributors.`

## 打包时要注意

`windeployqt` **不会**自动拷贝 Qt 的许可文件。对外发版前，请在 `release/<版本>/` 里补上：

- Qt 的 `LICENSE.LGPL3`（来自 Qt 安装目录的 `LICENSES/`）
- 本文件

这两件事写在这里，是因为它们属于"发版前必须做、但很容易忘"的一类。
