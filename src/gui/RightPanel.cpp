#include "RightPanel.h"
#include "mavlink/TelemetryState.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QAbstractSpinBox>
#include <QPainter>
#include <QPen>
#include <cmath>
#include <spdlog/spdlog.h>

namespace vtol {

// ══════════════════════════════════════════════════════════════════════════════
//  Panel color palette (matches gcs-panel-v3.html)
// ══════════════════════════════════════════════════════════════════════════════
namespace C {
    constexpr auto BG      = "#0c1225";
    constexpr auto CARD    = "#111a33";
    constexpr auto CARD_H  = "#162040";
    constexpr auto INPUT   = "#0a0f22";
    constexpr auto BD      = "#1a2545";
    constexpr auto BD_A    = "#2a3a6a";
    constexpr auto TXT     = "#e8ecf4";
    constexpr auto TXT_S   = "#6a7a9b";
    constexpr auto TXT_D   = "#3d4e6e";
    constexpr auto GREEN   = "#00e6a0";
    constexpr auto GREEN_D = "rgba(0,230,160,24)";
    constexpr auto BLUE    = "#3b8bff";
    constexpr auto BLUE_D  = "rgba(59,139,255,24)";
    constexpr auto ORANGE  = "#ff9933";
    constexpr auto ORANGE_D = "rgba(255,153,51,24)";
    constexpr auto RED     = "#ff4466";
    constexpr auto YELLOW  = "#ffd54f";
    constexpr auto CYAN    = "#00d4ff";
    constexpr auto MONO    = "Consolas";
    constexpr auto SANS    = "Segoe UI";
}

// ── Painted helper icons ──────────────────────────────────────────────────────
static QIcon makeCheckIcon(int sz, const QColor& col)
{
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(col, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(sz*0.20, sz*0.52), QPointF(sz*0.40, sz*0.75));
    p.drawLine(QPointF(sz*0.40, sz*0.75), QPointF(sz*0.80, sz*0.28));
    return QIcon(pm);
}

static QIcon makeChevronDown(int sz, const QColor& col)
{
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(col, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(sz*0.22, sz*0.35), QPointF(sz*0.50, sz*0.65));
    p.drawLine(QPointF(sz*0.50, sz*0.65), QPointF(sz*0.78, sz*0.35));
    return QIcon(pm);
}

static QIcon makeArrowIcon(int sz, const QColor& col, bool left)
{
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(col, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (left) {
        p.drawLine(QPointF(sz*0.6, sz*0.2), QPointF(sz*0.3, sz*0.5));
        p.drawLine(QPointF(sz*0.3, sz*0.5), QPointF(sz*0.6, sz*0.8));
    } else {
        p.drawLine(QPointF(sz*0.4, sz*0.2), QPointF(sz*0.7, sz*0.5));
        p.drawLine(QPointF(sz*0.7, sz*0.5), QPointF(sz*0.4, sz*0.8));
    }
    return QIcon(pm);
}

static QIcon makeGearIcon(int sz, const QColor& col)
{
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(col, 1.4));
    p.setBrush(Qt::NoBrush);
    double cx = sz/2.0, cy = sz/2.0;
    p.drawEllipse(QPointF(cx, cy), sz*0.15, sz*0.15);
    for (int i = 0; i < 8; ++i) {
        double a = i * M_PI / 4.0;
        p.drawLine(QPointF(cx + sz*0.22*std::cos(a), cy + sz*0.22*std::sin(a)),
                   QPointF(cx + sz*0.40*std::cos(a), cy + sz*0.40*std::sin(a)));
    }
    return QIcon(pm);
}

// ══════════════════════════════════════════════════════════════════════════════
//  PanelCell
// ══════════════════════════════════════════════════════════════════════════════

PanelCell::PanelCell(const QString& label, const QString& unit,
                     const QString& color, QWidget* parent)
    : QWidget(parent), m_unit(unit)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(3);

    m_labelWidget = new QLabel(label.toUpper());
    m_labelWidget->setStyleSheet(QString(
        "color: %1; font-size: 10px; font-weight: 500; "
        "letter-spacing: 0.5px; border: none;").arg(C::TXT_D));
    lay->addWidget(m_labelWidget);

    m_valueLabel = new QLabel("---");
    setColor(color);
    lay->addWidget(m_valueLabel);
}

void PanelCell::setValue(double value, int decimals)
{
    QString t = decimals == 0 ? QString::number(static_cast<int>(value))
                              : QString::number(value, 'f', decimals);
    if (!m_unit.isEmpty()) t += " " + m_unit;
    m_valueLabel->setText(t);
}

void PanelCell::setText(const QString& text) { m_valueLabel->setText(text); }

void PanelCell::setColor(const QString& color)
{
    if (m_color == color) return;
    m_color = color;
    m_valueLabel->setStyleSheet(QString(
        "color: %1; font-family: \"%2\"; font-size: 18px; "
        "font-weight: 700; line-height: 1.1; border: none;")
        .arg(color, C::MONO));
}

// ══════════════════════════════════════════════════════════════════════════════
//  RightPanel — construction
// ══════════════════════════════════════════════════════════════════════════════

RightPanel::RightPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

// ── Helpers ───────────────────────────────────────────────────────────────────

QFrame* RightPanel::makeSeparator()
{
    auto* f = new QFrame;
    f->setFixedHeight(1);
    f->setStyleSheet(QString("background-color: %1;").arg(C::BD));
    return f;
}

QFrame* RightPanel::makeDataRow(std::initializer_list<QWidget*> cells)
{
    auto* card = new QFrame;
    card->setStyleSheet(QString(
        "QFrame { background-color: %1; border: 1px solid %2; border-radius: 10px; }")
        .arg(C::CARD, C::BD));
    auto* grid = new QGridLayout(card);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    int i = 0;
    for (auto* cell : cells) {
        if (i > 0) {
            auto* sep = new QFrame;
            sep->setFixedWidth(1);
            sep->setStyleSheet(QString("background-color: %1;").arg(C::BD));
            grid->addWidget(sep, 0, i*2-1);
        }
        grid->addWidget(cell, 0, i*2);
        grid->setColumnStretch(i*2, 1);
        ++i;
    }
    return card;
}

QFrame* RightPanel::makeBlock(const QString& icon, const QString& title,
                               QWidget* content, QLabel** badgeOut)
{
    auto* block = new QFrame;
    block->setStyleSheet(QString(
        "QFrame { border: none; border-bottom: 1px solid %1; }").arg(C::BD));
    auto* lay = new QVBoxLayout(block);
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(10);

    // Header row
    auto* hdr = new QHBoxLayout;
    hdr->setSpacing(8);
    hdr->setContentsMargins(0,0,0,0);

    auto* iconLbl = new QLabel(icon);
    iconLbl->setStyleSheet(QString("color: %1; font-size: 14px; border: none;").arg(C::TXT_D));
    hdr->addWidget(iconLbl);

    auto* titleLbl = new QLabel(title.toUpper());
    titleLbl->setStyleSheet(QString(
        "color: %1; font-size: 11px; font-weight: 600; "
        "letter-spacing: 1px; border: none;").arg(C::TXT_S));
    hdr->addWidget(titleLbl);
    hdr->addStretch();

    if (badgeOut) {
        auto* badge = new QLabel;
        badge->setStyleSheet(QString(
            "color: %1; font-family: \"%2\"; font-size: 10px; font-weight: 700; "
            "padding: 3px 10px; border-radius: 5px; border: none; "
            "background-color: %3;").arg(C::TXT_D, C::MONO, C::INPUT));
        *badgeOut = badge;
        hdr->addWidget(badge);
    }

    lay->addLayout(hdr);
    lay->addWidget(content);
    return block;
}

QWidget* RightPanel::makeScrollPage(QWidget* content)
{
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");
    scroll->setWidget(content);
    return scroll;
}

// ══════════════════════════════════════════════════════════════════════════════
//  setupUi
// ══════════════════════════════════════════════════════════════════════════════

void RightPanel::setupUi()
{
    setStyleSheet(QString("background-color: %1;").arg(C::BG));
    setMinimumWidth(280);
    setMaximumWidth(800);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildHeader());
    root->addWidget(makeSeparator());
    root->addWidget(buildTabBar());

    m_stack = new QStackedWidget;
    m_stack->addWidget(makeScrollPage(buildMainTab()));
    m_stack->addWidget(makeScrollPage(buildRouteTab()));
    m_stack->addWidget(makeScrollPage(buildSystemTab()));
    root->addWidget(m_stack, 1);

    root->addWidget(makeSeparator());
    root->addWidget(buildCommandArea());
    root->addWidget(makeSeparator());
    root->addWidget(buildBottomNav());

    // Blink timer for status dot
    m_dotTimer = new QTimer(this);
    m_dotTimer->setInterval(1000);
    connect(m_dotTimer, &QTimer::timeout, this, [this] {
        m_dotBlink = !m_dotBlink;
        if (m_statusDot)
            m_statusDot->setStyleSheet(QString(
                "border-radius: 5px; background-color: %1;")
                .arg(m_dotBlink ? C::GREEN : "#1a3020"));
    });
}

// ══════════════════════════════════════════════════════════════════════════════
//  Header
// ══════════════════════════════════════════════════════════════════════════════

QWidget* RightPanel::buildHeader()
{
    auto* hdr = new QWidget;
    hdr->setFixedHeight(46);
    hdr->setStyleSheet(QString(
        "background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        " stop:0 #0f1630, stop:1 %1);").arg(C::BG));
    auto* lay = new QHBoxLayout(hdr);
    lay->setContentsMargins(14, 0, 14, 0);
    lay->setSpacing(8);

    m_btnConnect = new QPushButton(QStringLiteral("Подключить"));
    m_btnConnect->setFixedHeight(30);
    m_btnConnect->setStyleSheet(QString(
        "QPushButton { background-color: %1; color: #000; border: none; "
        "border-radius: 6px; padding: 0 14px; font-family: \"%3\"; "
        "font-size: 12px; font-weight: 700; }"
        "QPushButton:hover { background-color: #00ffb3; }")
        .arg(C::GREEN, C::SANS));
    lay->addWidget(m_btnConnect);

    m_btnDisconnect = new QPushButton(QStringLiteral("Откл."));
    m_btnDisconnect->setFixedHeight(30);
    m_btnDisconnect->setEnabled(false);
    m_btnDisconnect->setStyleSheet(QString(
        "QPushButton { background: transparent; color: %1; "
        "border: 1px solid %2; border-radius: 6px; "
        "padding: 0 12px; font-size: 12px; font-weight: 600; }"
        "QPushButton:hover { border-color: %3; color: %4; }"
        "QPushButton:disabled { color: %5; border-color: %2; }")
        .arg(C::TXT_S, C::BD, C::TXT_S, C::TXT, C::TXT_D));
    lay->addWidget(m_btnDisconnect);

    lay->addStretch();

    m_lblMode = new QLabel(QStringLiteral("---"));
    m_lblMode->setStyleSheet(QString(
        "color: %1; font-family: \"%2\"; font-size: 12px; font-weight: 600; "
        "padding: 4px 12px; border-radius: 4px; border: 1px solid %3; "
        "background-color: %4;")
        .arg(C::BLUE, C::MONO, QString("%1").arg(C::BLUE).replace("#","#30"), C::BLUE_D));
    lay->addWidget(m_lblMode);

    m_statusDot = new QFrame;
    m_statusDot->setFixedSize(10, 10);
    m_statusDot->setStyleSheet(QString(
        "border-radius: 5px; background-color: %1;").arg(C::TXT_D));
    lay->addWidget(m_statusDot);

    connect(m_btnConnect, &QPushButton::clicked, this, &RightPanel::connectRequested);
    connect(m_btnDisconnect, &QPushButton::clicked, this, &RightPanel::disconnectRequested);

    return hdr;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Tab bar
// ══════════════════════════════════════════════════════════════════════════════

QWidget* RightPanel::buildTabBar()
{
    auto* bar = new QWidget;
    bar->setFixedHeight(36);
    bar->setStyleSheet(QString("background-color: %1;").arg(C::BG));
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(12, 0, 12, 0);
    lay->setSpacing(0);

    const QStringList labels = {
        QStringLiteral("Основное"),
        QStringLiteral("Маршрут"),
        QStringLiteral("Система")
    };

    for (int i = 0; i < 3; ++i) {
        auto* col = new QWidget;
        col->setStyleSheet("background: transparent;");
        auto* colLay = new QVBoxLayout(col);
        colLay->setContentsMargins(0, 0, 0, 0);
        colLay->setSpacing(0);

        auto* btnRow = new QHBoxLayout;
        btnRow->setContentsMargins(0, 0, 0, 0);
        btnRow->setSpacing(4);

        m_tabBtns[i] = new QPushButton(labels[i]);
        m_tabBtns[i]->setStyleSheet(QString(
            "QPushButton { background: transparent; color: %1; font-family: \"%2\"; "
            "font-size: 12px; font-weight: 500; padding: 8px 6px 6px 6px; border: none; }"
            "QPushButton:hover { color: %3; }")
            .arg(C::TXT_S, C::SANS, C::TXT));
        m_tabBtns[i]->setCursor(Qt::PointingHandCursor);
        btnRow->addWidget(m_tabBtns[i]);

        if (i == 1) {
            m_routeBadge = new QLabel("0");
            m_routeBadge->setStyleSheet(QString(
                "color: %1; font-family: \"%2\"; font-size: 9px; font-weight: 700; "
                "background-color: %3; padding: 1px 5px; border-radius: 3px; "
                "border: none;").arg(C::BLUE, C::MONO, C::BLUE_D));
            btnRow->addWidget(m_routeBadge);
        }

        colLay->addLayout(btnRow);

        m_tabIndicators[i] = new QFrame;
        m_tabIndicators[i]->setFixedHeight(2);
        m_tabIndicators[i]->setStyleSheet(QString("background-color: %1;")
            .arg(i == 0 ? C::BLUE : "transparent"));
        colLay->addWidget(m_tabIndicators[i]);

        lay->addWidget(col, 1);

        const int ti = i;
        connect(m_tabBtns[i], &QPushButton::clicked, this, [this, ti] {
            switchTab(ti);
        });
    }

    // Active tab initial style
    m_tabBtns[0]->setStyleSheet(QString(
        "QPushButton { background: transparent; color: %1; font-family: \"%2\"; "
        "font-size: 12px; font-weight: 600; padding: 8px 6px 6px 6px; border: none; }"
        "QPushButton:hover { color: %1; }")
        .arg(C::TXT, C::SANS));

    return bar;
}

void RightPanel::switchTab(int index)
{
    if (index == m_activeTab) return;
    for (int i = 0; i < 3; ++i) {
        bool active = (i == index);
        m_tabBtns[i]->setStyleSheet(QString(
            "QPushButton { background: transparent; color: %1; font-family: \"%2\"; "
            "font-size: 12px; font-weight: %3; padding: 8px 6px 6px 6px; border: none; }"
            "QPushButton:hover { color: %4; }")
            .arg(active ? C::TXT : C::TXT_S, C::SANS,
                 active ? "600" : "500",
                 active ? C::TXT : C::TXT));
        m_tabIndicators[i]->setStyleSheet(QString("background-color: %1;")
            .arg(active ? C::BLUE : "transparent"));
    }
    m_stack->setCurrentIndex(index);
    m_activeTab = index;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Tab 0 — Основное
// ══════════════════════════════════════════════════════════════════════════════

QWidget* RightPanel::buildMainTab()
{
    auto* page = new QWidget;
    page->setStyleSheet("background: transparent;");
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // ── Телеметрия ──
    {
        m_airspeed    = new PanelCell("Возд. скорость", "м/с", C::GREEN);
        m_groundspeed = new PanelCell("Пут. скорость",  "м/с", C::BLUE);
        m_altAgl      = new PanelCell("Высота отн.",    "м",   C::TXT);

        // Wind cell with dropdown
        auto* windCell = new QWidget;
        windCell->setStyleSheet("background: transparent;");
        auto* wcLay = new QVBoxLayout(windCell);
        wcLay->setContentsMargins(10, 8, 10, 8);
        wcLay->setSpacing(3);

        auto* wHdr = new QHBoxLayout;
        wHdr->setContentsMargins(0,0,0,0);
        auto* wLbl = new QLabel("ВЕТЕР");
        wLbl->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: 500; "
                                     "letter-spacing: 0.5px; border: none;").arg(C::TXT_D));
        wHdr->addWidget(wLbl);
        wHdr->addStretch();

        m_btnWindDrop = new QPushButton;
        m_btnWindDrop->setIcon(makeChevronDown(14, QColor(C::TXT_S)));
        m_btnWindDrop->setIconSize({14, 14});
        m_btnWindDrop->setFixedSize(22, 22);
        m_btnWindDrop->setCursor(Qt::PointingHandCursor);
        m_btnWindDrop->setStyleSheet(QString(
            "QPushButton { background: transparent; border: 1px solid %1; border-radius: 4px; }"
            "QPushButton:hover { border-color: %2; }").arg(C::BD, C::BD_A));
        wHdr->addWidget(m_btnWindDrop);
        wcLay->addLayout(wHdr);

        m_wind = new PanelCell("", "", C::CYAN);
        // Remove the inner label of m_wind — it's already represented by wLbl
        // We just use setValue/setText on the value label
        m_wind->setFixedHeight(30);
        wcLay->addWidget(m_wind);

        m_battery = new PanelCell("Батарея",   "В", C::ORANGE);
        m_gps     = new PanelCell("GPS",        "",  C::GREEN);

        auto* telContent = new QWidget;
        telContent->setStyleSheet("background: transparent;");
        auto* telLay = new QVBoxLayout(telContent);
        telLay->setContentsMargins(0, 0, 0, 0);
        telLay->setSpacing(8);
        telLay->addWidget(makeDataRow({m_airspeed, m_groundspeed, m_altAgl}));
        telLay->addWidget(makeDataRow({windCell, m_battery, m_gps}));

        QLabel* dummy = nullptr;
        lay->addWidget(makeBlock(QStringLiteral("\u25CE"),
                                 QStringLiteral("Телеметрия"),
                                 telContent, &dummy));
        if (dummy) dummy->hide();
    }

    // ── Навигация ──
    {
        m_wpCell   = new PanelCell("Точка",      "",   C::GREEN);
        m_distCell = new PanelCell("Расстояние", "",   C::RED);
        m_etaCell  = new PanelCell("Время",      "",   C::TXT);
        m_xtkCell  = new PanelCell("Отклонение", "",   C::GREEN);

        auto* navContent = new QWidget;
        navContent->setStyleSheet("background: transparent;");
        auto* navLay = new QVBoxLayout(navContent);
        navLay->setContentsMargins(0, 0, 0, 0);
        navLay->setSpacing(0);
        navLay->addWidget(makeDataRow({m_wpCell, m_distCell, m_etaCell, m_xtkCell}));

        QLabel* dummy = nullptr;
        lay->addWidget(makeBlock(QStringLiteral("\u25C7"),
                                 QStringLiteral("Навигация"),
                                 navContent, &dummy));
        if (dummy) dummy->hide();
    }

    // ── Автопилот ──
    {
        auto* apOuter = new QWidget;
        apOuter->setStyleSheet("background: transparent;");
        auto* apOuterLay = new QVBoxLayout(apOuter);
        apOuterLay->setContentsMargins(0, 0, 0, 0);
        apOuterLay->setSpacing(0);

        m_apContent = new QWidget;
        m_apContent->setStyleSheet("background: transparent;");
        m_apContentLay = new QVBoxLayout(m_apContent);
        m_apContentLay->setContentsMargins(0, 0, 0, 0);
        m_apContentLay->setSpacing(6);
        apOuterLay->addWidget(m_apContent);

        lay->addWidget(makeBlock(QStringLiteral("\u2699"),
                                 QStringLiteral("Автопилот"),
                                 apOuter, &m_apBadge));

        rebuildAutopilotContent(false);
    }

    lay->addStretch();

    // Wind popup (modal popup above panel header area)
    setupWindPopup();

    return page;
}

void RightPanel::setupWindPopup()
{
    m_windPopup = new QFrame(this, Qt::Popup);
    m_windPopup->setStyleSheet(QString(
        "QFrame { background-color: %1; border: 1px solid %2; border-radius: 10px; }")
        .arg(C::CARD, C::BD_A));
    auto* lay = new QVBoxLayout(m_windPopup);
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("Задать ветер"));
    title->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: 700; border: none;")
                             .arg(C::TXT_S));
    lay->addWidget(title);

    QString spinStyle = QString(
        "QSpinBox { background-color: %1; color: %2; border: 1px solid %3; "
        "border-radius: 6px; padding: 4px 8px; font-family: \"%4\"; "
        "font-size: 13px; font-weight: 600; }"
        "QSpinBox:focus { border-color: %5; }")
        .arg(C::INPUT, C::TXT, C::BD, C::MONO, C::BLUE);

    auto addField = [&](const QString& lbl, QSpinBox*& sp, int lo, int hi, const QString& sfx) {
        auto* l = new QLabel(lbl);
        l->setStyleSheet(QString("color: %1; font-size: 10px; border: none;").arg(C::TXT_D));
        lay->addWidget(l);
        sp = new QSpinBox;
        sp->setButtonSymbols(QAbstractSpinBox::NoButtons);
        sp->setRange(lo, hi);
        sp->setSuffix(sfx);
        sp->setFixedHeight(30);
        sp->setStyleSheet(spinStyle);
        lay->addWidget(sp);
    };
    addField(QStringLiteral("Направление"), m_spinWindDir, 0, 360, QStringLiteral("°"));
    m_spinWindDir->setWrapping(true);
    addField(QStringLiteral("Скорость"),    m_spinWindSpd, 0, 30,  QStringLiteral(" м/с"));

    auto* btnApply = new QPushButton(QStringLiteral("Применить"));
    btnApply->setFixedHeight(30);
    btnApply->setStyleSheet(QString(
        "QPushButton { background-color: %1; color: #000; border: none; "
        "border-radius: 6px; font-size: 12px; font-weight: 700; }"
        "QPushButton:hover { background-color: #00ffb3; }").arg(C::GREEN));
    lay->addWidget(btnApply);
    m_windPopup->setFixedWidth(190);
    m_windPopup->adjustSize();

    connect(m_btnWindDrop, &QPushButton::clicked, this, [this] {
        if (m_windPopup->isVisible()) { m_windPopup->hide(); return; }
        auto pos = m_btnWindDrop->mapToGlobal(m_btnWindDrop->rect().bottomLeft());
        m_windPopup->move(pos.x() - m_windPopup->width() + m_btnWindDrop->width(), pos.y() + 4);
        m_windPopup->show();
    });
    connect(btnApply, &QPushButton::clicked, this, [this] {
        emit windOverrideRequested(m_spinWindDir->value(), m_spinWindSpd->value());
        m_windPopup->hide();
    });
}

void RightPanel::rebuildAutopilotContent(bool navActive)
{
    // Clear existing content
    QLayoutItem* item;
    while ((item = m_apContentLay->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    // Update badge
    if (m_apBadge) {
        if (navActive) {
            m_apBadge->setText(QStringLiteral("НАВИГАЦИЯ"));
            m_apBadge->setStyleSheet(QString(
                "color: #000; font-family: \"%1\"; font-size: 10px; font-weight: 700; "
                "padding: 3px 10px; border-radius: 5px; border: none; "
                "background-color: %2;").arg(C::MONO, C::GREEN));
        } else {
            m_apBadge->setText(QStringLiteral("РУЧНОЙ"));
            m_apBadge->setStyleSheet(QString(
                "color: %1; font-family: \"%2\"; font-size: 10px; font-weight: 700; "
                "padding: 3px 10px; border-radius: 5px; border: none; "
                "background-color: %3; border: 1px solid %4;")
                .arg(C::ORANGE, C::MONO, C::ORANGE_D,
                     QString(C::ORANGE).replace("#","#30")));
        }
    }

    // Mode line
    {
        auto* modeLine = new QFrame;
        modeLine->setStyleSheet(QString(
            "QFrame { background-color: %1; border: 1px solid %2; border-radius: 10px; }")
            .arg(C::CARD, C::BD));
        auto* ml = new QHBoxLayout(modeLine);
        ml->setContentsMargins(12, 8, 12, 8);
        auto* lbl = new QLabel(QStringLiteral("Режим"));
        lbl->setStyleSheet(QString("color: %1; font-size: 12px; border: none;").arg(C::TXT_S));
        ml->addWidget(lbl);
        ml->addStretch();
        m_apModeLine = new QLabel(navActive ? QStringLiteral("---") : QStringLiteral("—"));
        m_apModeLine->setStyleSheet(QString(
            "color: %1; font-family: \"%2\"; font-size: 14px; font-weight: 600; border: none;")
            .arg(navActive ? C::GREEN : C::TXT_D, C::MONO));
        ml->addWidget(m_apModeLine);
        m_apContentLay->addWidget(modeLine);
    }

    if (!navActive) {
        // Manual: one data row with dim target
        auto* rows = new QWidget;
        rows->setStyleSheet("background: transparent;");
        auto* rl = new QVBoxLayout(rows);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(0);

        auto* row = new QHBoxLayout;
        auto* lbl = new QLabel(QStringLiteral("Целевой курс"));
        lbl->setStyleSheet(QString("color: %1; font-size: 13px; border: none;").arg(C::TXT_S));
        row->addWidget(lbl);
        row->addStretch();
        m_apTarget = new QLabel(QStringLiteral("—"));
        m_apTarget->setStyleSheet(QString(
            "color: %1; font-family: \"%2\"; font-size: 15px; font-weight: 600; border: none;")
            .arg(C::TXT_D, C::MONO));
        row->addWidget(m_apTarget);
        rl->addLayout(row);
        rl->addStretch();

        m_apAction = nullptr; m_apError = nullptr; m_apAltError = nullptr;
        m_spinAlt = nullptr; m_spinRadius = nullptr; m_spinSpeed = nullptr;
        m_btnSetAlt = nullptr; m_btnSetRadius = nullptr; m_btnSetSpeed = nullptr;
        m_apContentLay->addWidget(rows);
        return;
    }

    // NAV mode: action + data rows + quick params
    m_apAction = new QLabel(QStringLiteral("---"));
    m_apAction->setAlignment(Qt::AlignCenter);
    m_apAction->setStyleSheet(QString(
        "color: %1; font-family: \"%2\"; font-size: 13px; border: none;")
        .arg(C::BLUE, C::MONO));
    m_apContentLay->addWidget(m_apAction);

    // Data rows
    auto addApRow = [this](QLabel*& val, const QString& labelText) {
        auto* rw = new QHBoxLayout;
        rw->setContentsMargins(0, 6, 0, 6);
        auto* lbl = new QLabel(labelText);
        lbl->setStyleSheet(QString("color: %1; font-size: 13px; border: none;").arg(C::TXT_S));
        rw->addWidget(lbl);
        rw->addStretch();
        val = new QLabel(QStringLiteral("---"));
        val->setStyleSheet(QString(
            "color: %1; font-family: \"%2\"; font-size: 15px; font-weight: 600; border: none;")
            .arg(C::TXT, C::MONO));
        rw->addWidget(val);

        auto* sep = new QFrame;
        sep->setFixedHeight(1);
        sep->setStyleSheet(QString("background-color: rgba(255,255,255,6);"));

        auto* wrapper = new QWidget;
        wrapper->setStyleSheet("background: transparent;");
        auto* wl = new QVBoxLayout(wrapper);
        wl->setContentsMargins(0,0,0,0);
        wl->setSpacing(0);
        wl->addLayout(rw);
        wl->addWidget(sep);
        m_apContentLay->addWidget(wrapper);
    };
    addApRow(m_apTarget,   QStringLiteral("Целевой курс"));
    addApRow(m_apError,    QStringLiteral("Ошибка курса"));
    addApRow(m_apAltError, QStringLiteral("Ошибка высоты"));

    // Quick params: Высота / Радиус / Скорость
    QString spinStyle = QString(
        "QSpinBox { background-color: %1; color: %2; border: none; "
        "font-family: \"%3\"; font-size: 15px; font-weight: 700; }"
        "QSpinBox:disabled { color: %4; }")
        .arg(C::CARD, C::TXT, C::MONO, C::TXT_D);
    QString btnStyle = QString(
        "QPushButton { background-color: %1; border: none; border-radius: 6px; }"
        "QPushButton:hover { background-color: #5aa0ff; }"
        "QPushButton:disabled { background-color: %2; }")
        .arg(C::BLUE, C::BD);
    auto checkIco = makeCheckIcon(18, QColor("#ffffff"));

    auto* qpGrid = new QHBoxLayout;
    qpGrid->setSpacing(8);
    qpGrid->setContentsMargins(0, 6, 0, 0);

    struct QpDef { const char* lbl; const char* sfx; int lo; int hi; int def; QSpinBox** sp; QPushButton** btn; };
    QpDef defs[] = {
        {"Высота",   " м",   10, 5000, 100, &m_spinAlt,    &m_btnSetAlt},
        {"Радиус",   " м",   30, 500,  150, &m_spinRadius, &m_btnSetRadius},
        {"Скорость", " м/с", 15,  35,   20, &m_spinSpeed,  &m_btnSetSpeed},
    };

    for (auto& d : defs) {
        auto* item = new QFrame;
        item->setStyleSheet(QString(
            "QFrame { background-color: %1; border: 1px solid %2; border-radius: 10px; }")
            .arg(C::CARD, C::BD));
        auto* il = new QHBoxLayout(item);
        il->setContentsMargins(10, 8, 8, 8);
        il->setSpacing(6);

        auto* info = new QWidget;
        info->setStyleSheet("background: transparent;");
        auto* infoL = new QVBoxLayout(info);
        infoL->setContentsMargins(0, 0, 0, 0);
        infoL->setSpacing(2);

        auto* lbl = new QLabel(QString(d.lbl).toUpper());
        lbl->setStyleSheet(QString(
            "color: %1; font-size: 10px; font-weight: 500; letter-spacing: 0.5px; border: none;")
            .arg(C::TXT_D));
        infoL->addWidget(lbl);

        *d.sp = new QSpinBox;
        (*d.sp)->setButtonSymbols(QAbstractSpinBox::NoButtons);
        (*d.sp)->setRange(d.lo, d.hi);
        (*d.sp)->setValue(d.def);
        (*d.sp)->setSuffix(QString(d.sfx));
        (*d.sp)->setEnabled(false);
        (*d.sp)->setFixedHeight(24);
        (*d.sp)->setStyleSheet(spinStyle);
        infoL->addWidget(*d.sp);
        il->addWidget(info, 1);

        *d.btn = new QPushButton;
        (*d.btn)->setIcon(checkIco);
        (*d.btn)->setIconSize({18, 18});
        (*d.btn)->setFixedSize(28, 28);
        (*d.btn)->setEnabled(false);
        (*d.btn)->setCursor(Qt::PointingHandCursor);
        (*d.btn)->setStyleSheet(btnStyle);
        il->addWidget(*d.btn);

        qpGrid->addWidget(item, 1);
    }

    auto* qpWrapper = new QWidget;
    qpWrapper->setStyleSheet("background: transparent;");
    qpWrapper->setLayout(qpGrid);
    m_apContentLay->addWidget(qpWrapper);

    // Connect spinbox buttons
    connect(m_btnSetAlt, &QPushButton::clicked, this, [this] {
        m_manualAlt = true;
        emit targetAltitudeChanged(m_spinAlt->value());
    });
    connect(m_btnSetRadius, &QPushButton::clicked, this, [this] {
        m_manualRadius = true;
        emit orbitRadiusChanged(m_spinRadius->value());
    });
    connect(m_btnSetSpeed, &QPushButton::clicked, this, [this] {
        m_manualSpeed = true;
        emit targetAirspeedChanged(m_spinSpeed->value());
    });
}

// ══════════════════════════════════════════════════════════════════════════════
//  Tab 1 — Маршрут
// ══════════════════════════════════════════════════════════════════════════════

QWidget* RightPanel::buildRouteTab()
{
    auto* page = new QWidget;
    page->setStyleSheet("background: transparent;");
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // Block body
    auto* body = new QWidget;
    body->setStyleSheet("background: transparent;");
    auto* bodyLay = new QVBoxLayout(body);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->setSpacing(8);

    // Column headers
    auto* colHdrs = new QWidget;
    colHdrs->setStyleSheet("background: transparent;");
    auto* hLay = new QHBoxLayout(colHdrs);
    hLay->setContentsMargins(4, 0, 4, 0);
    hLay->setSpacing(0);

    auto addColHdr = [&](const QString& text, int stretch) {
        auto* l = new QLabel(text.toUpper());
        l->setStyleSheet(QString(
            "color: %1; font-size: 9px; font-weight: 600; "
            "letter-spacing: 0.5px; border: none;").arg(C::TXT_D));
        l->setAlignment(stretch > 1 ? Qt::AlignLeft : Qt::AlignCenter);
        hLay->addWidget(l, stretch);
    };
    // Mimic: 30px | 1fr | 70px | 70px | 70px | 50px
    hLay->addSpacing(30);
    addColHdr("Тип",      4);
    addColHdr("Высота",   2);
    addColHdr("Радиус",   2);
    addColHdr("Скорость", 2);
    hLay->addSpacing(44);
    bodyLay->addWidget(colHdrs);

    // Waypoint list container
    m_wpListWidget = new QWidget;
    m_wpListWidget->setStyleSheet("background: transparent;");
    m_wpListLay = new QVBoxLayout(m_wpListWidget);
    m_wpListLay->setContentsMargins(0, 0, 0, 0);
    m_wpListLay->setSpacing(4);

    m_wpEmptyLabel = new QLabel(QStringLiteral("Нет точек маршрута"));
    m_wpEmptyLabel->setAlignment(Qt::AlignCenter);
    m_wpEmptyLabel->setStyleSheet(QString(
        "color: %1; font-size: 12px; padding: 20px; border: none;").arg(C::TXT_D));
    m_wpListLay->addWidget(m_wpEmptyLabel);

    bodyLay->addWidget(m_wpListWidget);

    // Action bar
    auto* actBar = new QHBoxLayout;
    actBar->setSpacing(6);

    auto makeActBtn = [&](const QString& text, bool primary = false) {
        auto* btn = new QPushButton(text);
        btn->setFixedHeight(28);
        btn->setCursor(Qt::PointingHandCursor);
        if (primary) {
            btn->setStyleSheet(QString(
                "QPushButton { background: %1; color: #000; border: none; "
                "border-radius: 6px; font-size: 11px; font-weight: 700; padding: 0 10px; }"
                "QPushButton:hover { background: #00ffb3; }"
                "QPushButton:checked { background: %2; color: #fff; }")
                .arg(C::GREEN, C::BLUE));
            btn->setCheckable(true);
        } else {
            btn->setStyleSheet(QString(
                "QPushButton { background: transparent; color: %1; "
                "border: 1px solid %2; border-radius: 6px; "
                "font-size: 11px; padding: 0 10px; }"
                "QPushButton:hover { border-color: %3; color: %4; }"
                "QPushButton:disabled { color: %5; }")
                .arg(C::TXT_S, C::BD, C::TXT_S, C::TXT, C::TXT_D));
        }
        return btn;
    };

    m_btnRpAdd    = makeActBtn(QStringLiteral("+ Добавить"), true);
    m_btnRpEdit   = makeActBtn(QStringLiteral("Изменить"));
    m_btnRpDelete = makeActBtn(QStringLiteral("Удалить"));
    auto* btnClear = makeActBtn(QStringLiteral("Очистить"));
    auto* btnLoad  = makeActBtn(QStringLiteral("Загр."));
    auto* btnSave  = makeActBtn(QStringLiteral("Сохр."));

    m_btnRpEdit->setEnabled(false);
    m_btnRpDelete->setEnabled(false);

    actBar->addWidget(m_btnRpAdd, 2);
    actBar->addWidget(m_btnRpEdit, 1);
    actBar->addWidget(m_btnRpDelete, 1);
    actBar->addWidget(btnClear, 1);
    actBar->addWidget(btnLoad, 1);
    actBar->addWidget(btnSave, 1);
    bodyLay->addLayout(actBar);

    connect(m_btnRpAdd, &QPushButton::clicked, this, [this] {
        emit addWaypointToggled(m_btnRpAdd->isChecked());
    });
    connect(m_btnRpEdit, &QPushButton::clicked, this, [this] {
        if (m_selWpIdx >= 0) emit editWaypointRequested(m_selWpIdx);
    });
    connect(m_btnRpDelete, &QPushButton::clicked, this, [this] {
        if (m_selWpIdx >= 0) emit deleteWaypointRequested(m_selWpIdx);
    });
    connect(btnClear, &QPushButton::clicked, this, &RightPanel::clearRouteRequested);
    connect(btnLoad,  &QPushButton::clicked, this, &RightPanel::loadRouteRequested);
    connect(btnSave,  &QPushButton::clicked, this, &RightPanel::saveRouteRequested);

    QLabel* dummy = nullptr;
    lay->addWidget(makeBlock(QStringLiteral("\u25C7"),
                             QStringLiteral("Маршрут"),
                             body, &dummy));
    if (dummy) dummy->hide();
    lay->addStretch();
    return page;
}

QString RightPanel::wpActionLabel(const QString& action, int turns) const
{
    if (action == "FLYTHROUGH")     return QStringLiteral("Пролёт");
    if (action == "ORBIT_TURNS")    return QStringLiteral("Кружение \xd7%1").arg(turns);
    if (action == "ORBIT_INFINITE") return QStringLiteral("\u221e Кружение");
    if (action == "ALTITUDE")       return QStringLiteral("Высота");
    return action;
}

void RightPanel::rebuildWpList()
{
    // Remove old items
    for (auto* w : m_wpItems) { m_wpListLay->removeWidget(w); delete w; }
    m_wpItems.clear();

    const bool empty = m_waypoints.isEmpty();
    m_wpEmptyLabel->setVisible(empty);
    m_btnRpEdit->setEnabled(false);
    m_btnRpDelete->setEnabled(false);
    m_selWpIdx = -1;

    for (int i = 0; i < m_waypoints.size(); ++i) {
        const auto& wp = m_waypoints[i];
        bool isActive = (i == m_activeWpIdx);

        QString action    = wp.value("action").toString();
        int orbitTurns    = wp.value("orbitTurns", 1).toInt();
        int alt           = static_cast<int>(wp.value("altitude").toDouble());
        int radius        = wp.value("radius", 150).toInt();
        int speed         = wp.value("speed", 20).toInt();
        QString typeStr   = wpActionLabel(action, orbitTurns);

        auto* item = new QFrame;
        item->setProperty("wpIdx", i);
        item->setStyleSheet(QString(
            "QFrame { background-color: %1; border: 1px solid %2; "
            "border-radius: 6px; }")
            .arg(isActive ? C::GREEN_D : C::CARD,
                 isActive ? C::GREEN   : C::BD));
        item->setCursor(Qt::PointingHandCursor);
        item->setFixedHeight(38);

        auto* il = new QHBoxLayout(item);
        il->setContentsMargins(8, 0, 8, 0);
        il->setSpacing(6);

        // Number circle
        auto* numLbl = new QLabel(QString::number(i+1));
        numLbl->setFixedSize(24, 24);
        numLbl->setAlignment(Qt::AlignCenter);
        numLbl->setStyleSheet(QString(
            "color: %1; font-family: \"%2\"; font-size: 11px; font-weight: 700; "
            "border-radius: 12px; background-color: %3; border: 1px solid %4;")
            .arg(isActive ? "#000" : C::TXT_S,
                 C::MONO,
                 isActive ? C::GREEN : C::INPUT,
                 isActive ? C::GREEN : C::BD));
        il->addWidget(numLbl);

        // Type
        auto* typeLbl = new QLabel(typeStr);
        typeLbl->setStyleSheet(QString(
            "color: %1; font-size: 12px; font-weight: 500; border: none;")
            .arg(isActive ? C::TXT : C::TXT_S));
        il->addWidget(typeLbl, 4);

        // Values
        auto addVal = [&](const QString& text) {
            auto* l = new QLabel(text);
            l->setAlignment(Qt::AlignCenter);
            l->setStyleSheet(QString(
                "color: %1; font-family: \"%2\"; font-size: 11px; border: none;")
                .arg(C::TXT_S, C::MONO));
            il->addWidget(l, 2);
        };
        addVal(QStringLiteral("%1 м").arg(alt));
        addVal(QStringLiteral("%1 м").arg(radius));
        addVal(QStringLiteral("%1 м/с").arg(speed));

        // Edit/Delete buttons
        auto* editBtn = new QPushButton(QStringLiteral("\u270e"));
        auto* delBtn  = new QPushButton(QStringLiteral("\xd7"));
        for (auto* b : {editBtn, delBtn}) {
            b->setFixedSize(22, 22);
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QString(
                "QPushButton { background: %1; border: none; border-radius: 4px; "
                "color: %2; font-size: 12px; }"
                "QPushButton:hover { background: %3; color: %4; }")
                .arg(C::INPUT, C::TXT_S, C::BD_A, C::TXT));
        }
        connect(editBtn, &QPushButton::clicked, this, [this, i] {
            emit editWaypointRequested(i);
        });
        connect(delBtn, &QPushButton::clicked, this, [this, i] {
            emit deleteWaypointRequested(i);
        });
        il->addWidget(editBtn);
        il->addWidget(delBtn);

        // Click to select
        const int ci = i;
        auto* filter = new QObject(item);
        item->installEventFilter(filter);
        connect(filter, &QObject::destroyed, []{});  // keep alive
        // Use press/release via child click detection
        item->setFocusPolicy(Qt::ClickFocus);

        m_wpListLay->insertWidget(m_wpListLay->count() - 1, item);
        // Stretch is at end, insert before it
        // Re-order: insert at correct position
        m_wpItems.append(item);

        connect(editBtn, &QPushButton::clicked, this, [this, ci] {
            m_selWpIdx = ci;
            m_btnRpEdit->setEnabled(true);
            m_btnRpDelete->setEnabled(true);
            emit centerOnWaypointRequested(ci);
        });
        connect(delBtn, &QPushButton::clicked, this, [this, ci] {
            m_selWpIdx = ci;
        });
    }

    // Fix layout order — rebuild fresh
    // Remove all, re-add in order
    while (m_wpListLay->count() > 0)
        m_wpListLay->takeAt(0);

    m_wpListLay->addWidget(m_wpEmptyLabel);
    for (auto* w : m_wpItems)
        m_wpListLay->addWidget(w);
}

// ══════════════════════════════════════════════════════════════════════════════
//  Tab 2 — Система
// ══════════════════════════════════════════════════════════════════════════════

QWidget* RightPanel::buildSystemTab()
{
    auto* page = new QWidget;
    page->setStyleSheet("background: transparent;");
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // ── Батарея ──
    {
        auto* body = new QWidget;
        body->setStyleSheet("background: transparent;");
        auto* bLay = new QVBoxLayout(body);
        bLay->setContentsMargins(0, 0, 0, 0);
        bLay->setSpacing(8);

        // Main battery bar
        auto* bar = new QFrame;
        bar->setStyleSheet(QString(
            "QFrame { background-color: %1; border: 1px solid %2; border-radius: 10px; }")
            .arg(C::CARD, C::BD));
        auto* bl = new QHBoxLayout(bar);
        bl->setContentsMargins(14, 10, 14, 10);
        bl->setSpacing(12);

        // Battery icon
        auto* battIconBox = new QFrame;
        battIconBox->setFixedSize(42, 21);
        battIconBox->setStyleSheet(QString(
            "QFrame { border: 2px solid %1; border-radius: 3px; background: transparent; }")
            .arg(C::GREEN));

        m_battFill = new QFrame(battIconBox);
        m_battFill->setGeometry(3, 3, 25, 13);
        m_battFill->setStyleSheet(QString(
            "background-color: %1; border-radius: 1px;").arg(C::GREEN));

        // Tip of battery
        auto* tip = new QFrame(battIconBox);
        tip->setGeometry(42, 6, 4, 9);
        tip->setStyleSheet(QString("background-color: %1; border-radius: 1px;").arg(C::GREEN));
        tip->setFixedSize(4, 9);

        bl->addWidget(battIconBox);

        // Voltage + spec
        auto* voltGroup = new QWidget;
        voltGroup->setStyleSheet("background: transparent;");
        auto* vl = new QVBoxLayout(voltGroup);
        vl->setContentsMargins(0,0,0,0);
        vl->setSpacing(2);
        m_battVoltLabel = new QLabel(QStringLiteral("---"));
        m_battVoltLabel->setStyleSheet(QString(
            "color: %1; font-family: \"%2\"; font-size: 18px; font-weight: 700; border: none;")
            .arg(C::GREEN, C::MONO));
        vl->addWidget(m_battVoltLabel);
        auto* specLbl = new QLabel(QStringLiteral("12S · 22000 мАч"));
        specLbl->setStyleSheet(QString("color: %1; font-size: 10px; border: none;").arg(C::TXT_S));
        vl->addWidget(specLbl);
        bl->addWidget(voltGroup);

        bl->addStretch();

        // Right: pct + time
        auto* rightGroup = new QWidget;
        rightGroup->setStyleSheet("background: transparent;");
        auto* rl = new QVBoxLayout(rightGroup);
        rl->setContentsMargins(0,0,0,0);
        rl->setSpacing(2);
        rl->setAlignment(Qt::AlignRight);

        m_battPctLabel = new QLabel(QStringLiteral("—%"));
        m_battPctLabel->setStyleSheet(QString(
            "color: %1; font-family: \"%2\"; font-size: 16px; font-weight: 700; border: none;")
            .arg(C::TXT, C::MONO));
        rl->addWidget(m_battPctLabel, 0, Qt::AlignRight);

        m_battTimeLabel = new QLabel(QStringLiteral("—"));
        m_battTimeLabel->setStyleSheet(QString(
            "color: %1; font-size: 10px; border: none;").arg(C::ORANGE));
        rl->addWidget(m_battTimeLabel, 0, Qt::AlignRight);
        bl->addWidget(rightGroup);

        bLay->addWidget(bar);

        // Metrics row: Ток / Потрачено / На маршрут
        auto* metricsRow = new QHBoxLayout;
        metricsRow->setSpacing(6);

        auto addBattMetric = [&](const QString& lbl, QLabel*& val, const QString& color) {
            auto* m = new QFrame;
            m->setStyleSheet(QString(
                "QFrame { background-color: %1; border: 1px solid %2; border-radius: 6px; }")
                .arg(C::CARD, C::BD));
            auto* ml = new QVBoxLayout(m);
            ml->setContentsMargins(10, 6, 10, 6);
            ml->setSpacing(2);
            auto* l = new QLabel(lbl.toUpper());
            l->setAlignment(Qt::AlignCenter);
            l->setStyleSheet(QString(
                "color: %1; font-size: 9px; letter-spacing: 0.5px; border: none;").arg(C::TXT_D));
            ml->addWidget(l);
            val = new QLabel(QStringLiteral("---"));
            val->setAlignment(Qt::AlignCenter);
            val->setStyleSheet(QString(
                "color: %1; font-family: \"%2\"; font-size: 13px; font-weight: 600; border: none;")
                .arg(color, C::MONO));
            ml->addWidget(val);
            metricsRow->addWidget(m, 1);
        };
        addBattMetric("Ток",         m_battCurrLabel,  C::ORANGE);
        addBattMetric("Потрачено",   m_battConsumed,   C::TXT);
        addBattMetric("На маршрут",  m_battSufficient, C::GREEN);

        bLay->addLayout(metricsRow);

        QLabel* dummy = nullptr;
        lay->addWidget(makeBlock(QStringLiteral("\u26a1"),
                                 QStringLiteral("Батарея"),
                                 body, &dummy));
        if (dummy) dummy->hide();
    }

    // ── Моторы ──
    {
        auto* body = new QWidget;
        body->setStyleSheet("background: transparent;");
        auto* grid = new QGridLayout(body);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(6);

        const char* names[] = {"M1 FL", "M2 FR", "M3 BL", "M4 BR", "Pusher"};
        for (int i = 0; i < 5; ++i) {
            auto* cell = new QFrame;
            cell->setStyleSheet(QString(
                "QFrame { background-color: %1; border: 1px solid %2; border-radius: 6px; }")
                .arg(C::CARD, C::BD));
            auto* cl = new QVBoxLayout(cell);
            cl->setContentsMargins(6, 6, 6, 6);
            cl->setSpacing(3);
            cl->setAlignment(Qt::AlignCenter);

            auto* dot = new QFrame;
            dot->setFixedSize(8, 8);
            dot->setStyleSheet(QString(
                "border-radius: 4px; background-color: %1;").arg(C::GREEN));
            cl->addWidget(dot, 0, Qt::AlignCenter);

            auto* nameLbl = new QLabel(names[i]);
            nameLbl->setAlignment(Qt::AlignCenter);
            nameLbl->setStyleSheet(QString(
                "color: %1; font-size: 9px; font-weight: 600; "
                "letter-spacing: 0.5px; border: none;").arg(C::TXT_S));
            cl->addWidget(nameLbl);

            m_motorVals[i] = new QLabel(QStringLiteral("---"));
            m_motorVals[i]->setAlignment(Qt::AlignCenter);
            m_motorVals[i]->setStyleSheet(QString(
                "color: %1; font-family: \"%2\"; font-size: 12px; font-weight: 600; border: none;")
                .arg(C::TXT, C::MONO));
            cl->addWidget(m_motorVals[i]);

            grid->addWidget(cell, i/5, i%5);  // single row of 5
        }
        // Actually 5 in one row
        // Re-do grid properly
        // (the grid above will put them all in row 0, cols 0..4)

        QLabel* dummy = nullptr;
        lay->addWidget(makeBlock(QStringLiteral("\u27f3"),
                                 QStringLiteral("Моторы"),
                                 body, &dummy));
        if (dummy) dummy->hide();
    }

    lay->addStretch();
    return page;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Command area
// ══════════════════════════════════════════════════════════════════════════════

QWidget* RightPanel::buildCommandArea()
{
    auto* area = new QWidget;
    area->setStyleSheet(QString("background-color: %1;").arg(C::BG));
    auto* lay = new QVBoxLayout(area);
    lay->setContentsMargins(12, 8, 12, 8);
    lay->setSpacing(8);

    // Notification slot
    m_notifWidget  = new NotificationWidget(this);
    m_notifManager = new NotificationManager(m_notifWidget, this);
    lay->addWidget(m_notifWidget);

    // Operational point card (hidden by default)
    m_opPointCard = new QFrame;
    m_opPointCard->setVisible(false);
    m_opPointCard->setStyleSheet(QString(
        "QFrame { background-color: %1; border: 1px solid %2; "
        "border-radius: 10px; border-left: 3px solid %3; }")
        .arg(C::GREEN_D, C::GREEN, C::GREEN));
    auto* opLay = new QVBoxLayout(m_opPointCard);
    opLay->setContentsMargins(14, 8, 14, 8);
    opLay->setSpacing(2);

    auto* opTitle = new QLabel(QStringLiteral("Оперативная точка"));
    opTitle->setStyleSheet(QString(
        "color: %1; font-size: 13px; font-weight: 700; border: none;").arg(C::GREEN));
    opLay->addWidget(opTitle);

    m_opPointDesc = new QLabel(QStringLiteral("Навигация к оперативной точке"));
    m_opPointDesc->setStyleSheet(QString(
        "color: %1; font-size: 11px; font-family: \"%2\"; border: none;")
        .arg(C::TXT_S, C::MONO));
    opLay->addWidget(m_opPointDesc);
    lay->addWidget(m_opPointCard);

    // Action buttons row
    auto* actRow = new QHBoxLayout;
    actRow->setSpacing(8);

    m_btnNav = new QPushButton(QStringLiteral("Навигация"));
    m_btnNav->setCheckable(true);
    m_btnNav->setEnabled(false);
    m_btnNav->setFixedHeight(38);
    m_btnNav->setStyleSheet(QString(
        "QPushButton { background-color: %1; color: #000; border: none; "
        "border-radius: 10px; font-family: \"%2\"; font-size: 13px; font-weight: 700; }"
        "QPushButton:hover { background-color: #00ffb3; }"
        "QPushButton:checked { background-color: %3; color: #fff; }"
        "QPushButton:checked:hover { background-color: %3; }"
        "QPushButton:disabled { background-color: %4; color: %5; }")
        .arg(C::GREEN, C::SANS, C::BLUE, C::INPUT, C::TXT_D));
    actRow->addWidget(m_btnNav, 1);

    m_btnResumeRoute = new QPushButton(QStringLiteral("Продолжить маршрут"));
    m_btnResumeRoute->setFixedHeight(38);
    m_btnResumeRoute->setVisible(false);
    m_btnResumeRoute->setStyleSheet(QString(
        "QPushButton { background-color: %1; color: #000; border: none; "
        "border-radius: 10px; font-family: \"%2\"; font-size: 13px; font-weight: 700; }"
        "QPushButton:hover { background-color: #ffaa44; }")
        .arg(C::ORANGE, C::SANS));
    actRow->addWidget(m_btnResumeRoute, 1);
    lay->addLayout(actRow);

    connect(m_btnNav, &QPushButton::clicked, this, [this](bool checked) {
        emit navToggled(checked);
    });
    connect(m_btnResumeRoute, &QPushButton::clicked, this, &RightPanel::resumeRouteRequested);

    return area;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Bottom nav
// ══════════════════════════════════════════════════════════════════════════════

QWidget* RightPanel::buildBottomNav()
{
    auto* nav = new QWidget;
    nav->setStyleSheet(QString(
        "background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        " stop:0 %1, stop:1 #0f1630);").arg(C::BG));
    auto* lay = new QVBoxLayout(nav);
    lay->setContentsMargins(10, 6, 10, 8);
    lay->setSpacing(4);

    // Main nav button style
    auto navBtnStyle = [](bool active = false) {
        return QString(
            "QPushButton { font-family: \"Segoe UI\"; font-size: 11px; font-weight: %1; "
            "padding: 7px 0; border: 1px solid %2; border-radius: 6px; "
            "background-color: %3; color: %4; }"
            "QPushButton:hover { border-color: %5; color: %6; }"
            "QPushButton:checked { background-color: %7; border-color: %8; color: %9; }"
            "QPushButton:disabled { color: %10; }")
            .arg(active ? "600" : "500")
            .arg(C::BD)
            .arg(active ? C::BLUE_D : "transparent")
            .arg(active ? C::BLUE : C::TXT_S)
            .arg(C::TXT_S, C::TXT, C::BLUE_D, C::BLUE, C::BLUE, C::TXT_D);
    };

    // Row 1: Follow / Home / Clear track
    auto* row1 = new QHBoxLayout;
    row1->setSpacing(5);

    m_btnFollow    = new QPushButton(QStringLiteral("Слежение"));
    m_btnHome      = new QPushButton(QStringLiteral("Домой"));
    m_btnClearTrack = new QPushButton(QStringLiteral("Трек"));

    for (auto* btn : {m_btnFollow, m_btnHome}) btn->setCheckable(true);

    m_btnFollow->setStyleSheet(navBtnStyle());
    m_btnHome->setStyleSheet(QString(
        "QPushButton { font-size: 11px; font-weight: 500; padding: 7px 0; "
        "border: 1px solid %1; border-radius: 6px; background: transparent; color: %2; }"
        "QPushButton:hover { border-color: %3; color: %4; }"
        "QPushButton:checked { background-color: %5; border-color: %5; color: #000; }"
        "QPushButton:disabled { color: %6; }")
        .arg(C::BD, C::TXT_S, C::TXT_S, C::TXT, C::ORANGE, C::TXT_D));
    m_btnClearTrack->setStyleSheet(navBtnStyle());

    for (auto* btn : {m_btnFollow, m_btnHome, m_btnClearTrack}) {
        btn->setFixedHeight(30);
        btn->setEnabled(false);
        row1->addWidget(btn, 1);
    }
    lay->addLayout(row1);

    // Row 2: secondary utility buttons (smaller)
    auto* row2 = new QHBoxLayout;
    row2->setSpacing(4);

    auto makeSmallBtn = [&](const QString& text, bool checkable = false) {
        auto* btn = new QPushButton(text);
        btn->setFixedHeight(26);
        btn->setCheckable(checkable);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QString(
            "QPushButton { font-size: 11px; font-weight: 500; padding: 0 8px; "
            "border: 1px solid %1; border-radius: 5px; "
            "background: transparent; color: %2; }"
            "QPushButton:hover { border-color: %3; color: %4; }"
            "QPushButton:checked { background-color: %5; border-color: %5; color: #fff; }"
            "QPushButton:disabled { color: %6; }")
            .arg(C::BD, C::TXT_S, C::TXT_S, C::TXT, C::BLUE, C::TXT_D));
        return btn;
    };

    m_btnSetPos    = makeSmallBtn(QStringLiteral("Коррекция"), true);
    m_btnSetHome   = makeSmallBtn(QStringLiteral("Дом"), true);
    m_btnLoadRoute = makeSmallBtn(QStringLiteral("Маршрут"));
    m_btnAddWp     = makeSmallBtn(QStringLiteral("+ ТЧК"), true);
    m_btnDrawZone  = makeSmallBtn(QStringLiteral("Зоны"), true);

    // Settings icon button
    m_btnSettings = new QPushButton;
    m_btnSettings->setFixedSize(26, 26);
    m_btnSettings->setIcon(makeGearIcon(16, QColor(C::TXT_S)));
    m_btnSettings->setIconSize({16, 16});
    m_btnSettings->setCursor(Qt::PointingHandCursor);
    m_btnSettings->setToolTip(QStringLiteral("Настройки"));
    m_btnSettings->setStyleSheet(QString(
        "QPushButton { background: transparent; border: 1px solid %1; border-radius: 5px; }"
        "QPushButton:hover { border-color: %2; }").arg(C::BD, C::TXT_S));

    for (auto* btn : {m_btnSetPos, m_btnSetHome, m_btnLoadRoute, m_btnAddWp, m_btnDrawZone})
        btn->setEnabled(false);

    row2->addWidget(m_btnSetPos, 2);
    row2->addWidget(m_btnSetHome, 1);
    row2->addWidget(m_btnLoadRoute, 2);
    row2->addWidget(m_btnAddWp, 2);
    row2->addWidget(m_btnDrawZone, 1);
    row2->addWidget(m_btnSettings);
    lay->addLayout(row2);

    // Row 3: WP info row
    auto* row3 = new QHBoxLayout;
    row3->setSpacing(4);
    row3->setContentsMargins(4, 0, 4, 0);

    auto* wpLbl = new QLabel(QStringLiteral("ТЧК"));
    wpLbl->setStyleSheet(QString("color: %1; font-size: 10px; border: none;").arg(C::TXT_D));
    row3->addWidget(wpLbl);

    m_spinWp = new QSpinBox;
    m_spinWp->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_spinWp->setMinimum(1);
    m_spinWp->setMaximum(1);
    m_spinWp->setEnabled(false);
    m_spinWp->setFixedSize(42, 24);
    m_spinWp->setStyleSheet(QString(
        "QSpinBox { background-color: %1; color: %2; border: 1px solid %3; "
        "border-radius: 5px; padding: 2px 6px; font-family: \"%4\"; "
        "font-size: 12px; font-weight: 600; }"
        "QSpinBox:disabled { color: %5; }").arg(C::INPUT, C::TXT, C::BD, C::MONO, C::TXT_D));
    row3->addWidget(m_spinWp);

    m_btnWpPrev = new QPushButton;
    m_btnWpPrev->setIcon(makeArrowIcon(14, QColor(C::TXT_S), true));
    m_btnWpPrev->setIconSize({14, 14});
    m_btnWpPrev->setFixedSize(26, 26);
    m_btnWpPrev->setEnabled(false);
    m_btnWpPrev->setStyleSheet(QString(
        "QPushButton { background: %1; border: 1px solid %2; border-radius: 5px; }"
        "QPushButton:hover { border-color: %3; }"
        "QPushButton:disabled { background: %4; }").arg(C::INPUT, C::BD, C::TXT_S, C::CARD));
    row3->addWidget(m_btnWpPrev);

    m_btnWpNext = new QPushButton;
    m_btnWpNext->setIcon(makeArrowIcon(14, QColor(C::TXT_S), false));
    m_btnWpNext->setIconSize({14, 14});
    m_btnWpNext->setFixedSize(26, 26);
    m_btnWpNext->setEnabled(false);
    m_btnWpNext->setStyleSheet(m_btnWpPrev->styleSheet());
    row3->addWidget(m_btnWpNext);

    row3->addStretch();
    lay->addLayout(row3);

    // Connections
    connect(m_btnFollow, &QPushButton::clicked, this, [this](bool checked) {
        emit followToggled(checked);
    });
    connect(m_btnHome, &QPushButton::clicked, this, [this](bool checked) {
        emit homeToggled(checked);
    });
    connect(m_btnClearTrack, &QPushButton::clicked, this, &RightPanel::clearTrackRequested);
    connect(m_btnSetPos, &QPushButton::clicked, this, [this](bool checked) {
        emit setPositionToggled(checked);
    });
    connect(m_btnSetHome, &QPushButton::clicked, this, [this](bool checked) {
        emit setHomeToggled(checked);
    });
    connect(m_btnLoadRoute, &QPushButton::clicked, this, &RightPanel::loadRouteRequested);
    connect(m_btnAddWp, &QPushButton::clicked, this, [this](bool checked) {
        emit addWaypointToggled(checked);
    });
    connect(m_btnDrawZone, &QPushButton::clicked, this, [this](bool checked) {
        emit drawZoneToggled(checked);
    });
    connect(m_btnSettings, &QPushButton::clicked, this, &RightPanel::settingsRequested);
    connect(m_btnWpPrev, &QPushButton::clicked, this, &RightPanel::wpPrevRequested);
    connect(m_btnWpNext, &QPushButton::clicked, this, &RightPanel::wpNextRequested);
    connect(m_spinWp, qOverload<int>(&QSpinBox::valueChanged),
            this, &RightPanel::wpSelected);

    return nav;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Public update methods
// ══════════════════════════════════════════════════════════════════════════════

void RightPanel::updateTelemetry(const TelemetryState& state)
{
    m_airspeed->setValue(state.airspeed(), 1);
    m_groundspeed->setValue(state.groundspeed(), 1);
    m_altAgl->setValue(state.altitudeAgl(), 0);

    m_wind->setText(QString("%1\xb0/%2")
                        .arg(static_cast<int>(state.windDirection()), 3, 10, QChar('0'))
                        .arg(state.windSpeed(), 0, 'f', 0));

    m_battery->setValue(state.batteryVoltage(), 1);

    if (state.gpsFix() >= 3) {
        m_gps->setText(QString("3D (%1)").arg(state.satellites()));
        m_gps->setColor(C::GREEN);
    } else {
        m_gps->setText(QString("NO (%1)").arg(state.satellites()));
        m_gps->setColor(C::RED);
    }

    // System tab — battery update
    if (m_battVoltLabel)
        m_battVoltLabel->setText(QString("%1 В").arg(state.batteryVoltage(), 0, 'f', 1));
    if (m_battCurrLabel)
        m_battCurrLabel->setText(QString("%1 А").arg(state.batteryCurrent(), 0, 'f', 1));
    // Percentage and time are not available — keep as "—"
}

void RightPanel::updateNavigation(int wpIdx, int total,
                                   double distance, double etaSec, double xtk)
{
    m_wpCell->setText(QString("%1/%2").arg(wpIdx).arg(total));
    m_distCell->setText(QString("%1 км").arg(distance / 1000.0, 0, 'f', 2));

    int m = static_cast<int>(etaSec) / 60;
    int s = static_cast<int>(etaSec) % 60;
    m_etaCell->setText(QString("%1:%2")
                           .arg(m, 2, 10, QChar('0'))
                           .arg(s, 2, 10, QChar('0')));

    QString sign = xtk >= 0 ? "+" : "";
    m_xtkCell->setText(QString("%1%2 м").arg(sign).arg(xtk, 0, 'f', 0));

    if (xtk > 50.0)
        m_xtkCell->setColor(C::RED);
    else if (xtk > 20.0)
        m_xtkCell->setColor(C::ORANGE);
    else
        m_xtkCell->setColor(C::GREEN);

    // Update WP info label in bottom nav
    if (m_lblWpInfo)
        m_lblWpInfo->setText(QString("ТЧК %1").arg(wpIdx));
}

void RightPanel::updateAutopilot(const QString& mode, const AutopilotStatus& status)
{
    bool navActive = (mode != "MANUAL");
    bool modeChanged = (m_lastApMode != mode);

    if (modeChanged) {
        m_lastApMode = mode;
        if (navActive != m_lastApNav) {
            m_lastApNav = navActive;
            rebuildAutopilotContent(navActive);

            m_manualAlt    = false;
            m_manualRadius = false;
            m_manualSpeed  = false;
        }
        if (navActive && m_spinAlt) {
            for (auto* w : {m_spinAlt, m_spinRadius, m_spinSpeed}) w->setEnabled(true);
            for (auto* w : {m_btnSetAlt, m_btnSetRadius, m_btnSetSpeed}) w->setEnabled(true);
        }
    }

    if (!navActive) {
        if (m_apTarget) m_apTarget->setText(QStringLiteral("—"));
        return;
    }

    // NAV mode updates
    if (m_apModeLine) {
        // formatAction for display
        QString actionDisplay;
        const QString& act = status.action;
        if (act.startsWith("ORBIT_INF")) actionDisplay = QStringLiteral("Кружение \u221e");
        else if (act.startsWith("TO_WAYPOINT")) actionDisplay = QStringLiteral("К точке");
        else if (act.startsWith("ORBIT_TURNS")) actionDisplay = QStringLiteral("Кружение");
        else if (act.startsWith("ALTITUDE_ORBIT")) actionDisplay = QStringLiteral("Набор высоты");
        else if (act == "IDLE") actionDisplay = QStringLiteral("Ожидание");
        else actionDisplay = act;

        m_apModeLine->setText(actionDisplay.isEmpty() ? QStringLiteral("—") : actionDisplay);
    }

    if (m_apAction) {
        QString actionFull = status.isOrbiting ? QStringLiteral("Кружение") : QStringLiteral("К точке");
        m_apAction->setText(actionFull);
        m_apAction->setStyleSheet(QString(
            "color: %1; font-family: \"%2\"; font-size: 13px; border: none;")
            .arg(status.isOrbiting ? C::ORANGE : C::BLUE, C::MONO));
    }

    if (m_apTarget && status.targetHeading >= 0)
        m_apTarget->setText(QString("%1\xb0").arg(status.targetHeading, 0, 'f', 0));

    if (m_apError) {
        m_apError->setText(QString("%1%2\xb0")
                               .arg(status.headingError >= 0 ? "+" : "")
                               .arg(status.headingError, 0, 'f', 1));
        m_apError->setStyleSheet(QString(
            "color: %1; font-family: \"%2\"; font-size: 15px; font-weight: 600; border: none;")
            .arg(std::abs(status.headingError) > 15 ? C::ORANGE : C::GREEN, C::MONO));
    }
    if (m_apAltError) {
        m_apAltError->setText(QString("%1%2 м")
                                  .arg(status.altitudeError >= 0 ? "+" : "")
                                  .arg(status.altitudeError, 0, 'f', 0));
        m_apAltError->setStyleSheet(QString(
            "color: %1; font-family: \"%2\"; font-size: 15px; font-weight: 600; border: none;")
            .arg(std::abs(status.altitudeError) > 5 ? C::ORANGE : C::GREEN, C::MONO));
    }

    if (m_spinAlt && !m_manualAlt) {
        int v = static_cast<int>(status.targetAltitude);
        if (v > 0) { m_spinAlt->blockSignals(true); m_spinAlt->setValue(v); m_spinAlt->blockSignals(false); }
    }
    if (m_spinRadius && !m_manualRadius) {
        int v = static_cast<int>(status.orbitRadius);
        if (v > 0) { m_spinRadius->blockSignals(true); m_spinRadius->setValue(v); m_spinRadius->blockSignals(false); }
    }
    if (m_spinSpeed && !m_manualSpeed) {
        int v = static_cast<int>(status.targetAirspeed);
        if (v > 0) { m_spinSpeed->blockSignals(true); m_spinSpeed->setValue(v); m_spinSpeed->blockSignals(false); }
    }
}

void RightPanel::refreshRoute(const QVector<QVariantMap>& waypoints, int activeIdx)
{
    m_waypoints   = waypoints;
    m_activeWpIdx = activeIdx;
    rebuildWpList();

    // Update route badge
    if (m_routeBadge)
        m_routeBadge->setText(QString::number(waypoints.size()));

    // Update bottom spinbox
    if (m_spinWp) {
        m_spinWp->blockSignals(true);
        m_spinWp->setMaximum(qMax(1, waypoints.size()));
        m_spinWp->blockSignals(false);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
//  State setters
// ══════════════════════════════════════════════════════════════════════════════

void RightPanel::setConnected(bool connected)
{
    m_btnConnect->setEnabled(!connected);
    m_btnDisconnect->setEnabled(connected);

    if (connected) {
        m_statusDot->setStyleSheet(QString("border-radius: 5px; background-color: %1;").arg(C::GREEN));
        m_dotTimer->start();
    } else {
        m_dotTimer->stop();
        m_statusDot->setStyleSheet(QString("border-radius: 5px; background-color: %1;").arg(C::TXT_D));
    }
}

void RightPanel::setFlightMode(const QString& mode)
{
    m_lblMode->setText(mode.isEmpty() ? QStringLiteral("---") : mode);
}

void RightPanel::setNavActive(bool active)
{
    if (m_btnNav) {
        m_btnNav->blockSignals(true);
        m_btnNav->setChecked(active);
        m_btnNav->blockSignals(false);
    }
}

void RightPanel::setFollowActive(bool active)
{
    if (m_btnFollow) {
        m_btnFollow->blockSignals(true);
        m_btnFollow->setChecked(active);
        m_btnFollow->blockSignals(false);
    }
}

void RightPanel::setHomeActive(bool active)
{
    if (m_btnHome) {
        m_btnHome->blockSignals(true);
        m_btnHome->setChecked(active);
        m_btnHome->blockSignals(false);
    }
}

void RightPanel::setResumeRouteVisible(bool visible, bool hasRoute)
{
    if (m_btnResumeRoute) {
        m_btnResumeRoute->setVisible(visible);
        m_btnResumeRoute->setText(hasRoute
            ? QStringLiteral("Продолжить маршрут")
            : QStringLiteral("Завершить"));
    }
}

void RightPanel::setPositionMode(bool active)
{
    if (m_btnSetPos) {
        m_btnSetPos->blockSignals(true);
        m_btnSetPos->setChecked(active);
        m_btnSetPos->blockSignals(false);
    }
}

void RightPanel::setHomePlacementMode(bool active)
{
    if (m_btnSetHome) {
        m_btnSetHome->blockSignals(true);
        m_btnSetHome->setChecked(active);
        m_btnSetHome->blockSignals(false);
    }
}

void RightPanel::setDrawZoneMode(bool active)
{
    if (m_btnDrawZone) {
        m_btnDrawZone->blockSignals(true);
        m_btnDrawZone->setChecked(active);
        m_btnDrawZone->blockSignals(false);
    }
}

void RightPanel::setAddWaypointMode(bool active)
{
    if (m_btnAddWp) {
        m_btnAddWp->blockSignals(true);
        m_btnAddWp->setChecked(active);
        m_btnAddWp->blockSignals(false);
    }
    if (m_btnRpAdd) {
        m_btnRpAdd->blockSignals(true);
        m_btnRpAdd->setChecked(active);
        m_btnRpAdd->blockSignals(false);
    }
}

void RightPanel::enableControls(bool enabled)
{
    for (auto* btn : {m_btnNav, m_btnFollow, m_btnHome, m_btnClearTrack,
                      m_btnSetPos, m_btnSetHome, m_btnLoadRoute, m_btnAddWp,
                      m_btnDrawZone})
        btn->setEnabled(enabled);

    if (m_spinWp) m_spinWp->setEnabled(enabled);
    if (m_btnWpPrev) m_btnWpPrev->setEnabled(enabled);
    if (m_btnWpNext) m_btnWpNext->setEnabled(enabled);
}

void RightPanel::setWaypointRange(int minVal, int maxVal)
{
    if (m_spinWp) {
        m_spinWp->blockSignals(true);
        m_spinWp->setRange(minVal, maxVal);
        m_spinWp->blockSignals(false);
    }
}

void RightPanel::setWaypointValue(int value)
{
    if (m_spinWp) {
        m_spinWp->blockSignals(true);
        m_spinWp->setValue(value);
        m_spinWp->blockSignals(false);
    }
    if (m_lblWpInfo)
        m_lblWpInfo->setText(QString("ТЧК %1").arg(value));
}

void RightPanel::setOperationalPoint(bool active, const QString& desc)
{
    if (m_opPointCard) m_opPointCard->setVisible(active);
    if (active && m_opPointDesc && !desc.isEmpty())
        m_opPointDesc->setText(desc);
}

} // namespace vtol
