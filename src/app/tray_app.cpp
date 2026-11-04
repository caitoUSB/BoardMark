#include "app/tray_app.h"

#include "app/version.h"
#include "app/autostart.h"
#include "ai/classifier.h"
#include "ai/organizer.h"
#include "ai/usage.h"
#include "capture/capture_engine.h"
#include "capture/window_detector.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QThread>
#include <QTimeEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QDesktopServices>
#include <QUrl>

namespace cm {

namespace {

/// 把界面上的目录输入框变成要存进配置的值。
///
/// **等于默认值就存空串。** 空串的含义是"跟着 exe 走"，运行时才算绝对路径。
/// 直接存绝对路径会出大事：一次「打开设置 → 点确定」就把开发机的路径
/// 写死进配置，跟着发布版跑到别人电脑上，老师那边所有记录都会写进
/// 一个不存在的目录。实测踩到过。
QString pathOrEmptyIfDefault(const QString &typed, const QString &defaultLeaf)
{
    const QString t = QDir::cleanPath(typed.trimmed());
    const QString def = QDir::cleanPath(
        QDir(QCoreApplication::applicationDirPath()).filePath(defaultLeaf));
    return (t.isEmpty() || t == def) ? QString() : t;
}

} // namespace

/// 在**后台线程**里跑定时整理。
///
/// 为什么必须开线程：一节课二十来张图要跑一两分钟，
/// 在主线程里跑的话托盘图标会整个卡死，Windows 还会认为程序没响应。
/// 手动「AI 整理记录…」有模态进度框撑着，定时整理没有，所以必须异步。
///
/// 筛选和标记全部交给 organizePendingSessions() —— 那是
/// 「只整理没整理过的」这条语义的唯一实现，这个类只负责搬到后台线程。
///
/// 注意：这个类**不能放进上面的匿名命名空间** —— moc 生成的代码按
/// `cm::AutoOrganizeWorker` 引用它，放匿名命名空间里会变成
/// `cm::{匿名}::AutoOrganizeWorker`，两边对不上，编译直接炸。
class AutoOrganizeWorker : public QThread {
    Q_OBJECT

public:
    QString recordingsRoot;
    QString archiveRoot;
    int okCount = 0;
    int failCount = 0;
    int identified = 0;
    int cachedHits = 0;

signals:
    void progress(const QString &text);
    void finished2(int ok, int fail);

protected:
    void run() override
    {
        const OrganizeBatchResult batch = organizePendingSessions(
            recordingsRoot, archiveRoot, false,
            [this](const QString &sessionName, int index, int count,
                   const OrganizeProgress &p) {
                emit progress(QStringLiteral("%1：%2/%3（第 %4/%5 个会话）")
                                  .arg(sessionName)
                                  .arg(p.done)
                                  .arg(p.total)
                                  .arg(index)
                                  .arg(count));
            });

        okCount = batch.sessionsOk;
        failCount = batch.sessionsFailed;
        identified = batch.identified;
        cachedHits = batch.cachedHits;
        emit finished2(okCount, failCount);
    }
};

namespace {

/// 画一个简易托盘图标：一块板子 + 一支笔
QIcon drawIcon(const QColor &pen)
{
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 板子
    p.setPen(QPen(QColor(0x33, 0x33, 0x33), 3));
    p.setBrush(QColor(0xF5, 0xF7, 0xFA));
    p.drawRoundedRect(QRectF(6, 8, 52, 40), 5, 5);

    // 板上的两行「字」
    p.setPen(QPen(QColor(0x88, 0x99, 0xAA), 3));
    p.drawLine(QPointF(15, 22), QPointF(43, 22));
    p.drawLine(QPointF(15, 31), QPointF(36, 31));

    // 笔
    p.setPen(Qt::NoPen);
    p.setBrush(pen);
    QPolygonF penShape;
    penShape << QPointF(44, 52) << QPointF(56, 40)
             << QPointF(60, 44) << QPointF(48, 56);
    p.drawPolygon(penShape);

    p.end();
    return QIcon(pm);
}

} // namespace

TrayApp::TrayApp(QObject *parent) : QObject(parent)
{
    m_settings = Settings::load();
}

TrayApp::~TrayApp()
{
    if (m_engine) {
        m_engine->requestStop();
        m_engine->wait(3000);
        delete m_engine;
        m_engine = nullptr;
    }
}

QIcon TrayApp::makeIcon(bool recording) const
{
    return drawIcon(recording ? QColor(0xE0, 0x3B, 0x3B)   // 记录中：红笔
                              : QColor(0x2E, 0x6D, 0xA4)); // 空闲：蓝笔
}

bool TrayApp::setup()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qCritical() << "系统没有可用的托盘区域";
        return false;
    }

    m_menu = new QMenu();

    m_actStart = m_menu->addAction(QStringLiteral("开始截图"));
    m_actStop = m_menu->addAction(QStringLiteral("停止截图"));
    m_menu->addSeparator();
    m_actAi = m_menu->addAction(QStringLiteral("AI 整理记录…"));
    m_actSettings = m_menu->addAction(QStringLiteral("设置…"));
    m_actFolder = m_menu->addAction(QStringLiteral("打开记录文件夹"));
    // 归档文件夹单独一项：录完课老师要看的其实是这个（判完学科的分好类的图），
    // 和「记录文件夹」（原始会话，按时间堆）是两回事，不能共用一个入口。
    m_actArchiveFolder = m_menu->addAction(QStringLiteral("打开归档文件夹"));
    m_menu->addSeparator();
    m_actAbout = m_menu->addAction(QStringLiteral("关于"));
    m_actQuit = m_menu->addAction(QStringLiteral("退出"));

    connect(m_actStart, &QAction::triggered, this, &TrayApp::onStart);
    connect(m_actStop, &QAction::triggered, this, &TrayApp::onStop);
    connect(m_actAi, &QAction::triggered, this, &TrayApp::onAiOrganize);
    connect(m_actSettings, &QAction::triggered, this, &TrayApp::onSettings);
    connect(m_actFolder, &QAction::triggered, this, &TrayApp::onOpenFolder);
    connect(m_actArchiveFolder, &QAction::triggered,
            this, &TrayApp::onOpenArchiveFolder);
    connect(m_actAbout, &QAction::triggered, this, &TrayApp::onAbout);
    connect(m_actQuit, &QAction::triggered, this, &TrayApp::onQuit);

    m_tray = new QSystemTrayIcon(makeIcon(false), this);
    m_tray->setContextMenu(m_menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, &TrayApp::onTrayActivated);

    m_tray->show();
    refreshMenu();
    updateTooltip();

    // 定时整理：每分钟检查一次（只是扫一下目录，很便宜），
    // 到了设定时间且有没整理的会话才真正动手。
    m_autoTimer = new QTimer(this);
    m_autoTimer->setInterval(60 * 1000);
    connect(m_autoTimer, &QTimer::timeout, this, &TrayApp::onAutoOrganizeTick);
    m_autoTimer->start();

    return true;
}

void TrayApp::maybeAutoStart()
{
    if (m_settings.startOnLaunch) {
        onStart();
    }
}

void TrayApp::refreshMenu()
{
    if (!m_menu) {
        return;
    }
    m_actStart->setEnabled(!m_recording);
    m_actStop->setEnabled(m_recording);

    if (m_tray) {
        m_tray->setIcon(makeIcon(m_recording));
        updateTooltip();
    }
}

void TrayApp::notify(const QString &title, const QString &body,
                     QSystemTrayIcon::MessageIcon icon)
{
    if (m_tray) {
        m_tray->showMessage(title, body, icon, 4000);
    }
}

void TrayApp::notifyMessage(const QString &title, const QString &body)
{
    notify(title, body, QSystemTrayIcon::Information);
}

// ------------------------------------------------------------------ 记录控制

void TrayApp::onStart()
{
    if (m_recording) {
        return;
    }

    m_settings = Settings::load();     // 每次开始都重读，改完配置不用重启
    m_lastCount = 0;

    m_engine = new CaptureEngine(m_settings, this);
    connect(m_engine, &CaptureEngine::sessionStarted,
            this, &TrayApp::onSessionStarted);
    connect(m_engine, &CaptureEngine::sessionStopped,
            this, &TrayApp::onSessionStopped);
    connect(m_engine, &CaptureEngine::statusMessage,
            this, &TrayApp::onStatusMessage);
    connect(m_engine, &CaptureEngine::frameSaved,
            this, &TrayApp::onFrameSaved);
    connect(m_engine, &CaptureEngine::failed,
            this, &TrayApp::onEngineFailed);

    m_recording = true;
    refreshMenu();
    m_engine->start();
}

void TrayApp::onStop()
{
    if (!m_recording || !m_engine) {
        return;
    }
    m_engine->requestStop();
    // 真正的收尾在 onSessionStopped 里做
}

void TrayApp::onToggle()
{
    if (m_recording) {
        onStop();
    } else {
        onStart();
    }
}

void TrayApp::onSessionStarted(const QString &dir)
{
    // 正在录的是哪一个会话。用来在"整理完成"的提示里说明
    // "现在还在录制中，后面写的下次会自动补上"。
    m_currentSessionDir = dir;
    notify(QStringLiteral("已开始记录"),
           QStringLiteral("正在监控 %1\n保存到：%2")
               .arg(m_settings.targetProcess, QDir::toNativeSeparators(dir)));
    qInfo() << "会话开始:" << dir;
}

void TrayApp::onSessionStopped(int frames)
{
    m_recording = false;
    m_currentSessionDir.clear();    // 录完了，提示里就不用再说"还在录制中"
    m_lastCount = frames;

    if (m_engine) {
        m_engine->wait(3000);
        m_engine->deleteLater();
        m_engine = nullptr;
    }

    refreshMenu();

    if (!m_quitting) {
        if (frames > 0) {
            notify(QStringLiteral("记录已结束"),
                   QStringLiteral("本次共保存 %1 张关键帧").arg(frames));
        } else {
            notify(QStringLiteral("记录已结束"),
                   QStringLiteral("本次没有保存任何内容。\n"
                                  "请确认目标软件开着、不是最小化、且没有被别的窗口盖住。"),
                   QSystemTrayIcon::Warning);
        }
    }
    qInfo() << "会话结束，关键帧数:" << frames;
}

void TrayApp::onFrameSaved(const QString &file, const QString &reason)
{
    Q_UNUSED(file);
    Q_UNUSED(reason);
}

void TrayApp::onStatusMessage(const QString &text)
{
    // 只更新 tooltip，不弹气泡，避免刷屏打扰老师
    m_lastStatus = text;
    updateTooltip();
    qInfo().noquote() << "[状态]" << text;
}

void TrayApp::updateTooltip()
{
    if (!m_tray) {
        return;
    }

    // 目标进程名一定要显示出来。
    // 实测踩过的坑：配置里 target_process 被改成了别的程序（测试残留），
    // 结果一直在记录错误的窗口，而界面上完全看不出来。
    // 把目标名字摆在托盘提示里，设错了立刻就看得见。
    const QString target = QStringLiteral("目标：%1").arg(m_settings.targetProcess);

    if (!m_recording) {
        m_tray->setToolTip(QStringLiteral("BoardMark · 未开始\n%1").arg(target));
        return;
    }

    QString tip = QStringLiteral("BoardMark · 记录中（已存 %1 张）\n%2")
                      .arg(m_lastCount)
                      .arg(target);
    if (!m_lastStatus.isEmpty()) {
        tip += QStringLiteral("\n") + m_lastStatus;
    }
    if (!m_autoStatus.isEmpty()) {
        tip += QStringLiteral("\n") + m_autoStatus;
    }
    m_tray->setToolTip(tip);
}

void TrayApp::onEngineFailed(const QString &text)
{
    m_recording = false;
    if (m_engine) {
        m_engine->wait(1000);
        m_engine->deleteLater();
        m_engine = nullptr;
    }
    refreshMenu();
    notify(QStringLiteral("记录失败"), text, QSystemTrayIcon::Critical);
}

// ------------------------------------------------------------------ 菜单动作

void TrayApp::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Trigger) {
        // 左键单击：给一个即时反馈
        if (m_recording) {
            notify(QStringLiteral("正在记录"),
                   QStringLiteral("已保存 %1 张关键帧").arg(m_lastCount));
        } else {
            notify(QStringLiteral("BoardMark"),
                   QStringLiteral("未开始。右键图标可开始记录。"));
        }
    }
}

void TrayApp::onAiOrganize()
{
    const QString recordingsRoot = m_settings.effectiveOutputDir();
    const QString archiveRoot = m_settings.effectiveArchiveDir();

    // **正在录制的会话也照样整理，这是有意的。**
    //
    // 老师完全可以在录制中途点一次「AI 整理」先看效果 —— 而且这正是预期用法。
    // 不会损坏任何东西：已判的帧走 classify.json 缓存（不花钱），归档是增量
    // 合并（不产生重复文件），而 organized.json 里记着"整理到第几帧"，
    // 所以接着写的新笔记下次点就会自动补上（见 isOrganized）。
    //
    // 反过来，如果在这里把正在录制的会话排掉，就会出现"录着课点整理没反应"
    // 的观感 —— 而它其实是有活可干的。
    const QStringList pending = findPendingSessions(recordingsRoot);
    if (pending.isEmpty()) {
        notify(QStringLiteral("AI 整理"),
               QStringLiteral("没有需要整理的新记录。\n"
                              "已经整理过的会话会自动跳过，不会重复调用模型。"));
        return;
    }

    const AiConfig cfg = loadAiConfig();
    if (!cfg.usable()) {
        // 没配 key 也让跑：整条流程会走完，只是全部落到「其他」。
        // 这样老师能先看到归档结构长什么样。
        const auto btn = QMessageBox::question(
            nullptr, QStringLiteral("AI 整理"),
            QStringLiteral("还没有配置视觉模型的 API Key。\n\n"
                           "现在整理的话，所有关键帧都会归到「其他」，"
                           "但归档结构会照常生成。\n\n"
                           "现在去设置里填写吗？"),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (btn == QMessageBox::Cancel) {
            return;
        }
        if (btn == QMessageBox::Yes) {
            onSettings();
            if (!loadAiConfig().usable()) {
                return;     // 还是没填，就先不跑了
            }
        }
    }

    QProgressDialog dlg(QStringLiteral("正在准备…"), QStringLiteral("取消"), 0, 100,
                        nullptr);
    dlg.setWindowTitle(QStringLiteral("AI 整理记录"));
    dlg.setWindowModality(Qt::ApplicationModal);
    dlg.setMinimumWidth(460);
    dlg.setAutoClose(false);
    dlg.setAutoReset(false);
    dlg.setToolTip(QStringLiteral(
        "取消之后：这一轮**没做完的会话不会被标记**，\n"
        "已经判好的图留在识别缓存里。再点一次「AI 整理记录…」\n"
        "就从断点继续 —— 已判过的秒过，不会重复花钱。"));
    dlg.show();

    // 可能要几分钟（每张图几秒）。这里刻意用同步调用 + processEvents，
    // 而不是再起一个线程 —— 进度对话框是模态的，重入风险可控，
    // 代码量也小得多。
    //
    // 取消按钮靠 wasCanceled() 传下去。processEvents() 会把点击送进来。
    // 取消的**响应时间** = 当前这张图的剩余时间：
    // 帧与帧之间、以及重试退避里都会检查，但已经发出去的那次 HTTP 请求
    // 中断不了（要中断得把 QNetworkReply 暴露出来），
    // 所以网络卡死时最坏要等到超时（默认 120 秒）。
    //
    // 筛选、标记、记账全在 organizePendingSessions() 里，
    // 和定时整理、命令行走的是同一份实现。
    const OrganizeBatchResult batch = organizePendingSessions(
        recordingsRoot, archiveRoot, false,
        [&dlg](const QString &sessionName, int index, int count,
               const OrganizeProgress &p) {
            dlg.setMaximum(qMax(1, p.total));
            dlg.setValue(p.done);
            dlg.setLabelText(QStringLiteral("正在整理 %1（第 %2/%3 个会话）\n%4")
                                 .arg(sessionName)
                                 .arg(index)
                                 .arg(count)
                                 .arg(p.message));
            QApplication::processEvents();
        },
        [&dlg]() { return dlg.wasCanceled(); });
    dlg.close();

    // ---- 中途停了：单独一套说法 ----
    //
    // 和"整理完成"分开说，而且**必须把原因摊出来** ——
    // 老师得知道是去充值、去改 Key、还是去查网络。
    if (batch.stoppedEarly()) {
        const QString title = batch.cancelled ? QStringLiteral("AI 整理已取消")
                                              : QStringLiteral("AI 整理已中断");
        QString msg;
        if (batch.cancelled) {
            msg = QStringLiteral("你已经取消，剩下的没有继续做。\n");
        } else {
            msg = QStringLiteral("遇到问题，提前停下了，剩下的没有继续做。\n\n"
                                 "原因：\n%1\n")
                      .arg(batch.stopReason);
        }
        msg += QStringLiteral("\n已经做完的 %1 个会话保留在归档里。\n"
                              "没做完的**没有**被标记成「已整理」——\n"
                              "问题解决后再点一次「AI 整理记录…」就会从断点继续，\n"
                              "已经判好的图走缓存，不会重复花钱。")
                   .arg(batch.sessionsOk);
        if (batch.identified > 0) {
            msg += QStringLiteral("\n\n本次已经识别 %1 张，消耗 %2 token")
                       .arg(batch.identified)
                       .arg(QLocale().toString(qint64(batch.promptTokens)
                                               + batch.completionTokens));
        }
        QMessageBox::warning(nullptr, title, msg);
        return;
    }

    if (batch.sessions == 0) {
        notify(QStringLiteral("AI 整理"), QStringLiteral("没有需要整理的新记录。"));
        return;
    }

    const QString dayDir = batch.dayDirs.isEmpty() ? archiveRoot
                                                   : batch.dayDirs.first();

    // ---- 花费要摊开给老师看 ----
    //
    // 这一行就是"有没有重复花钱"的答案：新识别的张数才是花了钱的，
    // 复用缓存的张数一分钱没花。
    QString msg = QStringLiteral("整理完成。\n\n"
                                 "处理会话 %1 个（成功 %2，失败 %3）\n"
                                 "关键帧 %4 张：新识别 %5 张，复用缓存 %6 张\n"
                                 "本次消耗 %7 token")
                      .arg(batch.sessions)
                      .arg(batch.sessionsOk)
                      .arg(batch.sessionsFailed)
                      .arg(batch.frames())
                      .arg(batch.identified)
                      .arg(batch.cachedHits)
                      .arg(QLocale().toString(
                          qint64(batch.promptTokens) + batch.completionTokens));
    if (batch.failed > 0) {
        msg += QStringLiteral("\n（%1 张识别失败，已归到「其他」）")
                   .arg(batch.failed);
        // **把失败原因也摊出来。**
        // 只报"N 张失败"的话，老师不知道该去充值、去查 key、还是去查网络 ——
        // 这三件事的处理方式完全不同。
        if (!batch.firstError.isEmpty()) {
            msg += QStringLiteral("\n原因（第一条）：\n%1").arg(batch.firstError);
        }
    }
    if (!batch.errors.isEmpty()) {
        msg += QStringLiteral("\n\n未完成的会话：\n· %1")
                   .arg(batch.errors.join(QStringLiteral("\n· ")));
    }
    // 还在录制中时说一句：这次只整理到"现在为止"。
    // 不说的话老师会以为整理漏了 —— 实际上后面写的下次点就会补上。
    if (!m_currentSessionDir.isEmpty()) {
        msg += QStringLiteral("\n\n注意：**现在还在录制中**，本次整理的是到现在为止的内容。\n"
                              "后面接着写的笔记不用管，下次点「AI 整理记录…」会自动补上"
                              "（已判过的图走缓存，不重复花钱）。");
    }
    msg += QStringLiteral("\n\n归档位置：\n%1")
               .arg(QDir::toNativeSeparators(dayDir));

    const auto btn = QMessageBox::information(
        nullptr, QStringLiteral("AI 整理完成"), msg,
        QMessageBox::Open | QMessageBox::Ok);
    if (btn == QMessageBox::Open) {
        // 打开归档文件夹本身，而不是某一个文件。
        // 归档里只剩一个索引文件（index.json，设了隐藏属性，老师不需要看到），
        // 而且老师真正想看的是分好类的学科文件夹，直接进目录最顺手。
        QDesktopServices::openUrl(QUrl::fromLocalFile(dayDir));
    }
}

void TrayApp::onAutoOrganizeTick()
{
    if (!m_settings.autoOrganizeEnabled) {
        return;
    }
    if (m_engine) {
        return;     // 正在录课，别去抢磁盘和网络
    }
    if (m_autoWorker) {
        return;     // 上一轮还没跑完
    }

    // 到了设定时间才动手。默认 18:30 —— 上课期间完全不联网，
    // 放学后再整理，既不占课堂网络，也不打断上课。
    const QTime now = QTime::currentTime();
    const QTime target(m_settings.autoOrganizeHour, m_settings.autoOrganizeMinute);
    if (now < target) {
        return;
    }

    const QStringList pending = findPendingSessions(m_settings.effectiveOutputDir());
    if (pending.isEmpty()) {
        return;
    }

    qInfo().noquote() << QStringLiteral("[定时整理] 发现 %1 个未整理的会话").arg(pending.size());

    auto *w = new AutoOrganizeWorker();
    w->recordingsRoot = m_settings.effectiveOutputDir();
    w->archiveRoot = m_settings.effectiveArchiveDir();
    m_autoWorker = w;

    connect(w, &AutoOrganizeWorker::progress, this, [this](const QString &t) {
        m_autoStatus = t;
        updateTooltip();
    });
    connect(w, &AutoOrganizeWorker::finished2, this,
            [this](int ok, int fail) {
                m_autoWorker->deleteLater();
                m_autoWorker = nullptr;
                m_autoStatus.clear();
                updateTooltip();

                QString msg = QStringLiteral("已自动整理 %1 次记录").arg(ok);
                if (fail > 0) {
                    msg += QStringLiteral("，%1 次失败").arg(fail);
                }
                notify(QStringLiteral("AI 整理完成"), msg);
                qInfo().noquote() << "[定时整理]" << msg;
            });
    w->start();
}

void TrayApp::onSettings()
{
    QDialog dlg;
    dlg.setWindowTitle(QStringLiteral("设置"));
    dlg.setSizeGripEnabled(true);   // 允许拖右下角调整大小

    // 内容放进滚动区，按钮留在滚动区外面。
    //
    // 实测踩到的坑：教室一体机和笔记本普遍开着 Windows 缩放（125% / 150% 很常见），
    // 这时设置页会高过屏幕，底部的「确定/取消」被裁到屏幕外，参数根本调不了，
    // 而且窗口还拖不动大小。所以：
    //   * 内容可滚动 —— 再高也够得着
    //   * 窗口可缩放 —— 老师能按自己习惯拉大
    //   * 初始尺寸按**屏幕可用区域**算，绝不超出屏幕
    auto *outer = new QVBoxLayout(&dlg);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(8);

    auto *scroll = new QScrollArea(&dlg);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget();
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 10, 0);   // 右边留出滚动条的位置
    auto *form = new QFormLayout();

    // 目标软件用**下拉框**而不是纯文本框。
    //
    // 实测踩过的坑：文本框里被填成了别的程序（测试残留下来的配置），
    // 结果程序一直在记录错误的窗口，界面上完全看不出来，
    // 还把会话窗口和控制台黑框一起存进了关键帧。
    // 下拉框列出「当前真的有窗口的进程」，选错的可能性大大降低。
    auto *procBox = new QComboBox();
    procBox->setEditable(true);        // 白板没开时也能手填
    procBox->setInsertPolicy(QComboBox::NoInsert);

    QStringList procs;
    for (const WindowInfo &w : WindowDetector::enumerate()) {
        if (!w.process.isEmpty() && !procs.contains(w.process)) {
            procs.append(w.process);
        }
    }
    procs.sort(Qt::CaseInsensitive);
    // 当前配置即使没在运行也要出现在列表里，否则会被静默改掉
    if (!m_settings.targetProcess.isEmpty()
        && !procs.contains(m_settings.targetProcess, Qt::CaseInsensitive)) {
        procs.prepend(m_settings.targetProcess);
    }
    // 第 0 项 = 自动识别。以前必须手填进程名，等于"只支持一款白板"。
    const QString autoLabel = QStringLiteral("自动识别（推荐）");
    procBox->addItem(autoLabel);
    procBox->addItems(procs);
    if (m_settings.targetProcess.isEmpty()) {
        procBox->setCurrentIndex(0);
    } else {
        procBox->setCurrentText(m_settings.targetProcess);
    }
    procBox->setToolTip(QStringLiteral(
        "「自动识别」会自己认出白板软件（希沃白板 EasiNote.exe、"
        "希沃轻白板 EasiNote5C.exe、鸿合 HiteTouchPro.exe …），\n"
        "并在录制过程中自动测出这款软件该裁掉多高的工具栏。\n\n"
        "识别不出来时，在这里手动选一次也能正常用 —— 标定照样自动进行。\n"
        "选错会一直记录错误的窗口，托盘提示里会显示当前目标，可以核对。"));

    auto *refreshBtn = new QPushButton(QStringLiteral("刷新列表"));
    refreshBtn->setToolTip(QStringLiteral("重新扫描当前有窗口的程序"));
    QObject::connect(refreshBtn, &QPushButton::clicked, procBox, [procBox, autoLabel]() {
        const QString keep = procBox->currentText();
        QStringList list;
        for (const WindowInfo &w : WindowDetector::enumerate()) {
            if (!w.process.isEmpty() && !list.contains(w.process)) {
                list.append(w.process);
            }
        }
        list.sort(Qt::CaseInsensitive);
        if (!keep.isEmpty() && keep != autoLabel
            && !list.contains(keep, Qt::CaseInsensitive)) {
            list.prepend(keep);
        }
        procBox->clear();
        procBox->addItem(autoLabel);
        procBox->addItems(list);
        if (keep == autoLabel) {
            procBox->setCurrentIndex(0);
        } else {
            procBox->setCurrentText(keep);
        }
    });

    auto *procRow = new QWidget();
    auto *procLayout = new QHBoxLayout(procRow);
    procLayout->setContentsMargins(0, 0, 0, 0);
    procLayout->addWidget(procBox, 1);
    procLayout->addWidget(refreshBtn);
    form->addRow(QStringLiteral("目标软件"), procRow);

    // ---- 自动识别的结果 + 本机标定状态 ----
    //
    // 这两行是给老师一个"看得见"的交代：现在会记录哪个软件、
    // 以及这款软件的工具栏位置有没有被自动测过。
    {
        auto *calRow = new QWidget();
        auto *calLayout = new QHBoxLayout(calRow);
        calLayout->setContentsMargins(0, 0, 0, 0);

        auto *calLabel = new QLabel();
        calLabel->setWordWrap(true);
        calLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        auto *calResetBtn = new QPushButton(QStringLiteral("清除并重新标定"));
        calResetBtn->setToolTip(QStringLiteral(
            "清掉本机所有白板的自动标定结果。\n"
            "下一次开始记录时会重新测一遍（约几秒钟）。\n\n"
            "什么时候用得上：标定结果明显不对（比如底部裁多了、"
            "工具栏没被裁掉，导致存下一堆废图）。"));

        auto describe = [&]() {
            const WhiteboardDetection det =
                detectWhiteboard(WindowDetector::enumerate(false));
            QStringList lines;
            QString effProc;

            // 和采集引擎里的优先级保持一致：
            //   设置里指定的进程在跑 -> 用它；没在跑 -> 自动识别兜底。
            // 这里必须如实说清楚"到底会用哪个"，否则老师看到的信息是错的。
            if (m_settings.targetProcess.isEmpty()) {
                effProc = det.processName;
                if (det.ok()) {
                    lines << QStringLiteral("自动识别：<b>%1</b>（%2）")
                                 .arg(det.displayName, det.processName);
                } else {
                    lines << QStringLiteral(
                        "自动识别：当前没有认出白板软件 —— 请在上面手动选一个");
                }
            } else {
                effProc = m_settings.targetProcess;
                const bool running =
                    !WindowDetector::findByProcess({effProc}, true).empty();
                lines << QStringLiteral("已手动指定：%1%2")
                             .arg(effProc,
                                  running
                                      ? QStringLiteral(
                                            "（别的白板在前台时仍会自动切过去）")
                                      : QStringLiteral(
                                            "（当前没在跑，会自动识别兜底）"));
            }

            if (!effProc.isEmpty()) {
                const BoardCalibration cal = m_settings.calibrationFor(effProc);
                if (cal.calibrated) {
                    lines << QStringLiteral(
                                 "本机标定：上 %1% / 下 %2% / 左 %3% / 右 %4%"
                                 "（用了 %5 帧%6）")
                                 .arg(cal.cropTopPct)
                                 .arg(cal.cropBottomPct)
                                 .arg(cal.cropLeftPct)
                                 .arg(cal.cropRightPct)
                                 .arg(cal.samples)
                                 .arg(cal.calibratedAt.isEmpty()
                                          ? QString()
                                          : QStringLiteral("，%1")
                                                .arg(cal.calibratedAt.left(16)
                                                         .replace(QLatin1Char('T'),
                                                                  QLatin1Char(' '))));
                } else {
                    lines << QStringLiteral(
                        "本机标定：还没测过 —— 下次开始记录时会自动测，"
                        "测定之前先用下面的默认裁切比例");
                }
            }
            calLabel->setText(lines.join(QStringLiteral("<br>")));
        };
        describe();

        QObject::connect(calResetBtn, &QPushButton::clicked, &dlg,
                         [this, describe]() {
                             if (m_settings.boardCalibrations.isEmpty()) {
                                 QMessageBox::information(
                                     nullptr, QStringLiteral("清除标定"),
                                     QStringLiteral("本机还没有任何标定记录。"));
                                 return;
                             }
                             const auto ans = QMessageBox::question(
                                 nullptr, QStringLiteral("清除标定"),
                                 QStringLiteral("清掉本机所有白板的自动标定结果？\n\n"
                                                "下一次开始记录时会重新测一遍。\n"
                                                "归档里的图不受影响。"),
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No);
                             if (ans != QMessageBox::Yes) {
                                 return;
                             }
                             m_settings.boardCalibrations.clear();
                             m_settings.save();
                             describe();
                         });

        calLayout->addWidget(calLabel, 1);
        calLayout->addWidget(calResetBtn);
        form->addRow(QStringLiteral("白板识别"), calRow);
    }

    auto *dirRow = new QWidget();
    auto *dirLayout = new QHBoxLayout(dirRow);
    dirLayout->setContentsMargins(0, 0, 0, 0);
    auto *dirEdit = new QLineEdit(m_settings.effectiveOutputDir());
    auto *dirBtn = new QPushButton(QStringLiteral("浏览…"));
    dirLayout->addWidget(dirEdit);
    dirLayout->addWidget(dirBtn);
    connect(dirBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString d = QFileDialog::getExistingDirectory(
            &dlg, QStringLiteral("选择记录保存位置"), dirEdit->text());
        if (!d.isEmpty()) {
            dirEdit->setText(d);
        }
    });
    form->addRow(QStringLiteral("记录保存位置"), dirRow);

    layout->addLayout(form);

    auto *capGroup = new QGroupBox(QStringLiteral("采样"));
    auto *capForm = new QFormLayout(capGroup);

    auto *idleSpin = new QSpinBox();
    idleSpin->setRange(200, 10000);
    idleSpin->setSingleStep(100);
    idleSpin->setSuffix(QStringLiteral(" 毫秒"));
    idleSpin->setValue(m_settings.idleIntervalMs);
    idleSpin->setToolTip(QStringLiteral("画面静止时的采样间隔。越大越省 CPU。"));
    capForm->addRow(QStringLiteral("静止时采样间隔"), idleSpin);

    auto *activeSpin = new QSpinBox();
    activeSpin->setRange(50, 2000);
    activeSpin->setSingleStep(10);
    activeSpin->setSuffix(QStringLiteral(" 毫秒"));
    activeSpin->setValue(m_settings.activeIntervalMs);
    activeSpin->setToolTip(QStringLiteral("检测到画面变化后的加密采样间隔。"));
    capForm->addRow(QStringLiteral("变化后采样间隔"), activeSpin);

    auto *spanSpin = new QSpinBox();
    spanSpin->setRange(10, 3600);
    spanSpin->setSingleStep(10);
    spanSpin->setSuffix(QStringLiteral(" 秒"));
    spanSpin->setValue(m_settings.maxSpanSeconds);
    spanSpin->setToolTip(QStringLiteral(
        "老师连续书写不停笔时，最久多久强制存一张，防止内容丢失。"));
    capForm->addRow(QStringLiteral("最长连续书写"), spanSpin);

    layout->addWidget(capGroup);

    auto *ruleGroup = new QGroupBox(QStringLiteral("关键帧判定（一般不用改）"));
    auto *ruleForm = new QFormLayout(ruleGroup);

    auto *replaceSpin = new QDoubleSpinBox();
    replaceSpin->setRange(0.05, 0.95);
    replaceSpin->setSingleStep(0.05);
    replaceSpin->setDecimals(2);
    replaceSpin->setValue(m_settings.replaceThreshold);
    replaceSpin->setToolTip(QStringLiteral(
        "相似度低于此值判定为「画面大面积替换」（翻页/清屏）。"));
    ruleForm->addRow(QStringLiteral("替换阈值"), replaceSpin);

    auto *moveSpin = new QSpinBox();
    moveSpin->setRange(1, 100);
    moveSpin->setValue(m_settings.moveThreshold);
    moveSpin->setToolTip(QStringLiteral(
        "位移超过多少个「缩略图像素」才认为画面在平移。\n"
        "缩略图宽 480，所以 1 像素约等于屏幕上的 4 像素。"));
    ruleForm->addRow(QStringLiteral("平移判定阈值"), moveSpin);

    auto *movePctSpin = new QSpinBox();
    movePctSpin->setRange(5, 80);
    movePctSpin->setSuffix(QStringLiteral(" %"));
    movePctSpin->setValue(int(m_settings.movePercent));
    movePctSpin->setToolTip(QStringLiteral(
        "累计平移达到画面高度的百分之多少，才值得把「移动前那一屏」存下来。\n"
        "调大：存得少；调小：存得多。老师小幅挪动几十像素时内容还在，不必浪费一张图。"));
    ruleForm->addRow(QStringLiteral("累计平移存图阈值"), movePctSpin);

    auto *sameSpin = new QDoubleSpinBox();
    sameSpin->setRange(0.5, 0.999);
    sameSpin->setSingleStep(0.01);
    sameSpin->setDecimals(2);
    sameSpin->setValue(m_settings.sameThreshold);
    sameSpin->setToolTip(QStringLiteral("相似度高于此值认为画面没变。"));
    ruleForm->addRow(QStringLiteral("「没变」阈值"), sameSpin);

    layout->addWidget(ruleGroup);

    auto *cropGroup = new QGroupBox(QStringLiteral("分析区域裁切（切掉白板软件自带的工具栏）"));
    auto *cropForm = new QFormLayout(cropGroup);

    auto makePct = [](int v, const QString &tip) {
        auto *sp = new QSpinBox();
        sp->setRange(0, 45);
        sp->setSuffix(QStringLiteral(" %"));
        sp->setValue(v);
        sp->setToolTip(tip);
        return sp;
    };
    const QString cropTip = QStringLiteral(
        "白板软件的工具栏、横幅在采集区域内部，而且每帧都一样。\n"
        "不裁掉会把相似度垫高、淹没真实位移，导致一张关键帧都存不下来。\n"
        "希沃白板实测：上 5%，下 23%。");
    auto *cropTop = makePct(m_settings.cropTopPct, cropTip);
    auto *cropBottom = makePct(m_settings.cropBottomPct, cropTip);
    auto *cropLeft = makePct(m_settings.cropLeftPct, cropTip);
    auto *cropRight = makePct(m_settings.cropRightPct, cropTip);
    cropForm->addRow(QStringLiteral("忽略上方"), cropTop);
    cropForm->addRow(QStringLiteral("忽略下方"), cropBottom);
    cropForm->addRow(QStringLiteral("忽略左侧"), cropLeft);
    cropForm->addRow(QStringLiteral("忽略右侧"), cropRight);

    layout->addWidget(cropGroup);

    // ---------------- 课后 AI 整理 ----------------
    auto *aiGroup = new QGroupBox(QStringLiteral("课后 AI 整理（判学科、归档）"));
    auto *aiForm = new QFormLayout(aiGroup);

    auto *aiUrlEdit = new QLineEdit(m_settings.aiBaseUrl);
    aiUrlEdit->setToolTip(QStringLiteral(
        "OpenAI 兼容接口地址，换模型只改这里，不动程序。\n"
        "  通义千问（阿里云百炼） https://dashscope.aliyuncs.com/compatible-mode/v1\n"
        "  智谱                 https://open.bigmodel.cn/api/paas/v4\n"
        "  豆包（火山方舟）       https://ark.cn-beijing.volces.com/api/v3\n"
        "  本地部署 Ollama       http://localhost:11434/v1   （隐私最好，截图不出本机）"));
    aiForm->addRow(QStringLiteral("接口地址"), aiUrlEdit);

    auto *aiKeyEdit = new QLineEdit(m_settings.aiApiKey);
    aiKeyEdit->setEchoMode(QLineEdit::Password);
    aiKeyEdit->setPlaceholderText(QStringLiteral("留空则只归档不判学科"));
    aiKeyEdit->setToolTip(QStringLiteral(
        "视觉模型的 API Key。留空时程序不会联网，\n"
        "所有关键帧会归到「其他」，但归档结构照常生成。"));
    aiForm->addRow(QStringLiteral("API Key"), aiKeyEdit);

    auto *aiModelEdit = new QLineEdit(m_settings.aiModel);
    aiModelEdit->setToolTip(QStringLiteral(
        "要调用的视觉模型名，例如 qwen-vl-max、glm-4v、qwen2.5-vl:7b（本地）。"));
    aiForm->addRow(QStringLiteral("模型名"), aiModelEdit);

    auto *aiDirRow = new QWidget();
    auto *aiDirLayout = new QHBoxLayout(aiDirRow);
    aiDirLayout->setContentsMargins(0, 0, 0, 0);
    auto *aiDirEdit = new QLineEdit(m_settings.effectiveArchiveDir());
    auto *aiDirBtn = new QPushButton(QStringLiteral("浏览…"));
    aiDirLayout->addWidget(aiDirEdit, 1);
    aiDirLayout->addWidget(aiDirBtn);
    QObject::connect(aiDirBtn, &QPushButton::clicked, &dlg, [&dlg, aiDirEdit]() {
        const QString d = QFileDialog::getExistingDirectory(
            &dlg, QStringLiteral("选择归档位置"), aiDirEdit->text());
        if (!d.isEmpty()) {
            aiDirEdit->setText(QDir::toNativeSeparators(d));
        }
    });
    aiForm->addRow(QStringLiteral("归档位置"), aiDirRow);

    // ---- 用量账本 ----
    //
    // 只记账、不显示"套餐还剩多少"：OpenAI 兼容接口拿不到账户余额
    // （那要走各家自己的账单 API 和另一套鉴权）。所以这里只呈现能确定的事实。
    {
        const UsageSummary us = usageSummary();
        const QLocale loc;

        auto *usageLabel = new QLabel();
        usageLabel->setWordWrap(true);
        usageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        auto line = [&loc](const QString &title, const UsageBucket &b) {
            return QStringLiteral("%1：输入 %2 + 输出 %3 = <b>%4</b> token"
                                  " ｜ %5 张图 ｜ %6 次调用")
                .arg(title,
                     loc.toString(b.promptTokens),
                     loc.toString(b.completionTokens),
                     loc.toString(b.total()))
                .arg(b.frames)
                .arg(b.calls);
        };

        QString text = QStringLiteral("<b>AI 用量</b><br>");
        if (us.monthEmpty()) {
            text += QStringLiteral("本月还没有调用过视觉模型。");
        } else {
            text += line(QStringLiteral("本月（%1）").arg(us.monthKey), us.month)
                    + QStringLiteral("<br>")
                    + line(QStringLiteral("今日（%1）").arg(us.dayKey), us.today);
        }
        usageLabel->setText(text);
        // 用**整行**而不是表单的"标签 + 值"两列：这两行字比较长，
        // 塞进值列会被换行切碎（"1 次调用"断成"1 次 / 调用"）。
        aiForm->addRow(usageLabel);

        auto *usageHint = new QLabel(QStringLiteral(
            "缓存命中的图不会重复调用模型，所以也不会重复计入这里。\n"
            "只统计本机本程序的调用 —— 同一个 Key 在别的软件里用掉的量"
            "这里看不到，所以它是参考值，不是账单。"));
        usageHint->setWordWrap(true);
        // 走主题里的 QLabel[m3="hint"]，不要在这里写死颜色 ——
        // 写死了就跟着不走主题（换配色时会留下这几个"漏网"的灰字）
        usageHint->setProperty("m3", "hint");
        aiForm->addRow(usageHint);
    }

    auto *aiHint = new QLabel(QStringLiteral(
        "判断在课后进行，上课时完全不联网。\n"
        "原图只写一次、之后只读；学科判断错了也不会影响原图。"));
    aiHint->setWordWrap(true);
    aiHint->setProperty("m3", "hint");
    aiForm->addRow(aiHint);

    // ---- 定时整理 ----
    auto *autoOrgChk = new QCheckBox(QStringLiteral("每天定时自动整理，不用手动点"));
    autoOrgChk->setChecked(m_settings.autoOrganizeEnabled);
    autoOrgChk->setToolTip(QStringLiteral(
        "到点后程序自己把「还没整理过的」录制记录判完学科并归档。\n"
        "已经整理过的会跳过 —— 不会重复调用模型、不会重复花额度。\n"
        "上课期间完全不联网，所以建议放在放学后。"));
    aiForm->addRow(autoOrgChk);

    auto *autoOrgTime = new QTimeEdit(
        QTime(m_settings.autoOrganizeHour, m_settings.autoOrganizeMinute));
    autoOrgTime->setDisplayFormat(QStringLiteral("HH:mm"));
    autoOrgTime->setToolTip(QStringLiteral(
        "每天到这个点检查一次。默认 18:30，正好放学后。\n"
        "到点时如果还有没整理的记录，就在后台开始整理（不弹窗、不打断你）。\n"
        "正在录课时会自动跳过，等录完下次到点再整理。"));
    aiForm->addRow(QStringLiteral("自动整理时间"), autoOrgTime);

    layout->addWidget(aiGroup);

    // ---- 开机自启动 ----
    //
    // 状态**读注册表**，不读 settings.json —— 老师可能在任务管理器里自己关掉了。
    // 配置里再存一份就会分歧：界面要么撒谎，要么下次启动把人家的选择覆盖回去。
    auto *bootChk = new QCheckBox(QStringLiteral("开机自动启动（登录后自动运行，不弹窗口）"));
    bootChk->setChecked(isAutoStartEnabled());
    bootChk->setToolTip(QStringLiteral(
        "写入当前用户的注册表启动项，**不需要管理员权限**：\n"
        "    %1\n\n"
        "想关掉的话，除了这里，也可以在「任务管理器 → 启动」里禁用它。\n"
        "那个页面显示的名字是「%2」。\n\n"
        "软件整个文件夹挪了位置之后，下次启动会自动把注册表里的路径改过来。")
                            .arg(autoStartRegistryPath(),
                                 QString::fromLatin1(kAutoStartName)));
    layout->addWidget(bootChk);

    auto *autoChk = new QCheckBox(QStringLiteral("程序启动后自动开始记录"));
    autoChk->setChecked(m_settings.startOnLaunch);
    autoChk->setToolTip(QStringLiteral(
        "和上面的「开机自动启动」配合使用 = 老师完全不用管。\n"
        "只开这一个、不开机自启动，就只在你手动双击程序时开始记录。"));
    layout->addWidget(autoChk);

    auto *fgChk = new QCheckBox(QStringLiteral("只记录目标窗口在前台时的画面"));
    fgChk->setChecked(m_settings.requireForeground);
    fgChk->setToolTip(QStringLiteral(
        "开着更安全：不会把弹出的聊天窗口、系统通知写进板书。\n"
        "如果老师习惯一边放课件一边切换窗口，可以关掉。"));
    layout->addWidget(fgChk);

    auto *dialogChk = new QCheckBox(
        QStringLiteral("检测到弹窗时暂停记录（登录窗、加载窗、图形选择面板）"));
    dialogChk->setChecked(m_settings.pauseOnExtraWindows);
    dialogChk->setToolTip(QStringLiteral(
        "白板软件弹登录窗、加载窗、图形面板时，它们是「同进程的另一个窗口」。\n"
        "这时画面的任何变化都不代表板书变化，必须整个跳过，\n"
        "否则会把弹窗当成一屏板书截下来（实测踩过）。\n\n"
        "如果误判太频繁（比如白板本来就常驻多个窗口），可以关掉。\n"
        "排查用命令：BoardMark.exe --watch 60"));
    layout->addWidget(dialogChk);

    auto *panelChk = new QCheckBox(
        QStringLiteral("画面里有工具面板时暂停记录（图形选择、更多菜单）"));
    panelChk->setChecked(m_settings.pauseOnPanel);
    panelChk->setToolTip(QStringLiteral(
        "这类面板不一定是独立窗口，靠窗口枚举抓不到，所以改用画面形态判断：\n"
        "手写笔画很细，腐蚀一下就没了；UI 面板是大块纯色，腐蚀完还剩一大片。\n"
        "这个判据跟底色明暗无关，深色黑板和白色白板一样有效。"));
    layout->addWidget(panelChk);

    auto *hint = new QLabel(QStringLiteral(
        "说明：软件只读取屏幕像素，不解析任何第三方软件的文件，也不修改它们。"));
    hint->setWordWrap(true);
    hint->setProperty("m3", "hint");
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    // ---- 恢复默认值 ----
    // 一线调试时改乱参数是常事，得有个一键回到出厂状态的地方。
    //
    // 刻意**不重置 API Key 和接口地址** —— 那几个填一次很麻烦，
    // 而且不属于"随手会改乱的参数"。这一点在确认框里明说了，
    // 免得老师点了之后发现 key 没了。
    auto *resetBtn = buttons->addButton(QStringLiteral("恢复默认值"),
                                        QDialogButtonBox::ResetRole);
    // 次要动作 -> M3 的 Text button（无边框、无底色）。
    // 必须在控件被 polish 之前设好，QSS 的属性选择器只在 polish 时求值一次。
    resetBtn->setProperty("m3", "text");
    QObject::connect(resetBtn, &QPushButton::clicked, &dlg, [&]() {
        const Settings def;     // 全部取结构体默认值，不读当前配置

        const auto ans = QMessageBox::question(
            &dlg, QStringLiteral("恢复默认值"),
            QStringLiteral("把所有采集和判定参数恢复成出厂默认？\n\n"
                           "会重置：目标软件、保存位置、采样间隔、判定阈值、\n"
                           "              裁切比例、归档位置\n"
                           "会保留：AI 接口地址、API Key、模型名、开机自启动\n"
                           "              （这几项不属于"
                           "「随手会改乱的采集参数」，不跟着重置）\n\n"
                           "恢复后还要点「确定」才会真正生效，点「取消」可以放弃。"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ans != QMessageBox::Yes) {
            return;
        }

        procBox->setCurrentIndex(0);        // 默认 = 自动识别
        dirEdit->setText(QDir::toNativeSeparators(def.effectiveOutputDir()));
        idleSpin->setValue(def.idleIntervalMs);
        activeSpin->setValue(def.activeIntervalMs);
        spanSpin->setValue(def.maxSpanSeconds);
        replaceSpin->setValue(def.replaceThreshold);
        moveSpin->setValue(def.moveThreshold);
        movePctSpin->setValue(int(def.movePercent));
        sameSpin->setValue(def.sameThreshold);
        cropTop->setValue(def.cropTopPct);
        cropBottom->setValue(def.cropBottomPct);
        cropLeft->setValue(def.cropLeftPct);
        cropRight->setValue(def.cropRightPct);
        aiDirEdit->setText(QDir::toNativeSeparators(def.effectiveArchiveDir()));
        autoChk->setChecked(def.startOnLaunch);
        fgChk->setChecked(def.requireForeground);
        dialogChk->setChecked(def.pauseOnExtraWindows);
        panelChk->setChecked(def.pauseOnPanel);
    });

    outer->addWidget(buttons);      // 在滚动区外面，永远可见

    // 初始尺寸按屏幕可用区域算。
    // 内容更高时出现滚动条，而不是把窗口撑出屏幕 —— 这条在开了缩放的
    // 教室一体机上尤其重要。
    QScreen *scr = QGuiApplication::screenAt(QCursor::pos());
    if (!scr) {
        scr = QGuiApplication::primaryScreen();
    }
    if (scr) {
        const QRect avail = scr->availableGeometry();
        const int wantW = qMax(560, content->sizeHint().width() + 60);
        const int wantH = content->sizeHint().height()
                          + buttons->sizeHint().height() + 60;
        const int w = qMin(wantW, int(avail.width() * 0.92));
        const int h = qMin(wantH, int(avail.height() * 0.90));
        dlg.resize(w, h);
        // 下限放宽：内容靠滚动看，不用逼着窗口比屏幕还大
        dlg.setMinimumSize(qMin(400, w), qMin(280, h));

        // 打一行日志：在教室一体机上排查"设置页显示不全"时，
        // 让老师把这一行发过来就能知道屏幕可用区到底多大
        qInfo() << "设置界面" << w << "x" << h
                << "｜屏幕可用区" << avail.width() << "x" << avail.height()
                << "｜内容需要" << content->sizeHint().height();
    }

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    // 目标软件：第 0 项是「自动识别」，存成空串表示自动。
    // 空串在运行时才展开成具体进程名 —— 换了台机器、换了款白板都不用改配置。
    {
        const QString typed = procBox->currentText().trimmed();
        if (typed.isEmpty() || typed == autoLabel) {
            m_settings.targetProcess.clear();
        } else {
            m_settings.targetProcess = typed;
        }
    }
    // 记录位置：和归档位置一样，**等于默认值就存空串**。
    //
    // 实测踩到的坑：界面上的输入框填的是「解析后的绝对路径」，
    // 直接存下来的话，一次「打开设置 → 点确定」就会把开发机的路径
    // （E:/BoardMark/build/recordings）写死进配置，跟着发布版跑到别人电脑上，
    // 于是老师那边一切记录都往一个不存在的目录里写。
    // 存空串 = "跟着 exe 走"，换到哪台机器都是那个 exe 旁边的目录。
    m_settings.outputDir = pathOrEmptyIfDefault(dirEdit->text(),
                                                QStringLiteral("recordings"));
    m_settings.idleIntervalMs = idleSpin->value();
    m_settings.activeIntervalMs = activeSpin->value();
    m_settings.maxSpanSeconds = spanSpin->value();
    m_settings.replaceThreshold = replaceSpin->value();
    m_settings.moveThreshold = moveSpin->value();
    m_settings.movePercent = movePctSpin->value();
    m_settings.sameThreshold = sameSpin->value();
    m_settings.startOnLaunch = autoChk->isChecked();
    m_settings.requireForeground = fgChk->isChecked();
    m_settings.pauseOnExtraWindows = dialogChk->isChecked();
    m_settings.pauseOnPanel = panelChk->isChecked();

    // 开机自启动写注册表（不进 settings.json —— 见 autostart.h 的说明）。
    // 只在状态真的变了才写，避免每次点确定都去动一遍注册表。
    if (bootChk->isChecked() != isAutoStartEnabled()) {
        if (!setAutoStartEnabled(bootChk->isChecked())) {
            QMessageBox::warning(
                nullptr, QStringLiteral("开机自启动"),
                QStringLiteral("写注册表失败，这一项没生效。\n\n"
                               "常见原因：被组策略或安全软件拦住了。\n"
                               "位置：%1")
                    .arg(autoStartRegistryPath()));
            // 把界面拨回真实状态，免得显示的和实际的不一致
            bootChk->setChecked(isAutoStartEnabled());
        }
    }
    m_settings.cropTopPct = cropTop->value();
    m_settings.cropBottomPct = cropBottom->value();
    m_settings.cropLeftPct = cropLeft->value();
    m_settings.cropRightPct = cropRight->value();
    m_settings.aiBaseUrl = aiUrlEdit->text().trimmed();
    m_settings.aiApiKey = aiKeyEdit->text().trimmed();
    m_settings.aiModel = aiModelEdit->text().trimmed();
    if (m_settings.aiModel.isEmpty()) {
        m_settings.aiModel = QStringLiteral("qwen-vl-max");
    }
    {
        // 归档目录同理：等于默认就存空串
        m_settings.aiOutputDir = pathOrEmptyIfDefault(aiDirEdit->text(),
                                                      QStringLiteral("板书笔记"));
    }
    m_settings.autoOrganizeEnabled = autoOrgChk->isChecked();
    m_settings.autoOrganizeHour = autoOrgTime->time().hour();
    m_settings.autoOrganizeMinute = autoOrgTime->time().minute();

    if (!m_settings.save()) {
        QMessageBox::warning(&dlg, QStringLiteral("设置"),
                             QStringLiteral("设置文件写入失败，本次修改可能不会保留。"));
    }

    notify(QStringLiteral("设置已保存"),
           QStringLiteral("目标软件：%1").arg(m_settings.targetProcess));
}

void TrayApp::onOpenFolder()
{
    const QString dir = m_settings.effectiveOutputDir();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void TrayApp::onOpenArchiveFolder()
{
    // 归档目录可能还不存在（一次都没整理过）—— 先建出来再打开，
    // 否则资源管理器会弹一个"找不到"的框，老师以为程序坏了。
    const QString dir = m_settings.effectiveArchiveDir();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void TrayApp::onAbout()
{
    QMessageBox::information(
        nullptr,
        QStringLiteral("关于"),
        QStringLiteral(
            "<b>BoardMark %1</b><br>课堂板书自动记录<br><br>"
            "后台记录白板软件上的板书，按「变化前那一帧」自动留存关键画面。<br><br>"
            "设计原则：<br>"
            "· 只读取屏幕像素，不解析、不修改任何第三方软件<br>"
            "· 原始截图只写一次，之后只读<br>"
            "· 不做逐字识别，原始画面永远是最可靠的依据<br><br>"
            "保存位置：%2")
            .arg(QString::fromUtf8(cm::versionString()),
                 QDir::toNativeSeparators(m_settings.effectiveOutputDir())));
}

bool TrayApp::confirmQuitWhileRecording()
{
    if (!m_recording) {
        return true;
    }
    const auto btn = QMessageBox::question(
        nullptr, QStringLiteral("正在记录"),
        QStringLiteral("现在还在记录中，已保存 %1 张关键帧。\n确定要退出吗？")
            .arg(m_lastCount),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return btn == QMessageBox::Yes;
}

void TrayApp::onQuit()
{
    if (!confirmQuitWhileRecording()) {
        return;
    }
    m_quitting = true;
    if (m_engine) {
        m_engine->requestStop();
        m_engine->wait(5000);
    }
    if (m_tray) {
        m_tray->hide();
    }
    QApplication::quit();
}

} // namespace cm

// Q_OBJECT 的类定义在 .cpp 里，需要让 moc 把生成的代码包含进来
#include "tray_app.moc"
