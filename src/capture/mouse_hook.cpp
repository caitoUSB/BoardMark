#include "capture/mouse_hook.h"

#include <QDebug>

#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace cm {

namespace {
// 低层钩子回调是 C 函数指针，拿不到 this，用文件级指针转交。
// 本程序只会存在一个鼠标钩子实例。
MouseHook *g_owner = nullptr;

LRESULT CALLBACK mouseProc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION && g_owner) {
        // 延迟探针：和"刚发出去的时刻"相减，得到钩子让鼠标多等了多久。
        // 时间戳"取走即清" —— 一次发送可能对应多个回调（鼠标消息不止一种），
        // 只算第一个，多出来的忽略，否则会量出越来越大的假延迟。
        if (const qint64 sent = g_owner->consumeProbeSentTick(); sent > 0) {
            g_owner->recordLatency(double(GetTickCount64() - ULONGLONG(sent)));
        }

        if (wparam == WM_LBUTTONDOWN) {
            g_owner->markClicked();
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
} // namespace

void MouseHookThread::stop()
{
    m_stop.store(true);
    // 唤醒正在 MsgWaitForMultipleObjects 里阻塞的循环，让它立刻退出
    if (HANDLE h = static_cast<HANDLE>(m_stopEvent.load())) {
        SetEvent(h);
    }
}

void MouseHookThread::run()
{
    // 停止信号用事件对象。
    //
    // **不要改回 PeekMessage + msleep 那种写法。**
    // WH_MOUSE_LL 的回调是靠给「安装钩子的这个线程」投递消息来触发的，
    // 线程必须及时把消息取走，回调才会跑；而在钩子返回之前，
    // **Windows 不会把鼠标输入交给任何程序**。
    // 所以"取完消息就睡 15 毫秒"= 每一次鼠标事件都要等最多 15ms 才被处理
    // —— 整台电脑的鼠标都会发卡。实测在一线教室被老师当场发现。
    //
    // MsgWaitForMultipleObjects 同时解决两件事：
    //   * 有消息 → 立刻返回，回调即时执行，鼠标零延迟
    //   * 没消息 → 真正阻塞，0% CPU（比每 15ms 醒一次还省电）
    //   * 停止事件一置位 → 也立刻返回，所以退出依然是及时的
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_stopEvent.store(stopEvent);

    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc,
                                   GetModuleHandleW(nullptr), 0);
    if (!hook) {
        qWarning() << "安装鼠标钩子失败（错误码" << GetLastError()
                   << "），将退化为纯轮询采样";
        m_owner->setInstalled(false);
        if (stopEvent) {
            CloseHandle(stopEvent);
        }
        m_stopEvent.store(nullptr);
        return;
    }

    m_owner->setInstalled(true);

    MSG msg;
    bool running = true;
    while (running && !m_stop.load()) {
        const DWORD r = MsgWaitForMultipleObjects(
            1, &stopEvent, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) {
            break;                      // 停止事件
        }
        if (r != WAIT_OBJECT_0 + 1) {
            break;                      // 出错，别死循环
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    Q_UNUSED(running);

    UnhookWindowsHookEx(hook);
    m_owner->setInstalled(false);
    if (stopEvent) {
        CloseHandle(stopEvent);
    }
    m_stopEvent.store(nullptr);
}

MouseHook::MouseHook(QObject *parent) : QObject(parent)
{
    g_owner = this;
}

MouseHook::~MouseHook()
{
    uninstall();
    if (g_owner == this) {
        g_owner = nullptr;
    }
}

bool MouseHook::install()
{
    if (m_thread) {
        return m_installed.load();
    }
    m_thread = new MouseHookThread(this);
    m_thread->start();

    for (int i = 0; i < 100 && !m_installed.load(); ++i) {
        QThread::msleep(10);
    }
    if (!m_installed.load()) {
        m_thread->stop();
        m_thread->wait(1000);
        delete m_thread;
        m_thread = nullptr;
        return false;
    }
    return true;
}

HookLatencyStats MouseHook::takeLatencyStats()
{
    std::lock_guard<std::mutex> lk(m_latMutex);
    HookLatencyStats s;
    s.samples = int(m_latencies.size());
    if (m_latencies.empty()) {
        return s;
    }

    double sum = 0;
    for (double v : m_latencies) {
        sum += v;
    }
    s.avgMs = sum / double(m_latencies.size());

    std::vector<double> sorted = m_latencies;
    std::sort(sorted.begin(), sorted.end());
    s.p50Ms = sorted[sorted.size() / 2];
    s.p95Ms = sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)];
    s.worstMs = sorted.back();

    m_latencies.clear();
    m_probing.store(false);
    return s;
}

void MouseHook::uninstall()
{
    if (!m_thread) {
        return;
    }
    m_thread->stop();
    m_thread->wait(2000);
    delete m_thread;
    m_thread = nullptr;
    m_installed.store(false);
}

} // namespace cm
