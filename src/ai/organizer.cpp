#include "ai/organizer.h"

#include "ai/classifier.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace cm {

namespace {

struct FrameEntry {
    QString file;
    qint64 timestampMs = 0;
    QString reason;
};

/// 把缓存里的 JSON 还原成判断结果。
///
/// 刻意**不还原 token 字段** —— 缓存命中意味着这次没有发请求、没有产生 token，
/// 保持 0 才能让"本次花了多少"算对。账本见 usage.h。
Classification classificationFromJson(const QJsonObject &o)
{
    Classification c;
    c.subject = o.value(QStringLiteral("subject")).toString();
    c.subjectConfidence =
        o.value(QStringLiteral("subject_confidence")).toString();
    c.topic = o.value(QStringLiteral("topic")).toString();
    c.topicConfidence = o.value(QStringLiteral("topic_confidence")).toString();
    for (const QJsonValue &k : o.value(QStringLiteral("keywords")).toArray()) {
        c.subjectKeywords << k.toString();
    }
    return c;
}

QJsonObject classificationToJson(const Classification &c)
{
    QJsonObject o;
    o[QStringLiteral("subject")] = c.subject;
    o[QStringLiteral("subject_confidence")] = c.subjectConfidence;
    o[QStringLiteral("topic")] = c.topic;
    o[QStringLiteral("topic_confidence")] = c.topicConfidence;
    QJsonArray kw;
    for (const QString &k : c.subjectKeywords) {
        kw.append(k);
    }
    o[QStringLiteral("keywords")] = kw;
    return o;
}

/// 读 session.json，取出关键帧清单
bool loadSession(const QString &sessionDir, QList<FrameEntry> *out, QString *error)
{
    const QString indexPath = QDir(sessionDir).filePath(QStringLiteral("session.json"));
    QFile f(indexPath);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("打不开 %1").arg(indexPath);
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) {
        *error = QStringLiteral("session.json 不是合法 JSON");
        return false;
    }

    const QJsonObject root = doc.object();
    const QDateTime started = QDateTime::fromString(
        root.value(QStringLiteral("started")).toString(), Qt::ISODate);

    for (const QJsonValue &v : root.value(QStringLiteral("frames")).toArray()) {
        const QJsonObject o = v.toObject();
        FrameEntry e;
        e.file = o.value(QStringLiteral("file")).toString();
        e.reason = o.value(QStringLiteral("reason")).toString();
        // 优先用记录里的时间；没有就退回到会话开始时间
        const QDateTime t = QDateTime::fromString(
            o.value(QStringLiteral("time")).toString(), Qt::ISODate);
        e.timestampMs = t.isValid()
                            ? t.toMSecsSinceEpoch()
                            : (started.isValid() ? started.toMSecsSinceEpoch() : 0);
        if (!e.file.isEmpty()) {
            out->append(e);
        }
    }

    if (out->isEmpty()) {
        *error = QStringLiteral("这个会话里没有关键帧");
        return false;
    }
    return true;
}

} // namespace

OrganizeResult organizeSession(const OrganizeOptions &opt,
                               const std::function<void(const OrganizeProgress &)> &onProgress,
                               const std::function<bool()> &isCancelled)
{
    OrganizeResult result;

    const QString sessionDir = QDir::cleanPath(opt.sessionDir);
    if (!QDir(sessionDir).exists()) {
        result.error = QStringLiteral("找不到会话目录：%1").arg(sessionDir);
        return result;
    }

    QList<FrameEntry> frames;
    QString err;
    if (!loadSession(sessionDir, &frames, &err)) {
        result.error = err;
        return result;
    }
    result.total = frames.size();

    const AiConfig cfg = loadAiConfig();
    Classifier classifier(cfg);
    result.usedStub = classifier.usingStub();

    // ---- 识别缓存 ----
    //
    // 缓存按**帧文件名**做键，是"只识别没识别过的"这条语义的判据。
    //
    // **必须把已有缓存整份读回来，再把本次新判的合并进去写回。**
    // 早先的写法是开 Truncate、只写"本次新判成功的条目"——
    // 那会把之前判好的条目一起丢掉：一次部分失败之后重跑，
    // 缓存里只剩这次成功的几张，下一次就把整批重新送模型。
    // 表现就是"越点越贵"，而且彻底破坏"只识别没识别过的"。
    //
    // forceReclassify（--reclassify）的语义是"忽略缓存重新判"，
    // 所以那种情况下从空集合开始，写回的就是本次的全新结果。
    const QString cachePath =
        QDir(sessionDir).filePath(QStringLiteral("classify.json"));
    QJsonObject cacheAll;       // 完整集合：已有 + 本次新判的
    if (!opt.forceReclassify) {
        QFile cf(cachePath);
        if (cf.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(cf.readAll());
            if (doc.isObject()) {
                cacheAll = doc.object();
            } else {
                qWarning() << "识别缓存不是合法 JSON，按空缓存处理:" << cachePath;
            }
        }
    }

    QList<ArchiveRecord> records;
    bool cacheChanged = false;
    int done = 0;

    for (const FrameEntry &fe : frames) {
        const QString path = QDir(sessionDir).filePath(fe.file);

        Classification c;
        const QJsonValue hit = cacheAll.value(fe.file);
        if (!opt.forceReclassify && hit.isObject()) {
            // 判过了 —— 复用，不调模型，不花钱
            c = classificationFromJson(hit.toObject());
            ++result.cachedHits;
            ++result.succeeded;
            if (onProgress) {
                onProgress({++done, result.total,
                            QStringLiteral("%1 → %2 · %3（复用缓存，不消耗额度）")
                                .arg(fe.file, c.subject, c.topic)});
            }
        } else {
            c = classifier.classify(path, isCancelled);
            // 只有请求真的发出去、拿到响应，才算"花了钱"。
            // 没配 Key、图片读不出来、超时这些情况不算 —— 见 requestSent 的说明。
            if (c.requestSent) {
                ++result.identified;
            }
            result.promptTokens += c.promptTokens;
            result.completionTokens += c.completionTokens;

            // 用户取消：立刻停。**不归档、不标记**，但下面照常把缓存写掉，
            // 所以下次再整理会从断点继续（已判的帧秒过、不花钱）。
            if (c.cancelled) {
                result.cancelled = true;
                result.stopReason = QStringLiteral("已取消");
                break;
            }

            // 根本性错误（Key 无效 / 余额用完 / 地址模型名错 / 模型不能看图）：
            // 剩下的图再试也是白等 6 秒退避，而且**不能**把这个会话标记成
            // 「已整理」—— 否则老师充值之后这节课永远是「未识别」，
            // 再也不会被任何入口自动重试。所以在这里停下。
            if (c.fatal) {
                result.aborted = true;
                result.stopReason = c.error;
                break;
            }

            if (c.ok()) {
                ++result.succeeded;
                cacheAll[fe.file] = classificationToJson(c);
                cacheChanged = true;
            } else {
                ++result.failed;
                if (result.firstError.isEmpty()) {
                    result.firstError = c.error;
                }
            }

            if (onProgress) {
                onProgress({++done, result.total,
                            c.ok() ? QStringLiteral("%1 → %2 · %3")
                                         .arg(fe.file, c.subject, c.topic)
                                   : QStringLiteral("%1 判断失败：%2")
                                         .arg(fe.file, c.error)});
            }
        }

        ArchiveRecord r;
        r.srcPath = path;
        r.timestampMs = fe.timestampMs;
        r.subject = c.subject;
        r.subjectConfidence = c.subjectConfidence;
        r.keywords = c.subjectKeywords;
        r.topic = c.topic;
        r.topicConfidence = c.topicConfidence;
        r.reason = fe.reason;
        // 记下来自哪个会话 —— archiver 靠它区分「同一张图重新整理」
        // 和「两次课的帧重名」，重名时才避让，不重名时复用原名
        r.sessionId = QFileInfo(sessionDir).fileName();
        records.append(r);
    }

    // ---- 先记账，再归档 ----
    //
    // 顺序不能反：token 是在上面 classify() 里就已经花掉的。
    // 万一归档失败（磁盘满、目标目录没权限），已经花掉的额度不该
    // 从账本里消失 —— 那会让老师看到的花费比真实值小。
    {
        UsageBucket used;
        used.promptTokens = result.promptTokens;
        used.completionTokens = result.completionTokens;
        used.frames = result.identified;
        used.calls = result.identified;     // 目前一张图一次调用
        recordUsage(used);
    }

    // ---- 识别缓存落盘 ----
    //
    // 写回的是**完整集合**（已有 + 本次新判），不是只有本次新判的。
    // 只有真的产生了新条目才写，避免无谓地刷新会话目录的时间戳。
    if (cacheChanged) {
        QSaveFile cf(cachePath);
        if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            cf.write(QJsonDocument(cacheAll).toJson(QJsonDocument::Indented));
            if (!cf.commit()) {
                qWarning() << "识别缓存写入失败，下次会重新识别:" << cachePath;
            }
        }
    }

    // ---- 中途停了就到此为止：不归档 ----
    //
    // 刻意**不归档**：把已经判好的那半截写进日期目录，会让归档里多出一批
    // 归到「其他」的帧，而下次续做时它们又要被重新整理一遍。
    // 缓存已经落盘了，续做时会直接命中，所以"不归档"不损失任何已完成的工作。
    if (result.aborted || result.cancelled) {
        return result;
    }

    result.archive = archiveFrames(opt.archiveRoot, records);
    if (!result.archive.ok()) {
        result.error = result.archive.error;
    }
    return result;
}

// ------------------------------------------------------------------ 定时整理

namespace {
const QString kMarkerName = QStringLiteral("organized.json");
}

bool isOrganized(const QString &sessionDir)
{
    QFile f(QDir(sessionDir).filePath(kMarkerName));
    if (!f.open(QIODevice::ReadOnly)) {
        return false;       // 没有标记 = 从来没整理过
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    f.close();

    // **必须拿标记里记的帧数和会话当前的帧数比，光看文件存不存在是不行的。**
    //
    // 老师在**录制中途**整理过一次之后，标记就永久生效了：他接着写在白板上的
    // 每一页都再也不会进归档，而且界面只会说一句「没有需要整理的新记录」——
    // 看起来像"没反应"。（实测复现：2 帧时整理 -> 标记 frames=2；又写到 4 帧
    // 再点整理 -> 提示"没有待整理的录制记录"，归档里只有 2 张。）
    //
    // 帧数变多了就说明"这个会话又长了"，应当重新视为待整理。
    // 重新整理**不会重复花钱、也不会产生重复文件**：
    //   * 已判过的帧命中 classify.json 缓存，不调模型（见 organizeSession）
    //   * 归档是增量合并，键取原图名，同一帧只会覆盖自己（见 archiveFrames）
    const int organizedFrames = o.value(QStringLiteral("frames")).toInt(-1);
    if (organizedFrames < 0) {
        // 老标记没记帧数，无从判断 —— 保守当成「已整理」，不去重复调用模型
        return true;
    }

    QList<FrameEntry> frames;
    QString err;
    if (!loadSession(sessionDir, &frames, &err)) {
        // 会话读不出来（正在被写 / 已损坏）：不当成已整理，
        // 交给 findPendingSessions 里"一张关键帧都没有就跳过"那条去挡
        return false;
    }
    return organizedFrames >= frames.size();
}

/// frameCount 必须传**这次整理时这个会话有多少帧** —— isOrganized() 拿它来
/// 判断"会话是不是后来又长了"，所以传错（比如传本次成功判定的张数）会让
/// 增长判断失准。调用点传的是 r.total（会话里的关键帧总数）。
bool markOrganized(const QString &sessionDir, const QString &archiveRoot,
                   int frameCount)
{
    QJsonObject o;
    o[QStringLiteral("organized_at")] =
        QDateTime::currentDateTime().toString(Qt::ISODate);
    o[QStringLiteral("archive")] = archiveRoot;
    o[QStringLiteral("frames")] = frameCount;

    QSaveFile f(QDir(sessionDir).filePath(kMarkerName));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return f.commit();
}

// ------------------------------------------------------------------ 批量整理

OrganizeBatchResult organizePendingSessions(
    const QString &recordingsRoot,
    const QString &archiveRoot,
    bool forceReclassify,
    const std::function<void(const QString &, int, int, const OrganizeProgress &)>
        &onProgress,
    const std::function<bool()> &isCancelled)
{
    OrganizeBatchResult batch;

    // **正在录制的会话照样会被挑出来整理，这是有意的。**
    //
    // 老师完全可以在录制中途点一次「AI 整理」先看效果 —— 而且这是预期用法。
    // 录制中整理不会造成任何损坏：已判的帧走 classify.json 缓存（不花钱），
    // 归档是增量合并（不产生重复文件），而 organized.json 里记着"整理到第几帧"，
    // 所以会话后来又长了会被自动重新视为待整理（见 isOrganized）。
    const QStringList pending = findPendingSessions(recordingsRoot);
    batch.sessions = pending.size();
    if (pending.isEmpty()) {
        return batch;
    }

    for (int i = 0; i < pending.size(); ++i) {
        const QString &sessionDir = pending[i];
        const QString sessionName = QFileInfo(sessionDir).fileName();

        // 上一轮的取消/中止已经生效了？那就别再开新会话
        if (isCancelled && isCancelled()) {
            batch.cancelled = true;
            batch.stopReason = QStringLiteral("已取消");
            break;
        }

        OrganizeOptions opt;
        opt.sessionDir = sessionDir;
        opt.archiveRoot = archiveRoot;
        opt.forceReclassify = forceReclassify;

        const OrganizeResult r = organizeSession(
            opt, [&](const OrganizeProgress &p) {
                if (onProgress) {
                    onProgress(sessionName, i + 1, pending.size(), p);
                }
            },
            isCancelled);

        // ---- 先把这一轮的统计并进去，再决定要不要停 ----
        // （停了也要把已经花掉的 token 报出来，不能凭空消失）
        batch.succeeded += r.succeeded;
        batch.failed += r.failed;
        batch.identified += r.identified;
        batch.cachedHits += r.cachedHits;
        batch.promptTokens += r.promptTokens;
        batch.completionTokens += r.completionTokens;
        batch.usedStub = batch.usedStub || r.usedStub;
        if (batch.firstError.isEmpty()) {
            batch.firstError = r.firstError;
        }
        for (const QString &d : r.archive.dayDirs) {
            if (!batch.dayDirs.contains(d)) {
                batch.dayDirs << d;
            }
        }

        // ---- 中途停了：**不标记**，并且整批不再往下做 ----
        //
        // 关键就在"不写 organized.json"：
        //   * 根本性错误（余额用完 / Key 无效）—— 不标记，充值后再点一次
        //     就从断点继续，而不是让这节课永远留在「未识别」
        //   * 用户取消 —— 同理，"取消 + 再点一次 = 暂停/继续"，
        //     已判的帧在 classify.json 里，续做时秒过、不花钱
        // 已经做完的会话在上面各轮里已经标好了，保留。
        if (r.aborted || r.cancelled) {
            batch.aborted = r.aborted;
            batch.cancelled = r.cancelled;
            batch.stopReason = r.stopReason;
            break;
        }

        if (r.ok() && r.total > 0) {
            // 打上「整理过了」—— 这是"任何入口都只整理没整理过的"的落点。
            // 写失败也继续，只是下次会被再整理一遍（走缓存，不花钱）。
            if (!markOrganized(sessionDir, archiveRoot, r.total)) {
                qWarning() << "无法写入 organized.json:" << sessionDir;
                batch.errors << QStringLiteral("%1：整理标记写入失败")
                                    .arg(sessionName);
            }
            ++batch.sessionsOk;
        } else {
            // 归档失败 **不写标记** —— 留着下次重试。
            ++batch.sessionsFailed;
            batch.errors << QStringLiteral("%1：%2")
                                .arg(sessionName,
                                     r.error.isEmpty()
                                         ? QStringLiteral("没有关键帧")
                                         : r.error);
        }
    }

    return batch;
}

QStringList findPendingSessions(const QString &recordingsRoot)
{
    QStringList out;
    QDir dir(recordingsRoot);
    if (!dir.exists()) {
        return out;
    }

    QList<QPair<QDateTime, QString>> found;
    for (const QFileInfo &fi :
         dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString path = QDir::cleanPath(fi.absoluteFilePath());

        if (!QFile::exists(QDir(path).filePath(QStringLiteral("session.json")))) {
            continue;
        }
        // 已经整理**完**了就不重复调用模型 —— 既省额度，
        // 也避免同一份素材出两种结果。
        // 注意 isOrganized 比的是帧数：录制中途整理过的会话，
        // 后来又写了新内容就仍然算待整理，不会被漏掉。
        if (isOrganized(path)) {
            continue;
        }
        // 一张关键帧都没有的会话（老师开了软件但没上课）也跳过
        if (QDir(path)
                .entryList({QStringLiteral("*.png"), QStringLiteral("*.jpg")},
                           QDir::Files, QDir::Name)
                .isEmpty()) {
            continue;
        }
        found.append({fi.lastModified(), path});
    }

    std::sort(found.begin(), found.end(),
              [](const QPair<QDateTime, QString> &a,
                 const QPair<QDateTime, QString> &b) { return a.first < b.first; });
    for (const auto &p : found) {
        out << p.second;
    }
    return out;
}

} // namespace cm
