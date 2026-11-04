#include "ai/note_organizer.h"

#include "ai/archiver.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>

namespace cm {

namespace {

// 学科归一到「其他」的规则由 archiver 统一提供（normalizeSubjectName /
// kUnknownDir）。**不要再在本地写一份** —— 两份实现迟早会不一致，
// 而且中文字符串比较一旦用错 QLatin1String 就会静默失效。
const QString kUnknownTopic = QStringLiteral("未识别");

} // namespace

bool parseNoteFileName(const QString &fileName, NoteFile *out)
{
    out->fileName = fileName;
    out->srcPath.clear();
    out->timestampMs = 0;
    out->page = 0;
    out->parsed = false;

    const QString base = QFileInfo(fileName).completeBaseName();

    // 形式一：20241109180213 第1页   （白板软件导出）
    static const QRegularExpression reExport(
        QStringLiteral(R"(^(\d{4})(\d{2})(\d{2})(\d{2})(\d{2})(\d{2})\s*第(\d+)页)"));
    QRegularExpressionMatch m = reExport.match(base);
    if (m.hasMatch()) {
        const QDateTime dt(QDate(m.captured(1).toInt(), m.captured(2).toInt(),
                                 m.captured(3).toInt()),
                           QTime(m.captured(4).toInt(), m.captured(5).toInt(),
                                 m.captured(6).toInt()));
        if (dt.isValid()) {
            out->timestampMs = dt.toMSecsSinceEpoch();
            out->page = m.captured(7).toInt();
            out->parsed = true;
            return true;
        }
    }

    // 形式二：2025-06-23 22 13 19   （普通截图，冒号被换成了空格）
    static const QRegularExpression reShot(
        QStringLiteral(R"(^(\d{4})-(\d{2})-(\d{2})[ _]+(\d{2})[ _](\d{2})[ _](\d{2}))"));
    m = reShot.match(base);
    if (m.hasMatch()) {
        const QDateTime dt(QDate(m.captured(1).toInt(), m.captured(2).toInt(),
                                 m.captured(3).toInt()),
                           QTime(m.captured(4).toInt(), m.captured(5).toInt(),
                                 m.captured(6).toInt()));
        if (dt.isValid()) {
            out->timestampMs = dt.toMSecsSinceEpoch();
            out->parsed = true;
            return true;
        }
    }

    // 兜底：拿文件自身的修改时间，总比丢掉强
    return false;
}

// ------------------------------------------------------------------ 主流程

namespace {

/// 一天一个文件夹里，一个学科下的一组图
struct Group {
    QString day;        // yyyy-MM-dd
    QString subject;    // 已归一化（空/其他/未分类 -> 其他，见 normalizeSubjectName）
    QList<NoteClassified> items;
};

} // namespace

NoteOrganizeResult organizeNoteFolder(
    const NoteOrganizeOptions &opt,
    const std::function<void(const NoteOrganizeProgress &)> &onProgress)
{
    NoteOrganizeResult result;

    QDir src(opt.sourceDir);
    if (!src.exists()) {
        result.error = QStringLiteral("找不到素材目录：%1").arg(opt.sourceDir);
        return result;
    }

    // ---- 1. 扫描并解析文件名 ----
    QStringList filters{QStringLiteral("*.png"), QStringLiteral("*.jpg"),
                        QStringLiteral("*.jpeg"), QStringLiteral("*.bmp")};
    QList<NoteFile> files;
    for (const QFileInfo &fi : src.entryInfoList(filters, QDir::Files, QDir::Name)) {
        NoteFile nf;
        parseNoteFileName(fi.fileName(), &nf);
        nf.srcPath = fi.absoluteFilePath();
        if (!nf.parsed) {
            // 解析不出时间戳就用文件修改时间，至少能分到正确的一天
            nf.timestampMs = fi.lastModified().toMSecsSinceEpoch();
        }
        files.append(nf);
    }
    if (files.isEmpty()) {
        result.error = QStringLiteral("目录里没有图片");
        return result;
    }
    std::sort(files.begin(), files.end(), [](const NoteFile &a, const NoteFile &b) {
        return a.timestampMs < b.timestampMs;
    });
    result.total = files.size();

    // ---- 2. 逐张判断（带缓存）----
    //
    // **缓存必须整份读回来、合并之后再写回。**
    // 早先是开 Truncate 只写"本次新判成功的条目"，会把之前判好的丢掉 ——
    // 一次部分失败之后重跑，缓存里只剩这次成功的，下一次整批重新送模型。
    // 和 organizer.cpp 里那处是同一个坑，改一处别忘了这一处。
    const QString cachePath =
        QDir(opt.outputDir).filePath(QStringLiteral("_classify_cache.json"));
    QJsonObject cacheAll;       // 完整集合：已有 + 本次新判的
    if (!opt.forceReclassify) {
        QFile cf(cachePath);
        if (cf.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(cf.readAll());
            if (doc.isObject()) {
                cacheAll = doc.object();
            }
        }
    }

    Classifier classifier(loadAiConfig());
    QList<NoteClassified> classified;
    bool cacheChanged = false;
    int done = 0;

    for (const NoteFile &nf : files) {
        Classification c;
        const QJsonValue hit = cacheAll.value(nf.fileName);
        if (!opt.forceReclassify && hit.isObject()) {
            // 判过了 —— 复用，不调模型，不花钱
            const QJsonObject v = hit.toObject();
            c.subject = v.value(QStringLiteral("subject")).toString();
            c.subjectConfidence =
                v.value(QStringLiteral("subject_confidence")).toString();
            c.topic = v.value(QStringLiteral("topic")).toString();
            c.topicConfidence =
                v.value(QStringLiteral("topic_confidence")).toString();
            for (const QJsonValue &k :
                 v.value(QStringLiteral("keywords")).toArray()) {
                c.subjectKeywords << k.toString();
            }
            ++result.cachedHits;
        } else {
            c = classifier.classify(nf.srcPath);
            // 只有请求真的发出去、拿到响应，才算"花了钱"
            if (c.requestSent) {
                ++result.identified;
            }
            result.promptTokens += c.promptTokens;
            result.completionTokens += c.completionTokens;
        }

        if (c.ok()) {
            ++result.succeeded;
            QJsonObject v;
            v[QStringLiteral("subject")] = c.subject;
            v[QStringLiteral("subject_confidence")] = c.subjectConfidence;
            v[QStringLiteral("topic")] = c.topic;
            v[QStringLiteral("topic_confidence")] = c.topicConfidence;
            QJsonArray kw;
            for (const QString &k : c.subjectKeywords) {
                kw.append(k);
            }
            v[QStringLiteral("keywords")] = kw;
            cacheAll[nf.fileName] = v;
            cacheChanged = true;
        } else {
            ++result.failed;
        }

        NoteClassified nc;
        nc.file = nf;
        nc.cls = c;
        classified.append(nc);

        if (onProgress) {
            onProgress({++done, result.total,
                        QStringLiteral("%1 → %2 · %3")
                            .arg(nf.fileName, normalizeSubjectName(c.subject),
                                 c.topic.isEmpty() ? kUnknownTopic : c.topic)});
        }
    }

    // 记账：token 在 classify() 里就已经花掉了，归档成不成功都要记上
    {
        UsageBucket used;
        used.promptTokens = result.promptTokens;
        used.completionTokens = result.completionTokens;
        used.frames = result.identified;
        used.calls = result.identified;     // 目前一张图一次调用
        recordUsage(used);
    }

    // 写回**完整集合**（已有 + 本次新判），不是只有本次新判的
    if (cacheChanged && QDir().mkpath(opt.outputDir)) {
        QSaveFile cf(cachePath);
        if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            cf.write(QJsonDocument(cacheAll).toJson(QJsonDocument::Indented));
            if (!cf.commit()) {
                qWarning() << "识别缓存写入失败，下次会重新识别:" << cachePath;
            }
        }
    }

    // ---- 3. 按「日期 / 学科」分组 ----
    QMap<QString, QMap<QString, QList<NoteClassified>>> byDaySubject;
    for (const NoteClassified &nc : classified) {
        const QString day = QDateTime::fromMSecsSinceEpoch(nc.file.timestampMs)
                                .toString(QStringLiteral("yyyy-MM-dd"));
        byDaySubject[day][normalizeSubjectName(nc.cls.subject)].append(nc);
    }

    // ---- 4. 落盘 ----
    for (auto dayIt = byDaySubject.begin(); dayIt != byDaySubject.end(); ++dayIt) {
        const QString day = dayIt.key();
        const QString dayDir = QDir(opt.outputDir).filePath(day);

        // 先把老归档修一下：raw/ 改名为 原图/、索引设成隐藏、
        // 删掉已废弃的 index.html 浏览页。
        // 必须在取 rawDirName() 之前调。
        fixupDayDir(dayDir);

        const QString rawSubDir = rawDirName(dayDir);
        const QString rawDir = QDir(dayDir).filePath(rawSubDir);
        if (!QDir().mkpath(rawDir)) {
            result.error = QStringLiteral("无法创建目录：%1").arg(rawDir);
            return result;
        }

        // 学科文件夹每次重建，**原图目录绝不动**。
        // 不重建的话，同一张图前后判出不同的知识点名，学科文件夹里会
        // 同时留着两份（旧名 + 新名）。原图只写一次、之后只读，这条铁律不变。
        clearDerivedFolders(dayDir);

        QJsonArray dayJson;

        for (auto subIt = dayIt.value().begin(); subIt != dayIt.value().end(); ++subIt) {
            Group g;
            g.day = day;
            g.subject = subIt.key();
            g.items = subIt.value();
            std::sort(g.items.begin(), g.items.end(),
                      [](const NoteClassified &a, const NoteClassified &b) {
                          if (a.file.timestampMs != b.file.timestampMs) {
                              return a.file.timestampMs < b.file.timestampMs;
                          }
                          return a.file.page < b.file.page;   // 同一批导出按页码
                      });

            const QString subjectDir = QDir(dayDir).filePath(g.subject);
            QDir().mkpath(subjectDir);

            int seq = 0;
            for (const NoteClassified &nc : g.items) {
                const QDateTime dt = QDateTime::fromMSecsSinceEpoch(nc.file.timestampMs);
                const QString stamp = dt.toString(QStringLiteral("HHmmss"));
                ++seq;

                // 原图进 原图/，保持原名（只写一次，之后只读）
                const QString rawName = nc.file.fileName;
                const QString rawPath = QDir(rawDir).filePath(rawName);
                linkOrCopyFile(nc.file.srcPath, rawPath);

                // 学科文件夹里的名字：<序号>_<知识点>.png
                //
                // 序号在同一天、同一学科内**连续编号**，而且放在最前面 ——
                // 这样文件夹按名称排序就是上课的先后顺序，一眼数得出"第几张"。
                //
                // 早先的写法是 <时刻>_<序号>_<知识点>，时刻排在最前面，
                // 一天里录了两次课就变成 01、02…08、01、02…，
                // 读起来完全不像 1、2、3、4。时刻没丢，在 index.json 里有。
                QString topic = sanitizeFileName(nc.cls.topic, QString());
                if (topic.isEmpty() || topic == kUnknownTopic) {
                    topic = kUnknownTopic;
                }
                const QString base = QStringLiteral("%1_%2")
                                         .arg(seq, 2, 10, QLatin1Char('0'))
                                         .arg(topic);
                const QString ext = QLatin1Char('.') + QFileInfo(rawName).suffix().toLower();
                const QString friendly = base + ext;
                linkOrCopyFile(rawPath, QDir(subjectDir).filePath(friendly));
                ++result.copied;

                QJsonObject o;
                o[QStringLiteral("time")] = dt.toString(QStringLiteral("HH:mm:ss"));
                o[QStringLiteral("timestamp")] = double(nc.file.timestampMs);
                o[QStringLiteral("source")] = rawName;
                o[QStringLiteral("saved_as")] = friendly;
                o[QStringLiteral("subject")] = g.subject;
                o[QStringLiteral("subject_confidence")] = nc.cls.subjectConfidence;
                o[QStringLiteral("topic")] = nc.cls.topic;
                o[QStringLiteral("topic_confidence")] = nc.cls.topicConfidence;
                o[QStringLiteral("page")] = nc.file.page;
                QJsonArray kw;
                for (const QString &k : nc.cls.subjectKeywords) {
                    kw.append(k);
                }
                o[QStringLiteral("keywords")] = kw;
                dayJson.append(o);
            }
        }

        // ---- 增量累加：把这一天早先整理过的记录接回来 ----
        //
        // 不接的话，第二次整理会把这天的索引**整个覆盖掉** ——
        // 实测现象是 原图/ 里躺着 4 张图、索引里只剩 2 张，
        // 另外 2 张从此在学科文件夹里看不到了。
        // 键取原图名（source），所以同一张图重复整理只覆盖自己。
        {
            QFile f(QDir(dayDir).filePath(QStringLiteral("index.json")));
            if (f.open(QIODevice::ReadOnly)) {
                QSet<QString> thisBatch;
                for (const QJsonValue &v : dayJson) {
                    thisBatch.insert(v.toObject()
                                         .value(QStringLiteral("source")).toString());
                }
                const QJsonArray old =
                    QJsonDocument::fromJson(f.readAll()).object()
                        .value(QStringLiteral("keyframes")).toArray();
                int carried = 0;
                for (const QJsonValue &v : old) {
                    const QJsonObject o = v.toObject();
                    const QString src = o.value(QStringLiteral("source")).toString();
                    const QString savedAs = o.value(QStringLiteral("saved_as")).toString();
                    if (src.isEmpty() || savedAs.isEmpty() || thisBatch.contains(src)) {
                        continue;
                    }
                    // 原图还在 原图/ 里；按它原来的名字重建硬链接
                    const QString rawPath = QDir(rawDir).filePath(src);
                    if (!QFile::exists(rawPath)) {
                        continue;
                    }
                    const QString subject =
                        sanitizeFileName(normalizeSubjectName(
                                             o.value(QStringLiteral("subject")).toString()),
                                         kUnknownDir, 20);
                    const QString subjectDir = QDir(dayDir).filePath(subject);
                    QDir().mkpath(subjectDir);
                    linkOrCopyFile(rawPath, QDir(subjectDir).filePath(savedAs));
                    dayJson.append(o);
                    ++carried;
                }
                if (carried > 0) {
                    result.carriedOver += carried;
                }
            }
        }

        QJsonObject head;
        head[QStringLiteral("date")] = day;
        head[QStringLiteral("count")] = dayJson.size();
        head[QStringLiteral("keyframes")] = dayJson;
        const QString jsonPath = QDir(dayDir).filePath(QStringLiteral("index.json"));
        QSaveFile jf(jsonPath);
        if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            jf.write(QJsonDocument(head).toJson(QJsonDocument::Indented));
            jf.commit();
        }
        markHidden(jsonPath);       // 写完再设，见 markHidden 的说明

        // 这里原来还会生成一个双击就能看的 index.html 浏览页。**功能已删除**
        // （CSS 里的 `width:100%%` 不合法，缩略图会被裁掉一半；维护者决定
        // 不修、直接去掉）。老归档里的 index.html 由 fixupDayDir() 顺手删掉。
        result.dayDirs << dayDir;
        ++result.dayCount;
    }

    return result;
}

} // namespace cm
