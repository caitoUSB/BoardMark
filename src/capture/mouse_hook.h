#pragma once

// 全局鼠标钩子：事件驱动地知道「老师点了一下」。
//
// 为什么需要它
// ------------
// 纯轮询要一直抓屏，4K 一体机上 CPU 占用可能到 20~40%，会影响老师书写流畅度。
// 而鼠标钩子在没有点击时**完全不抓屏**，占用接近 0。
//
// 钩子只负责「什么时候该看一眼」，不负责「点的哪个按钮」——
// 我们不关心按钮坐标（那种东西版本一升级就废），
// 只在点击之后确认「画面是不是真的变了」。
//
// 注意：低层钩子必须在**有消息循环的线程**里安装，所以下面这个线程类
// 负责装钩子并跑消息循环。

#include <QObject>
#include <QThread>

#include <atomic>
#include <mutex>
#include <vector>

namespace cm {

class MouseHook;

/// 钩子给系统鼠标加了多少延迟（毫秒）
struct HookLatencyStats {
    int samples = 0;
    double avgMs = 0;
    double p50Ms = 0;
    double p95Ms = 0;
    double worstMs = 0;
};

/// 钩子线程：安装 WH_MOUSE_LL 并维持消息循环
class MouseHookThread : public QThread {
public:
    explicit MouseHookThread(MouseHook *owner) : m_owner(owner) {}
    /// 请求退出，并唤醒正在等待消息的循环（实现放在 .cpp，避免头文件引入 windows.h）
    void stop();

protected:
    void run() override;

private:
    MouseHook *m_owner = nullptr;
    std::atomic<bool> m_stop{false};
    std::atomic<void *> m_stopEvent{nullptr};   ///< 停止事件句柄（HANDLE）
};

class MouseHook : public QObject {
    Q_OBJECT

public:
    explicit MouseHook(QObject *parent = nullptr);
    ~MouseHook() override;

    /// 安装钩子。返回是否成功（失败时程序应自动退化为纯轮询）。
    bool install();
    void uninstall();

    bool installed() const { return m_installed.load(); }

    /// 取出并清空「刚刚发生过左键按下」这个标志（线程安全）。
    bool takeClickFlag() { return m_clickFlag.exchange(false); }

    /// 供钩子回调调用
    void markClicked()
    {
        m_clickFlag.store(true);
        emit leftButtonPressed();
    }
    void setInstalled(bool v) { m_installed.store(v); }

    // ---- 延迟探针 ----
    //
    // 用途：量出「钩子给系统鼠标加了多少延迟」。
    // 低层钩子的机制是——Windows 把鼠标事件投递到安装钩子的线程，
    // **在钩子返回之前不把输入交给任何程序**。
    // 所以线程要是不及时取消息，每一次鼠标操作都会慢半拍。
    // 这个数字是 0 最好；上到毫秒级老师就能感觉出来。
    void startLatencyProbe() { m_probing.store(true); }
    HookLatencyStats takeLatencyStats();

    /// 探针用：记下"刚把鼠标事件发出去"的时刻。
    ///
    /// 量法说明（踩过坑，别再改回去）：
    /// 一开始想用 GetMessageTime() 拿"消息投递时刻"再和当前时刻相减，
    /// 结果量出 8.9 小时这种离谱数字 —— 因为 GetMessageTime() 返回的是
    /// **本线程最后一次取到的消息**的时间，而低层钩子的回调并不是在
    /// 我们的 PeekMessage 调用里被触发的，读到的是过期值。
    /// 改成"发送前打时间戳、回调里相减"，简单且可靠。
    void markProbeSent(qint64 tickMs) { m_probeSentTick.store(tickMs); }
    /// 取走并清空时间戳。
    /// 必须"取走就清"，否则一次发送对应多次回调（鼠标消息不止一种）时，
    /// 后面几次会拿同一个旧时间戳去减，量出越来越大的假延迟 ——
    /// 实测样本数会比发送次数多一倍，数字完全失真。
    qint64 consumeProbeSentTick()
    {
        return m_probeSentTick.exchange(0);
    }

    /// 供钩子回调调用
    void recordLatency(double ms)
    {
        if (!m_probing.load()) {
            return;
        }
        std::lock_guard<std::mutex> lk(m_latMutex);
        m_latencies.push_back(ms);
    }

signals:
    void leftButtonPressed();

private:
    std::atomic<bool> m_installed{false};
    std::atomic<bool> m_clickFlag{false};
    std::atomic<bool> m_probing{false};
    std::atomic<qint64> m_probeSentTick{0};
    std::mutex m_latMutex;
    std::vector<double> m_latencies;
    MouseHookThread *m_thread = nullptr;
};

} // namespace cm
