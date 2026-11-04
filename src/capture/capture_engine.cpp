#include "capture/capture_engine.h"

#include "capture/screen_capture.h"
#include "capture/window_detector.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>

namespace cm {

CaptureEngine::CaptureEngine(const Settings &settings, QObject *parent)
    : QThread(parent), m_settings(settings)
{
    m_params.replaceThreshold = m_settings.replaceThreshold;
    m_params.moveThreshold = m_settings.moveThreshold;
    m_params.movePercent = m_settings.movePercent;
    m_params.sameThreshold = m_settings.sameThreshold;
    m_params.maxSpanSeconds = m_settings.maxSpanSeconds;
    m_params.emptyInk = float(m_settings.emptyInk);
    m_params.panelThreshold = float(m_settings.panelThreshold);
}

CaptureEngine::~CaptureEngine()
{
    requestStop();
    wait(3000);
}

bool CaptureEngine::grabOnce(Frame &out)
{
    const QImage img = ScreenCapture::grab(m_rect);
    if (img.isNull()) {
        return false;
    }
    out.img = img;
    // 只分析裁掉固定 UI 之后的那一块；保存的仍然是完整原图。
    // 裁切比例优先用**本机标定**出来的（见 analysisCropFor）——
    // 它是在这台机器、这个 DPI 缩放下实测的，比全局默认贴合。
    out.gray = FrameAnalyzer::toGray(img, 480,
                                     m_settings.analysisCropFor(m_targetProcess,
                                                                img.size()));
    out.ms = QDateTime::currentMSecsSinceEpoch();
    return out.valid();
}

void CaptureEngine::flushCandidate(const QString &reason)
{
    if (!m_candidate.valid()) {
        return;
    }
    // 把候选帧交出去之后就清掉，这样重复调用不会重复存
    Frame frame = m_candidate;
    m_candidate = Frame();

    // 空白板没有保存价值
    if (frame.gray.ink() < m_params.emptyInk) {
        return;
    }
    // 和上一张已保存的几乎一样就不重复存
    if (m_lastSaved.valid()
        && FrameAnalyzer::similarity(m_lastSaved, frame.gray)
               > m_params.duplicateScore) {
        return;
    }
    if (m_session.saveFrame(frame.img, reason)) {
        m_lastSaved = frame.gray;
        m_lastSaveMs = QDateTime::currentMSecsSinceEpoch();
        m_spanStartMs = m_lastSaveMs;
        emit frameSaved(m_session.dir(), reason);
        emit statusMessage(QStringLiteral("已保存 %1 张 · %2")
                               .arg(m_session.frameCount())
                               .arg(reason));
    }
}

void CaptureEngine::run()
{
    // ---------------------------------------------------------- 准备
    const QString outRoot = m_settings.effectiveOutputDir();
    if (!m_session.start(outRoot, m_settings.targetProcess)) {
        emit failed(QStringLiteral("无法创建会话目录：%1").arg(outRoot));
        return;
    }
    emit sessionStarted(m_session.dir());

    // 鼠标钩子失败不致命，退化为纯轮询即可
    if (m_hook.install()) {
        emit statusMessage(QStringLiteral("鼠标钩子已启用（省电模式）"));
    } else {
        emit statusMessage(QStringLiteral("鼠标钩子不可用，使用轮询采样"));
    }

    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    m_spanStartMs = startMs;
    qint64 lastWindowCheckMs = 0;
    qint64 lastDiagMs = startMs;
    int samples = 0;

    // ---------------------------------------------------------- 主循环
    while (!m_stop.load()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        // 每 2 秒重新定位一次窗口：老师可能改了窗口大小或重启了白板
        if (!m_hwnd || now - lastWindowCheckMs > 2000) {
            lastWindowCheckMs = now;

            // 只枚举一次 —— 同一批窗口要按"有标题/无标题"筛两次，
            // 重复枚举纯属浪费（枚举是系统调用，几十个窗口就是几十次）。
            const auto allVisible = WindowDetector::enumerate(false);
            const auto allTitled = WindowDetector::enumerate(true);

            // ---- 先决定监控哪个进程 ----
            //
            // 优先级（关键：**"老师在用哪个" = 哪个在前台**）：
            //   ① 有**别的**白板正处在前台 -> 切过去
            //   ② 当前目标没窗口了        -> 退回到识别结果
            //   ③ 否则                    -> 沿用当前目标（粘性，避免来回切）
            //
            // 为什么①必须单独有一条：**旧白板往往根本没关。**
            // 老师上完"希沃白板5"直接打开"希沃轻白板"接着用，两个进程都有窗口，
            // 只看"当前目标还有没有窗口"是发现不了换课的 —— 会一直盯着后台那个。
            // 实测就是这个场景能做到自动切换。
            auto winsOf = [&](const QString &proc, bool titled) {
                const auto &src = titled ? allTitled : allVisible;
                return WindowDetector::filterByProcess(src, {proc});
            };

            if (m_targetProcess.isEmpty()) {
                m_targetProcess = m_settings.targetProcess;
            }

            const WhiteboardDetection det = detectWhiteboard(allVisible);
            const bool detUsable =
                det.ok() && !winsOf(det.processName, true).empty();
            const bool detIsOther =
                detUsable
                && det.processName.compare(m_targetProcess, Qt::CaseInsensitive)
                       != 0;

            auto wins = m_targetProcess.isEmpty()
                            ? std::vector<WindowInfo>()
                            : winsOf(m_targetProcess, true);

            // ① 别的白板在前台 -> 换课了，切过去
            // ② 当前目标没窗口了 -> 用识别结果兜底
            const bool switchToDetected =
                detIsOther && (det.foreground || wins.empty());
            if (switchToDetected) {
                // 换白板 = 换课。旧白板上的最后一屏马上就要看不到了，
                // 先落盘再切 —— 不然换课那一刻的板书会被静默丢掉。
                flushCandidate(QStringLiteral("切换到别的白板软件，保存上一屏"));
                qInfo() << "切换白板软件:" << det.displayName << det.processName
                        << "（依据" << det.reason << "）";
                emit statusMessage(
                    QStringLiteral("已切换到：%1%2")
                        .arg(det.displayName,
                             det.foreground ? QStringLiteral("（前台正在使用）")
                                            : QString()));
                m_targetProcess = det.processName;
                wins = winsOf(m_targetProcess, true);
                // 换了监控目标，一切从头开始
                m_hwnd = nullptr;
                m_candidate = Frame();
                m_lastSaved = GrayFrame();
                m_lastSaveMs = 0;
            } else if (m_targetProcess.isEmpty() && detUsable) {
                // ③ 还没有任何目标，识别到一个就用
                qInfo() << "自动识别到白板软件:" << det.displayName
                        << det.processName << "（依据" << det.reason << "）";
                emit statusMessage(QStringLiteral("已自动识别：%1")
                                       .arg(det.displayName));
                m_targetProcess = det.processName;
                wins = winsOf(m_targetProcess, true);
            }

            const auto allWins = m_targetProcess.isEmpty()
                                     ? std::vector<WindowInfo>()
                                     : winsOf(m_targetProcess, false);

            // 先挑出候选目标（已认定就沿用，否则挑最大的）
            const WindowInfo *best = WindowDetector::pickSticky(wins, m_hwnd);
            if (!best) {
                if (m_pausedReason != QLatin1String("notfound")) {
                    m_pausedReason = QStringLiteral("notfound");
                    emit statusMessage(
                        m_settings.targetProcess.isEmpty()
                            ? QStringLiteral("没找到白板软件，暂停记录"
                                             "（可在设置里手动指定）")
                            : QStringLiteral("找不到 %1，暂停记录")
                                  .arg(m_settings.targetProcess));
                }
                m_hwnd = nullptr;
                // 白板窗口没了 = 老师关了白板/下课了。**板书内容马上就要随着
                // 白板关闭永远消失**，而候选帧只存在内存里 —— 先把最后那一屏
                // 落盘再暂停。不然老师不点"停止截图"就直接关白板，写满的
                // 最后一页就再也拿不回来了。
                flushCandidate(QStringLiteral("白板窗口已关闭，保存最后一屏"));
                QThread::msleep(1000);
                continue;
            }
            if (best->minimized || !best->rect.isValid()) {
                if (m_pausedReason != QLatin1String("minimized")) {
                    m_pausedReason = QStringLiteral("minimized");
                    emit statusMessage(QStringLiteral("%1 已最小化，暂停记录")
                                           .arg(m_settings.targetProcess));
                }
                m_hwnd = nullptr;
                // 最小化之后抓不到画面了 —— 同样是"这一屏马上拿不到"，
                // 候选帧（最后一屏）先存下来。老师最小化白板然后关机是常见操作。
                flushCandidate(QStringLiteral("白板已最小化，保存最后一屏"));
                QThread::msleep(1000);
                continue;
            }

            // ---- 闸门：目标进程除了白板还弹出别的窗口时，整个跳过 ----
            //
            // 登录窗、加载窗、图形选择面板都会以「同进程的另一个窗口」出现。
            // 这时画面的任何变化都不代表板书变化，必须整个跳过 ——
            // 既不能采样，更不能把弹窗更新成候选帧。
            //
            // 注意要**排除掉候选目标本身**再数，否则目标自己会被算成
            // "额外窗口"，导致一个窗口都锁不上。
            if (m_settings.pauseOnExtraWindows) {
                const int others = WindowDetector::countSubstantialOthers(
                    allWins, best->hwnd, m_settings.extraWindowMinW,
                    m_settings.extraWindowMinH);
                if (others > 0) {
                    if (m_pausedReason != QLatin1String("dialog")) {
                        m_pausedReason = QStringLiteral("dialog");
                        emit statusMessage(
                            QStringLiteral("检测到弹窗（另有 %1 个窗口），暂停记录")
                                .arg(others));
                    }
                    m_hook.takeClickFlag();
                    QThread::msleep(500);
                    continue;
                }
            }

            // ---- 认定目标 ----
            if (m_hwnd != best->hwnd) {
                // 换了监控目标 —— 一切从头开始。
                // 这一句是防住「启动画面被存下来」的关键：
                // 不清空候选帧的话，新窗口的第一帧会和启动画面一比，
                // 判定成「大面积替换」，于是把启动画面当成一屏板书存了。
                const bool wasMonitoring = (m_hwnd != nullptr);
                m_hwnd = best->hwnd;
                m_rect = best->rect;
                m_candidate = Frame();
                m_lastSaved = GrayFrame();
                m_lastSaveMs = 0;
                m_spanStartMs = now;
                m_pausedReason.clear();
                // 换窗口/换软件就重新标定 —— 不同白板的工具栏完全不一样。
                // 已经标定过的进程不再重复标定（结果存在 settings.json 里）。
                m_calibrator.reset();
                m_calibrationActive =
                    !m_settings.calibrationFor(m_targetProcess).calibrated;
                if (m_calibrationActive) {
                    emit statusMessage(
                        QStringLiteral("正在自动标定 %1 的工具栏位置…")
                            .arg(m_targetProcess));
                }

                qInfo() << (wasMonitoring ? "切换监控窗口" : "开始监控窗口")
                        << best->title << best->className
                        << best->rect.width() << "x" << best->rect.height();
                emit statusMessage(QStringLiteral("%1：%2（%3x%4）")
                                       .arg(wasMonitoring ? QStringLiteral("已切换窗口")
                                                          : QStringLiteral("已找到窗口"),
                                            best->title)
                                       .arg(best->rect.width())
                                       .arg(best->rect.height()));
            } else {
                m_rect = best->rect;
                if (m_pausedReason == QLatin1String("dialog")) {
                    m_pausedReason.clear();
                }
            }
        }

        // 被别的窗口盖住时不要采：否则会把通知弹窗、聊天窗口写进板书
        if (m_settings.requireForeground && !WindowDetector::isForeground(m_hwnd)) {
            if (m_pausedReason != QLatin1String("occluded")) {
                m_pausedReason = QStringLiteral("occluded");
                emit statusMessage(QStringLiteral("目标窗口不在前台，暂停记录"));
            }
            m_hook.takeClickFlag();
            QThread::msleep(300);
            continue;
        }

        // ------------------------------------------------------ 采一帧
        QElapsedTimer cost;
        cost.start();

        Frame cur;
        if (!grabOnce(cur)) {
            QThread::msleep(300);
            continue;
        }

        // ------------------------------------------------ 白板裁切自动标定
        //
        // 裁切比例跟 DPI 缩放和分辨率走，写死任何一个在别的机器上都是错的，
        // 所以这里在录制过程中现测（见 BoardCalibrator）。
        //
        // 只在**该进程还没标定过**时跑，标定一次就永久生效（存进 settings.json）。
        // 标定输入必须是**未裁切**的画面 —— 标定要干的正是"找出该裁哪里"。
        if (m_calibrationActive && !m_calibrator.ready()) {
            const RowColProfile prof = FrameAnalyzer::rowColProfile(cur.img, 480);
            if (prof.valid()) {
                m_calibrator.feed(prof.rows, prof.srcH);
                if (m_calibrator.ready()) {
                    BoardCalibration cal = m_calibrator.result();
                    if (cal.calibrated) {
                        // 记下当时认出的软件名，设置界面和 --info 里要显示
                        cal.displayName = brandDisplayName(m_targetProcess);
                        m_settings.setCalibration(m_targetProcess, cal);
                        m_settings.save();
                        emit statusMessage(
                            QStringLiteral("已自动标定裁切：上%1% 下%2% 左%3% 右%4%"
                                           "（用了 %5 帧）")
                                .arg(cal.cropTopPct)
                                .arg(cal.cropBottomPct)
                                .arg(cal.cropLeftPct)
                                .arg(cal.cropRightPct)
                                .arg(cal.samples));
                        qInfo() << "自动标定完成:" << m_targetProcess
                                << "上" << cal.cropTopPct << "下" << cal.cropBottomPct
                                << "左" << cal.cropLeftPct << "右" << cal.cropRightPct
                                << "样本" << cal.samples;
                    } else {
                        qWarning() << "自动标定没得出可信结果，继续用全局默认裁切";
                    }
                    m_calibrationActive = false;    // 不管成没成，都别再占开销
                }
            }
        }

        // 记录这一轮的实际开销，用来做自适应节流
        const qint64 costMs = cost.elapsed();
        m_avgCostMs = (m_avgCostMs <= 0.0)
                          ? double(costMs)
                          : (m_avgCostMs * 0.8 + double(costMs) * 0.2);
        ++samples;

        // 每 5 秒报一次内部状态，方便排查「为什么一张都没存」
        if (now - lastDiagMs >= 5000) {
            lastDiagMs = now;
            emit statusMessage(
                QStringLiteral("已采样 %1 帧｜单帧 %2 ms｜当前墨量 %3｜候选墨量 %4")
                    .arg(samples)
                    .arg(m_avgCostMs, 0, 'f', 1)
                    .arg(cur.gray.ink(), 0, 'f', 2)
                    .arg(m_candidate.valid() ? m_candidate.gray.ink() : -1.0, 0, 'f', 2));
        }

        bool changedNow = false;

        // 当前这一帧里有大片 UI 面板（图形选择、更多菜单、登录框）→ 整个跳过。
        //
        // 关键在于**不能让它成为候选帧**：候选帧是「变化发生前的那一屏」，
        // 一旦被面板顶替，面板关掉时就会把「面板那一屏」当成一板书存下来 ——
        // 这正是实测中「添加爱心时截了一堆废图」的主要来源。
        //
        // 用画面形态（大块纯色）判断，而不是窗口枚举：这类面板不一定是独立窗口。
        if (m_settings.pauseOnPanel && cur.gray.hasPanel(float(m_settings.panelThreshold))) {
            if (m_pausedReason != QLatin1String("panel")) {
                m_pausedReason = QStringLiteral("panel");
                emit statusMessage(QStringLiteral("画面里有工具面板，暂停记录"));
            }
            m_hook.takeClickFlag();
            continue;
        }
        if (m_pausedReason == QLatin1String("panel")) {
            m_pausedReason.clear();
        }

        if (!m_candidate.valid()) {
            m_candidate = cur;
            changedNow = true;
        } else {
            const PairAnalysis a = analyzePair(m_candidate.gray, cur.gray,
                                               m_params, m_candidate.gray.h);
            const qint64 sinceSave = (m_lastSaveMs > 0)
                                         ? (now - m_lastSaveMs)
                                         : (now - startMs);
            const KeyframeDecision dec = decideKeyframe(
                m_params, a, m_candidate.gray, m_lastSaved,
                m_candidate.gray.h, now - m_spanStartMs, sinceSave);

            if (a.kind != PairKind::Same) {
                changedNow = true;
            }

            if (dec.save) {
                if (m_session.saveFrame(m_candidate.img, dec.reason)) {
                    m_lastSaved = m_candidate.gray;
                    m_lastSaveMs = now;
                    emit frameSaved(m_session.dir(), dec.reason);
                    emit statusMessage(QStringLiteral("已保存 %1 张 · %2")
                                           .arg(m_session.frameCount())
                                           .arg(dec.reason));
                }
                m_candidate = cur;
                m_spanStartMs = now;
                changedNow = true;
            } else if (dec.updateCandidate) {
                // 同一屏里继续写字：把候选更新成最新的一帧，保证内容最全
                m_candidate = cur;
            }
            // 平移期间不更新候选：否则累计位移永远归零，就判不出「老师拖走了」
        }

        if (changedNow) {
            m_activeUntilMs = now + m_settings.activeHoldMs;
        }

        // ------------------------------------------------------ 下一次采样
        const bool clicked = m_hook.takeClickFlag();
        const bool active = clicked || now < m_activeUntilMs;
        int sleepMs = active ? m_settings.activeIntervalMs : m_settings.idleIntervalMs;

        // 自适应节流：保证「抓屏开销」最多只占这个采样周期的 35%。
        // 这样无论机器多慢、分辨率多高，都不会把 CPU 吃满。
        if (m_avgCostMs > 0.0) {
            const int minInterval = int(m_avgCostMs / 0.35) + 1;
            if (sleepMs < minInterval) {
                sleepMs = minInterval;
            }
            if (sleepMs > 5000) {
                sleepMs = 5000;     // 再慢也别超过 5 秒采一次
            }
        }

        // 分片睡眠，保证停止请求能被及时响应
        while (sleepMs > 0 && !m_stop.load()) {
            const int step = qMin(sleepMs, 50);
            QThread::msleep(static_cast<unsigned long>(step));
            sleepMs -= step;
            if (m_hook.takeClickFlag()) {
                break;      // 老师点了东西，立刻醒过来看一眼
            }
        }
    }

    // ---------------------------------------------------------- 收尾
    // 退出前把「当前这一屏」也存下来，避免老师最后写的那点内容丢掉。
    // 用的是和"白板关闭/换课"同一个函数，去重和空白判断只有一份实现。
    flushCandidate(QStringLiteral("记录结束，保存当前画面"));

    const int total = m_session.frameCount();
    m_session.stop();
    emit sessionStopped(total);
}

} // namespace cm
