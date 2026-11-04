#include "storage/session.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace cm {

bool Session::start(const QString &rootDir, const QString &targetProcess)
{
    stop();

    QDir root(rootDir);
    if (!root.exists() && !root.mkpath(QStringLiteral("."))) {
        qWarning() << "无法创建记录目录:" << rootDir;
        return false;
    }

    m_started = QDateTime::currentDateTime();

    // 会话目录名：日期_开始时刻。同一秒重复启动时加后缀，不覆盖已有数据。
    const QString base = m_started.toString(QStringLiteral("yyyy-MM-dd_HHmmss"));
    QString name = base;
    for (int i = 1; root.exists(name) && i < 1000; ++i) {
        name = QStringLiteral("%1_%2").arg(base).arg(i);
    }

    m_dir = root.filePath(name);
    if (!QDir().mkpath(m_dir)) {
        qWarning() << "无法创建会话目录:" << m_dir;
        return false;
    }

    m_frames = QJsonArray();
    m_active = true;

    // 先写一份只有头部的索引，保证即使程序被强杀也留下痕迹
    QJsonObject head;
    head["started"] = m_started.toString(Qt::ISODate);
    head["target_process"] = targetProcess;
    head["frames"] = m_frames;
    QFile f(QDir(m_dir).filePath(QStringLiteral("session.json")));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(head).toJson(QJsonDocument::Indented));
    }

    return true;
}

void Session::stop()
{
    if (!m_active) {
        return;
    }
    writeIndex();
    m_active = false;
}

bool Session::saveFrame(const QImage &img, const QString &reason)
{
    if (!m_active || img.isNull()) {
        return false;
    }

    const QDateTime now = QDateTime::currentDateTime();
    QString name = now.toString(QStringLiteral("HHmmss")) + QStringLiteral(".png");

    // 同一秒内可能存多张（例如连续两次状态切换），加后缀避免互相覆盖
    QString path = QDir(m_dir).filePath(name);
    for (int i = 1; QFile::exists(path) && i < 1000; ++i) {
        name = now.toString(QStringLiteral("HHmmss")) + QStringLiteral("_%1.png").arg(i);
        path = QDir(m_dir).filePath(name);
    }

    if (!img.save(path, "PNG")) {
        qWarning() << "保存截图失败:" << path;
        return false;
    }

    QJsonObject rec;
    rec["file"] = name;
    rec["time"] = now.toString(Qt::ISODate);
    rec["reason"] = reason;
    m_frames.append(rec);

    writeIndex();
    return true;
}

bool Session::writeIndex()
{
    if (m_dir.isEmpty()) {
        return false;
    }

    QJsonObject head;
    head["started"] = m_started.toString(Qt::ISODate);
    head["updated"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    head["frame_count"] = m_frames.size();
    head["frames"] = m_frames;

    // 用 QSaveFile：写一半被强杀也不会把索引写坏
    QSaveFile f(QDir(m_dir).filePath(QStringLiteral("session.json")));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "无法写入 session.json";
        return false;
    }
    f.write(QJsonDocument(head).toJson(QJsonDocument::Indented));
    return f.commit();
}

} // namespace cm
