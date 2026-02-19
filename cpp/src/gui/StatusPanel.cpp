#include "StatusPanel.h"
#include "Theme.h"
#include "mavlink/TelemetryState.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QAbstractSpinBox>
#include <QPainter>
#include <QIcon>
#include <cmath>

namespace vtol {

// ═══════════════════════ Painted Icons ═══════════════════════

static QIcon paintedCheckIcon(int sz, const QColor& color)
{
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(sz * 0.20, sz * 0.52), QPointF(sz * 0.40, sz * 0.75));
    p.drawLine(QPointF(sz * 0.40, sz * 0.75), QPointF(sz * 0.80, sz * 0.28));
    return QIcon(pm);
}

static QIcon paintedChevronDown(int sz, const QColor& color)
{
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(sz * 0.22, sz * 0.35), QPointF(sz * 0.50, sz * 0.65));
    p.drawLine(QPointF(sz * 0.50, sz * 0.65), QPointF(sz * 0.78, sz * 0.35));
    return QIcon(pm);
}

// ═══════════════════════ MetricCell ═══════════════════════

MetricCell::MetricCell(const QString& label, const QString& unit,
                       const QString& color, QWidget* parent)
    : QWidget(parent), m_unit(unit)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(2);

    m_nameLabel = new QLabel(label);
    m_nameLabel->setStyleSheet(QString("color: %1; font-size: 10px; border: none;").arg(theme::TEXT_DIM));
    lay->addWidget(m_nameLabel);

    m_valueLabel = new QLabel("---");
    setColor(color);   // m_color is empty → guard passes → style applied
    lay->addWidget(m_valueLabel);
}

void MetricCell::setValue(double value, int decimals)
{
    QString text = decimals == 0 ? QString::number(static_cast<int>(value))
                                 : QString::number(value, 'f', decimals);
    if (!m_unit.isEmpty()) text += " " + m_unit;
    m_valueLabel->setText(text);
}

void MetricCell::setText(const QString& text) { m_valueLabel->setText(text); }

void MetricCell::setColor(const QString& color)
{
    if (m_color == color) return;  // skip setStyleSheet if unchanged
    m_color = color;
    m_valueLabel->setStyleSheet(QString(
        "color: %1; font-family: \"%2\"; font-size: 20px; font-weight: 700; border: none;")
        .arg(color, theme::FONT_MONO));
}

// ═══════════════════════ StatusPanel ═══════════════════════

StatusPanel::StatusPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

QFrame* StatusPanel::metricRow(std::initializer_list<QWidget*> cells)
{
    auto* card = new QFrame;
    card->setStyleSheet(QString(
        "QFrame { background-color: %1; border: 1px solid %2; border-radius: 8px; }")
        .arg(theme::BG_CARD, theme::BORDER));
    auto* grid = new QGridLayout(card);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    int i = 0;
    for (auto* cell : cells) {
        if (i > 0) {
            auto* sep = new QFrame;
            sep->setFixedWidth(1);
            sep->setStyleSheet(QString("background-color: %1;").arg(theme::BORDER));
            grid->addWidget(sep, 0, i * 2 - 1);
        }
        grid->addWidget(cell, 0, i * 2);
        grid->setColumnStretch(i * 2, 1);
        ++i;
    }
    return card;
}

void StatusPanel::setupUi()
{
    setStyleSheet(QString("background-color: %1;").arg(theme::BG_SIDEBAR));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto* content = new QWidget;
    content->setStyleSheet("background: transparent;");
    auto* lay = new QVBoxLayout(content);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(6);

    // Section header helper (icon + text, like Python's _SectionHeader with qtawesome)
    auto sectionHeader = [](const QString& icon, const QString& text) {
        auto* w = new QWidget;
        auto* hl = new QHBoxLayout(w);
        hl->setContentsMargins(0, 6, 0, 2);
        hl->setSpacing(6);
        auto* iconLbl = new QLabel(icon);
        iconLbl->setStyleSheet(QString(
            "color: %1; font-size: 12px;").arg(theme::TEXT_DIM));
        hl->addWidget(iconLbl);
        auto* lbl = new QLabel(text.toUpper());
        lbl->setStyleSheet(QString(
            "color: %1; font-size: 10px; font-weight: 700; letter-spacing: 1.2px;")
            .arg(theme::TEXT_DIM));
        hl->addWidget(lbl);
        hl->addStretch();
        return w;
    };

    auto separator = []() {
        auto* f = new QFrame;
        f->setFixedHeight(1);
        f->setStyleSheet(QString("background-color: %1;").arg(theme::BORDER));
        return f;
    };

    // ── TELEMETRY ──
    lay->addWidget(sectionHeader(QStringLiteral("\u25CE"), QStringLiteral("Телеметрия")));

    m_airspeed = new MetricCell("Возд. скорость", "м/с", theme::METRIC_SPEED);
    m_groundspeed = new MetricCell("Пут. скорость", "м/с", theme::METRIC_GS);
    m_altitudeAgl = new MetricCell("Высота отн.", "м", theme::METRIC_ALT);
    lay->addWidget(metricRow({m_airspeed, m_groundspeed, m_altitudeAgl}));

    // Wind cell with dropdown
    auto* windCell = new QWidget;
    auto* wcLay = new QVBoxLayout(windCell);
    wcLay->setContentsMargins(10, 8, 10, 8);
    wcLay->setSpacing(2);

    auto* windHdr = new QHBoxLayout;
    windHdr->setContentsMargins(0, 0, 0, 0);
    auto* windName = new QLabel("Ветер");
    windName->setStyleSheet(QString("color: %1; font-size: 10px; border: none;").arg(theme::TEXT_DIM));
    windHdr->addWidget(windName);
    windHdr->addStretch();

    m_btnWindDropdown = new QPushButton();
    m_btnWindDropdown->setIcon(paintedChevronDown(14, QColor(theme::TEXT_TERTIARY)));
    m_btnWindDropdown->setIconSize(QSize(14, 14));
    m_btnWindDropdown->setFixedSize(20, 20);
    m_btnWindDropdown->setCursor(Qt::PointingHandCursor);
    m_btnWindDropdown->setToolTip(QStringLiteral("Задать ветер"));
    m_btnWindDropdown->setStyleSheet(QString(
        "QPushButton { background: transparent; border: 1px solid %1; border-radius: 4px; }"
        "QPushButton:hover { background-color: %2; border-color: %3; }")
        .arg(theme::BORDER, theme::BG_HOVER, theme::BORDER_LIGHT));
    windHdr->addWidget(m_btnWindDropdown);
    wcLay->addLayout(windHdr);

    m_windValue = new QLabel("---");
    m_windValue->setStyleSheet(QString(
        "color: %1; font-family: \"%2\"; font-size: 20px; font-weight: 700; border: none;")
        .arg(theme::METRIC_WIND, theme::FONT_MONO));
    wcLay->addWidget(m_windValue);

    m_battery = new MetricCell("Батарея", "В", theme::METRIC_BAT);
    m_gps = new MetricCell("GPS", "", theme::METRIC_GPS);
    lay->addWidget(metricRow({windCell, m_battery, m_gps}));

    // Wind popup
    m_windPopup = new QFrame(this, Qt::Popup);
    m_windPopup->setStyleSheet(QString(
        "QFrame { background-color: %1; border: 1px solid %2; border-radius: 8px; }")
        .arg(theme::BG_CARD, theme::BORDER_LIGHT));
    auto* popLay = new QVBoxLayout(m_windPopup);
    popLay->setContentsMargins(12, 10, 12, 10);
    popLay->setSpacing(8);

    auto* popTitle = new QLabel("Задать ветер");
    popTitle->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: 700; border: none;")
                                .arg(theme::TEXT_TERTIARY));
    popLay->addWidget(popTitle);

    QString popSpinStyle = QString(
        "QSpinBox { background-color: %1; color: %2; border: 1px solid %3; border-radius: 6px; "
        "padding: 4px 8px; font-family: \"%4\"; font-size: 13px; font-weight: 600; }"
        "QSpinBox:focus { border-color: %5; }")
        .arg(theme::BG_INPUT, theme::TEXT_PRIMARY, theme::BORDER, theme::FONT_MONO, theme::PRIMARY);

    auto* dirLbl = new QLabel("Направление");
    dirLbl->setStyleSheet(QString("color: %1; font-size: 10px; border: none;").arg(theme::TEXT_DIM));
    popLay->addWidget(dirLbl);
    m_spinWindDir = new QSpinBox;
    m_spinWindDir->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_spinWindDir->setRange(0, 360);
    m_spinWindDir->setWrapping(true);
    m_spinWindDir->setSuffix("°");
    m_spinWindDir->setFixedHeight(30);
    m_spinWindDir->setStyleSheet(popSpinStyle);
    popLay->addWidget(m_spinWindDir);

    auto* spdLbl = new QLabel("Скорость");
    spdLbl->setStyleSheet(QString("color: %1; font-size: 10px; border: none;").arg(theme::TEXT_DIM));
    popLay->addWidget(spdLbl);
    m_spinWindSpeed = new QSpinBox;
    m_spinWindSpeed->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_spinWindSpeed->setRange(0, 30);
    m_spinWindSpeed->setSuffix(" м/с");
    m_spinWindSpeed->setFixedHeight(30);
    m_spinWindSpeed->setStyleSheet(popSpinStyle);
    popLay->addWidget(m_spinWindSpeed);

    m_btnSetWind = new QPushButton("  Применить");
    m_btnSetWind->setFixedHeight(30);
    m_btnSetWind->setCursor(Qt::PointingHandCursor);
    m_btnSetWind->setStyleSheet(QString(
        "QPushButton { background-color: %1; color: #fff; border: none; border-radius: 6px; font-size: 12px; font-weight: 600; }"
        "QPushButton:hover { background-color: %2; }").arg(theme::PRIMARY, theme::PRIMARY_HOVER));
    popLay->addWidget(m_btnSetWind);
    m_windPopup->setFixedWidth(180);
    m_windPopup->adjustSize();

    connect(m_btnWindDropdown, &QPushButton::clicked, this, [this]() {
        if (m_windPopup->isVisible()) { m_windPopup->hide(); return; }
        auto pos = m_btnWindDropdown->mapToGlobal(m_btnWindDropdown->rect().bottomLeft());
        m_windPopup->move(pos.x() - m_windPopup->width() + m_btnWindDropdown->width(), pos.y() + 4);
        m_windPopup->show();
    });
    connect(m_btnSetWind, &QPushButton::clicked, this, [this]() {
        emit windOverrideRequested(m_spinWindDir->value(), m_spinWindSpeed->value());
        m_windPopup->hide();
    });

    // ── NAVIGATION ──
    lay->addWidget(separator());
    lay->addWidget(sectionHeader(QStringLiteral("\u25C6"), QStringLiteral("Навигация")));

    m_waypoint  = new MetricCell("Точка",       "", theme::PRIMARY_LIGHT);
    m_distance  = new MetricCell("Расстояние",  "", theme::METRIC_ALT);
    auto* navRow1 = metricRow({m_waypoint, m_distance});
    navRow1->setObjectName("nav_r1");
    navRow1->setStyleSheet(QString(
        "#nav_r1 { background-color: %1; border: none; border-radius: 8px; }")
        .arg(theme::BG_CARD));
    lay->addWidget(navRow1);

    m_eta = new MetricCell("Время",       "", theme::METRIC_WIND);
    m_xtk = new MetricCell("Отклонение",  "", theme::METRIC_SPEED);
    auto* navRow2 = metricRow({m_eta, m_xtk});
    navRow2->setObjectName("nav_r2");
    navRow2->setStyleSheet(QString(
        "#nav_r2 { background-color: %1; border: none; border-radius: 8px; }")
        .arg(theme::BG_CARD));
    lay->addWidget(navRow2);

    // ── AUTOPILOT ──
    lay->addWidget(separator());
    lay->addWidget(sectionHeader(QStringLiteral("\u2699"), QStringLiteral("Автопилот")));

    // Mode card
    auto* apCard = new QFrame;
    apCard->setObjectName("ap_card");
    apCard->setStyleSheet(QString(
        "#ap_card { background-color: %1; border: 1px solid %2; border-radius: 8px; }")
        .arg(theme::BG_CARD, theme::BORDER));
    auto* apLay = new QVBoxLayout(apCard);
    apLay->setContentsMargins(10, 8, 10, 8);
    apLay->setSpacing(4);

    auto* modeRow = new QHBoxLayout;
    modeRow->setSpacing(8);
    auto* modeLbl = new QLabel("Режим");
    modeLbl->setStyleSheet(QString("color: %1; font-size: 12px; border: none;").arg(theme::TEXT_TERTIARY));
    modeRow->addWidget(modeLbl);
    modeRow->addStretch();

    m_apMode = new QLabel("РУЧНОЙ");
    m_apMode->setStyleSheet(QString(
        "color: %1; font-family: \"%2\"; font-size: 13px; font-weight: 700; "
        "padding: 3px 10px; border-radius: 4px; background-color: %3;")
        .arg(theme::TEXT_DIM, theme::FONT_MONO, theme::BG_INPUT));
    modeRow->addWidget(m_apMode);
    apLay->addLayout(modeRow);

    m_apAction = new QLabel("---");
    m_apAction->setAlignment(Qt::AlignCenter);
    m_apAction->setStyleSheet(QString(
        "color: %1; font-family: \"%2\"; font-size: 13px; border: none;")
        .arg(theme::TEXT_DIM, theme::FONT_MONO));
    apLay->addWidget(m_apAction);

    // Data rows
    auto addDataRow = [&](QLabel*& val, const QString& labelText) {
        auto* row = new QHBoxLayout;
        row->setSpacing(0);
        auto* lbl = new QLabel(labelText);
        lbl->setStyleSheet(QString("color: %1; font-size: 12px; border: none;").arg(theme::TEXT_TERTIARY));
        row->addWidget(lbl);
        row->addStretch();
        val = new QLabel("---");
        val->setStyleSheet(QString(
            "color: %1; font-family: \"%2\"; font-size: 15px; font-weight: 600; border: none;")
            .arg(theme::TEXT_PRIMARY, theme::FONT_MONO));
        row->addWidget(val);
        apLay->addLayout(row);
    };
    addDataRow(m_apTarget, "Целевой курс");
    addDataRow(m_apError, "Ошибка курса");
    addDataRow(m_apAltError, "Ошибка высоты");
    lay->addWidget(apCard);

    // Controls card
    auto* ctrlCard = new QFrame;
    ctrlCard->setObjectName("ctrl_card");
    ctrlCard->setStyleSheet(QString(
        "#ctrl_card { background-color: %1; border: 1px solid %2; border-radius: 8px; }")
        .arg(theme::BG_CARD, theme::BORDER));
    auto* ctrlLay = new QVBoxLayout(ctrlCard);
    ctrlLay->setContentsMargins(10, 8, 10, 8);
    ctrlLay->setSpacing(8);

    QString spinStyle = QString(
        "QSpinBox { background-color: %1; color: %2; border: 1px solid %3; border-radius: 8px; "
        "padding: 4px 10px; font-family: \"%4\"; font-size: 14px; font-weight: 600; selection-background-color: %5; }"
        "QSpinBox:focus { border-color: %5; }"
        "QSpinBox:disabled { background-color: %6; color: %7; border-color: %8; }")
        .arg(theme::BG_INPUT, theme::TEXT_PRIMARY, theme::BORDER,
             theme::FONT_MONO, theme::PRIMARY, theme::BG_CARD, theme::TEXT_DIM, theme::BORDER_SUBTLE);

    QString btnStyle = QString(
        "QPushButton { background-color: %1; border: none; border-radius: 8px; }"
        "QPushButton:hover { background-color: %2; }"
        "QPushButton:disabled { background-color: %3; border: 1px solid %4; }")
        .arg(theme::PRIMARY, theme::PRIMARY_HOVER, theme::BG_INPUT, theme::BORDER);

    auto checkIcon = paintedCheckIcon(20, Qt::white);

    auto addControl = [&](const QString& labelText, QSpinBox*& spin, QPushButton*& btn,
                           const QString& suffix, int lo, int hi, int defaultVal) {
        auto* lbl = new QLabel(labelText);
        lbl->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: 600; border: none;")
                               .arg(theme::TEXT_TERTIARY));
        ctrlLay->addWidget(lbl);

        auto* inputRow = new QHBoxLayout;
        inputRow->setSpacing(6);
        spin = new QSpinBox;
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setRange(lo, hi);
        spin->setValue(defaultVal);
        spin->setSuffix(suffix);
        spin->setEnabled(false);
        spin->setFixedHeight(34);
        spin->setStyleSheet(spinStyle);
        inputRow->addWidget(spin);

        btn = new QPushButton();
        btn->setIcon(checkIcon);
        btn->setIconSize(QSize(20, 20));
        btn->setFixedSize(34, 34);
        btn->setEnabled(false);
        btn->setStyleSheet(btnStyle);
        inputRow->addWidget(btn);

        ctrlLay->addLayout(inputRow);
    };

    addControl("Высота", m_spinTargetAlt, m_btnSetAlt, " м", 10, 5000, 100);
    addControl("Радиус", m_spinOrbitRadius, m_btnSetRadius, " м", 30, 500, 150);
    addControl("Скорость", m_spinTargetSpeed, m_btnSetSpeed, " м/с", 15, 35, 20);

    connect(m_btnSetAlt, &QPushButton::clicked, this, [this]() {
        m_manualAltOverride = true;
        emit targetAltitudeChanged(m_spinTargetAlt->value());
    });
    connect(m_btnSetRadius, &QPushButton::clicked, this, [this]() {
        m_manualRadiusOverride = true;
        emit orbitRadiusChanged(m_spinOrbitRadius->value());
    });
    connect(m_btnSetSpeed, &QPushButton::clicked, this, [this]() {
        m_manualSpeedOverride = true;
        emit targetAirspeedChanged(m_spinTargetSpeed->value());
    });

    lay->addWidget(ctrlCard);
    lay->addStretch();

    scroll->setWidget(content);
    outer->addWidget(scroll);
}

// ═══════════════════════ Updates ═══════════════════════

void StatusPanel::updateTelemetry(const TelemetryState& state)
{
    m_airspeed->setValue(state.airspeed(), 1);
    m_groundspeed->setValue(state.groundspeed(), 1);
    m_altitudeAgl->setValue(state.altitudeAgl(), 0);

    m_windValue->setText(QString("%1°/%2")
                             .arg(static_cast<int>(state.windDirection()), 3, 10, QChar('0'))
                             .arg(state.windSpeed(), 0, 'f', 0));

    m_battery->setValue(state.batteryVoltage(), 1);

    if (state.gpsFix() >= 3) {
        m_gps->setText(QString("3D (%1)").arg(state.satellites()));
        m_gps->setColor(theme::SUCCESS);
    } else {
        m_gps->setText(QString("NO FIX (%1)").arg(state.satellites()));
        m_gps->setColor(theme::ERROR_CLR);
    }
}

void StatusPanel::updateNavigation(int waypointIdx, int totalWaypoints,
                                     double distance, double etaSeconds, double xtk)
{
    m_waypoint->setText(QString("%1/%2").arg(waypointIdx).arg(totalWaypoints));
    m_distance->setText(QString("%1 км").arg(distance / 1000.0, 0, 'f', 2));
    int m = static_cast<int>(etaSeconds) / 60;
    int s = static_cast<int>(etaSeconds) % 60;
    m_eta->setText(QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')));
    QString sign = xtk >= 0 ? "+" : "";
    m_xtk->setText(QString("%1%2 м").arg(sign).arg(xtk, 0, 'f', 0));
}

void StatusPanel::updateAutopilot(const QString& mode, const AutopilotStatus& status)
{
    bool modeChanged = (m_lastApMode != mode);

    if (modeChanged) {
        m_lastApMode = mode;
        m_apMode->setText(mode == "NAV" ? QStringLiteral("НАВИГАЦИЯ") : QStringLiteral("РУЧНОЙ"));
    }

    if (mode == "MANUAL") {
        if (modeChanged) {
            m_apMode->setStyleSheet(QString(
                "color: %1; font-family: \"%2\"; font-size: 13px; font-weight: 700; "
                "padding: 3px 10px; border-radius: 4px; background-color: %3;")
                .arg(theme::TEXT_DIM, theme::FONT_MONO, theme::BG_INPUT));

            m_apAction->setText("---");
            m_apAction->setStyleSheet(QString("color: %1; font-family: '%2'; font-size: 13px; border: none;")
                                          .arg(theme::TEXT_DIM, theme::FONT_MONO));

            m_apTarget->setText("---");
            m_apError->setText("---");
            m_apAltError->setText("---");

            for (auto* w : {m_spinTargetAlt, m_spinOrbitRadius, m_spinTargetSpeed}) w->setEnabled(false);
            for (auto* w : {m_btnSetAlt, m_btnSetRadius, m_btnSetSpeed}) w->setEnabled(false);

            m_manualAltOverride = false;
            m_manualRadiusOverride = false;
            m_manualSpeedOverride = false;
            m_lastOrbitState = false;
        }
    } else {
        if (modeChanged) {
            m_apMode->setStyleSheet(QString(
                "color: #fff; font-family: \"%1\"; font-size: 13px; font-weight: 700; "
                "padding: 3px 10px; border-radius: 4px; background-color: %2;")
                .arg(theme::FONT_MONO, theme::SUCCESS));

            for (auto* w : {m_spinTargetAlt, m_spinOrbitRadius, m_spinTargetSpeed}) w->setEnabled(true);
            for (auto* w : {m_btnSetAlt, m_btnSetRadius, m_btnSetSpeed}) w->setEnabled(true);
        }

        m_apAction->setText(formatAction(status.action));

        if (status.isOrbiting != m_lastOrbitState || modeChanged) {
            m_lastOrbitState = status.isOrbiting;
            if (status.isOrbiting) {
                m_apAction->setStyleSheet(QString("color: %1; font-family: '%2'; font-size: 13px; font-weight: 700; border: none;")
                                              .arg(theme::WARNING, theme::FONT_MONO));
            } else {
                m_apAction->setStyleSheet(QString("color: %1; font-family: '%2'; font-size: 13px; border: none;")
                                              .arg(theme::PRIMARY_LIGHT, theme::FONT_MONO));
            }
        }

        if (status.targetHeading >= 0)
            m_apTarget->setText(QString("%1°").arg(status.targetHeading, 0, 'f', 0));
        m_apError->setText(QString("%1%2°").arg(status.headingError >= 0 ? "+" : "").arg(status.headingError, 0, 'f', 1));
        m_apAltError->setText(QString("%1%2 м").arg(status.altitudeError >= 0 ? "+" : "").arg(status.altitudeError, 0, 'f', 0));

        if (!m_manualAltOverride) {
            int v = static_cast<int>(status.targetAltitude);
            if (v > 0) { m_spinTargetAlt->blockSignals(true); m_spinTargetAlt->setValue(v); m_spinTargetAlt->blockSignals(false); }
        }
        if (!m_manualRadiusOverride) {
            int v = static_cast<int>(status.orbitRadius);
            if (v > 0) { m_spinOrbitRadius->blockSignals(true); m_spinOrbitRadius->setValue(v); m_spinOrbitRadius->blockSignals(false); }
        }
        if (!m_manualSpeedOverride) {
            int v = static_cast<int>(status.targetAirspeed);
            if (v > 0) { m_spinTargetSpeed->blockSignals(true); m_spinTargetSpeed->setValue(v); m_spinTargetSpeed->blockSignals(false); }
        }
    }
}

QString StatusPanel::formatAction(const QString& action)
{
    if (action.startsWith("ORBIT_") && action.contains('/')) {
        auto parts = action.split('(');
        QString orbitPart = parts[0].trimmed();
        QString vertical = parts.size() > 1 ? " (" + parts[1] : "";
        return "Круг " + orbitPart.split('_')[1] + vertical;
    }
    if (action.startsWith("ORBIT_INF")) {
        auto parts = action.split('(');
        return "Кружение ∞" + (parts.size() > 1 ? " (" + parts[1] : "");
    }
    if (action.startsWith("ALTITUDE_ORBIT")) {
        auto parts = action.split('(');
        return "Набор высоты" + (parts.size() > 1 ? " (" + parts[1] : "");
    }
    if (action.startsWith("TO_WAYPOINT")) {
        auto parts = action.split('(');
        return "К точке" + (parts.size() > 1 ? " (" + parts[1] : "");
    }
    if (action.startsWith("ОЖИДАНИЕ_ВЫСОТЫ")) {
        auto parts = action.split('(');
        return "Ожидание высоты" + (parts.size() > 1 ? " (" + parts[1] : "");
    }
    if (action == "ORBITING") return "Кружение";
    if (action == "IDLE") return "---";
    return action;
}

} // namespace vtol
