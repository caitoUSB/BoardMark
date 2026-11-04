#include "app/theme.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <QString>
#include <QStyleFactory>

namespace cm {

namespace m3 {
const char *kPrimary = "#6750A4";
const char *kOnPrimary = "#FFFFFF";
const char *kPrimaryContainer = "#EADDFF";
const char *kOnPrimaryContainer = "#21005D";
const char *kSurface = "#FFFBFE";
const char *kOnSurface = "#1C1B1F";
const char *kSurfaceContainer = "#F3EDF7";
const char *kOnSurfaceVariant = "#49454F";
const char *kOutline = "#79747E";
const char *kOutlineVariant = "#CAC4D0";
const char *kPrimaryState8 = "#F2EDF6";
const char *kPrimaryState12 = "#ECE6F3";
} // namespace m3

namespace {

/// 选一个存在的界面字体。M3 用 Roboto，中文环境退到微软雅黑。
QFont uiFont()
{
    const QStringList families = QFontDatabase::families();
    const QStringList wanted = {
        QStringLiteral("Microsoft YaHei UI"),
        QStringLiteral("Microsoft YaHei"),
        QStringLiteral("Segoe UI"),
    };
    for (const QString &w : wanted) {
        if (families.contains(w)) {
            return QFont(w, 10);
        }
    }
    return QApplication::font();
}

/// 调色板负责"没被 QSS 明确覆盖到的地方"（原生绘制的部分、禁用态、文本选中色等）。
/// QSS 负责形状和状态。两者都要给，只给 QSS 会留下一些漏网的原生颜色。
QPalette materialPalette()
{
    QPalette p;
    const QColor surface(m3::kSurface);
    const QColor onSurface(m3::kOnSurface);
    const QColor primary(m3::kPrimary);
    const QColor primaryContainer(m3::kPrimaryContainer);
    const QColor onPrimaryContainer(m3::kOnPrimaryContainer);
    const QColor disabled(m3::kOutlineVariant);

    p.setColor(QPalette::Window, surface);
    p.setColor(QPalette::WindowText, onSurface);
    p.setColor(QPalette::Base, QColor(QStringLiteral("#FFFFFF")));
    p.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#F7F2FA")));
    p.setColor(QPalette::Text, onSurface);
    p.setColor(QPalette::Button, surface);
    p.setColor(QPalette::ButtonText, onSurface);
    p.setColor(QPalette::BrightText, QColor(QStringLiteral("#FFFFFF")));
    p.setColor(QPalette::Highlight, primaryContainer);
    p.setColor(QPalette::HighlightedText, onPrimaryContainer);
    p.setColor(QPalette::PlaceholderText, QColor(m3::kOutline));
    p.setColor(QPalette::Link, primary);
    p.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#313033")));
    p.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#F4EFF4")));

    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight,
               QColor(QStringLiteral("#E7E0EC")));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return p;
}

QString materialStyleSheet()
{
    // 说明：`QWidget { background: transparent; }` 是刻意的 ——
    // 否则卡片（QGroupBox）里面的普通容器和标签会各自刷一层底色，
    // 在卡片上盖出一个个方块。需要实底的地方下面都单独指定了。
    return QStringLiteral(R"QSS(
/* ================= 基础 ================= */
QWidget {
    background-color: transparent;
    color: #1C1B1F;
    font-size: 14px;
}
QDialog, QMessageBox, QProgressDialog {
    background-color: #FFFBFE;
}
QLabel { background-color: transparent; }
QToolTip {
    background-color: #313033;
    color: #F4EFF4;
    border: none;
    border-radius: 4px;
    padding: 6px 10px;
    font-size: 12px;
}

/* 提示类文字：body-small + on-surface-variant */
QLabel[m3="hint"] {
    color: #49454F;
    font-size: 12px;
}

/* ================= 卡片（分组框）================= */
/*
 * 这里的 margin / padding 是刻意压过的：M3 的留白比 Qt 默认大得多，
 * 一开始照 M3 取值时设置页内容从 928px 涨到 1414px，
 * 而教室一体机上可用高度只有 824px —— 老师要多滚两屏。
 * 所以把纵向留白压到"看得出是卡片、但不至于太占地方"。
 */
QGroupBox {
    background-color: #F3EDF7;
    border: 1px solid #CAC4D0;
    border-radius: 12px;
    margin-top: 20px;
    padding: 0px 10px 8px 10px;
    font-weight: bold;
    color: #1C1B1F;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 4px;
    top: 0px;
    padding: 0px 0px 6px 0px;
    color: #49454F;
    background-color: transparent;
}

/* ================= 输入类（outlined text field）================= */
QLineEdit, QSpinBox, QDoubleSpinBox, QTimeEdit, QComboBox {
    background-color: #FFFFFF;
    border: 1px solid #79747E;
    border-radius: 4px;
    /* 右边留出 30px 给微调箭头 / 下拉箭头，否则长文本会压到箭头底下 */
    padding: 3px 30px 3px 9px;
    min-height: 20px;
    color: #1C1B1F;
    selection-background-color: #EADDFF;
    selection-color: #21005D;
}

/*
 * QSpinBox / QDoubleSpinBox / QTimeEdit / 可编辑的 QComboBox 内部都藏着一个
 * QLineEdit。上面那条 QLineEdit 规则会连它一起套上，于是**画出双边框**
 * （外面一层是 QSpinBox 的，里面一层是它内部编辑框的）。
 * 这里把内部编辑框的框线彻底去掉，只留外面那层。
 */
QSpinBox QLineEdit, QDoubleSpinBox QLineEdit,
QTimeEdit QLineEdit, QComboBox QLineEdit {
    border: none;
    background-color: transparent;
    padding: 0px;
    min-height: 0px;
}
QSpinBox QLineEdit:focus, QDoubleSpinBox QLineEdit:focus,
QTimeEdit QLineEdit:focus, QComboBox QLineEdit:focus {
    border: none;
    padding: 0px;
}

QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover,
QTimeEdit:hover, QComboBox:hover {
    border-color: #1C1B1F;
}
/* 聚焦时边框加粗到 2px，内边距相应减 1，避免整行跳动 */
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus,
QTimeEdit:focus, QComboBox:focus, QComboBox:on {
    border: 2px solid #6750A4;
    padding: 2px 29px 2px 8px;
}
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled,
QTimeEdit:disabled, QComboBox:disabled {
    background-color: #F3EDF7;
    border-color: #CAC4D0;
    color: #CAC4D0;
}

/* 微调按钮：去掉原生外框，只留一个可点的箭头区 */
QSpinBox::up-button, QDoubleSpinBox::up-button, QTimeEdit::up-button {
    subcontrol-origin: border;
    subcontrol-position: top right;
    width: 24px;
    border: none;
    background-color: transparent;
    border-top-right-radius: 4px;
}
QSpinBox::down-button, QDoubleSpinBox::down-button, QTimeEdit::down-button {
    subcontrol-origin: border;
    subcontrol-position: bottom right;
    width: 24px;
    border: none;
    background-color: transparent;
    border-bottom-right-radius: 4px;
}
QSpinBox::up-button:hover, QSpinBox::down-button:hover,
QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover,
QTimeEdit::up-button:hover, QTimeEdit::down-button:hover {
    background-color: #EADDFF;
}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow, QTimeEdit::up-arrow {
    image: url(:/m3/icons/chevron_up.svg);
    width: 12px;
    height: 8px;
}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow, QTimeEdit::down-arrow {
    image: url(:/m3/icons/chevron_down.svg);
    width: 12px;
    height: 8px;
}
QSpinBox::up-arrow:disabled, QSpinBox::up-arrow:off,
QSpinBox::down-arrow:disabled, QSpinBox::down-arrow:off {
    image: none;
}

/* 下拉框 */
QComboBox::drop-down {
    subcontrol-origin: border;
    subcontrol-position: center right;
    width: 28px;
    border: none;
    background-color: transparent;
}
QComboBox::down-arrow {
    image: url(:/m3/icons/chevron_down.svg);
    width: 12px;
    height: 8px;
}
QComboBox QAbstractItemView {
    background-color: #F3EDF7;
    border: 1px solid #CAC4D0;
    border-radius: 8px;
    padding: 6px;
    outline: none;
    selection-background-color: #EADDFF;
    selection-color: #21005D;
}

/* ================= 开关（替代复选框）================= */
QCheckBox {
    spacing: 8px;
    background-color: transparent;
}
QCheckBox::indicator {
    width: 44px;
    height: 24px;
    image: url(:/m3/icons/switch_off.svg);
}
QCheckBox::indicator:checked {
    image: url(:/m3/icons/switch_on.svg);
}
QCheckBox::indicator:disabled {
    image: url(:/m3/icons/switch_off_disabled.svg);
}

/* ================= 按钮 ================= */
/* 默认 = Outlined button；:default（对话框的确定键）= Filled button */
QPushButton {
    background-color: transparent;
    border: 1px solid #79747E;
    border-radius: 18px;
    padding: 0px 20px;
    min-height: 36px;
    min-width: 72px;
    color: #6750A4;
    font-weight: bold;
}
QPushButton:hover   { background-color: #F2EDF6; }
QPushButton:pressed { background-color: #ECE6F3; }
QPushButton:default {
    background-color: #6750A4;
    border: none;
    color: #FFFFFF;
}
QPushButton:default:hover   { background-color: #735EAB; }
QPushButton:default:pressed { background-color: #5A448F; }
QPushButton:disabled {
    background-color: transparent;
    border-color: #CAC4D0;
    color: #CAC4D0;
}
/* Text button（次要动作，例如「恢复默认值」）*/
QPushButton[m3="text"] {
    border: none;
    background-color: transparent;
    padding: 0px 12px;
    min-width: 56px;
}
QPushButton[m3="text"]:hover   { background-color: #F2EDF6; }
QPushButton[m3="text"]:pressed { background-color: #ECE6F3; }

QDialogButtonBox QPushButton { margin-left: 8px; }

/* ================= 菜单 ================= */
QMenu {
    background-color: #F3EDF7;
    border: 1px solid #CAC4D0;
    border-radius: 8px;
    padding: 8px;
}
QMenu::item {
    padding: 8px 16px;
    border-radius: 4px;
    color: #1C1B1F;
}
QMenu::item:selected {
    background-color: #EADDFF;
    color: #21005D;
}
QMenu::item:disabled { color: #CAC4D0; }
QMenu::separator {
    height: 1px;
    background-color: #CAC4D0;
    margin: 6px 8px;
}

/* ================= 滚动条 ================= */
QScrollArea { border: none; background-color: transparent; }
QScrollBar:vertical {
    background-color: transparent;
    width: 12px;
    margin: 0px;
}
QScrollBar::handle:vertical {
    background-color: #79747E;
    min-height: 40px;
    border-radius: 6px;
}
QScrollBar::handle:vertical:hover { background-color: #49454F; }
QScrollBar:horizontal {
    background-color: transparent;
    height: 12px;
    margin: 0px;
}
QScrollBar::handle:horizontal {
    background-color: #79747E;
    min-width: 40px;
    border-radius: 6px;
}
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background-color: transparent; }

/* ================= 进度条（M3 线性）================= */
QProgressBar {
    background-color: #E7E0EC;
    border: none;
    border-radius: 4px;
    min-height: 8px;
    max-height: 8px;
    color: transparent;
}
QProgressBar::chunk {
    background-color: #6750A4;
    border-radius: 4px;
}

/* ================= 其它 ================= */
QFileDialog QListView, QFileDialog QTreeView {
    background-color: #FFFFFF;
    border: 1px solid #CAC4D0;
    border-radius: 4px;
}
)QSS");
}

} // namespace

void applyMaterialTheme()
{
    // 先用 Fusion 打底：原生样式的部分控件是系统绘制的，QSS 盖不干净，
    // 而且 Win10 / Win11 长得不一样。见头文件说明。
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        QApplication::setStyle(fusion);
    }
    QApplication::setFont(uiFont());
    QApplication::setPalette(materialPalette());
    qApp->setStyleSheet(materialStyleSheet());
}

} // namespace cm
