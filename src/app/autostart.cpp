#include "app/autostart.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QSettings>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace cm {

const char *kAutoStartName = "BoardMark";
const char *kLegacyName = "ClassMind";

namespace {

/// Run 键的位置。QSettings 的 NativeFormat 在 Windows 上直接读写注册表。
QString runKeyPath()
{
    return QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
}

QSettings runKey()
{
    return QSettings(runKeyPath(), QSettings::NativeFormat);
}

/// 当前 exe 的完整路径，**带引号** —— 路径里有空格时不加引号会被截断成
/// 两个参数，Windows 启动时会去执行"C:\Program"这种东西。
QString quotedExePath()
{
    const QString p =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QLatin1Char('"') + p + QLatin1Char('"');
}

} // namespace

bool isAutoStartEnabled()
{
    QSettings s = runKey();
    return s.contains(QString::fromLatin1(kAutoStartName));
}

QString autoStartCommand()
{
    QSettings s = runKey();
    return s.value(QString::fromLatin1(kAutoStartName)).toString();
}

QString autoStartRegistryPath()
{
    return runKeyPath() + QLatin1Char('\\') + QString::fromLatin1(kAutoStartName);
}

bool setAutoStartEnabled(bool on)
{
    QSettings s = runKey();
    const QString name = QString::fromLatin1(kAutoStartName);

    if (on) {
        s.setValue(name, quotedExePath());
    } else {
        s.remove(name);
    }
    s.sync();

    if (s.status() != QSettings::NoError) {
        qWarning() << "写注册表失败（可能被组策略锁住）:" << runKeyPath();
        return false;
    }
    // 写完再读一次确认 —— "以为写进去了" 和 "真的写进去了" 是两回事
    return isAutoStartEnabled() == on;
}

bool refreshAutoStartPath()
{
    if (!isAutoStartEnabled()) {
        return false;   // 关着就别动它
    }
    const QString want = quotedExePath();
    if (autoStartCommand() == want) {
        return false;   // 已经是对的
    }
    qInfo() << "开机自启动记录的路径已过期，刷新为当前 exe:" << want;
    setAutoStartEnabled(true);
    return true;
}

// ------------------------------------------------------------------ 改名迁移

LegacyAutoStartResult migrateLegacyAutoStart()
{
    const QString legacy = QString::fromLatin1(kLegacyName);
    const QString modern = QString::fromLatin1(kAutoStartName);
    if (legacy == modern) {
        return LegacyAutoStartResult::Nothing;   // 名字没变过，无事可做
    }

    QSettings s = runKey();
    if (!s.contains(legacy)) {
        return LegacyAutoStartResult::Nothing;   // 没有旧项，正常情况
    }

    const QString oldCmd = s.value(legacy).toString();
    const bool modernWasOn = s.contains(modern);

    // 旧项在、新项不在 -> **迁移**，而不是删掉了事。
    //
    // 旧项的存在是「这位老师想要开机自启」的正面证据；新项从来没被写过，
    // 所以"新项不存在"不等于"老师关掉了"。只删不建就会把老师的开机自启
    // 静默关掉 —— 他会以为程序还跟着开机跑，实际上一整节课都没录到。
    if (!modernWasOn) {
        s.setValue(modern, quotedExePath());
    }
    // **只删自己那条旧值。** 不动任何一个别人的启动项。
    s.remove(legacy);
    s.sync();

    // 写完再读一次确认 —— "以为写进去了" 和 "真的写进去了" 是两回事。
    // 注册表被组策略 / 安全软件拦住时，上面这几行会静默不生效。
    const QSettings check = runKey();
    if (check.contains(legacy) || !check.contains(modern)) {
        qWarning() << "改名迁移：注册表读回校验失败（可能被组策略锁住）";
        return LegacyAutoStartResult::Failed;
    }

    if (modernWasOn) {
        qInfo().noquote()
            << QStringLiteral("改名迁移：新启动项本来就在，删掉多余的旧项 %1")
                   .arg(legacy);
        return LegacyAutoStartResult::Removed;
    }

    qInfo().noquote()
        << QStringLiteral("改名迁移：旧的 %1 启动项（原本指向 %2）已迁移成 %3，"
                          "开机自启保持不变")
               .arg(legacy, oldCmd, modern);
    return LegacyAutoStartResult::Migrated;
}

bool legacyInstanceRunning()
{
#ifdef _WIN32
    const QString legacy = QString::fromLatin1(kLegacyName);
    if (legacy == QString::fromLatin1(kAutoStartName)) {
        return false;
    }
    // 旧版用的是 "Global\<旧名>_SingleInstance"。这里只**查询**，
    // 不创建、不持锁 —— 查到就把句柄关掉，绝不影响对方。
    const std::wstring name =
        (QStringLiteral("Global\\%1_SingleInstance").arg(legacy)).toStdWString();
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
    if (h == nullptr) {
        return false;       // 不存在 = 旧版没在跑
    }
    CloseHandle(h);
    return true;
#else
    return false;
#endif
}

} // namespace cm
