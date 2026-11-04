#include "app/settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRect>
#include <QSize>
#include <QDebug>

namespace cm {

namespace {

/// 标定记录的键统一用小写进程名 —— Windows 上进程名不区分大小写
QString calKey(const QString &processName)
{
    return processName.trimmed().toLower();
}

BoardCalibration calFromJson(const QJsonObject &o)
{
    BoardCalibration c;
    c.cropTopPct = o.value(QStringLiteral("crop_top")).toInt();
    c.cropBottomPct = o.value(QStringLiteral("crop_bottom")).toInt();
    c.cropLeftPct = o.value(QStringLiteral("crop_left")).toInt();
    c.cropRightPct = o.value(QStringLiteral("crop_right")).toInt();
    c.calibrated = o.value(QStringLiteral("calibrated")).toBool(true);
    c.samples = o.value(QStringLiteral("samples")).toInt();
    c.calibratedAt = o.value(QStringLiteral("calibrated_at")).toString();
    c.displayName = o.value(QStringLiteral("display_name")).toString();
    // 越界的一律夹回来，外部改坏了配置也要能跑
    c.cropTopPct = qBound(0, c.cropTopPct, 45);
    c.cropBottomPct = qBound(0, c.cropBottomPct, 45);
    c.cropLeftPct = qBound(0, c.cropLeftPct, 45);
    c.cropRightPct = qBound(0, c.cropRightPct, 45);
    return c;
}

QJsonObject calToJson(const BoardCalibration &c)
{
    QJsonObject o;
    o[QStringLiteral("crop_top")] = c.cropTopPct;
    o[QStringLiteral("crop_bottom")] = c.cropBottomPct;
    o[QStringLiteral("crop_left")] = c.cropLeftPct;
    o[QStringLiteral("crop_right")] = c.cropRightPct;
    o[QStringLiteral("calibrated")] = c.calibrated;
    o[QStringLiteral("samples")] = c.samples;
    o[QStringLiteral("calibrated_at")] = c.calibratedAt;
    o[QStringLiteral("display_name")] = c.displayName;
    return o;
}

} // namespace

QRect Settings::analysisCrop(const QSize &frame) const
{
    if (!frame.isValid()) {
        return QRect();
    }
    const int t = frame.height() * qBound(0, cropTopPct, 45) / 100;
    const int b = frame.height() * qBound(0, cropBottomPct, 45) / 100;
    const int l = frame.width() * qBound(0, cropLeftPct, 45) / 100;
    const int r = frame.width() * qBound(0, cropRightPct, 45) / 100;

    const int h = frame.height() - t - b;
    const int w = frame.width() - l - r;
    if (h < 64 || w < 64) {
        return QRect();     // 裁得太狠还不如不裁
    }
    return QRect(l, t, w, h);
}

BoardCalibration Settings::calibrationFor(const QString &processName) const
{
    return boardCalibrations.value(calKey(processName));
}

void Settings::setCalibration(const QString &processName,
                              const BoardCalibration &cal)
{
    const QString k = calKey(processName);
    if (k.isEmpty()) {
        return;
    }
    BoardCalibration c = cal;
    c.calibrated = true;
    c.calibratedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    boardCalibrations[k] = c;
}

QRect Settings::analysisCropFor(const QString &processName,
                                const QSize &frame) const
{
    const BoardCalibration c = calibrationFor(processName);
    if (!c.calibrated) {
        return analysisCrop(frame);     // 还没标定过 -> 用全局默认
    }
    if (!frame.isValid()) {
        return QRect();
    }
    const int t = frame.height() * qBound(0, c.cropTopPct, 45) / 100;
    const int b = frame.height() * qBound(0, c.cropBottomPct, 45) / 100;
    const int l = frame.width() * qBound(0, c.cropLeftPct, 45) / 100;
    const int r = frame.width() * qBound(0, c.cropRightPct, 45) / 100;

    const int h = frame.height() - t - b;
    const int w = frame.width() - l - r;
    if (h < 64 || w < 64) {
        return analysisCrop(frame);
    }
    return QRect(l, t, w, h);
}

QString Settings::configPath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("settings.json"));
}

QString Settings::effectiveOutputDir() const
{
    if (!outputDir.trimmed().isEmpty()) {
        return QDir::cleanPath(outputDir.trimmed());
    }
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("recordings"));
}

QString Settings::effectiveArchiveDir() const
{
    if (!aiOutputDir.trimmed().isEmpty()) {
        return QDir::cleanPath(aiOutputDir.trimmed());
    }
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("板书笔记"));
}

Settings Settings::load()
{
    Settings s;
    QFile f(configPath());
    if (!f.open(QIODevice::ReadOnly)) {
        // 第一次运行：写一份默认配置出去，方便用户看到有哪些可调项
        s.save();
        return s;
    }

    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "设置文件解析失败，使用默认设置:" << err.errorString();
        return s;
    }

    const QJsonObject o = doc.object();
    auto str = [&](const char *k, const QString &def) {
        return o.contains(k) ? o.value(k).toString(def) : def;
    };
    auto num = [&](const char *k, int def) {
        return o.contains(k) ? o.value(k).toInt(def) : def;
    };
    auto dbl = [&](const char *k, double def) {
        return o.contains(k) ? o.value(k).toDouble(def) : def;
    };
    auto boolean = [&](const char *k, bool def) {
        return o.contains(k) ? o.value(k).toBool(def) : def;
    };

    s.targetProcess   = str("target_process", s.targetProcess);
    s.idleIntervalMs  = num("idle_interval_ms", s.idleIntervalMs);
    s.activeIntervalMs = num("active_interval_ms", s.activeIntervalMs);
    s.activeHoldMs    = num("active_hold_ms", s.activeHoldMs);
    s.replaceThreshold = dbl("replace_threshold", s.replaceThreshold);
    s.moveThreshold   = num("move_threshold", s.moveThreshold);
    s.movePercent     = dbl("move_percent", s.movePercent);
    s.sameThreshold   = dbl("same_threshold", s.sameThreshold);
    s.maxSpanSeconds  = num("max_span_seconds", s.maxSpanSeconds);
    s.outputDir       = str("output_dir", s.outputDir);
    s.startOnLaunch   = boolean("start_on_launch", s.startOnLaunch);
    s.requireForeground = boolean("require_foreground", s.requireForeground);
    s.pauseOnExtraWindows = boolean("pause_on_extra_windows", s.pauseOnExtraWindows);
    s.extraWindowMinW  = num("extra_window_min_w", s.extraWindowMinW);
    s.extraWindowMinH  = num("extra_window_min_h", s.extraWindowMinH);
    s.pauseOnPanel     = boolean("pause_on_panel", s.pauseOnPanel);
    s.panelThreshold   = dbl("panel_threshold", s.panelThreshold);
    s.emptyInk         = dbl("empty_ink", s.emptyInk);
    s.aiBaseUrl        = str("ai_base_url", s.aiBaseUrl);
    s.aiApiKey         = str("ai_api_key", s.aiApiKey);
    s.aiModel          = str("ai_model", s.aiModel);
    s.aiOutputDir      = str("ai_output_dir", s.aiOutputDir);
    s.autoOrganizeEnabled = boolean("auto_organize", s.autoOrganizeEnabled);
    s.autoOrganizeHour    = num("auto_organize_hour", s.autoOrganizeHour);
    s.autoOrganizeMinute  = num("auto_organize_minute", s.autoOrganizeMinute);
    s.cropTopPct      = num("crop_top_percent", s.cropTopPct);
    s.cropBottomPct   = num("crop_bottom_percent", s.cropBottomPct);
    s.cropLeftPct     = num("crop_left_percent", s.cropLeftPct);
    s.cropRightPct    = num("crop_right_percent", s.cropRightPct);

    // 按进程分开存的本机白板标定
    s.boardCalibrations.clear();
    {
        const QJsonObject cals =
            o.value(QStringLiteral("board_calibrations")).toObject();
        for (auto it = cals.begin(); it != cals.end(); ++it) {
            const QJsonObject v = it.value().toObject();
            if (!v.isEmpty()) {
                s.boardCalibrations.insert(it.key().toLower(), calFromJson(v));
            }
        }
    }

    // 防御：外部改坏了配置也要能跑
    s.idleIntervalMs = qBound(100, s.idleIntervalMs, 10000);
    s.activeIntervalMs = qBound(50, s.activeIntervalMs, 5000);
    s.replaceThreshold = qBound(0.05, s.replaceThreshold, 0.95);
    s.sameThreshold = qBound(0.05, s.sameThreshold, 0.999);
    s.moveThreshold = qBound(2, s.moveThreshold, 500);
    s.maxSpanSeconds = qBound(10, s.maxSpanSeconds, 3600);

    return s;
}

bool Settings::save() const
{
    QJsonObject o;
    o["_说明"] = QStringLiteral(
        "target_process 留空 = 自动识别白板软件；也可填进程名，例如 "
        "EasiNote.exe（希沃白板）/ EasiNote5C.exe（希沃轻白板）/ HiteTouchPro.exe（鸿合）");
    o["target_process"] = targetProcess;
    o["idle_interval_ms"] = idleIntervalMs;
    o["active_interval_ms"] = activeIntervalMs;
    o["active_hold_ms"] = activeHoldMs;
    o["replace_threshold"] = replaceThreshold;
    o["move_threshold"] = moveThreshold;
    o["move_percent"] = movePercent;
    o["same_threshold"] = sameThreshold;
    o["max_span_seconds"] = maxSpanSeconds;
    o["output_dir"] = outputDir;
    o["start_on_launch"] = startOnLaunch;
    o["require_foreground"] = requireForeground;
    o["pause_on_extra_windows"] = pauseOnExtraWindows;
    o["extra_window_min_w"] = extraWindowMinW;
    o["extra_window_min_h"] = extraWindowMinH;
    o["pause_on_panel"] = pauseOnPanel;
    o["panel_threshold"] = panelThreshold;
    o["empty_ink"] = emptyInk;
    o["ai_base_url"] = aiBaseUrl;
    o["ai_api_key"] = aiApiKey;
    o["ai_model"] = aiModel;
    o["ai_output_dir"] = aiOutputDir;
    o["auto_organize"] = autoOrganizeEnabled;
    o["auto_organize_hour"] = autoOrganizeHour;
    o["auto_organize_minute"] = autoOrganizeMinute;
    o["crop_top_percent"] = cropTopPct;
    o["crop_bottom_percent"] = cropBottomPct;
    o["crop_left_percent"] = cropLeftPct;
    o["crop_right_percent"] = cropRightPct;

    // 按进程分开存的本机白板标定。键是小写进程名。
    {
        QJsonObject cals;
        for (auto it = boardCalibrations.begin(); it != boardCalibrations.end();
             ++it) {
            if (!it.value().isEmpty()) {
                cals[it.key()] = calToJson(it.value());
            }
        }
        o["board_calibrations"] = cals;
    }

    QFile f(configPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "无法写入设置文件:" << configPath();
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

} // namespace cm
