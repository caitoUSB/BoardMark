#include "capture/window_detector.h"

#include <QDebug>
#include <QFileInfo>

#include <vector>

namespace cm {

QString WindowDetector::enableDpiAwareness()
{
#ifdef _WIN32
    // 优先 per-monitor v2；失败则退到 system aware
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return QStringLiteral("per-monitor-v2");
    }
    if (SetProcessDPIAware()) {
        return QStringLiteral("system");
    }
#endif
    return QStringLiteral("none");
}

QString WindowDetector::processNameOf(quint32 pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        return QString();
    }
    wchar_t buf[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    QString full;
    if (QueryFullProcessImageNameW(h, 0, buf, &size)) {
        full = QString::fromWCharArray(buf);
    }
    CloseHandle(h);
    return QFileInfo(full).fileName();   // 只留 "EasiNote.exe"
}

static QString stemOf(const QString &nameOrPath)
{
    return QFileInfo(nameOrPath).completeBaseName().toLower();
}

struct EnumContext {
    bool requireTitle = true;
    std::vector<WindowInfo> *out = nullptr;
};

static BOOL CALLBACK enumProc(HWND hwnd, LPARAM lparam)
{
    auto *ctx = reinterpret_cast<EnumContext *>(lparam);

    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    const int len = GetWindowTextLengthW(hwnd);
    if (ctx->requireTitle && len <= 0) {
        return TRUE;
    }

    WindowInfo info;
    info.hwnd = hwnd;
    info.visible = true;

    if (len > 0) {
        std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(hwnd, buf.data(), len + 1);
        info.title = QString::fromWCharArray(buf.data());
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    info.pid = pid;
    info.process = WindowDetector::processNameOf(pid);

    // 类名很有用：系统对话框常见 "#32770"，加载窗、面板各有自己的类
    wchar_t cls[256] = {};
    if (GetClassNameW(hwnd, cls, 256) > 0) {
        info.className = QString::fromWCharArray(cls);
    }

    // 最小化的窗口坐标会变成 (-32000,-32000)、尺寸 0x0。
    // 这种情况绝不能当作「没找到窗口」—— 老师上课随时会把白板最小化。
    info.minimized = WindowDetector::isMinimized(hwnd);
    info.rect = info.minimized ? QRect() : WindowDetector::clientRectOnScreen(hwnd);

    ctx->out->push_back(info);
    return TRUE;
}

std::vector<WindowInfo> WindowDetector::enumerate(bool requireTitle)
{
    std::vector<WindowInfo> out;
    EnumContext ctx;
    ctx.requireTitle = requireTitle;
    ctx.out = &out;
    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

std::vector<WindowInfo> WindowDetector::filterByProcess(
    const std::vector<WindowInfo> &windows, const QStringList &processNames)
{
    QList<QString> wanted;
    for (const QString &n : processNames) {
        wanted.append(stemOf(n));
    }

    std::vector<WindowInfo> out;
    for (const WindowInfo &w : windows) {
        if (wanted.contains(stemOf(w.process))) {
            out.push_back(w);
        }
    }
    return out;
}

std::vector<WindowInfo> WindowDetector::findByProcess(const QStringList &processNames,
                                                      bool requireTitle)
{
    return filterByProcess(enumerate(requireTitle), processNames);
}

const WindowInfo *WindowDetector::pickBest(const std::vector<WindowInfo> &windows)
{
    const WindowInfo *best = nullptr;
    for (const WindowInfo &w : windows) {
        if (w.minimized || !w.rect.isValid()) {
            continue;
        }
        if (!best || w.rect.width() * w.rect.height()
                         > best->rect.width() * best->rect.height()) {
            best = &w;
        }
    }
    // 全都被最小化时，仍然返回一个，让上层能给出「请先点开希沃」的提示
    if (!best && !windows.empty()) {
        best = &windows.front();
    }
    return best;
}

const WindowInfo *WindowDetector::pickSticky(const std::vector<WindowInfo> &windows,
                                             HWND preferred)
{
    // 一旦认定了监控目标就沿用下去。
    // 这是为了防住「登录窗 / 加载窗比白板还大」导致的监控目标被抢走 ——
    // 那种情况下每次重挑都会把弹窗当成白板，于是把弹窗截下来。
    if (preferred) {
        for (const WindowInfo &w : windows) {
            if (w.hwnd == preferred && !w.minimized && w.rect.isValid()) {
                return &w;
            }
        }
    }
    return pickBest(windows);
}

int WindowDetector::countSubstantialOthers(const std::vector<WindowInfo> &windows,
                                           HWND except, int minW, int minH)
{
    int n = 0;
    for (const WindowInfo &w : windows) {
        if (w.hwnd == except) {
            continue;
        }
        if (w.substantial(minW, minH)) {
            ++n;
        }
    }
    return n;
}

bool WindowDetector::isMinimized(HWND hwnd)
{
    return IsIconic(hwnd) != FALSE;
}

bool WindowDetector::isForeground(HWND hwnd)
{
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) {
        return true;
    }
    // 希沃可能把真正的板子放在子窗口里，往上找到根窗口再比
    return GetAncestor(fg, GA_ROOT) == GetAncestor(hwnd, GA_ROOT);
}

QRect WindowDetector::clientRectOnScreen(HWND hwnd)
{
    if (isMinimized(hwnd)) {
        return QRect();
    }
    RECT rc = {};
    if (!GetClientRect(hwnd, &rc)) {
        return QRect();
    }
    POINT pt = {0, 0};
    if (!ClientToScreen(hwnd, &pt)) {
        return QRect();
    }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        return QRect();
    }
    return QRect(pt.x, pt.y, w, h);
}

} // namespace cm
