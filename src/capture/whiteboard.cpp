#include "capture/whiteboard.h"

#include <QDebug>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace cm {

namespace {

/// 已知白板软件。
///
/// **加新条目之前先确认进程名，别猜。** 猜错会让程序去监控一个根本不是白板的
/// 进程，而且界面上看不出来（这个坑 README 里记过）。不确定的宁可先不加 ——
/// 老师在设置里手动选一次也能正常用，标定照样自动进行。
///
/// 目前这几条的来源：
///   EasiNote.exe     项目原有文档（settings.h 的提示、README）
///   EasiNote5C.exe   实测：希沃轻白板的窗口类名是
///                    HwndWrapper[EasiNote5C;;...]，进程名即 EasiNote5C.exe
///   HiteTouchPro.exe 项目原有文档（README「换成鸿合等其他白板软件」一节）
const std::vector<WhiteboardBrand> &brands()
{
    static const std::vector<WhiteboardBrand> kBrands = {
        {QStringLiteral("EasiNote.exe"), QStringLiteral("希沃白板 5"),
         {QStringLiteral("希沃白板"), QStringLiteral("EasiNote")}},
        {QStringLiteral("EasiNote5C.exe"), QStringLiteral("希沃轻白板"),
         {QStringLiteral("希沃轻白板")}},
        {QStringLiteral("HiteTouchPro.exe"), QStringLiteral("鸿合白板"),
         {QStringLiteral("鸿合"), QStringLiteral("HiteTouch")}},
    };
    return kBrands;
}

/// 进程名去掉扩展名、转小写 —— "EasiNote.exe" == "easinote"
QString stem(const QString &nameOrPath)
{
    return QFileInfo(nameOrPath).completeBaseName().toLower();
}

/// 在某段剖面里找"跨帧一致边缘"。
///
/// 阈值取 max(kEdgeFloor, 该段内边缘强度的 92 分位)：
/// 用相对分位是为了适应不同画面的边缘强度量级，
/// 但下限保证"这一片本来就很平坦"时不会硬凑出一个结果
/// （希沃轻白板顶部最强的"一致边缘"只有 0.67，用它当阈值就会误判出裁切）。
const float kEdgeFloor = 2.0f;

struct EdgeHit {
    bool found = false;
    int index = 0;      ///< 命中处的下标（行号或列号）
    float strength = 0.0f;
};

/// 在一个区间里找一致边缘。
/// 顶部/左侧要的是"固定块的下沿"，所以取区间内**最大**的命中下标；
/// 底部/右侧要的是"固定块的上沿"，所以取**最小**的命中下标。
EdgeHit findEdge(const std::vector<float> &d, int from, int to, bool takeLargest)
{
    EdgeHit hit;
    const int lo = std::max(0, from);
    const int hi = std::min<int>(to, int(d.size()));
    if (hi - lo < 4) {
        return hit;
    }

    std::vector<float> seg(d.begin() + lo, d.begin() + hi);
    std::vector<float> sorted(seg);
    std::sort(sorted.begin(), sorted.end());
    const int p92 = sorted[std::min<int>(int(sorted.size()) - 1,
                                         int(sorted.size()) * 92 / 100)];
    const float thr = std::max(kEdgeFloor, float(p92));

    for (int i = lo; i < hi; ++i) {
        if (d[i] > thr) {
            if (!hit.found) {
                hit.found = true;
                hit.index = i;
                hit.strength = d[i];
            } else if (takeLargest) {
                hit.index = i;
                hit.strength = d[i];
            } else {
                // 取最小下标，第一个命中就够了
                break;
            }
        }
    }
    return hit;
}

} // namespace

const std::vector<WhiteboardBrand> &knownWhiteboards()
{
    return brands();
}

QString brandDisplayName(const QString &processName)
{
    const QString s = stem(processName);
    for (const WhiteboardBrand &b : brands()) {
        if (stem(b.processName) == s) {
            return b.displayName;
        }
    }
    return QString();
}

// ------------------------------------------------------------------ 自动识别

WhiteboardDetection detectWhiteboard(const std::vector<WindowInfo> &windows)
{
    WhiteboardDetection best;
    int bestRank = -1;
    int bestBrand = 0;
    int bestArea = 0;

    for (const WindowInfo &w : windows) {
        // 最小化的窗口不参与 —— 白板最小化时压根抓不到画面
        if (w.process.isEmpty() || w.minimized || !w.rect.isValid()) {
            continue;
        }
        const bool fg = WindowDetector::isForeground(w.hwnd);

        for (const WhiteboardBrand &b : brands()) {
            int brand = 0;
            QStringList why;

            if (stem(w.process) == stem(b.processName)) {
                brand += 3;
                why << QStringLiteral("进程名 %1").arg(w.process);
            }
            for (const QString &hint : b.titleHints) {
                if (!hint.isEmpty() && w.title.contains(hint)) {
                    brand += 2;
                    why << QStringLiteral("标题含「%1」").arg(hint);
                    break;      // 同一款只算一次标题分
                }
            }
            // 品牌证据不足就不认。**不要"猜一个最大的窗口"** ——
            // 选错目标会一直记录错误的窗口，而界面上看不出来（README 记过这个坑）。
            if (brand < 2) {
                continue;
            }
            if (fg) {
                why << QStringLiteral("在前台");
            }

            // 排名：前台的一律优先，这样"后台开着旧白板、前台在用新白板"能选对。
            // 同为前台（或同为后台）时，先看品牌证据强弱，再看窗口大小
            // （白板铺满屏幕，登录窗/浮层不会）。
            const int area = w.rect.width() * w.rect.height();
            const int rank = brand + (fg ? 10 : 0);

            if (rank > bestRank
                || (rank == bestRank && brand > bestBrand)
                || (rank == bestRank && brand == bestBrand && area > bestArea)) {
                bestRank = rank;
                bestBrand = brand;
                bestArea = area;
                best.processName = b.processName;
                best.displayName = b.displayName;
                best.recognized = true;
                best.foreground = fg;
                best.reason = why.join(QStringLiteral(" + "));
            }
        }
    }

    return best;
}

// ------------------------------------------------------------------ 自动标定

void BoardCalibrator::reset()
{
    m_minRowEdge.clear();
    m_prevRows.clear();
    m_rows = 0;
    m_srcH = 0;
    m_frames = 0;
    m_changes = 0;
}

void BoardCalibrator::feed(const std::vector<float> &rows, int srcH)
{
    if (rows.size() < 32 || srcH <= 0) {
        return;
    }
    // 画面尺寸变了（老师改了分辨率/换了窗口），之前的统计作废
    if (m_srcH != srcH || m_rows != int(rows.size())) {
        reset();
        m_srcH = srcH;
        m_rows = int(rows.size());
    }

    // 空间导数：相邻行的均值差。固定 UI 的边沿在每帧都出现在同一位置。
    std::vector<float> rowEdge(m_rows > 0 ? m_rows - 1 : 0, 0.0f);
    for (size_t y = 0; y + 1 < rows.size(); ++y) {
        rowEdge[y] = std::fabs(rows[y + 1] - rows[y]);
    }

    if (m_frames == 0) {
        m_minRowEdge = rowEdge;
    } else {
        for (size_t i = 0; i < rowEdge.size() && i < m_minRowEdge.size(); ++i) {
            m_minRowEdge[i] = std::min(m_minRowEdge[i], rowEdge[i]);
        }
    }

    // 内容变过没有 —— 用来保证"跨帧最弱边缘"这件事真的成立。
    // 抓屏是无损的（BitBlt + PNG），画面完全没变时逐行差值就是 0，
    // 所以这个阈值只需要"略大于 0"，不必留噪声余量。
    if (!m_prevRows.empty() && m_prevRows.size() == rows.size()) {
        double d = 0.0;
        for (size_t i = 0; i < rows.size(); ++i) {
            d += std::fabs(double(rows[i]) - double(m_prevRows[i]));
        }
        d /= double(rows.size());
        if (d > 0.2) {
            ++m_changes;
        }
    }

    m_prevRows = rows;
    ++m_frames;
}

BoardCalibration BoardCalibrator::result() const
{
    BoardCalibration c;
    if (m_rows < 8 || m_srcH <= 0) {
        return c;
    }

    // 只在中间以外找：固定 UI 只会出现在画面的边缘。
    // 取中间 40% 以外的区域，避免把板书内容里的横线当成工具栏边沿。
    const int rowTopHi = m_rows * 20 / 100;     // 顶部 20%
    const int rowBotLo = m_rows * 60 / 100;     // 底部 40%

    // 顶部：顶部固定块（横幅之类）的**下沿** = 区间内最大的命中下标
    const EdgeHit top = findEdge(m_minRowEdge, 0, rowTopHi, true);
    // 底部：工具栏的**上沿** = 区间内最小的命中下标
    const EdgeHit bottom = findEdge(m_minRowEdge, rowBotLo, m_rows, false);

    // 行号 -> 物理像素 -> 百分比。用 ceil 向上取整：宁可多裁一点。
    auto pctRows = [&](int rowCount) {
        const int px = rowCount * m_srcH / m_rows;
        return int(std::ceil(double(px) * 100.0 / double(m_srcH)));
    };

    if (top.found) {
        c.cropTopPct = pctRows(top.index);
    }
    if (bottom.found) {
        c.cropBottomPct = pctRows(m_rows - bottom.index);
    }
    // 左右**故意保持 0**，理由见头文件"只标上下，不标左右"一节。

    // 兜底：算出来太离谱（超过 45%）说明锁到别的东西上了，宁可不用。
    // 45% 和 Settings::analysisCrop() 的上下限保持一致。
    if (c.cropTopPct > 45 || c.cropBottomPct > 45) {
        qWarning() << "自动标定结果过大，放弃本次标定："
                   << c.cropTopPct << c.cropBottomPct;
        return BoardCalibration();
    }

    c.calibrated = true;
    c.samples = m_frames;
    return c;
}

} // namespace cm
