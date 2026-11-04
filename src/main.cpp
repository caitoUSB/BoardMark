// BoardMark · 课堂板书自动记录 · 程序入口
//
// 平时以托盘程序运行（无主窗口）；带上命令行参数时走控制台诊断模式，
// 方便开发和排错：
//
//     BoardMark                      以托盘程序运行
//     BoardMark --info               打印窗口信息后退出
//     BoardMark --grab 10 --out DIR  连续抓 10 帧并测速后退出
//     BoardMark --fullscreen         配合 --grab 抓整个屏幕
//
// 写代码时的两条底线，不要破坏：
//   1. 只读取屏幕像素，不解析、不修改、不注入任何第三方软件
//   2. 原始截图只写一次，之后只读

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include "ai/archiver.h"
#include "ai/classifier.h"
#include "ai/note_organizer.h"
#include "ai/organizer.h"
#include "analysis/frame_analyzer.h"
#include "analysis/keyframe_rules.h"
#include "app/autostart.h"
#include "app/settings.h"
#include "app/theme.h"
#include "app/version.h"
#include "app/tray_app.h"
#include "capture/capture_engine.h"
#include "capture/mouse_hook.h"
#include "capture/screen_capture.h"
#include "capture/window_detector.h"
#include "capture/whiteboard.h"

#include <cstdio>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace cm;

// ------------------------------------------------------------------ 基础设施

static void setupConsole()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

// Qt 默认消息处理器在 Windows 上会按本地代码页转码，控制台设了 UTF-8 也乱码。
// 这里直接写 UTF-8 字节，交互式控制台和重定向到管道两种情况都正确。
static void utf8MessageHandler(QtMsgType type, const QMessageLogContext &,
                               const QString &msg)
{
    const char *prefix = "";
    switch (type) {
    case QtWarningMsg:  prefix = "[!] "; break;
    case QtCriticalMsg: prefix = "[X] "; break;
    case QtFatalMsg:    prefix = "[F] "; break;
    default: break;
    }
    const QByteArray utf8 = msg.toUtf8();
    std::fputs(prefix, stderr);
    std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    if (type == QtFatalMsg) {
        std::abort();
    }
}

/// 单实例：已经有同名互斥体，说明程序已在运行
static bool acquireSingleInstance()
{
#ifdef _WIN32
    CreateMutexW(nullptr, TRUE, L"Global\\BoardMark_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return false;
    }
#endif
    return true;
}

/// 把本进程的控制台窗口隐藏掉（托盘模式用）。
///
/// **这是必须的，不是为了好看。** 屏幕抓取抓的是「屏幕在目标窗口矩形内
/// 显示的内容」，任何盖在白板上方的窗口都会被一起抓下来 ——
/// 实测就有一次把程序自己的控制台黑框连同板书存进了关键帧。
///
/// 保留控制台子系统（命令行诊断照常能用），只隐藏窗口。
static void hideOwnConsole()
{
#ifdef _WIN32
    if (HWND con = GetConsoleWindow()) {
        ShowWindow(con, SW_HIDE);
    }
#endif
}

/// 托盘模式下把日志写到 exe 同目录的 BoardMark.log。
/// 控制台被隐藏了，信息不能就这么丢掉 —— 出问题时让老师把这个文件发过来。
static void startFileLogging()
{
    static QFile logFile;
    logFile.setFileName(QDir(QCoreApplication::applicationDirPath())
                            .filePath(QStringLiteral("BoardMark.log")));
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }

    static QTextStream stream(&logFile);
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &,
                              const QString &msg) {
        const char *tag = "INFO";
        switch (type) {
        case QtWarningMsg:  tag = "WARN"; break;
        case QtCriticalMsg: tag = "ERR "; break;
        case QtFatalMsg:    tag = "FATAL"; break;
        default: break;
        }
        stream << QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"))
               << " [" << tag << "] " << msg << '\n';
        stream.flush();
    });
}

// ------------------------------------------------------------------ 控制台诊断

static int cmdInfo()
{
    const auto all = WindowDetector::enumerate();
    const Settings s = Settings::load();

    // 派生一个"实际会监控哪个进程"：设置留空时自动识别。
    // 这一步和采集引擎里的逻辑一致（见 CaptureEngine::run）。
    QString effective = s.targetProcess;
    const WhiteboardDetection det =
        detectWhiteboard(WindowDetector::enumerate(false));
    if (effective.isEmpty() && det.ok()) {
        effective = det.processName;
    }

    qInfo().noquote() << QStringLiteral("%1 ｜ %2")
                             .arg(QString::fromUtf8(cm::versionBanner()),
                                  QStringLiteral(CM_BUILD_DATE));
    qInfo().noquote() << QStringLiteral("目标软件设置：%1")
                             .arg(s.targetProcess.isEmpty()
                                      ? QStringLiteral("自动识别")
                                      : s.targetProcess);
    if (s.targetProcess.isEmpty()) {
        if (det.ok()) {
            qInfo().noquote()
                << QStringLiteral("  自动识别结果：%1（%2）｜依据 %3")
                       .arg(det.displayName, det.processName, det.reason);
        } else {
            qInfo().noquote()
                << "  自动识别结果：没认出来。可以在设置里手动选一个 —— "
                   "标定照样会自动进行。";
        }
    }

    // 本机标定（按进程分开存，所以每个进程都要单独报）
    if (s.boardCalibrations.isEmpty()) {
        qInfo().noquote() << "本机白板标定：还没有（开始记录时会自动测）";
    } else {
        qInfo().noquote() << "本机白板标定：";
        for (auto it = s.boardCalibrations.begin();
             it != s.boardCalibrations.end(); ++it) {
            const BoardCalibration &c = it.value();
            qInfo().noquote()
                << QStringLiteral("    %1  上%2% 下%3% 左%4% 右%5%"
                                  "（%6 帧%7）%8")
                       .arg(it.key())
                       .arg(c.cropTopPct)
                       .arg(c.cropBottomPct)
                       .arg(c.cropLeftPct)
                       .arg(c.cropRightPct)
                       .arg(c.samples)
                       .arg(c.calibratedAt.isEmpty()
                                ? QString()
                                : QStringLiteral("，%1").arg(c.calibratedAt))
                       .arg(c.displayName.isEmpty()
                                ? QString()
                                : QStringLiteral("  %1").arg(c.displayName));
        }
    }
    qInfo().noquote() << QStringLiteral("共发现 %1 个带标题的可见顶层窗口")
                             .arg(all.size());

    // 开机自启动：注册表才是事实来源，所以直接读它，不读配置
    if (isAutoStartEnabled()) {
        qInfo().noquote() << QStringLiteral("开机自启动：已开启  %1")
                                 .arg(autoStartCommand());
    } else {
        qInfo().noquote() << QStringLiteral("开机自启动：未开启（%1）")
                                 .arg(autoStartRegistryPath());
    }
    qInfo().noquote() << "";

    const auto wins = effective.isEmpty()
                          ? std::vector<WindowInfo>()
                          : WindowDetector::findByProcess({effective});
    if (wins.empty()) {
        if (effective.isEmpty()) {
            qInfo().noquote() << "没有认出白板软件，也没有手工指定。";
            qInfo().noquote() << "（从下面的列表里找到它，填进设置里）";
        } else {
            qInfo().noquote() << QStringLiteral("没有找到进程名含 «%1» 的窗口。")
                                     .arg(effective);
            qInfo().noquote() << "（从下面的列表里找到正确的进程名，填进设置里）";
        }
    }
    for (const WindowInfo &w : wins) {
        qInfo().noquote() << "  [目标窗口]";
        qInfo().noquote() << QStringLiteral("    标题 : %1").arg(w.title);
        qInfo().noquote() << QStringLiteral("    进程 : %1 (PID %2)")
                                 .arg(w.process).arg(w.pid);
        if (w.minimized) {
            qInfo().noquote() << "    状态 : [最小化] 抓不到画面";
        } else {
            qInfo().noquote() << QStringLiteral("    区域 : (%1,%2) %3 x %4")
                                     .arg(w.rect.x()).arg(w.rect.y())
                                     .arg(w.rect.width()).arg(w.rect.height());
        }
        qInfo().noquote() << QStringLiteral("    前台 : %1")
                                 .arg(WindowDetector::isForeground(w.hwnd) ? "是" : "否");
        qInfo().noquote() << "";
    }

    qInfo().noquote() << "当前所有可见顶层窗口：";
    for (const WindowInfo &w : all) {
        qInfo().noquote() << QStringLiteral("    %1  «%2»")
                                 .arg(w.process, -28).arg(w.title);
    }
    return 0;
}

// ------------------------------------------------------------------ 窗口变化监视

/// 盯着目标进程的窗口列表，一有变化就打印。
///
/// 用途：让老师**在希沃里真实操作一遍**（登录、开图形选择、重启白板），
/// 我们就能看清那些弹窗长什么样、叫什么类名、多大尺寸 ——
/// 然后才谈得上"怎么把它们识别出来并跳过"。
static int cmdWatch(int seconds, const Settings &s)
{
    qInfo().noquote() << QStringLiteral("监视 %1 的窗口变化，持续 %2 秒")
                             .arg(s.targetProcess).arg(seconds);
    qInfo().noquote() << "请在这段时间里做这些操作：";
    qInfo().noquote() << "  1. 登录希沃账号（弹出登录窗口）";
    qInfo().noquote() << "  2. 打开作图/图形选择（弹出图形面板）";
    qInfo().noquote() << "  3. 关掉希沃再重新打开（出现加载窗口）";
    qInfo().noquote() << "";

    QString lastSig;
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < qint64(seconds) * 1000) {
        // 关键：把**无标题窗口**也列出来。
        // 图形选择面板、浮层这类窗口往往没有标题，只看有标题的会漏掉。
        const auto wins = WindowDetector::findByProcess({s.targetProcess}, false);

        QString sig;
        for (const WindowInfo &w : wins) {
            sig += QStringLiteral("%1|%2|%3|%4x%5|%6\n")
                       .arg(reinterpret_cast<quintptr>(w.hwnd))
                       .arg(w.title).arg(w.className)
                       .arg(w.rect.width()).arg(w.rect.height())
                       .arg(w.minimized ? "min" : "");
        }
        // 前台窗口也要看：白板被同进程的弹窗盖住时，前台会变成弹窗
        const HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        wchar_t fgTitle[256] = {};
        GetWindowTextW(fg, fgTitle, 256);
        wchar_t fgClass[256] = {};
        GetClassNameW(fg, fgClass, 256);
        sig += QStringLiteral("FG|%1|%2|%3")
                   .arg(QString::fromWCharArray(fgTitle),
                        QString::fromWCharArray(fgClass))
                   .arg(WindowDetector::processNameOf(fgPid));

        if (sig != lastSig) {
            lastSig = sig;
            qInfo().noquote() << "";
            qInfo().noquote() << QStringLiteral("-------- %1 秒时发生变化 --------")
                                     .arg(timer.elapsed() / 1000.0, 0, 'f', 1);
            if (wins.empty()) {
                qInfo().noquote() << "  （没有窗口）";
            }
            for (const WindowInfo &w : wins) {
                qInfo().noquote()
                    << QStringLiteral("  0x%1  %2x%3  «%4»  类名=%5%6%7")
                           .arg(reinterpret_cast<quintptr>(w.hwnd), 0, 16)
                           .arg(w.rect.width()).arg(w.rect.height())
                           .arg(w.title, w.className)
                           .arg(w.title.isEmpty() ? QStringLiteral("  [无标题]")
                                                  : QString())
                           .arg(w.minimized ? "  [最小化]" : "");
            }
            qInfo().noquote() << QStringLiteral("  前台: «%1»  类名=%2  进程=%3")
                                     .arg(QString::fromWCharArray(fgTitle),
                                          QString::fromWCharArray(fgClass),
                                          WindowDetector::processNameOf(fgPid));
        }

        QThread::msleep(200);
    }

    qInfo().noquote() << "";
    qInfo().noquote() << "监视结束。";
    return 0;
}

static int cmdGrab(int count, int intervalMs, const QString &outDir, bool fullScreen,
                   const QSize &region)
{
    QRect rect;
    QString label;

    if (region.isValid() && region.width() > 0) {
        rect = QRect(0, 0, region.width(), region.height());
        label = QStringLiteral("指定区域 %1x%2（诊断用）")
                    .arg(region.width()).arg(region.height());
    } else if (fullScreen) {
        rect = QRect(GetSystemMetrics(SM_XVIRTUALSCREEN),
                     GetSystemMetrics(SM_YVIRTUALSCREEN),
                     GetSystemMetrics(SM_CXVIRTUALSCREEN),
                     GetSystemMetrics(SM_CYVIRTUALSCREEN));
        label = QStringLiteral("整个屏幕（诊断用）");
    } else {
        const Settings s = Settings::load();
        const auto wins = WindowDetector::findByProcess({s.targetProcess});
        const WindowInfo *best = WindowDetector::pickBest(wins);
        if (!best) {
            qCritical().noquote() << QStringLiteral("找不到 %1 的窗口。")
                                         .arg(s.targetProcess);
            return 3;
        }
        if (best->minimized || !best->rect.isValid()) {
            qCritical().noquote() << "目标窗口处于最小化状态，抓不到画面。";
            return 3;
        }
        rect = best->rect;
        label = QStringLiteral("«%1» %2").arg(best->title, best->process);
    }

    QDir dir(outDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        qCritical().noquote() << QStringLiteral("无法创建目录：%1").arg(outDir);
        return 4;
    }

    qInfo().noquote() << QStringLiteral("采集目标 : %1").arg(label);
    qInfo().noquote() << QStringLiteral("采集区域 : (%1, %2)  %3 x %4")
                             .arg(rect.x()).arg(rect.y())
                             .arg(rect.width()).arg(rect.height());
    qInfo().noquote() << "";
    qInfo().noquote() << QStringLiteral("%1 %2 %3")
                             .arg(QStringLiteral("序号"), -6)
                             .arg(QStringLiteral("抓屏"), -10)
                             .arg(QStringLiteral("分析"));
    qInfo().noquote() << QString(58, QLatin1Char('-'));

    qint64 totalGrab = 0;
    qint64 totalAnalyze = 0;
    int saved = 0;
    int done = 0;

    for (int i = 0; i < count; ++i) {
        QElapsedTimer t;
        t.start();
        const QImage img = ScreenCapture::grab(rect);
        const qint64 grabMs = t.elapsed();

        if (img.isNull()) {
            qWarning().noquote() << QStringLiteral("第 %1 帧抓取失败：%2")
                                        .arg(i).arg(ScreenCapture::lastError());
            continue;
        }

        t.restart();
        const GrayFrame gray = FrameAnalyzer::toGray(img, 480);
        const qint64 analyzeMs = t.elapsed();

        totalGrab += grabMs;
        totalAnalyze += analyzeMs;
        ++done;

        const QString path = dir.filePath(
            QStringLiteral("grab_%1.png").arg(i, 4, 10, QLatin1Char('0')));
        if (img.save(path, "PNG")) {
            ++saved;
        }

        qInfo().noquote() << QStringLiteral("%1 %2 %3")
                                 .arg(i, -6)
                                 .arg(QStringLiteral("%1 ms").arg(grabMs), -10)
                                 .arg(QStringLiteral("%1 ms   墨量 %2")
                                          .arg(analyzeMs, 3)
                                          .arg(gray.ink(), 0, 'f', 2));
        if (i + 1 < count && intervalMs > 0) {
            Sleep(static_cast<DWORD>(intervalMs));
        }
    }

    qInfo().noquote() << "";
    qInfo().noquote() << QStringLiteral("已保存 %1 张到 %2")
                             .arg(saved)
                             .arg(QDir::toNativeSeparators(dir.absolutePath()));
    if (done > 0) {
        const double g = double(totalGrab) / done;
        const double a = double(totalAnalyze) / done;
        qInfo().noquote() << QStringLiteral("平均抓屏 %1 ms，平均分析 %2 ms，合计 %3 ms")
                                 .arg(g, 0, 'f', 1).arg(a, 0, 'f', 1).arg(g + a, 0, 'f', 1);
        qInfo().noquote() << "决定采样率上限的是「抓屏」那一项；"
                             "分析只在缩略图上做，很便宜。";
    }
    return saved > 0 ? 0 : 5;
}

// ------------------------------------------------------------------ 离线回放

/// 把一个文件夹里的截图按顺序喂给关键帧判定逻辑，打印它会怎么切。
/// 用途：拿真实课堂素材验证判定规则，不用每次开着白板手工试。
static int cmdAnalyze(const QString &dirPath, const Settings &s)
{
    QDir dir(dirPath);
    if (!dir.exists()) {
        qCritical().noquote() << QStringLiteral("目录不存在：%1").arg(dirPath);
        return 3;
    }

    QStringList names = dir.entryList({QStringLiteral("*.png"), QStringLiteral("*.jpg"),
                                       QStringLiteral("*.jpeg"), QStringLiteral("*.bmp")},
                                      QDir::Files, QDir::Name);
    if (names.isEmpty()) {
        qCritical().noquote() << "目录里没有图片";
        return 3;
    }

    KeyframeParams p;
    p.replaceThreshold = s.replaceThreshold;
    p.moveThreshold = s.moveThreshold;
    p.movePercent = s.movePercent;
    p.sameThreshold = s.sameThreshold;
    p.maxSpanSeconds = s.maxSpanSeconds;
    p.emptyInk = float(s.emptyInk);
    p.panelThreshold = float(s.panelThreshold);

    qInfo().noquote() << QStringLiteral("回放 %1 张｜替换阈值 %2｜平移阈值 %3 像素｜"
                                        "累计平移满 %4% 才存｜兜底 %5 秒")
                             .arg(names.size())
                             .arg(p.replaceThreshold).arg(p.moveThreshold)
                             .arg(p.movePercent).arg(p.maxSpanSeconds);
    qInfo().noquote() << QStringLiteral("分析裁切：上 %1%  下 %2%  左 %3%  右 %4%")
                             .arg(s.cropTopPct).arg(s.cropBottomPct)
                             .arg(s.cropLeftPct).arg(s.cropRightPct);
    qInfo().noquote() << "";
    qInfo().noquote() << QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8 %9")
                             .arg(QStringLiteral("序号"), -5)
                             .arg(QStringLiteral("文件"), -14)
                             .arg(QStringLiteral("类型"), -12)
                             .arg(QStringLiteral("累计位移"), -12)
                             .arg(QStringLiteral("未动相似"), -10)
                             .arg(QStringLiteral("对齐相似"), -10)
                             .arg(QStringLiteral("墨量 候选|当前"), -14)
                             .arg(QStringLiteral("面板 候选|当前"), -14)
                             .arg(QStringLiteral("判定"));
    qInfo().noquote() << QString(128, QLatin1Char('-'));

    GrayFrame candidate;
    GrayFrame lastSaved;
    qint64 spanStart = 0;
    qint64 lastSaveAt = 0;
    qint64 firstAt = 0;
    int savedCount = 0;

    // 顺便拿这批素材跑一遍**白板裁切自动标定**，看它会算出什么。
    // 用途：换一款白板软件时，先用真实截图确认标定结果对不对，
    // 不用真的开着白板录一节课才知道。
    BoardCalibrator calibrator;

    for (int i = 0; i < names.size(); ++i) {
        const QString path = dir.filePath(names[i]);
        QImage img(path);
        if (img.isNull()) {
            continue;
        }
        {
            // 标定必须在**未裁切**的画面上做 —— 它要干的正是"找出该裁哪里"
            const RowColProfile prof = FrameAnalyzer::rowColProfile(img, 480);
            if (prof.valid()) {
                calibrator.feed(prof.rows, prof.srcH);
            }
        }
        const GrayFrame g = FrameAnalyzer::toGray(img, 480, s.analysisCrop(img.size()));
        if (!g.valid()) {
            continue;
        }

        const qint64 t = QFileInfo(path).lastModified().toMSecsSinceEpoch();
        if (!candidate.valid()) {
            if (s.pauseOnPanel && g.hasPanel(float(s.panelThreshold))) {
                continue;
            }
            candidate = g;
            spanStart = t;
            firstAt = t;
            continue;
        }

        // 和引擎保持一致：当前这一帧里有 UI 面板就整个跳过，
        // 既不采样也不让它顶替候选帧。不加这一步，回放结果和实跑会不一致。
        if (s.pauseOnPanel && g.hasPanel(float(s.panelThreshold))) {
            qInfo().noquote() << QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8 %9")
                                     .arg(i, -5)
                                     .arg(names[i], -14)
                                     .arg(QStringLiteral("有工具面板"), -12)
                                     .arg(QString(), -12).arg(0.0, 9, 'f', 3)
                                     .arg(0.0, 9, 'f', 3)
                                     .arg(QStringLiteral("-|%1").arg(g.ink(), 0, 'f', 3), -14)
                                     .arg(QStringLiteral("-|%1").arg(g.solidRatio, 0, 'f', 3), -14)
                                     .arg(QStringLiteral("跳过（不算板书画面）"));
            continue;
        }

        const PairAnalysis a = analyzePair(candidate, g, p, candidate.h);
        const qint64 sinceSave = (lastSaveAt > 0) ? (t - lastSaveAt) : (t - firstAt);
        const KeyframeDecision dec =
            decideKeyframe(p, a, candidate, lastSaved, candidate.h, t - spanStart, sinceSave);

        QString verdict;
        if (dec.save) {
            // 写清楚存的到底是哪一帧 —— 被存的是**候选帧**（变化前那一屏），
            // 不是当前这一帧。不写清楚很容易看错（我自己就看错过）。
            verdict = QStringLiteral("★ 保存 %1（墨量 %2）—— %3")
                          .arg(names[i - 1])
                          .arg(candidate.ink(), 0, 'f', 3)
                          .arg(dec.reason);
            lastSaved = candidate;
            lastSaveAt = t;
            candidate = g;
            spanStart = t;
            ++savedCount;
        } else if (dec.updateCandidate) {
            verdict = QStringLiteral("继续");
            candidate = g;
        } else {
            verdict = QStringLiteral("继续累积位移");
        }

        qInfo().noquote() << QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8 %9")
                                 .arg(i, -5)
                                 .arg(names[i], -14)
                                 .arg(QString::fromUtf8(pairKindName(a.kind)), -12)
                                 .arg(QStringLiteral("%1,%2").arg(a.dx).arg(a.dy), -12)
                                 .arg(a.score00, 9, 'f', 3)
                                 .arg(a.score, 9, 'f', 3)
                                 .arg(QStringLiteral("%1|%2").arg(candidate.ink(), 0, 'f', 3)
                                          .arg(g.ink(), 0, 'f', 3), -14)
                                 .arg(QStringLiteral("%1|%2")
                                          .arg(candidate.solidRatio, 0, 'f', 3)
                                          .arg(g.solidRatio, 0, 'f', 3), -14)
                                 .arg(verdict);
    }

    qInfo().noquote() << "";
    qInfo().noquote() << QStringLiteral("输入 %1 帧  ->  保存 %2 张关键帧（压缩到约 %3%）")
                             .arg(names.size())
                             .arg(savedCount + 1)
                             .arg(names.isEmpty() ? 0 : (savedCount + 1) * 100 / names.size());

    // ---- 白板裁切自动标定：这批素材会算出什么 ----
    qInfo().noquote() << "";
    const int needF = BoardCalibrator::kMinFrames;
    const int needC = BoardCalibrator::kMinChanges;
    qInfo().noquote() << QStringLiteral("自动标定参考（需要 %1 帧 + %2 次内容变化）：")
                             .arg(needF)
                             .arg(needC);
    qInfo().noquote() << QStringLiteral("    这批素材：%1 帧，检测到 %2 次内容变化")
                             .arg(calibrator.frames())
                             .arg(calibrator.changes());
    const BoardCalibration cal = calibrator.result();
    if (cal.calibrated) {
        qInfo().noquote() << QStringLiteral("    算出裁切：上 %1%  下 %2%  左 %3%  右 %4%")
                                 .arg(cal.cropTopPct)
                                 .arg(cal.cropBottomPct)
                                 .arg(cal.cropLeftPct)
                                 .arg(cal.cropRightPct);
        if (!calibrator.ready()) {
            qInfo().noquote() << "    ⚠ 样本还不够，这个数只是参考 —— "
                                 "实际录制时会一直测到够为止才采用";
        }
        qInfo().noquote() << QStringLiteral("    （当前设置里的裁切是 上 %1% 下 %2% 左 %3% 右 %4%）")
                                 .arg(s.cropTopPct).arg(s.cropBottomPct)
                                 .arg(s.cropLeftPct).arg(s.cropRightPct);
    } else {
        qInfo().noquote() << "    没测出可信的固定 UI 边缘（画面可能太单一，"
                             "或者这批素材里白板内容没怎么动过）";
    }
    return 0;
}

// ------------------------------------------------------------------ 无界面记录测试

/// 不开托盘，直接跑采集引擎若干秒后正常收尾。
/// 用途：验证「采集 → 判定 → 落盘」这条完整链路，也方便在没有桌面的环境里跑。
static int cmdRecord(int seconds, const Settings &s)
{
    auto *engine = new CaptureEngine(s);

    QObject::connect(engine, &CaptureEngine::sessionStarted,
        [](const QString &d) { qInfo().noquote() << "[会话] " << d; });
    QObject::connect(engine, &CaptureEngine::statusMessage,
        [](const QString &t) { qInfo().noquote() << "[状态] " << t; });
    QObject::connect(engine, &CaptureEngine::frameSaved,
        [](const QString &, const QString &r) {
            qInfo().noquote() << "[保存] " << r;
        });
    QObject::connect(engine, &CaptureEngine::failed,
        [](const QString &t) { qCritical().noquote() << "[失败] " << t; });

    QEventLoop loop;
    QObject::connect(engine, &CaptureEngine::sessionStopped,
        [&](int n) {
            qInfo().noquote() << QStringLiteral("收尾完成，共保存 %1 张关键帧").arg(n);
            loop.quit();
        });

    qInfo().noquote() << QStringLiteral("开始无界面记录 %1 秒，目标进程 %2")
                             .arg(seconds).arg(s.targetProcess);
    engine->start();
    QTimer::singleShot(seconds * 1000, engine, &CaptureEngine::requestStop);
    // 超时保护：万一收尾卡住也要退出
    QTimer::singleShot((seconds + 15) * 1000, &loop, &QEventLoop::quit);
    loop.exec();

    engine->requestStop();
    engine->wait(5000);
    delete engine;
    return 0;
}

// ------------------------------------------------------------------ 课后 AI 整理

/// 打印一批整理的统计（含"花了多少"）。
static void printBatchStats(const OrganizeBatchResult &batch)
{
    qInfo().noquote() << "";
    // 中途停了要先说，而且要说清"没标记"，否则脚本化的调用会以为跑完了
    if (batch.stoppedEarly()) {
        if (batch.cancelled) {
            qInfo().noquote() << "**已取消** —— 剩下的会话没有继续做。";
        } else {
            qInfo().noquote() << "**提前中断** —— 剩下的会话没有继续做。";
            qInfo().noquote() << QStringLiteral("原因：%1").arg(batch.stopReason);
        }
        qInfo().noquote() << QStringLiteral(
            "没做完的会话**没有**写 organized.json，所以下次再跑会从断点继续；"
            "已经判好的帧在 classify.json 里，会命中缓存、不重复花钱。");
        qInfo().noquote() << "（正在处理的那一个没计进下面的成功/失败）";
    }
    qInfo().noquote() << QStringLiteral("会话 %1 个：成功 %2，失败 %3")
                             .arg(batch.sessions)
                             .arg(batch.sessionsOk)
                             .arg(batch.sessionsFailed);
    qInfo().noquote() << QStringLiteral("关键帧 %1 张：新识别 %2（花钱），"
                                        "复用缓存 %3（不花钱），失败 %4")
                             .arg(batch.frames())
                             .arg(batch.identified)
                             .arg(batch.cachedHits)
                             .arg(batch.failed);
    const qint64 total = qint64(batch.promptTokens) + batch.completionTokens;
    qInfo().noquote() << QStringLiteral("本次消耗 token：输入 %1 + 输出 %2 = %3")
                             .arg(batch.promptTokens)
                             .arg(batch.completionTokens)
                             .arg(total);
    if (!batch.firstError.isEmpty()) {
        qInfo().noquote() << QStringLiteral("失败原因（第一条）：%1")
                                 .arg(batch.firstError);
    }
    if (!batch.dayDirs.isEmpty()) {
        qInfo().noquote() << "归档到：";
        for (const QString &d : batch.dayDirs) {
            qInfo().noquote() << QStringLiteral("  → %1").arg(d);
        }
    }
    if (!batch.errors.isEmpty()) {
        qInfo().noquote() << "未完成的会话：";
        for (const QString &e : batch.errors) {
            qInfo().noquote() << QStringLiteral("  ! %1").arg(e);
        }
    }
}

/// 课后 AI 整理。
///
/// 不带 --session-dir 时整理**所有还没整理过的**会话 —— 和托盘菜单、
/// 定时整理走的是同一个 organizePendingSessions()，
/// 保证"任何入口都只整理没整理过的"这条语义只有一份实现。
/// 已经整理过的会跳过，所以重复执行不会重复调用模型。
static int cmdClassify(const QString &sessionArg, const QString &archiveArg,
                       bool force, const Settings &s)
{
    const QString archiveRoot = archiveArg.trimmed().isEmpty()
                                    ? s.effectiveArchiveDir()
                                    : QDir::cleanPath(archiveArg.trimmed());
    const QString sessionDir = sessionArg.trimmed().isEmpty()
                                   ? QString()
                                   : QDir::cleanPath(sessionArg.trimmed());

    const AiConfig cfg = loadAiConfig();
    Classifier probe(cfg);
    qInfo().noquote() << QStringLiteral("归档目录：%1").arg(archiveRoot);
    qInfo().noquote() << QStringLiteral("视觉模型：%1").arg(probe.describe());
    if (probe.usingStub()) {
        qInfo().noquote()
            << "（没配 API Key，所有内容会归到「其他」。"
               "整条流程仍然会走完，可以用来验证归档结构。）";
    }
    if (force) {
        qInfo().noquote() << "（--reclassify：忽略识别缓存，重新调用模型）";
    }
    qInfo().noquote() << "";

    // ---- 指定了单个会话：只做它 ----
    if (!sessionDir.isEmpty()) {
        if (!QDir(sessionDir).exists()) {
            qCritical().noquote()
                << QStringLiteral("找不到会话目录：%1").arg(sessionDir);
            return 3;
        }
        qInfo().noquote() << QStringLiteral("会话目录：%1").arg(sessionDir);

        OrganizeOptions opt;
        opt.sessionDir = sessionDir;
        opt.archiveRoot = archiveRoot;
        opt.forceReclassify = force;

        const OrganizeResult res =
            organizeSession(opt, [](const OrganizeProgress &p) {
                qInfo().noquote() << QStringLiteral("[%1/%2] %3")
                                         .arg(p.done).arg(p.total).arg(p.message);
            });

        if (!res.ok()) {
            qCritical().noquote() << QStringLiteral("整理失败：%1").arg(res.error);
            return 4;
        }

        // 根本性错误（余额用完 / Key 无效 / 地址模型名错）时**不能打标记** ——
        // 打了老师充值之后这节课就永远是「未识别」，再也不会被自动重试。
        if (res.aborted) {
            qCritical().noquote()
                << QStringLiteral("提前中断：%1").arg(res.stopReason);
            qCritical().noquote() << QStringLiteral(
                "没有写 organized.json，问题解决后重跑本命令会从断点继续"
                "（已判好的帧命中缓存，不重复花钱）。");
            return 5;
        }
        if (res.cancelled) {
            qCritical().noquote() << "已取消，没有写 organized.json。";
            return 5;
        }

        // 显式整理了某个会话也要打标记 —— 否则定时整理会把它再做一遍
        markOrganized(sessionDir, archiveRoot, res.total);

        OrganizeBatchResult batch;
        batch.sessions = 1;
        batch.sessionsOk = 1;
        batch.succeeded = res.succeeded;
        batch.failed = res.failed;
        batch.identified = res.identified;
        batch.cachedHits = res.cachedHits;
        batch.promptTokens = res.promptTokens;
        batch.completionTokens = res.completionTokens;
        batch.firstError = res.firstError;
        batch.dayDirs = res.archive.dayDirs;
        printBatchStats(batch);

        qInfo().noquote() << "";
        qInfo().noquote() << QStringLiteral("归档文件夹：%1")
                                 .arg(QDir::toNativeSeparators(
                                     res.archive.dayDirs.isEmpty()
                                         ? archiveRoot
                                         : res.archive.dayDirs.first()));
        qInfo().noquote()
            << "（index.json 已设为隐藏，是给程序读的；"
               "需要在资源管理器里「显示隐藏的项目」才看得到）";
        return 0;
    }

    // ---- 没指定：整理所有还没整理过的 ----
    const QStringList pending = findPendingSessions(s.effectiveOutputDir());
    if (pending.isEmpty()) {
        qInfo().noquote() << "没有需要整理的新记录。";
        qInfo().noquote()
            << "（已经整理过的会话会自动跳过，不会重复调用模型；"
               "要重做某一个会话请用 --session-dir 指定）";
        return 0;
    }
    qInfo().noquote() << QStringLiteral("发现 %1 个还没整理过的会话").arg(pending.size());
    qInfo().noquote() << "";

    const OrganizeBatchResult batch = organizePendingSessions(
        s.effectiveOutputDir(), archiveRoot, force,
        [](const QString &sessionName, int index, int count,
           const OrganizeProgress &p) {
            qInfo().noquote() << QStringLiteral("[%1/%2] %3  %4/%5  %6")
                                     .arg(index)
                                     .arg(count)
                                     .arg(sessionName)
                                     .arg(p.done)
                                     .arg(p.total)
                                     .arg(p.message);
        });

    printBatchStats(batch);
    // 中途停了也算失败退出码 —— 脚本化调用（比如批处理）要能靠退出码发现
    if (batch.stoppedEarly()) {
        return 5;
    }
    return batch.sessionsFailed > 0 ? 1 : 0;
}

// ------------------------------------------------------------------ AI 自检

/// 把一张图发给配置好的模型，报告它到底能不能用、能不能看图。
///
/// 这个命令解决的是最实际的问题：模型名填错（填成纯文本模型）、
/// key 没开通视觉权限、接口地址写错 —— 这几种情况的报错都很含糊，
/// 直接看原始回复最快，不用来回猜。
static int cmdAiTest(const QString &imageArg, const Settings &s)
{
    QString image = imageArg.trimmed();
    if (image.isEmpty() || !QFile::exists(image)) {
        qCritical().noquote()
            << "找不到测试图片。用法：BoardMark.exe --ai-test <一张板书截图>";
        return 3;
    }

    qInfo().noquote() << QStringLiteral("用这张图测试：%1").arg(image);
    qInfo().noquote() << "";

    Classifier c(loadAiConfig());
    qInfo().noquote() << c.selfTest(image);
    return 0;
}

// ------------------------------------------------------------------ 整理平铺截图

/// 整理一个「平铺的截图文件夹」：按日期分天，按内容分学科，提炼知识点重命名。
/// 和 --classify 的区别：那个读录制会话，这个读一堆散图。
static int cmdNoteOrganize(const QString &srcArg, const QString &outArg,
                           bool force, const Settings &s)
{
    NoteOrganizeOptions opt;
    opt.sourceDir = QDir::cleanPath(srcArg.trimmed());
    opt.outputDir = outArg.trimmed().isEmpty()
                        ? QDir(s.effectiveArchiveDir()).filePath(QStringLiteral("整理结果"))
                        : QDir::cleanPath(outArg.trimmed());
    opt.forceReclassify = force;

    if (opt.sourceDir.isEmpty() || !QDir(opt.sourceDir).exists()) {
        qCritical().noquote()
            << QStringLiteral("找不到素材目录：%1").arg(opt.sourceDir);
        return 3;
    }

    Classifier probe(loadAiConfig());
    qInfo().noquote() << QStringLiteral("素材目录：%1").arg(opt.sourceDir);
    qInfo().noquote() << QStringLiteral("输出目录：%1").arg(opt.outputDir);
    qInfo().noquote() << QStringLiteral("视觉模型：%1").arg(probe.describe());
    qInfo().noquote() << "";

    const NoteOrganizeResult res =
        organizeNoteFolder(opt, [](const NoteOrganizeProgress &p) {
            qInfo().noquote() << QStringLiteral("[%1/%2] %3")
                                     .arg(p.done).arg(p.total).arg(p.message);
        });

    if (!res.ok()) {
        qCritical().noquote() << QStringLiteral("整理失败：%1").arg(res.error);
        return 4;
    }

    qInfo().noquote() << "";
    qInfo().noquote() << QStringLiteral("共 %1 张：判断成功 %2，失败 %3，归档 %4")
                             .arg(res.total).arg(res.succeeded)
                             .arg(res.failed).arg(res.copied);
    qInfo().noquote() << QStringLiteral("分成 %1 天").arg(res.dayCount);
    if (res.promptTokens > 0 || res.completionTokens > 0) {
        const int total = res.promptTokens + res.completionTokens;
        qInfo().noquote()
            << QStringLiteral("本次消耗 token：输入 %1 + 输出 %2 = %3")
                   .arg(res.promptTokens).arg(res.completionTokens).arg(total);
        qInfo().noquote()
            << QStringLiteral("  参考：百炼免费额度每模型 100 万，"
                              "按这个用量大约还能跑 %1 次这么多图")
                   .arg(total > 0 ? 1000000 / total : 0);
    }
    for (const QString &d : res.dayDirs) {
        qInfo().noquote() << QStringLiteral("  → %1").arg(d);
    }
    return 0;
}

// ------------------------------------------------------------------ 恢复默认设置

/// 把所有参数恢复成出厂默认。
///
/// **保留 AI 的接口地址、Key、模型名** —— 那几个填一次很麻烦，
/// 而且不属于"随手会改乱的参数"。要和设置界面里的「恢复默认值」保持一致。
static int cmdResetSettings()
{
    const Settings old = Settings::load();
    Settings def;                        // 全部取结构体默认值
    def.aiBaseUrl = old.aiBaseUrl;       // 这三个保留
    def.aiApiKey = old.aiApiKey;
    def.aiModel = old.aiModel;

    if (!def.save()) {
        qCritical().noquote() << QStringLiteral("写入失败：%1").arg(Settings::configPath());
        return 4;
    }

    qInfo().noquote() << QStringLiteral("已恢复默认设置：%1").arg(Settings::configPath());
    qInfo().noquote() << QStringLiteral("  目标软件      %1")
                             .arg(def.targetProcess.isEmpty()
                                      ? QStringLiteral("自动识别（默认）")
                                      : def.targetProcess);
    qInfo().noquote() << QStringLiteral("  采样间隔      %1 / %2 毫秒")
                             .arg(def.idleIntervalMs).arg(def.activeIntervalMs);
    qInfo().noquote() << QStringLiteral("  裁切比例      上%1% 下%2% 左%3% 右%4%")
                             .arg(def.cropTopPct).arg(def.cropBottomPct)
                             .arg(def.cropLeftPct).arg(def.cropRightPct);
    if (!def.aiApiKey.isEmpty()) {
        qInfo().noquote() << QStringLiteral("  AI 配置已保留（Key %1 位）")
                                 .arg(def.aiApiKey.size());
    }
    return 0;
}

// ------------------------------------------------------------------ 设置界面自检

/// 直接把设置对话框打开，方便在开了 Windows 缩放的机器上验证界面。
/// 几秒后自动退出，也可以手动关掉对话框。
static int cmdShowSettings()
{
    TrayApp tray;
    if (!tray.setup()) {
        qCritical().noquote() << "托盘初始化失败，无法显示设置界面";
        return 1;
    }

    // 先让托盘图标出来，再弹对话框
    QTimer::singleShot(400, &tray, [&tray]() { tray.showSettings(); });
    // 兜底：万一没人关它，两分钟后自己退出，免得留下一个僵尸进程
    QTimer::singleShot(120000, qApp, &QCoreApplication::quit);

    qInfo().noquote() << "设置界面已打开。检查内容是否完整、能否滚动、能否拖右下角缩放。";
    return qApp->exec();
}

// ------------------------------------------------------------------ 补跑定时整理

/// 把「还没整理过」的会话整理一遍。
///
/// 定时整理走的是同一套逻辑（findPendingSessions + organizeSession +
/// markOrganized），这里只是把它做成可以手动触发、可以直接看到结果的形式 ——
/// 否则"定时到底有没有生效"只能靠等，没法验证。
/// 老师那边如果错过了定时点，也可以跑这个补一次。
static int cmdOrganizePending(const Settings &s)
{
    // 和 --classify、托盘菜单走同一个函数 —— 这三条路以前各写了一份
    // 循环和标记逻辑，正是"某些入口会重复整理"的来源。
    const OrganizeBatchResult batch = organizePendingSessions(
        s.effectiveOutputDir(), s.effectiveArchiveDir(), false,
        [](const QString &sessionName, int index, int count,
           const OrganizeProgress &p) {
            qInfo().noquote() << QStringLiteral("[%1/%2] %3  %4/%5  %6")
                                     .arg(index)
                                     .arg(count)
                                     .arg(sessionName)
                                     .arg(p.done)
                                     .arg(p.total)
                                     .arg(p.message);
        });

    if (batch.sessions == 0) {
        qInfo().noquote() << QStringLiteral("没有待整理的录制记录。");
        qInfo().noquote()
            << QStringLiteral("（有关键帧、且还没整理过的会话才会被处理）");
        return 0;
    }

    printBatchStats(batch);
    qInfo().noquote() << QStringLiteral("归档位置：%1").arg(s.effectiveArchiveDir());
    return batch.sessionsFailed > 0 ? 1 : 0;
}

// ------------------------------------------------------------------ 鼠标钩子延迟实测

/// 量出「钩子给系统鼠标加了多少延迟」。
///
/// 为什么专门做这个：老师反馈"点了开始截图以后鼠标有延迟"。
/// 低层钩子的机制是 —— Windows 把鼠标事件投递到安装钩子的线程，
/// **在钩子返回之前，不把输入交给任何程序**。
/// 所以那个线程必须及时取消息；晚一点取，全系统的鼠标就跟着晚一点动。
///
/// 这个命令会持续制造鼠标移动，然后报告延迟分布。
/// **目标是 0 毫秒级**；到了几毫秒，老师就能感觉出来。
static int cmdHookBench(int seconds)
{
    MouseHook hook;
    if (!hook.install()) {
        qCritical().noquote()
            << "鼠标钩子装不上（可能被杀毒软件拦了）。程序会自动退化为纯轮询，功能不受影响。";
        return 1;
    }
    qInfo().noquote() << "钩子已装好，开始采样" << seconds << "秒…";
    qInfo().noquote() << "（期间会轻微移动鼠标，属正常现象）";

    hook.startLatencyProbe();

    // 造鼠标事件。用 SendInput，和真实鼠标走的是同一条路径。
    const qint64 endAt = QDateTime::currentMSecsSinceEpoch() + qint64(seconds) * 1000;
    int sent = 0;
    while (QDateTime::currentMSecsSinceEpoch() < endAt) {
        // 先打时间戳再发事件，回调里和它相减 = 钩子让鼠标多等的时间
        hook.markProbeSent(qint64(GetTickCount64()));

        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        in.mi.dx = (sent % 2) ? 1 : -1;   // 左右各挪 1 像素，不干扰使用
        in.mi.dy = 0;
        SendInput(1, &in, sizeof(INPUT));
        ++sent;
        QThread::msleep(20);
    }
    hook.markProbeSent(0);

    QThread::msleep(300);   // 等最后几个回调跑完
    const HookLatencyStats st = hook.takeLatencyStats();
    hook.uninstall();

    qInfo().noquote() << "";
    qInfo().noquote() << QStringLiteral("造了 %1 次鼠标事件，收到 %2 个样本")
                             .arg(sent).arg(st.samples);
    if (st.samples == 0) {
        qWarning().noquote() << "一个样本都没收到 —— 钩子可能没真正生效";
        return 1;
    }
    qInfo().noquote() << QStringLiteral("钩子额外增加的延迟："
                                        "平均 %1 ms ｜ 中位 %2 ms ｜ 95% %3 ms ｜ 最差 %4 ms")
                             .arg(st.avgMs, 0, 'f', 2)
                             .arg(st.p50Ms, 0, 'f', 2)
                             .arg(st.p95Ms, 0, 'f', 2)
                             .arg(st.worstMs, 0, 'f', 2);
    qInfo().noquote() << "";
    if (st.p95Ms < 1.0) {
        qInfo().noquote() << "结论：几乎无感。鼠标不会有可察觉的延迟。";
    } else if (st.p95Ms < 4.0) {
        qInfo().noquote() << "结论：轻微。多数人感觉不出来，但可以再优化。";
    } else {
        qWarning().noquote()
            << "结论：**明显延迟**。老师能感觉出来，需要继续优化。";
    }
    return 0;
}

// ------------------------------------------------------------------ 入口

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("BoardMark"));
    QApplication::setOrganizationName(QStringLiteral("BoardMark"));
    // 关掉对话框时不要把程序退出掉 —— 它是托盘程序
    QApplication::setQuitOnLastWindowClosed(false);

    // Material 3 亮色外观。必须在 QApplication 构造之后、任何界面创建之前调。
    applyMaterialTheme();

    setupConsole();
    qInstallMessageHandler(utf8MessageHandler);

    const QString dpiMode = WindowDetector::enableDpiAwareness();

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("BoardMark —— 课堂板书自动记录"));
    parser.addHelpOption();
    QCommandLineOption optInfo(QStringLiteral("info"),
        QStringLiteral("打印窗口信息后退出"));
    QCommandLineOption optGrab(QStringLiteral("grab"),
        QStringLiteral("连续抓 N 帧做诊断后退出"), QStringLiteral("N"));
    QCommandLineOption optOut(QStringLiteral("out"),
        QStringLiteral("诊断抓帧的输出目录"), QStringLiteral("DIR"),
        QStringLiteral("grabs"));
    QCommandLineOption optInterval(QStringLiteral("interval"),
        QStringLiteral("诊断抓帧的间隔毫秒"), QStringLiteral("MS"),
        QStringLiteral("500"));
    QCommandLineOption optFullScreen(QStringLiteral("fullscreen"),
        QStringLiteral("配合 --grab，抓整个屏幕"));
    QCommandLineOption optRegion(QStringLiteral("region"),
        QStringLiteral("配合 --grab，只抓左上角 WxH 区域（测带宽用）"),
        QStringLiteral("WxH"));
    QCommandLineOption optAnalyze(QStringLiteral("analyze"),
        QStringLiteral("离线回放一个截图目录，打印关键帧判定结果"),
        QStringLiteral("DIR"));
    QCommandLineOption optRecord(QStringLiteral("record"),
        QStringLiteral("无界面跑采集引擎 N 秒后收尾（验证完整链路）"),
        QStringLiteral("N"));
    QCommandLineOption optNoForeground(QStringLiteral("no-foreground"),
        QStringLiteral("配合 --record：不要求目标窗口在前台"));
    QCommandLineOption optWatch(QStringLiteral("watch"),
        QStringLiteral("监视目标进程窗口变化 N 秒（排查弹窗误截图）"),
        QStringLiteral("N"));
    QCommandLineOption optClassify(QStringLiteral("classify"),
        QStringLiteral("课后 AI 整理：整理所有还没整理过的会话（已整理的自动跳过）"));
    // 注意选项名**不能叫 --session**：
    // Qt 的 QApplication 构造函数会预先吃掉它自己认识的参数，而
    // `-session` / `--session` 正是 Qt 的会话管理选项 —— Qt 会在把参数交给
    // 我们之前，把 `--session` **连同它的值一起删掉**。
    // 症状极其难查：程序收到的参数凭空少两个，而且 `--session=X` 形式却正常
    // （Qt 只做精确匹配）。实测在这个坑上花了不少时间。
    QCommandLineOption optSession(QStringLiteral("session-dir"),
        QStringLiteral("配合 --classify，只整理指定的这一个会话（默认整理所有未整理的）"),
        QStringLiteral("DIR"));
    QCommandLineOption optArchive(QStringLiteral("archive"),
        QStringLiteral("配合 --classify，指定归档目录"),
        QStringLiteral("DIR"));
    QCommandLineOption optReclassify(QStringLiteral("reclassify"),
        QStringLiteral("配合 --classify，忽略缓存重新调用模型"));
    QCommandLineOption optAiTest(QStringLiteral("ai-test"),
        QStringLiteral("自检视觉模型：把一张图发过去，报告能不能用"),
        QStringLiteral("IMAGE"), QString());
    QCommandLineOption optNoteOrganize(QStringLiteral("organize-note"),
        QStringLiteral("整理平铺的截图文件夹：按日期/学科归类并重命名"),
        QStringLiteral("SRC"));
    QCommandLineOption optNoteOut(QStringLiteral("note-out"),
        QStringLiteral("配合 --organize-note，指定输出目录"),
        QStringLiteral("DIR"));
    QCommandLineOption optShowSettings(QStringLiteral("show-settings"),
        QStringLiteral("直接打开设置界面（在开了缩放的机器上验证界面）"));
    QCommandLineOption optResetSettings(QStringLiteral("reset-settings"),
        QStringLiteral("把所有参数恢复成出厂默认（保留 AI 的 Key 和接口地址）"));
    QCommandLineOption optOrganizePending(QStringLiteral("organize-pending"),
        QStringLiteral("把还没整理过的录制记录整理一遍（定时整理的手动版）"));
    QCommandLineOption optHookBench(QStringLiteral("hook-bench"),
        QStringLiteral("实测鼠标钩子给系统加了多少延迟"),
        QStringLiteral("SECONDS"), QStringLiteral("8"));
    parser.addOption(optInfo);
    parser.addOption(optGrab);
    parser.addOption(optOut);
    parser.addOption(optInterval);
    parser.addOption(optFullScreen);
    parser.addOption(optRegion);
    parser.addOption(optAnalyze);
    parser.addOption(optRecord);
    parser.addOption(optNoForeground);
    parser.addOption(optWatch);
    parser.addOption(optClassify);
    parser.addOption(optSession);
    parser.addOption(optArchive);
    parser.addOption(optReclassify);
    parser.addOption(optAiTest);
    parser.addOption(optNoteOrganize);
    parser.addOption(optNoteOut);
    parser.addOption(optShowSettings);
    parser.addOption(optResetSettings);
    parser.addOption(optOrganizePending);
    parser.addOption(optHookBench);
    parser.process(app);

    if (parser.isSet(optOrganizePending)) {
        return cmdOrganizePending(Settings::load());
    }

    if (parser.isSet(optHookBench)) {
        return cmdHookBench(qMax(1, parser.value(optHookBench).toInt()));
    }

    if (parser.isSet(optResetSettings)) {
        return cmdResetSettings();
    }

    if (parser.isSet(optShowSettings)) {
        return cmdShowSettings();
    }

    if (parser.isSet(optNoteOrganize)) {
        return cmdNoteOrganize(parser.value(optNoteOrganize),
                               parser.value(optNoteOut),
                               parser.isSet(optReclassify), Settings::load());
    }
    if (parser.isSet(optAiTest)) {
        return cmdAiTest(parser.value(optAiTest), Settings::load());
    }

    if (parser.isSet(optClassify)) {
        return cmdClassify(parser.value(optSession), parser.value(optArchive),
                           parser.isSet(optReclassify), Settings::load());
    }

    if (parser.isSet(optWatch)) {
        return cmdWatch(qMax(1, parser.value(optWatch).toInt()), Settings::load());
    }

    if (parser.isSet(optRecord)) {
        Settings s = Settings::load();
        if (parser.isSet(optNoForeground)) {
            s.requireForeground = false;
        }
        return cmdRecord(qMax(1, parser.value(optRecord).toInt()), s);
    }
    if (parser.isSet(optAnalyze)) {
        return cmdAnalyze(parser.value(optAnalyze), Settings::load());
    }

    QSize region;
    if (parser.isSet(optRegion)) {
        const QStringList parts = parser.value(optRegion).split(QLatin1Char('x'));
        if (parts.size() == 2) {
            region = QSize(parts[0].toInt(), parts[1].toInt());
        }
    }

    if (parser.isSet(optGrab)) {
        const int n = parser.value(optGrab).toInt();
        if (n <= 0) {
            qCritical().noquote() << "--grab 需要一个正整数";
            return 2;
        }
        return cmdGrab(n, parser.value(optInterval).toInt(),
                       parser.value(optOut), parser.isSet(optFullScreen), region);
    }
    if (parser.isSet(optInfo)) {
        qInfo().noquote() << QStringLiteral("Qt %1，DPI：%2").arg(QT_VERSION_STR, dpiMode);
        return cmdInfo();
    }

    // ------------------------------------------------------ 托盘模式
    if (!acquireSingleInstance()) {
        QMessageBox::information(
            nullptr, QStringLiteral("BoardMark"),
            QStringLiteral("程序已经在运行了，请看屏幕右下角的托盘图标。"));
        return 0;
    }

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(
            nullptr, QStringLiteral("BoardMark"),
            QStringLiteral("系统托盘不可用，程序无法在后台运行。"));
        return 1;
    }

    // 托盘模式下把控制台窗口藏起来。
    //
    // 这不是为了好看 —— 屏幕抓取抓的是「屏幕在该窗口矩形内显示的内容」，
    // 任何盖在白板上方的窗口都会被一起抓下来。实测就有一次把程序的
    // 控制台黑框连同板书一起存进了关键帧。
    //
    // 保留控制台子系统（命令行诊断照常能用），只是把窗口隐藏；
    // 日志改写到文件，信息不丢。
    hideOwnConsole();
    startFileLogging();

    // 改名迁移（ClassMind -> BoardMark，2026-10-08）。
    //
    // **必须在 refreshAutoStartPath() 之前**：迁移会把"没有新项"变成"有新项"，
    // 之后 refresh 才有机会纠正可能过期的路径。
    //
    // 这一步不是"清掉旧项"那么简单 —— 开机自启的状态**只存在注册表里**
    // （不存 settings.json），所以旧项是"老师想要开机自启"的唯一证据。
    // 只删不建会把老师的开机自启**静默关掉**：他以为程序还跟着开机跑，
    // 实际上一整节课都没录到。见 autostart.h 里 migrateLegacyAutoStart 的说明。
    const LegacyAutoStartResult legacyAuto = migrateLegacyAutoStart();

    // 开机自启动记的是绝对路径。老师把整个文件夹挪过位置之后，注册表里那条
    // 还是旧路径，开机就会启动失败。**只在已经开着的时候**顺手改过来；
    // 关着就绝不去打开它（老师可能在任务管理器里特意关掉了）。
    refreshAutoStartPath();

    // 旧版互斥体名变了，新旧两版互不识别，所以查一下旧版是不是还在跑。
    const bool oldVersionAlive = legacyInstanceRunning();

    TrayApp tray;
    if (!tray.setup()) {
        QMessageBox::critical(nullptr, QStringLiteral("BoardMark"),
                              QStringLiteral("托盘初始化失败。"));
        return 1;
    }

    // 开机自启是被**继承**过来的 —— 老师什么都没做，状态却跟以前一样，
    // 但这件事他不知情。所以要说一声（托盘气泡，不打断）。
    if (legacyAuto == LegacyAutoStartResult::Migrated
        || legacyAuto == LegacyAutoStartResult::Failed) {
        const bool migrated = (legacyAuto == LegacyAutoStartResult::Migrated);
        QTimer::singleShot(2500, &tray, [&tray, migrated]() {
            if (migrated) {
                tray.notifyMessage(
                    QStringLiteral("BoardMark"),
                    QStringLiteral("已把「开机自动启动」从旧版本 ClassMind 继承过来，"
                                   "行为不变。\n不想开机自启的话，在设置里关掉即可。"));
            } else {
                tray.notifyMessage(
                    QStringLiteral("BoardMark"),
                    QStringLiteral("改名时没能改写注册表里的开机自启动项，"
                                   "可能被组策略或安全软件拦住了。\n"
                                   "请打开设置，把「开机自动启动」重新打开一次。"));
            }
        });
    }

    // 两个程序同时录屏会存出两份重复的会话，所以要明确提示一次。
    // 用托盘气泡而不是模态框 —— 这只是过渡期的一次性提示，
    // 不该每次开机都拦住老师。
    if (oldVersionAlive) {
        qWarning() << "检测到旧版本 ClassMind 仍在运行 —— 两个程序可能同时录屏";
        QTimer::singleShot(3000, &tray, []() {
            QMessageBox::warning(
                nullptr, QStringLiteral("BoardMark"),
                QStringLiteral(
                    "检测到**旧版本的 ClassMind 还在运行**。\n\n"
                    "改名之后新旧两版互不识别，两个程序会**同时录屏**，\n"
                    "存出两份重复的录像。\n\n"
                    "建议：在任务栏右键退出旧的 ClassMind，或者用任务管理器\n"
                    "结束它，然后只保留 BoardMark。\n\n"
                    "（旧的注册表启动项已经迁移到 BoardMark，"
                    "下次开机不会再启动旧的 ClassMind。）"));
        });
    }

    // 稍等一会儿再自动开始，让托盘图标先显示出来
    QTimer::singleShot(800, &tray, &TrayApp::maybeAutoStart);

    qInfo().noquote() << "BoardMark 已启动，请在屏幕右下角托盘区找到图标。";
    qInfo().noquote() << QStringLiteral("程序目录：%1")
                             .arg(QDir::toNativeSeparators(
                                 QCoreApplication::applicationDirPath()));

    return app.exec();
}
