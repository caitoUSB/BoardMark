#include "analysis/keyframe_rules.h"

#include <algorithm>
#include <cstdlib>

namespace cm {

const char *pairKindName(PairKind k)
{
    switch (k) {
    case PairKind::Same:    return "画面没变";
    case PairKind::Writing: return "同屏写字";
    case PairKind::Shift:   return "整体平移";
    case PairKind::Replace: return "大面积替换";
    }
    return "?";
}

PairAnalysis analyzePair(const GrayFrame &candidate, const GrayFrame &current,
                         const KeyframeParams &params, int frameHeight)
{
    PairAnalysis a;
    if (!candidate.valid() || !current.valid()) {
        return a;
    }

    const ShiftResult r = FrameAnalyzer::estimateShift(candidate, current);
    a.dx = r.dx;
    a.dy = r.dy;
    a.score = r.score;
    a.score00 = r.score00;

    // 判断顺序很重要：
    //   1) 先看「几乎没变」
    //   2) 再看「按位移对齐后能不能对上」—— 能对上就是平移
    //   3) 怎么都对不上，才是大面积替换
    if (r.score00 >= params.sameThreshold) {
        a.kind = PairKind::Same;
        return a;
    }

    const int mag = std::max(std::abs(r.dx), std::abs(r.dy));
    if (mag > params.moveThreshold && r.score >= params.alignedMinScore) {
        a.kind = PairKind::Shift;
        return a;
    }

    if (r.score00 < params.replaceThreshold) {
        a.kind = PairKind::Replace;
        return a;
    }

    a.kind = PairKind::Writing;
    return a;
}

KeyframeDecision decideKeyframe(const KeyframeParams &params,
                                const PairAnalysis &a,
                                const GrayFrame &candidate,
                                const GrayFrame &lastSaved,
                                int frameHeight,
                                qint64 spanMs,
                                qint64 msSinceLastSave)
{
    KeyframeDecision d;

    if (!candidate.valid()) {
        return d;
    }

    // 空白板没有保存价值：白占空间，还会干扰后续归档
    if (candidate.ink() < params.emptyInk) {
        return d;
    }

    // 候选帧里有大片 UI 面板（图形选择、更多菜单、登录框）→ 拍到的不是板书。
    // 用形态判断（大块纯色），跟底色明暗无关，白板黑板一样有效。
    if (candidate.hasPanel(params.panelThreshold)) {
        return d;
    }

    // 和上一张已保存的几乎一样就不重复存
    if (lastSaved.valid()) {
        const double dup = FrameAnalyzer::similarity(lastSaved, candidate);
        if (dup > params.duplicateScore) {
            return d;
        }
    }

    // 最小保存间隔：兜住「画面正在快速变化」的短暂过程
    // （框选、缩放、连续翻页时每帧都差别很大，不加这道闸会一口气存出一串）
    if (params.minSaveIntervalMs > 0
        && msSinceLastSave < params.minSaveIntervalMs) {
        return d;
    }

    switch (a.kind) {
    case PairKind::Shift: {
        // 候选帧在平移期间**不会**被更新，所以 a.dx/a.dy 就是累计位移。
        // 只有累计位移大到「原来那一屏的内容快要被推出画面」时，
        // 才值得把候选帧存下来。
        // 小幅度挪动（几十像素）内容还在，不必浪费一张图。
        const int need = std::max(16, int(frameHeight * params.movePercent / 100.0));
        if (std::abs(a.dy) >= need || std::abs(a.dx) >= need) {
            d.save = true;
            d.reason = QStringLiteral("画面整体平移，累计 dx=%1 dy=%2（超过 %3）")
                           .arg(a.dx).arg(a.dy).arg(need);
        }
        d.updateCandidate = false;      // 候选帧保持不动，继续累积
        return d;
    }

    case PairKind::Replace:
        d.save = true;
        d.reason = QStringLiteral("画面大面积替换（未动相似度 %1，对齐后也仅 %2）")
                       .arg(a.score00, 0, 'f', 2)
                       .arg(a.score, 0, 'f', 2);
        return d;

    case PairKind::Same:
    case PairKind::Writing:
    default:
        break;
    }

    // 长时间只写不停，兜底存一张，防止内容丢失
    if (params.maxSpanSeconds > 0
        && spanMs > qint64(params.maxSpanSeconds) * 1000) {
        d.save = true;
        d.reason = QStringLiteral("长时间只写不停，已超过 %1 秒")
                       .arg(params.maxSpanSeconds);
        return d;
    }

    return d;
}

} // namespace cm
