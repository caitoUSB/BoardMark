#include "ai/archiver.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace cm {

// 所有"没判出学科"的内容都归到这一个文件夹。
// 早先叫 `_未分类`，现在统一叫「其他」—— 和模型返回、界面上看到的字一致。
const QString kUnknownDir = QStringLiteral("其他");
// 归档是给老师看的，目录名用中文。
// 老归档里是 "raw"，见 kLegacyRawDir 的说明。
const QString kRawDir = QStringLiteral("原图");
const QString kLegacyRawDir = QStringLiteral("raw");

QString normalizeSubjectName(const QString &s)
{
    const QString t = s.trimmed();
    // 注意：这几个比较**必须**用 QStringLiteral。
    // 换成 QLatin1String 会按 Latin-1 解释中文，永远比不中，归一化静默失效。
    if (t.isEmpty()
        || t == QStringLiteral("其他")
        || t == QStringLiteral("未分类")
        || t == QStringLiteral("未识别")) {
        return kUnknownDir;
    }
    return t;
}

QString sanitizeFileName(const QString &name, const QString &fallback, int maxLen)
{
    static const QRegularExpression illegal(
        QStringLiteral(R"([\\/:*?"<>|\r\n\t]+)"));
    QString s = name;
    s.replace(illegal, QStringLiteral("_"));
    s = s.trimmed();
    while (s.startsWith(QLatin1Char('.'))) {
        s.remove(0, 1);
    }
    s.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral(" "));
    if (s.isEmpty()) {
        s = fallback;
    }
    return s.left(maxLen);
}

QString linkOrCopyFile(const QString &src, const QString &dst)
{
    if (QFile::exists(dst)) {
        return QStringLiteral("exists");
    }
#ifdef _WIN32
    if (CreateHardLinkW(reinterpret_cast<const wchar_t *>(dst.utf16()),
                        reinterpret_cast<const wchar_t *>(src.utf16()),
                        nullptr)) {
        return QStringLiteral("link");
    }
#endif
    if (QFile::copy(src, dst)) {
        return QStringLiteral("copy");
    }
    return QStringLiteral("fail");
}

void markHidden(const QString &path)
{
#ifdef _WIN32
    if (!QFile::exists(path)) {
        return;
    }
    // 保留原有属性，只把 HIDDEN 位加上去 —— 直接覆盖属性会把只读等标记抹掉
    const DWORD attrs = GetFileAttributesW(
        reinterpret_cast<const wchar_t *>(path.utf16()));
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    SetFileAttributesW(reinterpret_cast<const wchar_t *>(path.utf16()),
                       attrs | FILE_ATTRIBUTE_HIDDEN);
#else
    Q_UNUSED(path);
#endif
}

QString rawDirName(const QString &dayDir)
{
    const QDir d(dayDir);
    if (QDir(d.filePath(kRawDir)).exists()) {
        return kRawDir;
    }
    if (QDir(d.filePath(kLegacyRawDir)).exists()) {
        return kLegacyRawDir;
    }
    return kRawDir;     // 都还没有 -> 新建，用中文名
}

void fixupDayDir(const QString &dayDir)
{
    QDir d(dayDir);
    if (!d.exists()) {
        return;
    }

    // ---- 1. 老版本的 raw/ 改名成 原图/ ----
    //
    // 只在"中文名还不存在"时才改，避免把已有的 原图/ 覆盖掉。
    // 改名失败（图被别的程序打开、磁盘只读）就只告警：
    // rawDirName() 会继续用老名字读，归档照常能跑，原图一张都不会丢。
    const QString legacy = d.filePath(kLegacyRawDir);
    const QString modern = d.filePath(kRawDir);
    if (QDir(legacy).exists() && !QDir(modern).exists()) {
        if (QDir().rename(legacy, modern)) {
            qInfo() << "原图目录已改名为" << kRawDir << ":" << modern;
        } else {
            qWarning() << "原图目录改名失败，继续沿用旧名" << kLegacyRawDir << ":"
                       << legacy;
        }
    }

    // ---- 2. 索引文件藏起来 ----
    markHidden(d.filePath(QStringLiteral("index.json")));

    // ---- 3. 清掉已废弃的 index.html ----
    //
    // 浏览页（index.html）这个功能已经删掉了，只剩 index.json。老归档里
    // 还留着当年生成的 index.html（隐藏属性），而且它的 CSS 是坏的
    // （`width:100%%` 不是合法值，浏览器会丢掉这条声明，缩略图被裁成一半）。
    // 留着只会让老师点开看到一个坏页面，所以顺手删掉。
    //
    // **只删这一个确定的文件名**，不用通配符、不递归 —— 万一老师自己在
    // 同目录放了别的东西，一律不碰。
    const QString legacyHtml = d.filePath(QStringLiteral("index.html"));
    if (QFile::exists(legacyHtml)) {
        // 隐藏属性不影响删除，但删除前先确认它确实是我们的产物再动手
        if (QFile::remove(legacyHtml)) {
            qInfo() << "已删除废弃的浏览页:" << legacyHtml;
        } else {
            qWarning() << "删除废弃的浏览页失败（图可能被占用）:" << legacyHtml;
        }
    }
}

void clearDerivedFolders(const QString &dayDir)
{
    QDir d(dayDir);
    if (!d.exists()) {
        return;
    }
    const QFileInfoList subs =
        d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &fi : subs) {
        // 原图目录是原图的存放处，**绝对不动**。
        //
        // 老名字 raw/ 也要一起保护：老归档还没被 fixupDayDir 改名时
        // 它就是原图目录，漏掉它等于第一次重新整理就把老师的原图全删了。
        // 其余子目录都是程序按判断结果生成的，可以安全重建。
        const QString name = fi.fileName();
        if (name.compare(kRawDir, Qt::CaseInsensitive) == 0
            || name.compare(kLegacyRawDir, Qt::CaseInsensitive) == 0) {
            continue;
        }
        QDir(fi.absoluteFilePath()).removeRecursively();
    }
}

ArchiveResult archiveFrames(const QString &root, const QList<ArchiveRecord> &records)
{
    ArchiveResult result;

    if (records.isEmpty()) {
        result.error = QStringLiteral("没有可归档的关键帧");
        return result;
    }

    // 按日期分组。日期来自时间戳，100% 准确，不需要 AI 猜。
    QMap<QString, QList<ArchiveRecord>> byDay;
    for (const ArchiveRecord &r : records) {
        const QString day = QDateTime::fromMSecsSinceEpoch(r.timestampMs)
                                .toString(QStringLiteral("yyyy-MM-dd"));
        byDay[day].append(r);
    }

    for (auto it = byDay.begin(); it != byDay.end(); ++it) {
        const QString day = it.key();
        const QString dayDir = QDir(root).filePath(day);

        // 先把老归档修一下：raw/ 改名为 原图/、索引设成隐藏、
        // 删掉已废弃的 index.html 浏览页。
        // 必须在取 rawDirName() 之前调 —— 否则这一天还会被当成"老归档"处理。
        fixupDayDir(dayDir);

        const QString rawSubDir = rawDirName(dayDir);
        const QString rawDir = QDir(dayDir).filePath(rawSubDir);
        if (!QDir().mkpath(rawDir)) {
            result.error = QStringLiteral("无法创建目录：%1").arg(rawDir);
            return result;
        }

        // ---- 增量累加：先把这一天已经归档过的记录读回来 ----
        //
        // 不这么做的话，同一天第二次整理会把第一次的记录**整个覆盖掉** ——
        // 原图还在 原图/ 里，但索引里再也看不到它们了。
        // 键取原图文件名（由时间戳生成），所以同一帧重复整理只会覆盖自己，
        // 不会产生第二份。
        QMap<QString, QJsonObject> merged;
        {
            QFile f(QDir(dayDir).filePath(QStringLiteral("index.json")));
            if (f.open(QIODevice::ReadOnly)) {
                const QJsonArray old =
                    QJsonDocument::fromJson(f.readAll()).object()
                        .value(QStringLiteral("keyframes")).toArray();
                for (const QJsonValue &v : old) {
                    const QJsonObject o = v.toObject();
                    const QString key = o.value(QStringLiteral("file")).toString();
                    if (!key.isEmpty()) {
                        merged.insert(key, o);
                    }
                }
            }
        }

        // 学科文件夹是按判断结果生成的，每次重建；
        // **原图目录绝不动**（原图只写一次、之后只读）。
        clearDerivedFolders(dayDir);

        QList<ArchiveRecord> items = it.value();
        std::sort(items.begin(), items.end(),
                  [](const ArchiveRecord &a, const ArchiveRecord &b) {
                      return a.timestampMs < b.timestampMs;
                  });

        // ---- 写入本次的记录（覆盖同日同名的旧记录）----
        //
        // 重名处理：原图文件名只用 HHMMSS，同一天两次课的帧理论上会撞在同一秒上。
        // 撞了要避让，**但同一个会话重新整理不算撞车** —— 那是同一张图，
        // 应该复用原名，不能每整理一次就多出 _2、_3。
        // 所以判断依据是记录里存的 sessionId，而不是"名字有没有被用过"。
        QHash<QString, QString> nameOwner;   // 原图文件名 -> sessionId
        for (auto mit = merged.begin(); mit != merged.end(); ++mit) {
            nameOwner.insert(mit.key(),
                             mit.value().value(QStringLiteral("session")).toString());
        }

        for (const ArchiveRecord &r : items) {
            const QDateTime dt = QDateTime::fromMSecsSinceEpoch(r.timestampMs);
            const QString stamp = dt.toString(QStringLiteral("HHmmss"));
            const QString ext =
                QFileInfo(r.srcPath).suffix().isEmpty()
                    ? QStringLiteral(".png")
                    : QLatin1Char('.') + QFileInfo(r.srcPath).suffix().toLower();

            // 原图进 原图/，只写一次
            QString rawName = stamp + ext;
            for (int n = 2;; ++n) {
                const QString owner = nameOwner.value(rawName);
                if (owner.isEmpty() || owner == r.sessionId) {
                    break;      // 没人占，或者就是本会话自己的，可以复用
                }
                rawName = QStringLiteral("%1_%2%3").arg(stamp).arg(n).arg(ext);
            }
            nameOwner.insert(rawName, r.sessionId);

            const QString rawPath = QDir(rawDir).filePath(rawName);
            const QString how = linkOrCopyFile(r.srcPath, rawPath);
            if (how == QLatin1String("link")) {
                ++result.linked;
            } else if (how == QLatin1String("copy")) {
                ++result.copied;
            } else if (how == QLatin1String("fail")) {
                result.error = QStringLiteral("无法写入：%1").arg(rawPath);
                return result;
            }

            QJsonObject o;
            o[QStringLiteral("time")] = dt.toString(QStringLiteral("HH:mm:ss"));
            o[QStringLiteral("timestamp")] = double(r.timestampMs);
            o[QStringLiteral("file")] = rawName;
            o[QStringLiteral("subject")] = r.subject;
            o[QStringLiteral("subject_confidence")] = r.subjectConfidence;
            o[QStringLiteral("topic")] = r.topic;
            o[QStringLiteral("topic_confidence")] = r.topicConfidence;
            o[QStringLiteral("reason")] = r.reason;
            if (!r.sessionId.isEmpty()) {
                o[QStringLiteral("session")] = r.sessionId;
            }
            if (!r.supersededBy.isEmpty()) {
                o[QStringLiteral("superseded_by")] = r.supersededBy;
            }
            QJsonArray kw;
            for (const QString &k : r.keywords) {
                kw.append(k);
            }
            o[QStringLiteral("keywords")] = kw;
            merged.insert(rawName, o);
        }

        // ---- 用合并后的完整集合重建学科文件夹 ----
        QList<QJsonObject> dayRecords = merged.values();
        std::sort(dayRecords.begin(), dayRecords.end(),
                  [](const QJsonObject &a, const QJsonObject &b) {
                      return a.value(QStringLiteral("timestamp")).toDouble()
                             < b.value(QStringLiteral("timestamp")).toDouble();
                  });

        // 每个学科各自从 01 开始连续编号
        QHash<QString, int> seqBySubject;

        for (const QJsonObject &o : dayRecords) {
            const QString rawName = o.value(QStringLiteral("file")).toString();
            const QString rawPath = QDir(rawDir).filePath(rawName);
            if (!QFile::exists(rawPath) || !o.value(QStringLiteral("superseded_by"))
                                                .toString().isEmpty()) {
                if (!o.value(QStringLiteral("superseded_by")).toString().isEmpty()) {
                    ++result.superseded;
                }
                continue;   // 被顶替的或原图不在的，不进学科文件夹
            }

            // 先把学科名归一化，再 sanitize。
            // 顺序不能颠倒：sanitizeFileName 会把非法字符换成下划线，
            // 万一学科名里带了特殊字符，归一化时的字符串比较就再也匹配不上了。
            const QString rawSubject =
                o.value(QStringLiteral("subject")).toString();
            QString subject = sanitizeFileName(normalizeSubjectName(rawSubject),
                                               QString(), 20);
            if (subject == kUnknownDir) {
                // 归一化之后还是「其他」，说明这一帧没判出学科
                ++result.unknownSubject;
            }

            const QString topic =
                sanitizeFileName(o.value(QStringLiteral("topic")).toString(), QString());
            const QString subjectDir = QDir(dayDir).filePath(subject);
            QDir().mkpath(subjectDir);

            // 文件名 = <序号>_<知识点>.png
            //
            // 序号在同一天、同一学科内**连续编号**，不按时间戳分段落重来 ——
            // 否则一天里录了两次课就会看到 01、02…08、01、02…，
            // 排在一起读不出"第几张"。
            // 序号放在最前面，文件夹按名称排序就是上课的先后顺序。
            //
            // 时刻不进文件名了，但没丢：index.json 里有。
            const int seq = ++seqBySubject[subject];
            const QString ext = QLatin1Char('.') + QFileInfo(rawName).suffix();
            const QString friendly =
                (topic.isEmpty() || topic == QStringLiteral("未识别"))
                    ? QStringLiteral("%1_未识别%2").arg(seq, 2, 10, QLatin1Char('0')).arg(ext)
                    : QStringLiteral("%1_%2%3")
                          .arg(seq, 2, 10, QLatin1Char('0'))
                          .arg(topic, ext);
            linkOrCopyFile(rawPath, QDir(subjectDir).filePath(friendly));
        }

        // ---- index.json ----
        QJsonObject head;
        head[QStringLiteral("date")] = day;
        head[QStringLiteral("count")] = dayRecords.size();
        QJsonArray arr;
        for (const QJsonObject &o : dayRecords) {
            arr.append(o);
        }
        head[QStringLiteral("keyframes")] = arr;

        const QString jsonPath = QDir(dayDir).filePath(QStringLiteral("index.json"));
        QSaveFile jf(jsonPath);
        if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            jf.write(QJsonDocument(head).toJson(QJsonDocument::Indented));
            jf.commit();
        }
        markHidden(jsonPath);       // 写完再设，见 markHidden 的说明

        // 这里原来还会生成一个双击就能看的 index.html 浏览页。**功能已删除**：
        // 它的 CSS 是坏的（`width:100%%` 不合法 -> 缩略图被裁掉一半），
        // 而维护者决定不修、直接去掉这个功能。归档现在只有文件夹结构和
        // index.json。老归档里的 index.html 由 fixupDayDir() 顺手删掉。
        result.dayDirs << dayDir;
        ++result.dayCount;
    }

    return result;
}

} // namespace cm
