#pragma once

// 程序设置：存成一个 JSON 文件，放在 exe 同目录，方便老师直接改。
//
// 刻意把「目标进程名」做成可配置项，而不是硬编码 "EasiNote.exe"：
//   * 法律上 —— 产品定位是「通用屏幕记录工具」，不是针对某一家厂商的外挂
//   * 市场上 —— 天然可以覆盖鸿合等其他白板软件
//   * 维护上 —— 希沃改进程名时，用户自己改配置就行

#include <QMap>
#include <QRect>
#include <QSize>
#include <QString>

namespace cm {

/// 某款白板软件**在本机**标定出来的采集参数。
///
/// 为什么不随程序发布
/// ------------------
/// 裁切是"画面高度的百分比"，而工具栏的物理高度 =
/// 「逻辑高度 × Windows 缩放比」，画面高度 = 屏幕物理分辨率。
/// 实测同一款软件：1920x1080@100% 约占 6.1%，@125% 约占 7.6%，
/// 3840x2160@150% 约占 4.6% —— 写死任何一个在别的机器上都是错的。
/// 所以只能本机测，测完按**进程名**分开存（一台教室电脑可能装两套白板）。
struct BoardCalibration {
    int cropTopPct = 0;
    int cropBottomPct = 0;
    int cropLeftPct = 0;
    int cropRightPct = 0;
    bool calibrated = false;    ///< false = 还没标定过，用全局默认值
    int samples = 0;            ///< 标定用了多少帧（给人看的可信度参考）
    QString calibratedAt;       ///< ISO 时间，方便判断是不是很久以前测的
    QString displayName;        ///< 当时的显示名，例如「希沃轻白板」

    bool isEmpty() const
    {
        return !calibrated && cropTopPct == 0 && cropBottomPct == 0
               && cropLeftPct == 0 && cropRightPct == 0;
    }
};

struct Settings {
    // ---- 采集目标 ----
    /// 要记录的白板软件进程名。
    ///
    /// **留空（默认）= 自动识别**：程序从当前窗口里认出白板软件，
    /// 见 detectWhiteboard()。填了就用填的那个 —— 尊重老师的显式选择，
    /// 而且这样升级上来的老配置行为完全不变。
    ///
    /// 原来默认写死 "EasiNote.exe"，等于"只有希沃白板能用"；
    /// 改成留空之后，不同老师用不同白板都能自动适配。
    QString targetProcess;

    // ---- 采样 ----
    int idleIntervalMs = 1000;      ///< 画面静止时的采样间隔
    int activeIntervalMs = 120;     ///< 检测到变化后的加密采样间隔
    int activeHoldMs = 3000;        ///< 变化后维持加密采样的时长

    // ---- 关键帧判定 ----
    double replaceThreshold = 0.35; ///< 相似度低于此值 -> 判定为「画面大面积替换」
    int moveThreshold = 3;          ///< 位移超过此值（缩略图像素）-> 认为在平移
    double movePercent = 25.0;      ///< 累计位移达到画面高度的百分之多少才值得存一张
    double sameThreshold = 0.90;    ///< 相似度高于此值 -> 判定为「画面几乎没变」
    int maxSpanSeconds = 180;       ///< 长时间只写不停时，强制存一张的间隔（秒）

    // ---- 分析区域裁切（百分比）----
    // 白板软件自带的工具栏、横幅等固定 UI 在采集区域内部，而且每帧都一样。
    // 不裁掉的话，它们会把相似度垫得很高、把真实位移淹没 ——
    // ---- 分析区域裁切（百分比）----
    // 白板软件自带的工具栏、横幅等固定 UI 在采集区域内部，而且每帧都一样。
    // 不裁掉的话，它们会把相似度垫得很高、把真实位移淹没 ——
    // 实测不裁切时，34 帧真实拖动素材只会存下 1 张。
    //
    // **这四个值是"全局默认"，只在该进程还没被本机标定过时使用。**
    // 程序会在录制过程中自动测出这款白板真正需要裁多少（见 BoardCalibrator），
    // 测出来之后按进程名存进 boardCalibrations，此处的值就被绕过了。
    // 默认值沿用希沃白板的标定（顶部 5% 是激活横幅）。
    int cropTopPct = 5;
    int cropBottomPct = 23;
    int cropLeftPct = 0;
    int cropRightPct = 0;

    /// 按进程名分开存的本机标定结果。键是**小写进程名**。
    ///
    /// 分开存是必须的：一台教室电脑上可能装了希沃白板和希沃轻白板两套，
    /// 它们的工具栏高度完全不同（实测都是底部约 7~8%，但顶部一个有一个没有）。
    QMap<QString, BoardCalibration> boardCalibrations;

    // ---- 存储 ----
    QString outputDir;              ///< 空 = exe 同目录下的 recordings/

    // ---- 行为 ----
    bool startOnLaunch = false;     ///< 程序启动后自动开始记录
    /// 是否要求目标窗口在前台才采集。
    /// 开着更安全（不会把弹出的聊天窗口、通知写进板书）；
    /// 关掉适合老师会一边放课件一边切窗口的场景。
    bool requireForeground = true;

    /// 目标进程弹出额外窗口时是否暂停记录。
    /// 登录窗、加载窗、图形选择面板都会以「同进程的另一个窗口」出现，
    /// 这时画面的任何变化都不代表板书变化，必须整个跳过 ——
    /// 否则会把弹窗当成一屏板书截下来。
    bool pauseOnExtraWindows = true;
    int extraWindowMinW = 160;      ///< 小于这个尺寸的窗口不算「弹窗」
    int extraWindowMinH = 120;

    /// 画面里出现大片 UI 面板时是否暂停。
    /// 图形选择面板、更多菜单这类东西**不一定是独立窗口**，
    /// 靠窗口枚举抓不到，得靠画面形态判断（大块纯色区域）。
    bool pauseOnPanel = true;
    double panelThreshold = 0.02;   ///< 大块纯色占比超过多少算面板

    /// 墨量低于此值就认为屏幕基本是空的，不值得存。
    /// **只用来卡掉全空屏（息屏黑屏、刚清空的板），不要拿来判断"内容够不够多"**：
    /// 真实板书可以低到 0.002，而"几乎空板"可以高到 0.008，两组分布重叠。
    /// 实测取 0.010 时，34 帧真实素材会被砍到只剩 5 张。
    double emptyInk = 0.003;

    // ---- 课后 AI 整理（判学科、概括知识点、归档）----
    /// 接口地址。默认阿里云百炼的 OpenAI 兼容端点。
    /// 任何 OpenAI 兼容服务都能直接用，包括本地部署的 Ollama / LM Studio：
    ///     通义千问（百炼） https://dashscope.aliyuncs.com/compatible-mode/v1
    ///     智谱            https://open.bigmodel.cn/api/paas/v4
    ///     豆包（火山方舟）  https://ark.cn-beijing.volces.com/api/v3
    ///     本地 Ollama     http://localhost:11434/v1
    QString aiBaseUrl = QStringLiteral(
        "https://dashscope.aliyuncs.com/compatible-mode/v1");
    QString aiApiKey;                    ///< 空 = 不调模型，全部归到「其他」
    /// 默认用 flash：实测在"标题 + 公式 + 古诗 + 单词表"四类真实板书上
    /// 判断全对，而速度约 1 秒/张。
    /// 如果发现判不准，可以换成 qwen3-vl-plus 或 qwen-vl-max（更贵更慢）。
    ///
    /// **模型名里必须带 vl** —— 填成纯文本模型（例如 qwen3.7-plus）
    /// 程序不会报错，但模型根本看不见图。--ai-test 会提示这一点。
    QString aiModel = QStringLiteral("qwen3-vl-flash");
    /// 归档根目录。空 = exe 同目录下的「板书笔记」
    QString aiOutputDir;

    // ---- 定时整理 ----
    /// 每天定点自动整理，不用老师手动点。
    /// 上课期间完全不联网，所以放在课后 —— 默认 18:30，正好放学后。
    /// 到点时只整理「还没整理过的」会话，整理过的跳过，不会重复花额度。
    bool autoOrganizeEnabled = false;
    int  autoOrganizeHour = 18;
    int  autoOrganizeMinute = 30;

    static Settings load();
    bool save() const;

    /// 配置文件路径（exe 同目录 settings.json）
    static QString configPath();

    /// 实际使用的输出目录
    QString effectiveOutputDir() const;

    /// 实际使用的归档目录
    QString effectiveArchiveDir() const;

    /// 按百分比算出实际要分析的区域（像素）。frame 是采集到的画面尺寸。
    /// 用的是**全局默认**的裁切比例；要按进程取标定值请用下面的 analysisCropFor()。
    QRect analysisCrop(const QSize &frame) const;

    /// 某个进程实际要用的裁切比例：
    /// **该进程有本机标定就用标定值，没有才退回全局默认。**
    ///
    /// 标定值是这台机器上实测出来的，一定比全局默认更贴合 ——
    /// 因为全局默认是在别的机器、别的 DPI 缩放下标定的。
    QRect analysisCropFor(const QString &processName, const QSize &frame) const;

    /// 取某个进程的标定记录（没有就返回空的）。
    BoardCalibration calibrationFor(const QString &processName) const;

    /// 写入某个进程的标定记录（同时记录时间和样本数）。
    void setCalibration(const QString &processName, const BoardCalibration &cal);
};

} // namespace cm
