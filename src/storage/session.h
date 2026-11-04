#pragma once

// 会话：一次连续的记录过程。
//
// 上课时还不知道学科（那要等课后 AI 判断），所以先按「日期_开始时间」建目录，
// 关键帧以时刻命名，并同步维护一份 session.json 记录每帧的保存原因。
//
// 目录结构：
//     recordings/
//     └── 2026-09-30_084012/
//         ├── 084012.png
//         ├── 084530.png
//         └── session.json

#include <QDateTime>
#include <QImage>
#include <QJsonArray>
#include <QString>

namespace cm {

class Session {
public:
    Session() = default;

    /// 开始一个新会话。rootDir 为空时使用 exe 同目录下的 recordings/
    bool start(const QString &rootDir, const QString &targetProcess);

    /// 结束当前会话，落盘 session.json
    void stop();

    bool active() const { return m_active; }
    QString dir() const { return m_dir; }
    int frameCount() const { return m_frames.size(); }

    /// 保存一帧原始截图。reason 说明为什么保存它（写进 session.json）。
    bool saveFrame(const QImage &img, const QString &reason);

private:
    bool writeIndex();

    bool m_active = false;
    QString m_dir;
    QDateTime m_started;
    QJsonArray m_frames;
};

} // namespace cm
