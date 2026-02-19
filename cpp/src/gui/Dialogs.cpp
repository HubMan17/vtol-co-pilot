#include "Dialogs.h"
#include "Theme.h"
#include "core/Config.h"
#include "navigation/ZoneManager.h"
#include "navigation/ZoneChecker.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <spdlog/spdlog.h>

namespace vtol {

// ═════════════════════════════════════════════════════════════════════════
//  Helper: styled OK button
// ═════════════════════════════════════════════════════════════════════════
static QPushButton* makeOkButton(const QString& text)
{
    auto* btn = new QPushButton(text);
    btn->setFixedHeight(32);
    btn->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; color: #fff; border: none; "
        "border-radius: 6px; padding: 0 14px; font-weight: 600; }"
        "QPushButton:hover { background-color: %2; }")
        .arg(theme::PRIMARY, theme::PRIMARY_HOVER));
    return btn;
}

static QPushButton* makeCancelButton()
{
    auto* btn = new QPushButton(QStringLiteral("Отмена"));
    btn->setFixedHeight(32);
    return btn;
}

static QHBoxLayout* buttonRow(QPushButton* cancel, QPushButton* ok,
                               QPushButton* extra = nullptr)
{
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    if (extra) row->addWidget(extra);
    row->addWidget(cancel);
    row->addStretch();
    row->addWidget(ok);
    return row;
}

// ═════════════════════════════════════════════════════════════════════════
//  WaypointDialog
// ═════════════════════════════════════════════════════════════════════════

WaypointDialog::WaypointDialog(double lat, double lon,
                               const ZoneChecker* zoneChecker,
                               QWidget* parent)
    : QDialog(parent), m_lat(lat), m_lon(lon), m_zoneChecker(zoneChecker)
{
    setupUi();
    theme::applyDarkTitlebar(static_cast<quintptr>(winId()));
}

void WaypointDialog::setupUi()
{
    setWindowTitle(QStringLiteral("Добавить точку маршрута"));
    setMinimumWidth(320);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(16, 16, 16, 16);

    // ── Coordinates ──
    auto* coordGroup = new QGroupBox(QStringLiteral("Координаты"));
    auto* coordForm = new QFormLayout(coordGroup);
    coordForm->setSpacing(6);

    auto* lblLat = new QLabel(QString::number(m_lat, 'f', 6));
    lblLat->setStyleSheet(QStringLiteral("color: %1; font-family: '%2'; font-weight: 600;")
                           .arg(theme::TEXT_PRIMARY, theme::FONT_MONO));
    auto* lblLon = new QLabel(QString::number(m_lon, 'f', 6));
    lblLon->setStyleSheet(lblLat->styleSheet());

    coordForm->addRow(QStringLiteral("Широта:"), lblLat);
    coordForm->addRow(QStringLiteral("Долгота:"), lblLon);
    layout->addWidget(coordGroup);

    // ── Parameters ──
    auto* paramsGroup = new QGroupBox(QStringLiteral("Параметры"));
    auto* paramsForm = new QFormLayout(paramsGroup);
    paramsForm->setSpacing(8);

    m_spinAltitude = new QSpinBox;
    m_spinAltitude->setRange(10, 5000);
    m_spinAltitude->setValue(100);
    m_spinAltitude->setSuffix(QStringLiteral(" м"));
    connect(m_spinAltitude, qOverload<int>(&QSpinBox::valueChanged),
            this, [this]{ checkZoneRestriction(); });
    paramsForm->addRow(QStringLiteral("Высота:"), m_spinAltitude);

    m_chkClimbEnroute = new QCheckBox(QStringLiteral("Набирать высоту в процессе полёта"));
    paramsForm->addRow(QString(), m_chkClimbEnroute);

    m_comboType = new QComboBox;
    m_comboType->addItem(QStringLiteral("Пролёт"),              QStringLiteral("FLYTHROUGH"));
    m_comboType->addItem(QStringLiteral("Кружить N кругов"),    QStringLiteral("ORBIT_TURNS"));
    m_comboType->addItem(QStringLiteral("Кружить бесконечно"),  QStringLiteral("ORBIT_INFINITE"));
    m_comboType->addItem(QStringLiteral("Набор/смена высоты"),  QStringLiteral("ALTITUDE"));
    connect(m_comboType, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &WaypointDialog::onTypeChanged);
    paramsForm->addRow(QStringLiteral("Тип:"), m_comboType);

    m_spinRadius = new QSpinBox;
    m_spinRadius->setRange(10, 500);
    m_spinRadius->setValue(150);
    m_spinRadius->setSuffix(QStringLiteral(" м"));
    paramsForm->addRow(QStringLiteral("Радиус принятия:"), m_spinRadius);
    layout->addWidget(paramsGroup);

    // ── Orbit ──
    m_orbitGroup = new QGroupBox(QStringLiteral("Кружение"));
    auto* orbitForm = new QFormLayout(m_orbitGroup);
    orbitForm->setSpacing(8);

    m_spinOrbitRadius = new QSpinBox;
    m_spinOrbitRadius->setRange(30, 500);
    m_spinOrbitRadius->setValue(150);
    m_spinOrbitRadius->setSuffix(QStringLiteral(" м"));
    orbitForm->addRow(QStringLiteral("Радиус:"), m_spinOrbitRadius);

    m_spinOrbitTurns = new QSpinBox;
    m_spinOrbitTurns->setRange(1, 100);
    m_spinOrbitTurns->setValue(1);
    orbitForm->addRow(QStringLiteral("Кругов:"), m_spinOrbitTurns);

    m_orbitGroup->setVisible(false);
    layout->addWidget(m_orbitGroup);

    // ── Zone warning ──
    m_lblZoneWarning = new QLabel;
    m_lblZoneWarning->setWordWrap(true);
    m_lblZoneWarning->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; background-color: %2; border: 1px solid %1; "
        "border-radius: 4px; padding: 6px 10px; font-weight: 600; }")
        .arg(theme::ERROR_CLR, theme::ERROR_BG));
    m_lblZoneWarning->setVisible(false);
    layout->addWidget(m_lblZoneWarning);

    // ── Buttons ──
    auto* btnCancel = makeCancelButton();
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    m_btnOk = makeOkButton(QStringLiteral("Добавить"));
    connect(m_btnOk, &QPushButton::clicked, this, &QDialog::accept);

    layout->addLayout(buttonRow(btnCancel, m_btnOk));

    onTypeChanged(0);
    checkZoneRestriction();
}

void WaypointDialog::onTypeChanged(int /*index*/)
{
    auto typeId = m_comboType->currentData().toString();
    bool isOrbit = (typeId == "ORBIT_TURNS" || typeId == "ORBIT_INFINITE" || typeId == "ALTITUDE");
    m_orbitGroup->setVisible(isOrbit);
    m_spinOrbitTurns->setEnabled(typeId == "ORBIT_TURNS");
    adjustSize();
}

void WaypointDialog::checkZoneRestriction()
{
    if (!m_zoneChecker) return;

    auto res = m_zoneChecker->isPointRestricted(m_lat, m_lon, m_spinAltitude->value());
    if (res.restricted) {
        m_lblZoneWarning->setText(QString::fromStdString(res.reason));
        m_lblZoneWarning->setVisible(true);
        m_btnOk->setEnabled(false);
    } else {
        m_lblZoneWarning->setVisible(false);
        m_btnOk->setEnabled(true);
    }
    adjustSize();
}

WaypointDialog::Result WaypointDialog::result() const
{
    Result r;
    r.lat = m_lat;
    r.lon = m_lon;
    r.altitude = m_spinAltitude->value();
    r.action = m_comboType->currentData().toString();
    r.radius = m_spinRadius->value();
    r.climbEnroute = m_chkClimbEnroute->isChecked();
    if (r.action == "ORBIT_TURNS" || r.action == "ORBIT_INFINITE" || r.action == "ALTITUDE") {
        r.orbitRadius = m_spinOrbitRadius->value();
        if (r.action == "ORBIT_TURNS")
            r.orbitTurns = m_spinOrbitTurns->value();
    }
    return r;
}

// ═════════════════════════════════════════════════════════════════════════
//  ZonePropertiesDialog
// ═════════════════════════════════════════════════════════════════════════

ZonePropertiesDialog::ZonePropertiesDialog(const NoFlyZone* zone, QWidget* parent)
    : QDialog(parent), m_zone(zone)
{
    setupUi();
    theme::applyDarkTitlebar(static_cast<quintptr>(winId()));
}

void ZonePropertiesDialog::setupUi()
{
    bool editing = (m_zone != nullptr);
    setWindowTitle(editing ? QStringLiteral("Редактировать зону")
                          : QStringLiteral("Запретная зона"));
    setMinimumWidth(340);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(16, 16, 16, 16);

    // ── Parameters ──
    auto* paramsGroup = new QGroupBox(QStringLiteral("Параметры"));
    auto* paramsForm = new QFormLayout(paramsGroup);
    paramsForm->setSpacing(8);

    m_editName = new QLineEdit;
    if (editing && !m_zone->name.empty())
        m_editName->setText(QString::fromStdString(m_zone->name));
    paramsForm->addRow(QStringLiteral("Имя:"), m_editName);

    m_editDescription = new QTextEdit;
    m_editDescription->setFixedHeight(60);
    if (editing && !m_zone->description.empty())
        m_editDescription->setPlainText(QString::fromStdString(m_zone->description));
    paramsForm->addRow(QStringLiteral("Описание:"), m_editDescription);

    m_spinAltitude = new QSpinBox;
    m_spinAltitude->setRange(0, 10000);
    m_spinAltitude->setSingleStep(10);
    m_spinAltitude->setSuffix(QStringLiteral(" м"));
    m_spinAltitude->setSpecialValueText(QStringLiteral(" "));
    if (editing && m_zone->altitude.has_value())
        m_spinAltitude->setValue(static_cast<int>(m_zone->altitude.value()));
    paramsForm->addRow(QStringLiteral("Макс. высота:"), m_spinAltitude);

    layout->addWidget(paramsGroup);

    // ── Avoidance overrides ──
    auto* avoidGroup = new QGroupBox(QStringLiteral("Обход автопилотом"));
    auto* avoidForm = new QFormLayout(avoidGroup);
    avoidForm->setSpacing(8);

    m_comboAvoidMode = new QComboBox;
    m_comboAvoidMode->addItem(QStringLiteral("По умолчанию (глобальные)"), QString());
    m_comboAvoidMode->addItem(QStringLiteral("Выключено"),                QStringLiteral("disabled"));
    m_comboAvoidMode->addItem(QStringLiteral("Всегда избегать"),          QStringLiteral("always"));
    m_comboAvoidMode->addItem(QStringLiteral("Ниже высоты зоны"),        QStringLiteral("below_altitude"));
    if (editing && m_zone->avoid_mode.has_value()) {
        int idx = m_comboAvoidMode->findData(QString::fromStdString(m_zone->avoid_mode.value()));
        if (idx >= 0) m_comboAvoidMode->setCurrentIndex(idx);
    }
    avoidForm->addRow(QStringLiteral("Режим:"), m_comboAvoidMode);

    m_spinBuffer = new QSpinBox;
    m_spinBuffer->setRange(0, 5000);
    m_spinBuffer->setSingleStep(50);
    m_spinBuffer->setSuffix(QStringLiteral(" м"));
    m_spinBuffer->setSpecialValueText(QStringLiteral("По умолчанию"));
    if (editing && m_zone->buffer.has_value())
        m_spinBuffer->setValue(static_cast<int>(m_zone->buffer.value()));
    avoidForm->addRow(QStringLiteral("Буфер:"), m_spinBuffer);

    layout->addWidget(avoidGroup);

    // ── Buttons ──
    QPushButton* btnDelete = nullptr;
    if (editing) {
        btnDelete = new QPushButton(QStringLiteral("Удалить"));
        btnDelete->setFixedHeight(32);
        btnDelete->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: %1; color: %2; border: 1px solid %2; "
            "border-radius: 6px; padding: 0 12px; font-weight: 600; }"
            "QPushButton:hover { background-color: %2; color: #fff; }")
            .arg(theme::ERROR_BG, theme::ERROR_CLR));
        connect(btnDelete, &QPushButton::clicked, this, [this]{
            m_deleteRequested = true;
            done(DELETE_REQUESTED);
        });
    }

    auto* btnCancel = makeCancelButton();
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    m_btnOk = makeOkButton(editing ? QStringLiteral("Сохранить")
                                   : QStringLiteral("Создать"));
    connect(m_btnOk, &QPushButton::clicked, this, &QDialog::accept);

    layout->addLayout(buttonRow(btnCancel, m_btnOk, btnDelete));
}

ZonePropertiesDialog::Result ZonePropertiesDialog::result() const
{
    Result r;
    r.name = m_editName->text().trimmed();
    r.description = m_editDescription->toPlainText().trimmed();
    int alt = m_spinAltitude->value();
    r.altitude = (alt > 0) ? static_cast<double>(alt) : 0.0;
    r.avoidMode = m_comboAvoidMode->currentData().toString();
    int buf = m_spinBuffer->value();
    r.buffer = (buf > 0) ? static_cast<double>(buf) : 0.0;
    return r;
}

// ═════════════════════════════════════════════════════════════════════════
//  ZoneSettingsDialog
// ═════════════════════════════════════════════════════════════════════════

ZoneSettingsDialog::ZoneSettingsDialog(const ZoneAvoidanceConfig& config,
                                       QWidget* parent)
    : QDialog(parent), m_config(config)
{
    setupUi();
    theme::applyDarkTitlebar(static_cast<quintptr>(winId()));
}

void ZoneSettingsDialog::setupUi()
{
    setWindowTitle(QStringLiteral("Настройки обхода зон"));
    setMinimumWidth(380);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(16, 16, 16, 16);

    // ── Settlements ──
    auto* settlementGroup = new QGroupBox(QStringLiteral("Населённые пункты"));
    auto* sl = new QFormLayout(settlementGroup);
    sl->setSpacing(8);

    m_comboSettlementMode = new QComboBox;
    m_comboSettlementMode->addItem(QStringLiteral("Выключено"),              QStringLiteral("disabled"));
    m_comboSettlementMode->addItem(QStringLiteral("Всегда избегать"),        QStringLiteral("always"));
    m_comboSettlementMode->addItem(QStringLiteral("Ниже заданной высоты"),   QStringLiteral("below_altitude"));
    {
        int idx = m_comboSettlementMode->findData(QString::fromStdString(m_config.settlement_mode));
        if (idx >= 0) m_comboSettlementMode->setCurrentIndex(idx);
    }
    connect(m_comboSettlementMode, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]{ onSettlementModeChanged(); });
    sl->addRow(QStringLiteral("Режим:"), m_comboSettlementMode);

    m_lblSettlementAltitude = new QLabel(QStringLiteral("Мин. высота:"));
    m_spinSettlementAltitude = new QSpinBox;
    m_spinSettlementAltitude->setRange(10, 5000);
    m_spinSettlementAltitude->setValue(static_cast<int>(m_config.settlement_min_altitude));
    m_spinSettlementAltitude->setSuffix(QStringLiteral(" м"));
    sl->addRow(m_lblSettlementAltitude, m_spinSettlementAltitude);

    m_spinSettlementBuffer = new QSpinBox;
    m_spinSettlementBuffer->setRange(0, 5000);
    m_spinSettlementBuffer->setSingleStep(50);
    m_spinSettlementBuffer->setValue(static_cast<int>(m_config.settlement_buffer));
    m_spinSettlementBuffer->setSuffix(QStringLiteral(" м"));
    sl->addRow(QStringLiteral("Буфер:"), m_spinSettlementBuffer);

    layout->addWidget(settlementGroup);

    // ── NoFly zones ──
    auto* noflyGroup = new QGroupBox(QStringLiteral("Запретные зоны (по умолчанию)"));
    auto* nl = new QFormLayout(noflyGroup);
    nl->setSpacing(8);

    m_comboNoflyMode = new QComboBox;
    m_comboNoflyMode->addItem(QStringLiteral("Выключено"),              QStringLiteral("disabled"));
    m_comboNoflyMode->addItem(QStringLiteral("Всегда избегать"),        QStringLiteral("always"));
    m_comboNoflyMode->addItem(QStringLiteral("Ниже высоты зоны"),      QStringLiteral("below_altitude"));
    {
        int idx = m_comboNoflyMode->findData(QString::fromStdString(m_config.nofly_mode));
        if (idx >= 0) m_comboNoflyMode->setCurrentIndex(idx);
    }
    nl->addRow(QStringLiteral("Режим:"), m_comboNoflyMode);

    m_spinNoflyBuffer = new QSpinBox;
    m_spinNoflyBuffer->setRange(0, 5000);
    m_spinNoflyBuffer->setSingleStep(50);
    m_spinNoflyBuffer->setValue(static_cast<int>(m_config.nofly_buffer));
    m_spinNoflyBuffer->setSuffix(QStringLiteral(" м"));
    nl->addRow(QStringLiteral("Буфер:"), m_spinNoflyBuffer);

    layout->addWidget(noflyGroup);

    // ── Buttons ──
    auto* btnCancel = makeCancelButton();
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    auto* btnOk = makeOkButton(QStringLiteral("Сохранить"));
    connect(btnOk, &QPushButton::clicked, this, &QDialog::accept);

    layout->addLayout(buttonRow(btnCancel, btnOk));

    onSettlementModeChanged();
}

void ZoneSettingsDialog::onSettlementModeChanged()
{
    bool isAlt = m_comboSettlementMode->currentData().toString() == "below_altitude";
    m_spinSettlementAltitude->setVisible(isAlt);
    m_lblSettlementAltitude->setVisible(isAlt);
    adjustSize();
}

ZoneAvoidanceConfig ZoneSettingsDialog::getConfig() const
{
    ZoneAvoidanceConfig cfg;
    cfg.settlement_mode = m_comboSettlementMode->currentData().toString().toStdString();
    cfg.settlement_min_altitude = m_spinSettlementAltitude->value();
    cfg.settlement_buffer = m_spinSettlementBuffer->value();
    cfg.nofly_mode = m_comboNoflyMode->currentData().toString().toStdString();
    cfg.nofly_buffer = m_spinNoflyBuffer->value();
    return cfg;
}

// ═════════════════════════════════════════════════════════════════════════
//  SettingsDialog
// ═════════════════════════════════════════════════════════════════════════

SettingsDialog::SettingsDialog(const GuiConfig& config, QWidget* parent)
    : QDialog(parent), m_config(config)
{
    setupUi();
    theme::applyDarkTitlebar(static_cast<quintptr>(winId()));
}

void SettingsDialog::setupUi()
{
    setWindowTitle(QStringLiteral("Настройки"));
    setMinimumWidth(360);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(16, 16, 16, 16);

    // ── Map ──
    auto* mapGroup = new QGroupBox(QStringLiteral("Карта"));
    auto* ml = new QFormLayout(mapGroup);
    ml->setSpacing(8);

    m_spinTrackLength = new QSpinBox;
    m_spinTrackLength->setRange(100, 200000);
    m_spinTrackLength->setSingleStep(100);
    m_spinTrackLength->setValue(m_config.track_length);
    m_spinTrackLength->setSuffix(QStringLiteral(" точек"));
    ml->addRow(QStringLiteral("Длина трека:"), m_spinTrackLength);

    layout->addWidget(mapGroup);

    // ── Buttons ──
    auto* btnCancel = makeCancelButton();
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    auto* btnOk = makeOkButton(QStringLiteral("Сохранить"));
    connect(btnOk, &QPushButton::clicked, this, &QDialog::accept);

    layout->addLayout(buttonRow(btnCancel, btnOk));
}

GuiConfig SettingsDialog::getGuiConfig() const
{
    GuiConfig cfg = m_config;
    cfg.track_length = m_spinTrackLength->value();
    return cfg;
}

} // namespace vtol
