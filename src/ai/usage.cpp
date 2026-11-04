#include "ai/usage.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>

namespace cm {

namespace {

/// 只保留最近这么多个月。键是 "yyyy-MM"，字典序就是时间序，
/// 所以直接排序取尾巴即可，不需要解析成日期。
const int kKeepMonths = 24;

QString monthKeyOf(const QDateTime &dt)
{
    return dt.toString(QStringLiteral("yyyy-MM"));
}

QString dayKeyOf(const QDateTime &dt)
{
    return dt.toString(QStringLiteral("yyyy-MM-dd"));
}

QJsonObject loadRoot()
{
    QFile f(usageFilePath());
    if (!f.open(QIODevice::ReadOnly)) {
        return QJsonObject();       // 第一次运行，正常
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) {
        qWarning() << "用量账本不是合法 JSON，按空账本处理:" << usageFilePath();
        return QJsonObject();
    }
    return doc.object();
}

bool storeRoot(const QJsonObject &root)
{
    // 用 QSaveFile：写一半被强杀也不会把账本写坏
    QSaveFile f(usageFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "无法写入用量账本:" << usageFilePath();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

UsageBucket bucketFrom(const QJsonObject &o)
{
    UsageBucket b;
    // token 数可能很大，QJsonValue::toInt() 是 32 位会截断，
    // 所以走 toDouble() 再转 qint64。
    b.promptTokens = qint64(o.value(QStringLiteral("prompt_tokens")).toDouble());
    b.completionTokens =
        qint64(o.value(QStringLiteral("completion_tokens")).toDouble());
    b.frames = o.value(QStringLiteral("frames")).toInt();
    b.calls = o.value(QStringLiteral("calls")).toInt();
    return b;
}

QJsonObject bucketTo(const UsageBucket &b)
{
    QJsonObject o;
    o[QStringLiteral("prompt_tokens")] = double(b.promptTokens);
    o[QStringLiteral("completion_tokens")] = double(b.completionTokens);
    o[QStringLiteral("frames")] = b.frames;
    o[QStringLiteral("calls")] = b.calls;
    return o;
}

/// 把一个桶加到某个 JSON 桶上，返回合并后的 JSON
QJsonObject bucketPlus(const QJsonObject &base, const UsageBucket &delta)
{
    UsageBucket b = bucketFrom(base);
    b.add(delta);
    return bucketTo(b);
}

} // namespace

void UsageBucket::add(const UsageBucket &other)
{
    promptTokens += other.promptTokens;
    completionTokens += other.completionTokens;
    frames += other.frames;
    calls += other.calls;
}

QString usageFilePath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("usage.json"));
}

void recordUsage(const UsageBucket &delta)
{
    if (delta.isEmpty()) {
        return;     // 全部走缓存的一次整理：不该产生任何写盘
    }

    const QDateTime now = QDateTime::currentDateTime();
    const QString mk = monthKeyOf(now);
    const QString dk = dayKeyOf(now);

    QJsonObject root = loadRoot();
    QJsonObject months = root.value(QStringLiteral("months")).toObject();

    // 月合计
    QJsonObject month = bucketPlus(months.value(mk).toObject(), delta);

    // 月内当日小计 —— "今日"就是从这里读的
    QJsonObject days = month.value(QStringLiteral("days")).toObject();
    days[dk] = bucketPlus(days.value(dk).toObject(), delta);
    month[QStringLiteral("days")] = days;

    months[mk] = month;

    // 丢掉太老的月份
    if (months.size() > kKeepMonths) {
        QStringList keys = months.keys();
        std::sort(keys.begin(), keys.end());
        while (keys.size() > kKeepMonths) {
            months.remove(keys.takeFirst());
        }
    }

    root[QStringLiteral("months")] = months;
    root[QStringLiteral("updated")] = now.toString(Qt::ISODate);
    root[QStringLiteral("_说明")] = QStringLiteral(
        "本机 AI 用量账本，由 BoardMark 自动维护。只统计本程序的调用："
        "同一个 API Key 在别的软件里用掉的量这里看不到。删除本文件即清零。");

    storeRoot(root);
}

UsageSummary usageSummary()
{
    const QDateTime now = QDateTime::currentDateTime();

    UsageSummary s;
    s.monthKey = monthKeyOf(now);
    s.dayKey = dayKeyOf(now);

    const QJsonObject months =
        loadRoot().value(QStringLiteral("months")).toObject();
    const QJsonObject month = months.value(s.monthKey).toObject();

    s.month = bucketFrom(month);
    s.today = bucketFrom(month.value(QStringLiteral("days")).toObject()
                             .value(s.dayKey).toObject());
    return s;
}

} // namespace cm
