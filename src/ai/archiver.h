#pragma once

// 归档：把关键帧整理成「日期 / 学科」的文件夹结构。
//
// 一条铁律
// --------
// **原图/ 里的原始截图永远只写一次，之后只读。**
// 学科判断可能出错，知识点可能读错，但只要原图还在，一切都能重来。
// 所以任何整理动作都不能修改 原图/ 里的文件 —— 这里只往里面"新增"。
//
// 关于目录名从 "raw" 改成 "原图"
// ------------------------------
// 归档是给老师看的，目录名就该是中文。但**老归档里躺着的是 raw/**，
// 所以这里有两个名字：
//     kRawDir        新名字，新建和迁移都用它
//     kLegacyRawDir  旧名字，只为了两件事：
//                      1) clearDerivedFolders 必须连它一起保护，
//                         否则第一次整理老归档就会把老师的原图整目录删掉
//                      2) 名字被占用（改名失败）时仍然能读到图
// 处理某一天的目录前先调 fixupDayDir()，它会顺手把 raw/ 改名成 原图/。
//
// 重复文件用**硬链接**而不是复制：同一块磁盘上的同一个文件有两个名字，
// 既不占额外空间，也不会因为"两份副本"产生不一致。
// 硬链接失败（跨盘、权限）时自动退回复制。

#include <QList>
#include <QString>
#include <QStringList>

namespace cm {

/// 待归档的一帧
struct ArchiveRecord {
    QString srcPath;                 ///< 原始截图路径
    qint64  timestampMs = 0;
    QString subject;                 ///< 空 / "其他" / "未分类" 都会落到 kUnknownDir
    QString subjectConfidence;
    QStringList keywords;            ///< 判断依据，供老师核对
    QString topic;
    QString topicConfidence;
    QString reason;                  ///< 为什么存下这一帧（来自采集引擎）

    /// 来自哪一次录制会话（目录名，例如 2026-10-01_090000）。
    ///
    /// 用来解决跨会话的重名问题：原图的文件名只用 HHMMSS，
    /// 同一天两次课的帧理论上会撞在同一秒上。撞了要避让，
    /// **但"同一个会话重新整理一遍"不算撞车** —— 那是同一张图，
    /// 应该复用原名，不能每整理一次就多出一份 `_2`、`_3`。
    QString sessionId;

    /// 被哪一张顶替了。非空表示这一帧已经被内容更全的版本取代 ——
    /// 原图仍然保留在 原图/，但不会出现在学科文件夹里。
    /// （去重功能还没接上，字段先留着，接口不用再改一次。）
    QString supersededBy;
};

struct ArchiveResult {
    int dayCount = 0;
    int linked = 0;
    int copied = 0;
    int unknownSubject = 0;
    int superseded = 0;
    QStringList dayDirs;
    QString error;

    bool ok() const { return error.isEmpty(); }
};

/// 把任意文字变成合法的 Windows 文件名（去掉 \ / : * ? " < > | 等）
QString sanitizeFileName(const QString &name, const QString &fallback = QStringLiteral("未命名"),
                         int maxLen = 40);

/// 优先建硬链接，失败则退回复制。
/// 返回 "link" / "copy" / "exists" / "fail"。
/// 硬链接不占额外空间，也不会出现"两份副本不一致"。
QString linkOrCopyFile(const QString &src, const QString &dst);

/// 归档到 root/<YYYY-MM-DD>/ 。
/// 结构：
///     <root>/<日期>/原图/           原始截图（只写一次，之后只读）
///     <root>/<日期>/<学科>/          硬链接，按知识点命名
///     <root>/<日期>/其他/            判不出学科的（= kUnknownDir）
///     <root>/<日期>/index.json      （设了隐藏属性）
///
/// **没有 index.html 了。** 那个"双击就能看的浏览页"功能已删除（它的 CSS
/// 是坏的，缩略图被裁掉一半，维护者决定不修、直接去掉）。老归档里的
/// index.html 由 fixupDayDir() 顺手删掉。
ArchiveResult archiveFrames(const QString &root, const QList<ArchiveRecord> &records);

/// 清掉某一天里**由程序生成**的学科文件夹，只留 原图/ 和 index.json。
///
/// 重新整理时先调它。否则会出这个问题：
/// 同一张图第一次判成"抛物线的参数方程"、第二次判成"抛物线的几何性质"，
/// 文件名不同 → 两份都留着，学科文件夹里出现重复内容。
///
/// **绝不动 原图/（以及老归档的 raw/）** —— 原图只写一次、之后只读，
/// 是这条流水线的铁律。所以这里只删既不是这两个名字的子目录，别的一概不碰。
void clearDerivedFolders(const QString &dayDir);

/// 给文件加上 Windows 隐藏属性。index.json 是给程序读的，
/// 摆在归档文件夹里只会让老师困惑，藏起来更干净。
///
/// 必须在**每次写完之后**再调：QSaveFile 是"写临时文件再改名覆盖"，
/// 换上去的新文件是没有属性的，写入前的隐藏标记一律丢失。
void markHidden(const QString &path);

/// 某一天归档目录里，原图到底放在哪个子目录名下（不带路径，只有名字）。
///
/// 正常情况就是 kRawDir；老归档可能还是 kLegacyRawDir
/// （fixupDayDir 改名失败时也会走到这一支）。
/// 三个都不存在时返回 kRawDir —— 也就是新建时用中文名。
QString rawDirName(const QString &dayDir);

/// 处理某一天的归档目录之前调一次，做三件修旧的事情：
///   1) 老版本的 raw/ 改名成 原图/（新名字已存在就不动，改名失败只告警）
///   2) 把已有的 index.json 设成隐藏
///   3) **删掉已废弃的 index.html 浏览页**（只删这一个确定的文件名）
/// 三个操作都是"能做就做"，失败不影响归档继续跑。
void fixupDayDir(const QString &dayDir);

/// 未分类文件夹名。**所有判不出学科的内容都归到这一个文件夹。**
///
/// 模型本身只会返回「其他」（它被限制在 kSubjects 里选），但老数据、
/// 手改过的索引、以及别的入口可能给出「未分类」「未识别」——
/// 这些一律折叠成同一个名字，否则归档里会同时冒出好几个"没分类"的文件夹。
///
/// 早先这个名字是 `_未分类`，靠下划线排在最前面；现在统一叫「其他」，
/// 和老师在界面上、在模型返回里看到的那两个字一致。
extern const QString kUnknownDir;

/// 把学科名归一化：空 / 其他 / 未分类 / 未识别 -> kUnknownDir（其他）。
///
/// **这里必须用 QStringLiteral 比较中文。** `QLatin1String("其他")` 会把这
/// 几个字按 Latin-1 逐字节解释（"其他" 变成 6 个字符），和真正的 QString
/// 永远不相等，归一化会**静默失效** —— 这个坑 v0.9.0 里就在，
/// 表现为"没分类"的内容散在各自的文件夹里，`_未分类` 一次都没出现过。
QString normalizeSubjectName(const QString &s);

/// 原图子目录名（中文）。新建归档和迁移老归档都用它。
extern const QString kRawDir;

/// 原图子目录的旧名字（v0.9.0 及以前）。**只用于兼容，不要拿它新建目录。**
extern const QString kLegacyRawDir;

} // namespace cm
