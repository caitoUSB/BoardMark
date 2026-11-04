#pragma once

// 帧分析：把截图变成可以快速比较的「灰度帧」，并判断两帧之间发生了什么。
//
// 设计要点（都是踩坑换来的，不要随意改）：
//
// 1) 全部在**降采样后的小图**上做。原图 1920x1080 直接算太贵；降到宽 480
//    之后一次比较不到 1ms。我们只需要判断「画面是否发生了大幅变化」，
//    不需要像素级精度。
//
// 2) 用「逐行 / 逐列墨量」的一维投影求位移。整页笔画被汇总成一条曲线，
//    比直接做二维模板匹配稳得多，而且快一个数量级。
//
// 3) 墨量定义为「该行/列均值相对底色的偏离量」。这样对底色明暗**完全不敏感**：
//      白板黑字：有笔画处均值更低 -> 偏离为负
//      黑板白字：有笔画处均值更高 -> 偏离为正
//    取绝对值后两者统一。实测希沃默认就是深墨绿的黑板模式（底色灰度约 30、
//    白色笔迹约 203），这一条不是可选项。

#include <QImage>

#include <cstddef>
#include <vector>

namespace cm {

/// 分析用的灰度帧
struct GrayFrame {
    int w = 0;
    int h = 0;
    std::vector<float> pix;          ///< 灰度值，行优先，范围 0..255
    std::vector<float> rowProfile;   ///< 逐行墨量
    std::vector<float> colProfile;   ///< 逐列墨量
    float inkLevel = 0.0f;           ///< 墨量，见 ink()
    float solidRatio = 0.0f;         ///< 大块纯色占比，见 hasPanel()

    bool valid() const { return w > 0 && h > 0 && !pix.empty(); }
    float at(int x, int y) const { return pix[static_cast<size_t>(y) * w + x]; }

    /// 「墨量」= 全画面像素相对底色的**平均偏离**，归一化到 0..1。
    ///
    /// 底色取全画面像素中位数，所以对底色明暗完全不敏感
    /// （黑板白字、白板黑字都成立），息屏黑屏会得到 0，天然被挡掉。
    ///
    /// 两个踩过的坑，别再改回去：
    ///
    /// 1. 不能用「每行均值相对中位数的偏差」。那个量度的是内容在垂直方向
    ///    有多集中，不是内容有多少。实测反例：
    ///        密密麻麻铺满的手写 -> 0.26      中间一块 UI 面板 -> 30.68
    ///    完全颠倒，导致「空白板过滤」形同虚设。
    ///
    /// 2. 不能用「偏离超过某阈值的像素占比」。分析图是 4 倍降采样的，
    ///    一根 2 像素宽的笔迹在 4x4 块里被平均成 51，背景 30，差值只有 21，
    ///    卡在阈值下面会被整片当成背景 —— 实测密密麻麻的手写算出 0.00。
    float ink() const { return inkLevel; }

    /// 这一帧里有没有「大块纯色区域」—— 用来识别白板软件弹出的 UI 面板
    /// （图形选择、更多菜单、登录框等），这些不该被当板书存下来。
    ///
    /// 判据是**形态**不是**亮度**：手写笔画只有一两个像素宽，被腐蚀一下就没了；
    /// UI 面板几百像素宽，腐蚀完还剩一大片。
    /// 所以这个方法**跟底色明暗无关** —— 深色黑板、白色白板一样成立。
    ///
    /// 反面教训：一开始想用「墨量高就是面板」，在深色黑板上确实能分开
    /// （面板 0.05~0.13，板书 0.01~0.03），但老师一切成白板就完全失效。
    bool hasPanel(float threshold = 0.02f) const { return solidRatio > threshold; }
};

/// 两帧之间的位移估计结果
struct ShiftResult {
    int dx = 0;             ///< 内容相对上一帧向左移动了多少
    int dy = 0;             ///< 内容相对上一帧向上移动了多少
    double score = 0.0;     ///< 按 (dx,dy) 对齐后的相似度，越接近 1 越好
    double score00 = 0.0;   ///< 「其实没动」这个假设的相似度

    bool moved(int thr = 8) const
    {
        return (dx > thr || -dx > thr || dy > thr || -dy > thr);
    }
};

/// 一帧的行/列均值剖面 —— 自动标定白板裁切用。
///
/// 比 toGray() 便宜得多：只做降采样 + 求均值，不算墨量、不做腐蚀、
/// 不算投影墨量。而且它**不裁切** —— 标定本来就是要找出该裁哪里。
struct RowColProfile {
    std::vector<float> rows;    ///< 每行均值（降采样之后）
    std::vector<float> cols;    ///< 每列均值
    int srcW = 0;               ///< 原始画面物理宽高，用来把行号换算成百分比
    int srcH = 0;

    bool valid() const
    {
        return srcW > 0 && srcH > 0 && rows.size() >= 32 && cols.size() >= 32;
    }
};

class FrameAnalyzer {
public:
    /// 把 QImage 转成分析用的灰度帧。targetW 是降采样后的目标宽度。
    ///
    /// crop 用来**切掉固定 UI**（白板软件自带的工具栏、横幅等）。
    /// 这一步不是可选的：固定 UI 每一帧都一模一样，会把相似度垫得很高，
    /// 把真实的画面位移彻底淹没 —— 实测不裁切时 34 帧真实拖动素材
    /// 会被判成「一张都不用存」。传空 QRect 表示不裁。
    static GrayFrame toGray(const QImage &img, int targetW = 480,
                            const QRect &crop = QRect());

    /// 只求行/列均值（**不裁切**）。给自动标定用，见 BoardCalibrator。
    static RowColProfile rowColProfile(const QImage &img, int targetW = 480);

    /// 零均值归一化互相关：对整体亮度和对比度的变化免疫。
    static double zncc(const float *a, const float *b, std::size_t n);

    /// 两帧在 (0,0) 对齐（不位移）下的相似度。
    static double similarity(const GrayFrame &a, const GrayFrame &b);

    /// 一维投影互相关：求把 pb 平移多少才能和 pa 对上。
    static int bestShift1D(const std::vector<float> &pa,
                           const std::vector<float> &pb,
                           int maxShift, double *bestScore = nullptr);

    /// 估计整体位移：行投影求 dy，列投影求 dx。
    static ShiftResult estimateShift(const GrayFrame &prev,
                                     const GrayFrame &cur,
                                     int maxShiftY = 0, int maxShiftX = 0);

    /// 按 (dx,dy) 对齐后，两帧重叠区的相似度。重叠太小返回 -1。
    static double overlapScore(const GrayFrame &a, const GrayFrame &b,
                               int dx, int dy);
};

} // namespace cm
