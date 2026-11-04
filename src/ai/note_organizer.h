#pragma once

// 整理一个「平铺的截图文件夹」：按日期分天，按内容分学科，提炼知识点重命名。
//
// 和 organizer 的区别
// ------------------
//   organizer    读的是**录制会话**（session.json + 时间戳齐全，天然有序）
//   note_organizer 读的是**一堆平铺的截图**（文件名里带时间戳，可能来自
//                  白板软件导出，也可能是老师随手截的）
//
// 有两条实测得来的结论，决定了这里的做法
// ------------------------------------
//   1. **时间戳只能定到"哪天"，定不了学科。**
//      实测一个导出文件里可以同时含物理和数学（老师把几次课的内容一起导出了）。
//      所以学科必须看图片内容。
//   2. **文件名里的时间戳是"导出/截图"的时间，不是上课时间。**
//      实测大量文件集中在 18:0x 和 22:1x。所以只做到"哪天哪个科目"，
//      不假装知道这是第几节课。

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

#include "ai/classifier.h"
#include "ai/usage.h"

namespace cm {

/// 从一个文件名里解析出来的信息
struct NoteFile {
    QString srcPath;
    QString fileName;
    qint64  timestampMs = 0;
    int     page = 0;       ///< 「第N页」里的 N；没有就是 0
    bool    parsed = false; ///< 时间戳没解析出来时为 false
};

/// 支持两种命名（都是实测素材里出现的）：
///     20241109180213 第1页.png     白板软件导出，14 位时间戳 + 页码
///     2025-06-23 22 13 19.png     普通截图，冒号被文件名规则换成了空格
bool parseNoteFileName(const QString &fileName, NoteFile *out);

struct NoteClassified {
    NoteFile file;
    Classification cls;
};

struct NoteOrganizeOptions {
    QString sourceDir;
    QString outputDir;
    /// 忽略缓存重新调用模型。默认 false —— 重跑既费钱又可能得到不同结果。
    bool forceReclassify = false;
};

struct NoteOrganizeProgress {
    int done = 0;
    int total = 0;
    QString message;
};

struct NoteOrganizeResult {
    int total = 0;
    int succeeded = 0;
    int failed = 0;
    int copied = 0;
    int dayCount = 0;
    /// 从这天的旧索引里接回来的记录数（增量累加，不是本次判断的）
    int carriedOver = 0;
    /// 本次**真正调用模型**的张数 —— 只有这些花了 token
    int identified = 0;
    /// 命中 _classify_cache.json、没花钱的张数
    int cachedHits = 0;
    /// 本次实际消耗（走缓存的不计）
    int promptTokens = 0;
    int completionTokens = 0;
    QStringList dayDirs;
    QString error;

    bool ok() const { return error.isEmpty(); }
};

NoteOrganizeResult organizeNoteFolder(
    const NoteOrganizeOptions &opt,
    const std::function<void(const NoteOrganizeProgress &)> &onProgress = {});

} // namespace cm
