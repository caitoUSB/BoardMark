#pragma once

// 窗口探测：枚举顶层窗口、找希沃白板、判断状态。
//
// 设计要点（都是踩坑换来的）：
//   * 用「进程名去扩展名后比较」—— "EasiNote.exe" != "easinote" 这个坑踩过
//   * 最小化的窗口坐标会变成 (-32000,-32000)、尺寸 0x0，
//     不能当成「没找到窗口」，必须让上层看得见并给出提示
//   * 客户区而不是整窗 —— 标题栏和边框也是固定 UI，会污染后续的画面比对
//   * DPI 感知必须在任何 GetWindowRect 之前开启，
//     否则一体机常见的 150%/200% 缩放下坐标是错的

#include <QString>
#include <QRect>
#include <QStringList>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace cm {

struct WindowInfo {
    HWND    hwnd      = nullptr;
    quint32 pid       = 0;
    QString process;            // 例如 "EasiNote.exe"
    QString title;
    QString className;          // 窗口类名。系统对话框常见 "#32770"
    QRect   rect;               // 客户区的屏幕坐标；最小化时为空
    bool    minimized = false;
    bool    visible   = false;

    /// 「有实质尺寸」—— 排除掉 1x1 的消息窗、幽灵窗、工具提示窗。
    /// 用来判断目标进程是不是弹出了额外的对话框。
    bool substantial(int minW = 160, int minH = 120) const
    {
        return !minimized && rect.isValid()
               && rect.width() >= minW && rect.height() >= minH;
    }
};

class WindowDetector {
public:
    /// 必须在任何窗口坐标 API 之前调用一次。返回生效的模式名。
    static QString enableDpiAwareness();

    /// 枚举所有带标题的可见顶层窗口。
    static std::vector<WindowInfo> enumerate(bool requireTitle = true);

    /// 按进程名（不含扩展名，不区分大小写）筛选。
    static std::vector<WindowInfo> findByProcess(const QStringList &processNames,
                                                 bool requireTitle = true);

    /// 在**已经枚举好**的列表里按进程名筛选。
    /// 采集主循环里要同时按"有标题/无标题"筛同一批窗口，
    /// 用这个可以只枚举一次，别重复调 enumerate()。
    static std::vector<WindowInfo> filterByProcess(
        const std::vector<WindowInfo> &windows, const QStringList &processNames);

    /// 在候选里挑最适合采集的：优先非最小化、客户区面积最大的那个。
    ///
    /// 注意：**不要每次都重新挑**。登录窗、加载窗出现时可能比白板还大，
    /// 每次重挑就会把监控目标换成那个弹窗，于是把弹窗截下来。
    /// 正确做法是一旦认定就沿用（见 CaptureEngine 的 sticky 逻辑）。
    static const WindowInfo *pickBest(const std::vector<WindowInfo> &windows);

    /// 在上面的基础上优先沿用 preferred（如果它还在且可用）。
    static const WindowInfo *pickSticky(const std::vector<WindowInfo> &windows,
                                        HWND preferred);

    /// 数一数除了 except 之外，还有几个「有实质尺寸」的可见窗口。
    /// 返回值 > 0 通常意味着目标进程弹出了对话框、加载窗或面板。
    static int countSubstantialOthers(const std::vector<WindowInfo> &windows,
                                      HWND except,
                                      int minW = 160, int minH = 120);

    static bool isMinimized(HWND hwnd);

    /// 目标窗口是否真的是当前活动窗口。
    /// 判定是严格的：必须就是它本身，或者它的根窗口。
    /// 宽松判定会把「同进程的登录窗盖在白板上」当成正常状态。
    static bool isForeground(HWND hwnd);

    /// 客户区的屏幕坐标；最小化或失败时返回空 QRect。
    static QRect clientRectOnScreen(HWND hwnd);

    /// 由 PID 取进程文件名（例如 "EasiNote.exe"）。取不到返回空串。
    static QString processNameOf(quint32 pid);
};

} // namespace cm
