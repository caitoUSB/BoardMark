#pragma once

// 采集引擎：整个程序的心脏。
//
// 核心原则只有一句：**保存「变化发生前的那一帧」。**
// 因为那一帧正是「老师刚写满、还没挪走」的完整画面。
//
// 三种触发：
//   1. 画面整体平移        -> 存平移前那一帧（老师拖动板书）
//   2. 画面大面积替换      -> 存替换前那一帧（点加页 / 翻页 / 清屏）
//   3. 长时间只写不停      -> 强制存一张（兜底，防止内容丢失）
//
// 采样策略：自适应
//   - 画面静止时用 idleIntervalMs（慢），CPU 占用接近 0
//   - 检测到变化后用 activeIntervalMs（快），维持 activeHoldMs
//   - 鼠标钩子捕获到点击时立即采样一次，不用等定时器

#include <QImage>
#include <QThread>

#include <atomic>

#include "analysis/frame_analyzer.h"
#include "analysis/keyframe_rules.h"
#include "app/settings.h"
#include "capture/mouse_hook.h"
#include "capture/whiteboard.h"
#include "storage/session.h"

namespace cm {

class CaptureEngine : public QThread {
    Q_OBJECT

public:
    explicit CaptureEngine(const Settings &settings, QObject *parent = nullptr);
    ~CaptureEngine() override;

    void requestStop() { m_stop.store(true); }

signals:
    /// 会话已开始，参数是会话目录
    void sessionStarted(const QString &dir);
    /// 会话已结束，参数是保存的关键帧数量
    void sessionStopped(int frameCount);
    /// 状态文字，用于托盘气泡提示
    void statusMessage(const QString &text);
    /// 保存了一张关键帧
    void frameSaved(const QString &file, const QString &reason);
    /// 致命错误（例如找不到目标窗口）
    void failed(const QString &text);

protected:
    void run() override;

private:
    /// 一帧的完整信息：原始图（用于落盘）+ 分析用灰度
    struct Frame {
        QImage img;
        GrayFrame gray;
        qint64 ms = 0;
        bool valid() const { return !img.isNull() && gray.valid(); }
    };

    bool grabOnce(Frame &out);

    /// 把当前候选帧落盘（如果值得存）。
    ///
    /// **凡是"这一屏马上就拿不到了"的场合都要先调它。** 候选帧是
    /// 「老师刚写满、还没挪走」的那一屏 —— 下课那会儿它往往就是**最后一页**，
    /// 而它只存在内存里，调用方一清空就永远没了。
    ///
    /// 内部自带两道闸：墨量太低不存、和上一张已存帧几乎一样也不存。
    /// 调用后候选帧会被清空，所以重复调用不会重复存。
    void flushCandidate(const QString &reason);

    /// 当前实际在监控哪个进程 —— 可能是设置里指定的，
    /// 也可能是自动识别出来的（设置留空时）。
    QString targetProcess() const { return m_targetProcess; }

    Settings m_settings;
    KeyframeParams m_params;
    std::atomic<bool> m_stop{false};

    Session m_session;
    MouseHook m_hook;

    /// 实际监控的进程名。设置里留空时由 detectWhiteboard() 填上。
    QString m_targetProcess;

    /// 白板裁切自动标定。该进程已经标定过就不再跑。
    BoardCalibrator m_calibrator;
    bool m_calibrationActive = false;

    // 候选关键帧：上次保存之后，「最新最全」的那一屏
    Frame m_candidate;
    // 上一张已保存的帧，用来避免存出重复内容
    GrayFrame m_lastSaved;
    qint64 m_spanStartMs = 0;      ///< 当前这一屏是从什么时候开始写的
    qint64 m_activeUntilMs = 0;    ///< 到这个时刻之前都用加密采样
    qint64 m_lastSaveMs = 0;       ///< 上一张关键帧是什么时候存的
    /// 暂停原因只提示一次，避免每秒刷一条把日志淹掉
    QString m_pausedReason;

    HWND m_hwnd = nullptr;
    QRect m_rect;

    /// 抓屏 + 分析的滑动平均耗时（毫秒）。
    /// 实测 BitBlt 抓屏有一个 10~20ms 的固定开销（DWM 合成同步导致），
    /// 跟抓多大面积基本无关。所以必须按实测耗时自适应地放慢采样，
    /// 否则在慢机器或 4K 一体机上会把 CPU 吃满、影响老师书写。
    double m_avgCostMs = 0.0;
};

} // namespace cm
