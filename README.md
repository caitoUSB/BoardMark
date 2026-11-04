# BoardMark

Windows 平台的白板板书自动记录工具。程序常驻通知区域，通过屏幕采集与关键帧判定，在板书发生实质变化时保存整屏画面；课后可按「日期 / 学科」归档。
这个软件全是AI写的！！！

[![License](https://img.shields.io/badge/license-AGPL--3.0-blue.svg?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2B%20x64-lightgrey.svg?style=flat-square)](#运行环境)
[![Release](https://img.shields.io/github/v/release/caitoUSB/BoardMark.svg?style=flat-square)](https://github.com/caitoUSB/BoardMark/releases/latest)

## 相关链接

- [下载最新版本](https://github.com/caitoUSB/BoardMark/releases/latest)
- [更新日志](CHANGELOG.md)
- [问题反馈](https://github.com/caitoUSB/BoardMark/issues)
- [技术说明](docs/技术说明.md)（部署、参数、故障排查与设计取舍）

## 主要特性

- 自动识别白板窗口：内置希沃白板 5、希沃轻白板、鸿合的支持，其他白板可手动指定进程名。
- 事件驱动采样：画面静止时 1 秒一次，检测到变化后切换至 120 毫秒。
- 关键帧判定：区分同屏书写、翻页、整屏替换与长距离平移，保存「变化发生前」的完整画面。
- 白板关闭、最小化或被切走前，先落盘当前画面。
- 按百分比自动标定裁切区域，剔除白板工具栏；更换分辨率或教室无需重新设置。
- 可选 AI 归档：由视觉模型判断学科，归入「日期 / 学科」目录；未配置 API Key 时退化为按时间归档。
- 单实例运行、开机自启、绿色免安装。
- 输出为原始 PNG，只写一次，不做二次压缩或覆盖。

## 运行环境

| 项目 | 要求 |
| --- | --- |
| 系统 | Windows 10（1809 及以上）/ Windows 11，64 位 |
| 依赖 | 无。Qt 运行库随发布包分发，无需安装 .NET 或 VC++ 运行库 |
| 磁盘 | 程序约 85 MB；板书约 3–10 MB/课时 |
| 内存 | 常驻约 100 MB |
| 权限 | 普通用户即可。开机自启写入 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` |
| 网络 | 默认不联网；仅 AI 归档时访问所配置的模型服务 |

## 安装

1. 从 [Releases](https://github.com/caitoUSB/BoardMark/releases/latest) 下载 `BoardMark-vX.Y.Z-win64.zip`，解压到固定目录，例如 `D:\BoardMark`。
2. 运行 `BoardMark.exe`。程序没有主窗口，启动后驻留在通知区域。
3. 右键托盘图标 → 设置，确认目标软件、保存位置与开机自启。

板书默认保存在程序目录下，因此不建议将程序放在桌面、下载目录或启用了还原卡的磁盘分区。

## 使用

托盘菜单：

| 菜单项 | 说明 |
| --- | --- |
| 开始截图 / 停止截图 | 启动或暂停采集 |
| 设置 | 目标软件、采样间隔、裁切、保存位置、AI 接口 |
| AI 整理记录 | 对未归档的会话执行学科判断与归档 |
| 打开记录文件夹 | 定位到 `recordings\` |
| 打开归档文件夹 | 定位到 `板书笔记\` |
| 关于 / 退出 | — |

一次课堂的操作只有三步：上课前确认托盘菜单为「停止截图」，课中不作干预，课后停止截图并执行「AI 整理记录」，将 `板书笔记\<日期>\<学科>\` 交付。

## 输出目录

```
BoardMark\
├── recordings\
│   └── 2026-11-04_190308\           一次录制一个文件夹，命名「日期_时刻」
│       ├── 190324.png               原始截图，命名「时分秒」
│       └── session.json             帧清单，含每张的保存原因
├── 板书笔记\
│   └── 2026-11-04\
│       ├── 原图\                    原始帧的硬链接，只读
│       ├── 数学\01_一元二次方程.png  按学科归档，序号即上课顺序
│       ├── 其他\                    未识别出学科的帧
│       └── index.json               当日索引
├── settings.json                    配置
├── usage.json                       AI 用量账本
└── BoardMark.log                    运行日志
```

未识别的截图归入 `板书笔记\<日期>\其他\`。归档目录中是硬链接，与原始记录共用同一份数据，删除其中一方不影响另一方。

## 配置

设置通过 `settings.json` 保存，常用的几项：

| 键 | 默认值 | 说明 |
| --- | --- | --- |
| `target_process` | 空 | 目标进程名，留空为自动识别 |
| `idle_interval_ms` | `1000` | 静止时的采样间隔 |
| `active_interval_ms` | `120` | 检测到变化后的采样间隔 |
| `replace_threshold` | `0.35` | 整屏替换判定阈值，调低则保存更频繁 |
| `max_span_seconds` | `180` | 持续书写时的强制保存间隔 |
| `ai_api_key` | 空 | 留空则不启用 AI 归档 |

完整参数、取值含义与调参方法见[技术说明](docs/技术说明.md)。

## AI 归档（可选）

接口兼容 OpenAI Chat Completions，默认使用 DashScope 的 `qwen3-vl-flash`。归档只上传筛选后的关键帧（JPEG，长边不超过 1280），不包含窗口标题、进程名、文件路径与用户名。Key 无效、余额不足或模型不可用时，批处理立即停止并保留原始截图，不影响后续重新归档。

## 常见问题

**启动后通知区域没有图标。** 先查看程序目录下的 `BoardMark.log`。常见原因是安全软件拦截，或已存在运行中的实例（程序为单实例）。

**安全软件报毒。** 可执行文件未做数字签名，且包含屏幕采集、全局鼠标钩子与开机自启，易触发启发式告警。将程序目录加入信任区即可；如需校验文件完整性，可核对 Release 页面提供的 SHA256。

**未识别到白板，或没有生成截图。** 在设置中手动指定目标进程名。程序默认仅采集目标窗口处于前台且未被完全遮挡时的画面。

**截图过多或过少。** 调整 `replace_threshold`、`same_threshold`、`move_percent` 与 `max_span_seconds`，详见[技术说明](docs/技术说明.md)。

**系统缩放为 125% 或 150%。** 无需额外设置，裁切区域按百分比标定。

**多显示器。** 仅采集目标窗口所在的显示器。

**迁移到其他电脑。** 复制 `settings.json` 即可保留配置，其中可能包含 API Key。裁切标定与分辨率相关，建议在新机器上重新标定。

**开机自启未生效。** 若程序以管理员身份运行，或系统使用还原卡，注册表启动项可能不生效，可将快捷方式放入启动文件夹。

## 隐私

程序默认不联网，不统计、不埋点、不上报崩溃。仅执行 AI 归档时，将关键帧与提示词发送至所配置的模型服务。

课堂画面可能包含学生姓名、成绩与人脸，录制前应告知任课教师，并按学校规定处理与保存。

## 从源码构建

需要 Qt 6.11+（Core、Gui、Widgets、Network）、MinGW-w64 g++ 13+ 或 MSVC 2019+、CMake 3.21+、Ninja。

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_PREFIX_PATH=E:/Environment/mingw-w64/ucrt64 `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

关键帧判定可离线验证：把若干张板书截图放进一个目录，指定该目录即可，程序会输出每帧的判定结果与最终保留张数。

```powershell
build\BoardMark.exe --analyze D:\板书截图
```

代码结构、参数含义、故障排查与设计约定见 [docs/技术说明.md](docs/技术说明.md)。

## 许可

[AGPL-3.0](LICENSE)。校内使用与自行部署无需付费；分发修改版或以网络服务形式提供时，需按同一许可公开源代码。第三方组件声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
