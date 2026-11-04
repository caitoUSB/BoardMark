#include "analysis/frame_analyzer.h"

#include <algorithm>
#include <cmath>

namespace cm {

namespace {

/// 求中位数（会复制一份，调用不频繁，可接受）
float medianOf(std::vector<float> v)
{
    if (v.empty()) {
        return 0.0f;
    }
    const std::size_t n = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + n, v.end());
    return v[n];
}

} // namespace

GrayFrame FrameAnalyzer::toGray(const QImage &img, int targetW, const QRect &crop)
{
    GrayFrame g;
    if (img.isNull() || targetW <= 0) {
        return g;
    }

    QImage src = img;
    if (src.format() != QImage::Format_RGB32
        && src.format() != QImage::Format_ARGB32) {
        src = src.convertToFormat(QImage::Format_RGB32);
    }

    // 分析区域：默认整幅，指定 crop 时只取其中一块（切掉固定 UI）
    QRect r(0, 0, src.width(), src.height());
    if (!crop.isNull() && !crop.isEmpty()) {
        const QRect c = crop.intersected(r);
        if (c.width() >= 32 && c.height() >= 32) {
            r = c;
        }
    }

    const int step = std::max(1, r.width() / targetW);
    g.w = (r.width() + step - 1) / step;
    g.h = (r.height() + step - 1) / step;
    if (g.w < 8 || g.h < 8) {
        return GrayFrame();
    }

    g.pix.resize(static_cast<std::size_t>(g.w) * g.h);

    // 逐块取平均降采样（比单点抽样抗噪）
    for (int y = 0; y < g.h; ++y) {
        const int sy0 = r.top() + y * step;
        const int sy1 = std::min(sy0 + step, r.bottom() + 1);
        for (int x = 0; x < g.w; ++x) {
            const int sx0 = r.left() + x * step;
            const int sx1 = std::min(sx0 + step, r.right() + 1);

            float sum = 0.0f;
            int cnt = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(sy));
                for (int sx = sx0; sx < sx1; ++sx) {
                    const QRgb p = line[sx];
                    sum += 0.299f * qRed(p) + 0.587f * qGreen(p) + 0.114f * qBlue(p);
                    ++cnt;
                }
            }
            g.pix[static_cast<std::size_t>(y) * g.w + x] = cnt ? sum / cnt : 0.0f;
        }
    }

    // 逐行 / 逐列墨量 = 该行(列)均值相对中位数的偏离
    g.rowProfile.resize(g.h);
    std::vector<float> rows(g.h, 0.0f);
    for (int y = 0; y < g.h; ++y) {
        float s = 0.0f;
        for (int x = 0; x < g.w; ++x) {
            s += g.at(x, y);
        }
        rows[y] = s / g.w;
    }
    const float rowMed = medianOf(rows);
    for (int y = 0; y < g.h; ++y) {
        g.rowProfile[y] = std::fabs(rows[y] - rowMed);
    }

    // 墨量 = 全画面像素相对底色的平均偏离（归一化到 0..1）。
    // 底色取全画面像素的中位数 —— 黑板上白字、白板上黑字都成立，
    // 不需要预先知道是哪种板。息屏黑屏时中位数=0、偏离也是 0，墨量自然为 0。
    //
    // 用「平均偏离」而不是「超过阈值的像素占比」：分析图是 4 倍降采样的，
    // 细笔迹会被平均得很淡，硬阈值会把整片笔迹误判成背景（实测密笔画算成 0.00）。
    {
        std::vector<float> all(g.pix);
        const float bg = medianOf(all);
        double sum = 0.0;
        for (float v : g.pix) {
            sum += std::fabs(double(v) - double(bg));
        }
        g.inkLevel = float(sum / double(g.pix.size()) / 255.0);

        // 「大块纯色」检测 —— 用来识别白板软件弹的 UI 面板。
        //
        // 手写笔画只有一两个像素宽，水平/垂直各腐蚀一次就没了；
        // UI 面板几百像素宽，腐蚀完还剩一大片。用形态而不是亮度判断，
        // 所以深色黑板和白色白板一样有效。
        //
        // 用可分离腐蚀（先横后纵），每像素常数次比较，代价 O(n)。
        const int W = g.w;
        const int H = g.h;
        const int R = 2;                        // 腐蚀半径，判断「至少 5x5 的实心块」
        constexpr float kSolidDelta = 40.0f;

        std::vector<unsigned char> mask(size_t(W) * H);
        for (size_t i = 0; i < g.pix.size(); ++i) {
            mask[i] = (std::fabs(g.pix[i] - bg) > kSolidDelta) ? 1u : 0u;
        }

        std::vector<unsigned char> horiz(size_t(W) * H, 0);
        for (int y = 0; y < H; ++y) {
            const size_t row = size_t(y) * W;
            for (int x = R; x < W - R; ++x) {
                unsigned char v = 1;
                for (int k = -R; k <= R && v; ++k) {
                    v &= mask[row + size_t(x + k)];
                }
                horiz[row + size_t(x)] = v;
            }
        }

        size_t cnt = 0;
        for (int y = R; y < H - R; ++y) {
            for (int x = 0; x < W; ++x) {
                unsigned char v = 1;
                for (int k = -R; k <= R && v; ++k) {
                    v &= horiz[size_t(y + k) * W + size_t(x)];
                }
                cnt += v;
            }
        }
        g.solidRatio = float(cnt) / float(size_t(W) * H);
    }

    g.colProfile.resize(g.w);
    std::vector<float> cols(g.w, 0.0f);
    for (int x = 0; x < g.w; ++x) {
        float s = 0.0f;
        for (int y = 0; y < g.h; ++y) {
            s += g.at(x, y);
        }
        cols[x] = s / g.h;
    }
    const float colMed = medianOf(cols);
    for (int x = 0; x < g.w; ++x) {
        g.colProfile[x] = std::fabs(cols[x] - colMed);
    }

    return g;
}

RowColProfile FrameAnalyzer::rowColProfile(const QImage &img, int targetW)
{
    RowColProfile p;
    if (img.isNull() || targetW <= 0) {
        return p;
    }

    QImage src = img;
    if (src.format() != QImage::Format_RGB32
        && src.format() != QImage::Format_ARGB32) {
        src = src.convertToFormat(QImage::Format_RGB32);
    }

    const int W = src.width();
    const int H = src.height();
    const int step = std::max(1, W / targetW);
    const int gw = (W + step - 1) / step;
    const int gh = (H + step - 1) / step;
    if (gw < 32 || gh < 32) {
        return p;
    }

    // 逐块取平均（和 toGray 一致），只保留行/列均值
    std::vector<double> rowSum(gh, 0.0);
    std::vector<double> colSum(gw, 0.0);
    std::vector<int> rowCnt(gh, 0);
    std::vector<int> colCnt(gw, 0);

    for (int y = 0; y < gh; ++y) {
        const int sy0 = y * step;
        const int sy1 = std::min(sy0 + step, H);
        for (int x = 0; x < gw; ++x) {
            const int sx0 = x * step;
            const int sx1 = std::min(sx0 + step, W);

            double sum = 0.0;
            int cnt = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const QRgb *line =
                    reinterpret_cast<const QRgb *>(src.constScanLine(sy));
                for (int sx = sx0; sx < sx1; ++sx) {
                    const QRgb c = line[sx];
                    sum += 0.299 * qRed(c) + 0.587 * qGreen(c) + 0.114 * qBlue(c);
                    ++cnt;
                }
            }
            const double v = cnt ? sum / cnt : 0.0;
            rowSum[y] += v;
            rowCnt[y] += 1;
            colSum[x] += v;
            colCnt[x] += 1;
        }
    }

    p.rows.resize(gh);
    for (int y = 0; y < gh; ++y) {
        p.rows[y] = rowCnt[y] ? float(rowSum[y] / rowCnt[y]) : 0.0f;
    }
    p.cols.resize(gw);
    for (int x = 0; x < gw; ++x) {
        p.cols[x] = colCnt[x] ? float(colSum[x] / colCnt[x]) : 0.0f;
    }
    p.srcW = W;
    p.srcH = H;
    return p;
}

double FrameAnalyzer::zncc(const float *a, const float *b, std::size_t n)
{
    if (n == 0) {
        return 0.0;
    }

    double ma = 0.0;
    double mb = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);

    double num = 0.0;
    double da = 0.0;
    double db = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = a[i] - ma;
        const double y = b[i] - mb;
        num += x * y;
        da += x * x;
        db += y * y;
    }
    if (da < 1e-9 || db < 1e-9) {
        return 0.0;
    }
    return num / std::sqrt(da * db);
}

double FrameAnalyzer::similarity(const GrayFrame &a, const GrayFrame &b)
{
    if (!a.valid() || !b.valid()) {
        return 0.0;
    }
    const int w = std::min(a.w, b.w);
    const int h = std::min(a.h, b.h);
    if (w < 8 || h < 8) {
        return 0.0;
    }

    std::vector<float> va;
    std::vector<float> vb;
    va.reserve(static_cast<std::size_t>(w) * h);
    vb.reserve(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            va.push_back(a.at(x, y));
            vb.push_back(b.at(x, y));
        }
    }
    return zncc(va.data(), vb.data(), va.size());
}

int FrameAnalyzer::bestShift1D(const std::vector<float> &pa,
                               const std::vector<float> &pb,
                               int maxShift, double *bestScore)
{
    const int n = static_cast<int>(std::min(pa.size(), pb.size()));
    if (n < 32) {
        if (bestScore) {
            *bestScore = 0.0;
        }
        return 0;
    }

    // 重叠太少时互相关会给出虚假高分，必须限制搜索范围
    const int minOverlap = std::max(16, n / 4);
    maxShift = std::min(maxShift, n - minOverlap);
    if (maxShift < 1) {
        if (bestScore) {
            *bestScore = 0.0;
        }
        return 0;
    }

    int bestD = 0;
    double bestS = -2.0;
    for (int d = -maxShift; d <= maxShift; ++d) {
        const int len = (d >= 0) ? (n - d) : (n + d);
        if (len < minOverlap) {
            continue;
        }
        const float *a = (d >= 0) ? (pa.data() + d) : pa.data();
        const float *b = (d >= 0) ? pb.data() : (pb.data() - d);
        const double s = zncc(a, b, static_cast<std::size_t>(len));
        if (s > bestS) {
            bestS = s;
            bestD = d;
        }
    }

    if (bestScore) {
        *bestScore = (bestS < -1.0) ? 0.0 : bestS;
    }
    return bestD;
}

ShiftResult FrameAnalyzer::estimateShift(const GrayFrame &prev,
                                         const GrayFrame &cur,
                                         int maxShiftY, int maxShiftX)
{
    ShiftResult r;
    if (!prev.valid() || !cur.valid()) {
        return r;
    }
    if (maxShiftY <= 0) {
        maxShiftY = prev.h / 2;
    }
    if (maxShiftX <= 0) {
        maxShiftX = prev.w / 2;
    }

    r.dy = bestShift1D(prev.rowProfile, cur.rowProfile, maxShiftY);
    r.dx = bestShift1D(prev.colProfile, cur.colProfile, maxShiftX);
    r.score = overlapScore(prev, cur, r.dx, r.dy);
    r.score00 = overlapScore(prev, cur, 0, 0);
    return r;
}

double FrameAnalyzer::overlapScore(const GrayFrame &a, const GrayFrame &b,
                                   int dx, int dy)
{
    if (!a.valid() || !b.valid()) {
        return -1.0;
    }

    // cur[y, x] == prev[y + dy, x + dx]
    const int ay0 = std::max(0, dy);
    const int ay1 = a.h + std::min(0, dy);
    const int by0 = std::max(0, -dy);
    const int by1 = b.h + std::min(0, -dy);
    const int ax0 = std::max(0, dx);
    const int ax1 = a.w + std::min(0, dx);
    const int bx0 = std::max(0, -dx);
    const int bx1 = b.w + std::min(0, -dx);

    const int hh = std::min(ay1 - ay0, by1 - by0);
    const int ww = std::min(ax1 - ax0, bx1 - bx0);
    if (hh < 16 || ww < 16) {
        return -1.0;
    }

    std::vector<float> va;
    std::vector<float> vb;
    va.reserve(static_cast<std::size_t>(hh) * ww);
    vb.reserve(static_cast<std::size_t>(hh) * ww);
    for (int y = 0; y < hh; ++y) {
        for (int x = 0; x < ww; ++x) {
            va.push_back(a.at(ax0 + x, ay0 + y));
            vb.push_back(b.at(bx0 + x, by0 + y));
        }
    }
    return zncc(va.data(), vb.data(), va.size());
}

} // namespace cm
