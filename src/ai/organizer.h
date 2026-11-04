#pragma once

// 课后整理流水线：读一次录制会话 → 逐张判学科 → 归档成「日期 / 学科」结构。
//
// 命令行和托盘菜单共用这一份逻辑，避免两边行为不一致。
//
// 为什么放在**课后**而不是录制时实时跑
// ------------------------------------
//   * 上课时完全不依赖网络。网络断了、API 挂了都不影响录制
//   * 失败可以重试，不用重录
//   * 老师手里有完整的一批图，模型判断时上下文更完整
//
// 两条贯穿全项目的语义（所有入口必须一致，改动前先读）
// --------------------------------------------------
//   1. **整理：任何入口都只整理没整理过的。**
//      判据是会话目录里的 organized.json。唯一实现是
//      organizePendingSessions() —— 托盘菜单、--classify、--organize-pending、
//      定时整理全都调它，不允许各写一份筛选逻辑。
//   2. **识别：任何入口都只识别没识别过的。**
//      判据是会话目录里的 classify.json（按帧文件名做键）。
//      注意"没识别过"不等于"没成功过"：判断失败的帧不写进缓存，
//      所以显式重新整理时会重试它们。

#include <QString>

#include <functional>

#include "ai/archiver.h"
#include "ai/usage.h"

namespace cm {

struct OrganizeOptions {
    QString sessionDir;      ///< 录制会话目录（含 session.json 和 PNG）
    QString archiveRoot;     ///< 归档根目录
    /// 已经整理过的会话是否重新跑一遍。
    /// 默认 false —— 重跑会重新调用模型，既费钱又可能得到不同的结果。
    bool forceReclassify = false;
};

struct OrganizeProgress {
    int done = 0;
    int total = 0;
    QString message;
};

struct OrganizeResult {
    int total = 0;
    /// 有可用判断结果的帧数（**含走缓存的**）
    int succeeded = 0;
    int failed = 0;
    /// 本次**真正送到模型**的帧数 —— 只有这些花了 token。
    /// 没配 Key、图片读不出来、请求超时都不算（见 Classification::requestSent）。
    int identified = 0;
    /// 命中 classify.json、没花钱的帧数
    int cachedHits = 0;
    int promptTokens = 0;
    int completionTokens = 0;
    bool usedStub = false;
    ArchiveResult archive;
    QString error;

    /// **遇到根本性错误提前停了**（Key 无效、余额用完、地址/模型名错、
    /// 模型不能看图 —— 见 Classification::fatal）。
    /// 这时**没有归档、也没有写 organized.json**，所以充值后再整理一次
    /// 就能从断点补上，不会永久留在「未识别」。
    bool aborted = false;
    /// 用户中途取消。语义和 aborted 一样（不归档、不标记），只是原因不同。
    bool cancelled = false;
    /// 为什么停的（给界面显示的人话）
    QString stopReason;
    /// 本次第一条**单张失败**的原因。批量整理最后要把原因摊给老师看，
    /// 否则只报"N 张失败"，老师不知道该去充值还是该去查 key。
    QString firstError;

    bool ok() const { return error.isEmpty(); }
};

/// 整理一个会话。onProgress 可以为空。
/// 单张失败不会中断整批 —— 那一张会落到「其他」，其余照常归档。
/// 但 `fatal` 和取消会提前停下（见 OrganizeResult::aborted / cancelled）。
///
/// isCancelled 可以为空。传进来的话会在帧与帧之间检查。
///
/// **本函数不写 organized.json** —— 要不要标记「整理过了」由调用方决定。
/// 批量入口（organizePendingSessions）会自动标记；显式指定单个会话
/// （--session-dir）时调用方也应当标记，否则定时整理会把它再做一遍。
OrganizeResult organizeSession(
    const OrganizeOptions &opt,
    const std::function<void(const OrganizeProgress &)> &onProgress = {},
    const std::function<bool()> &isCancelled = {});

// ------------------------------------------------------------------ 批量整理

/// 一批整理的合计
struct OrganizeBatchResult {
    int sessions = 0;           ///< 处理了几个会话
    int sessionsOk = 0;
    int sessionsFailed = 0;
    int succeeded = 0;
    int failed = 0;
    int identified = 0;         ///< 真正调用模型的帧数（花钱的部分）
    int cachedHits = 0;         ///< 复用缓存的帧数（不花钱）
    int promptTokens = 0;
    int completionTokens = 0;
    bool usedStub = false;
    QStringList dayDirs;
    QStringList errors;

    /// 整批被提前停掉了（根本性错误 或 用户取消）—— 见 OrganizeResult 上同名成员。
    /// 这时**没有给"正在处理/还没处理"的会话写 organized.json**，
    /// 充值或改好设置后再点一次就能从断点继续。
    bool aborted = false;
    bool cancelled = false;
    QString stopReason;
    /// 第一条单张失败的原因（给完成对话框显示用的）
    QString firstError;

    /// 本次共处理了几帧
    int frames() const { return succeeded + failed; }

    bool stoppedEarly() const { return aborted || cancelled; }
};

/// **「只整理没整理过的」这条语义只在这一个函数里实现。**
///
/// 挑出 recordingsRoot 下所有还没整理过的会话（判据见 findPendingSessions），
/// 从早到晚逐个 organizeSession()，成功就写 organized.json 标记，
/// 并把这一轮的 token 用量记进账本。
///
/// 托盘菜单、--classify、--organize-pending、定时整理全都调它。
/// **不要在别处再写一份筛选或标记逻辑** —— 那正是过去
/// "手动整理完定时又整理一遍"的来源。
///
/// **正在录制的会话照样会被整理，这是有意的。** 老师可以在录制中途点一次
/// 「AI 整理」先看效果，而且这是预期用法。录制中整理不会损坏任何东西：
///   * 已判的帧命中 classify.json 缓存，不调模型、不花钱
///   * 归档是增量合并，键取原图名，同一帧只会覆盖自己
///   * organized.json 里记着"整理时有多少帧"，所以会话后来又长了
///     会被自动重新视为待整理（见 isOrganized）
///
/// onProgress 参数：会话名、第几个（从 1 开始）、共几个、会话内进度。
///
/// isCancelled 可以为空。一旦返回 true，**整个批次立刻停**：
/// 已经做完的会话保留（它们的 organized.json 已经写好），
/// 正在做和还没做的**都不标记**，所以下次点还能接着做。
OrganizeBatchResult organizePendingSessions(
    const QString &recordingsRoot,
    const QString &archiveRoot,
    bool forceReclassify = false,
    const std::function<void(const QString &sessionName, int index, int count,
                             const OrganizeProgress &)> &onProgress = {},
    const std::function<bool()> &isCancelled = {});

// ------------------------------------------------------------------ 定时整理

/// 某个会话**是不是已经整理完了**。
///
/// **判据是「标记里记的帧数 >= 会话现在的帧数」，不是「标记文件存不存在」。**
///
/// 只判断文件存不存在会造成一个很隐蔽的 bug：老师在**录制中途**整理过一次
/// 之后，标记就永久生效了，他接着写的每一页都再也进不了归档，而界面只会说
/// 「没有需要整理的新记录」—— 看起来像"没反应"。
/// （实测复现：2 帧时整理 -> 标记 frames=2；又写到 4 帧再点整理 ->
///   提示"没有待整理的录制记录"，归档里只有 2 张。）
///
/// 帧数变多 = 会话又长了 = 应当重新整理。重新整理不花钱也不产生重复文件，
/// 见上面 organizePendingSessions 的说明。
bool isOrganized(const QString &sessionDir);

/// 在会话目录里写「已整理」标记。
/// 所有整理入口都靠它避免重复调用模型 —— 重复调用既费额度，
/// 又可能让同一份素材得到两种结果。
///
/// frameCount 必须传**这个会话当时的关键帧总数**（不是本次成功判定的张数）：
/// isOrganized() 拿它判断"会话后来又长了没有"。
bool markOrganized(const QString &sessionDir, const QString &archiveRoot,
                   int frameCount);

/// 找出**还没整理完**的会话，按时间从早到晚排列。
/// 一张关键帧都没有的会话会跳过（老师开了软件但没上课）。
QStringList findPendingSessions(const QString &recordingsRoot);

} // namespace cm
