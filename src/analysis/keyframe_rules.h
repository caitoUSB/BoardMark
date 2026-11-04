#pragma once

// 关键帧判定规则：把「什么时候该留一张」这件事从采集循环里独立出来。
//
// 独立出来的好处很实际：可以用**真实素材离线回放**来验证判定对不对，
// 而不用每次开着白板手工试。命令行的 --analyze 就是干这个的。
//
// 核心原则只有一句：保存「变化发生前的那一帧」。
// 因为那一帧正是「老师刚写满、还没挪走」的完整画面。
//
// 三种情况要分清，否则会误判
// --------------------------
//   同一屏里写字 : 未动相似度高，没有位移
//   画面整体平移 : 未动相似度**也低**（稀疏笔画一错位就对不上），
//                  但按位移对齐后相似度很高
//   画面大面积替换: 未动相似度低，而且**怎么对齐都对不上**
//
// 最初我把「未动相似度低」直接当成「大面积替换」，结果所有拖动都被误判。
// 必须先看位移，再看替换。

#include <QString>

#include "analysis/frame_analyzer.h"

namespace cm {

struct KeyframeParams {
    double replaceThreshold = 0.35;  ///< 未动相似度低于此值 -> 疑似大面积替换
    double alignedMinScore = 0.50;   ///< 对齐后相似度高于此值 -> 认为位移可信
    int moveThreshold = 3;           ///< 位移超过此值（缩略图像素）-> 认为在平移
    double movePercent = 25.0;       ///< 累计位移达到画面高度的百分之多少才值得存
    double sameThreshold = 0.90;     ///< 未动相似度高于此值 -> 画面几乎没变
    int maxSpanSeconds = 180;        ///< 长时间只写不停时强制存一张
    /// 低于此墨量视为「屏幕基本是空的」，不保存。
    /// 单位是**平均偏离 / 255**（0..1）。
    ///
    /// 这个阈值只能回答「**屏幕是不是全空的**」，回答不了「内容够不够多」——
    /// 因为两组分布是重叠的，实测标定：
    ///   息屏黑屏 0.000，空板 0.001，
    ///   稀疏手写（真实板书）0.002~0.008，密笔画 0.020~0.031，
    ///   白板软件的 UI 面板 0.050~0.130。
    /// 真实板书可以低到 0.002，而"几乎空板"可以高到 0.008，
    /// 所以**不能用墨量去判断"这屏值不值得存"**，只能贴着噪声底卡掉全空屏。
    /// 误判"内容够不够"的后果很严重：实测取 0.015 时，34 帧真实素材被砍到只剩 5 张。
    float emptyInk = 0.003f;

    /// 大到什么程度的「大块纯色区域」就认为画面里有 UI 面板（图形选择、
    /// 更多菜单、登录框）。实测希沃的「更多」面板约占画面 30%，
    /// 腐蚀后仍剩很多；手写笔画腐蚀后接近 0。2% 是很宽松的分界。
    float panelThreshold = 0.02f;
    double duplicateScore = 0.93;    ///< 与上一张已保存帧过于相似则跳过
    /// 两张关键帧之间的最小间隔。
    /// 用来兜住「画面正在快速变化」的短暂过程 —— 比如老师框选、缩放、
    /// 连续翻页时，每一帧都和前一张差别很大，不加这道闸会一口气存出一串。
    int minSaveIntervalMs = 1500;
};

/// 两帧之间的关系
enum class PairKind {
    Same,       ///< 画面几乎没变
    Writing,    ///< 同一屏里继续写字
    Shift,      ///< 画面整体平移（老师拖动板书）
    Replace,    ///< 画面大面积替换（翻页 / 清屏）
};

struct PairAnalysis {
    PairKind kind = PairKind::Same;
    int dx = 0;             ///< 相对候选帧的位移。因为候选帧在平移期间不动，
                            ///< 所以这就是**累计位移**。
    int dy = 0;
    double score = 0.0;     ///< 按位移对齐后的相似度
    double score00 = 0.0;   ///< 不位移时的相似度
};

struct KeyframeDecision {
    bool save = false;
    QString reason;
    /// 是否应该把候选帧更新为当前帧。
    /// 平移期间**不能**更新 —— 否则候选帧跟着一起移动，累计位移永远归零，
    /// 就再也判不出「老师拖走了」。
    bool updateCandidate = true;
};

/// 分析两帧的关系。纯计算，无状态。
PairAnalysis analyzePair(const GrayFrame &candidate, const GrayFrame &current,
                         const KeyframeParams &params, int frameHeight);

/// 决定「候选帧」是否值得保存。
/// msSinceLastSave 是距上一张关键帧保存过了多久（毫秒）；还没存过就传一个大值。
KeyframeDecision decideKeyframe(const KeyframeParams &params,
                                const PairAnalysis &a,
                                const GrayFrame &candidate,
                                const GrayFrame &lastSaved,
                                int frameHeight,
                                qint64 spanMs,
                                qint64 msSinceLastSave);

const char *pairKindName(PairKind k);

} // namespace cm
