#pragma once

// 托盘程序：整个软件的对外界面。
//
// 第一版刻意**不做主窗口** —— 老师平时根本不需要看到它。
// 右键托盘图标就能开始/停止记录、改设置。
//
// 交互设计考虑：
//   * 记录中时左键点托盘 -> 弹出当前状态（已存多少张）
//   * 关键事件用气泡提示，但**不刷屏**（只在开始/停止/出错时提示）
//   * 退出时如果还在记录，先问一句，避免误关丢数据

#include <QObject>
#include <QSystemTrayIcon>

#include "app/settings.h"

class QAction;
class QMenu;
class QIcon;

namespace cm {

class CaptureEngine;

class TrayApp : public QObject {
    Q_OBJECT

public:
    explicit TrayApp(QObject *parent = nullptr);
    ~TrayApp() override;

    /// 建立托盘图标与菜单。失败返回 false（例如系统没有托盘）。
    bool setup();

    /// 若设置为启动即记录，则调用
    void maybeAutoStart();

    /// 打开设置对话框。给命令行诊断用（--show-settings）——
    /// 在开了 Windows 缩放的教室一体机上验证界面放不放得下，
    /// 不用真的去点托盘菜单。
    void showSettings() { onSettings(); }

    /// 弹一条托盘气泡。给 main.cpp 用 —— 改名迁移把老师的开机自启继承过来
    /// 这件事必须让他知道（他什么都没做，状态却是改过的）。
    void notifyMessage(const QString &title, const QString &body);

private slots:
    void onStart();
    void onStop();
    void onToggle();
    void onAiOrganize();
    void onAutoOrganizeTick();
    void onSettings();
    void onOpenFolder();
    void onOpenArchiveFolder();
    void onAbout();
    void onQuit();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

    void onSessionStarted(const QString &dir);
    void onSessionStopped(int frames);
    void onStatusMessage(const QString &text);
    void onFrameSaved(const QString &file, const QString &reason);
    void onEngineFailed(const QString &text);

private:
    void refreshMenu();
    void updateTooltip();
    void notify(const QString &title, const QString &body,
                QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information);
    QIcon makeIcon(bool recording) const;
    bool confirmQuitWhileRecording();

    Settings m_settings;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;

    QAction *m_actStart = nullptr;
    QAction *m_actStop = nullptr;
    QAction *m_actAi = nullptr;
    QAction *m_actSettings = nullptr;
    QAction *m_actFolder = nullptr;
    QAction *m_actArchiveFolder = nullptr;
    QAction *m_actAbout = nullptr;
    QAction *m_actQuit = nullptr;

    CaptureEngine *m_engine = nullptr;
    int m_lastCount = 0;
    QString m_lastStatus;       ///< 最近一条状态，显示在托盘提示里

    // ---- 定时整理 ----
    class AutoOrganizeWorker *m_autoWorker = nullptr;
    QTimer *m_autoTimer = nullptr;
    QString m_currentSessionDir;   ///< 正在录制的会话。整理**不**避开它（录制中途
                                   ///< 也要能整理）；只用来在完成提示里说明
                                   ///< "现在还在录制中，后面写的下次会自动补上"。
    QString m_autoStatus;          ///< 定时整理的状态，显示在托盘提示里
    bool m_recording = false;
    bool m_quitting = false;
};

} // namespace cm
