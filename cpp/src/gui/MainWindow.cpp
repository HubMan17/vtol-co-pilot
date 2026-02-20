#include "MainWindow.h"
#include "Theme.h"
#include "Dialogs.h"
#include "navigation/Calculations.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QMenu>
#include <QCursor>
#include <QCloseEvent>
#include <QApplication>
#include <QtConcurrent>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QIcon>
#include <spdlog/spdlog.h>
#include <cmath>

namespace vtol {

// ═════════════════════════════════════════════════════════════════════════
//  Construction
// ═════════════════════════════════════════════════════════════════════════

MainWindow::MainWindow(AppConfig config, QWidget* parent)
    : QMainWindow(parent)
    , m_config(std::move(config))
    , m_connection(m_config.mavlink, this)
    , m_proxy(m_config.mavlink.proxy_port, this)
    , m_zoneChecker(m_zoneManager, m_config.zone_avoidance)
    , m_pathPlanner(m_zoneChecker)
    , m_autopilot(m_connection, m_config.autopilot, this)
    , m_tileProxy(this)
{
    m_autopilot.setRoutePlanner(&m_routePlanner);
    m_autopilot.setZoneChecker(&m_zoneChecker);
    m_autopilot.setPathPlanner(&m_pathPlanner);

    // Settlement cache for zone checking
    auto cacheDir = QApplication::applicationDirPath() + "/cache/settlements_v5";
    if (QDir(cacheDir).exists())
        m_zoneChecker.setSettlementCacheDir(cacheDir.toStdString());

    qApp->setStyleSheet(theme::stylesheet());

    setupUi();
    setupConnections();
    setupTimer();
    applyConfig();

    theme::applyDarkTitlebar(static_cast<quintptr>(winId()));
    showMaximized();

    // Deferred zone loading — let the event loop process UI first
    QTimer::singleShot(0, this, &MainWindow::loadZones);

    SPDLOG_INFO("[MainWindow] Initialized");
}

MainWindow::~MainWindow() = default;

// ═════════════════════════════════════════════════════════════════════════
//  UI Setup
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::setupUi()
{
    setWindowTitle("VTOL Co-Pilot");
    setMinimumSize(1200, 800);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(4);
    setCentralWidget(splitter);

    // ── LEFT: Map ──
    m_mapWidget = new MapWidget(this);
    m_mapWidget->setMinimumWidth(400);

    // Apply saved layer visibility BEFORE loadMap (so MapLegend reads correct state)
    auto* be = m_mapWidget->backend();
    be->setShowTrack(m_config.gui.show_track);
    be->setShowWaypoints(m_config.gui.show_waypoints);
    be->setShowZones(m_config.gui.show_zones);
    be->setShowSettlements(m_config.gui.show_settlements);

    // Start tile proxy BEFORE loading QML so Plugin gets the URL at creation time
    int port = m_tileProxy.startProxy();
    if (port > 0) {
        m_mapWidget->setTileServerUrl(
            QStringLiteral("http://127.0.0.1:%1/").arg(port));
        SPDLOG_INFO("[MainWindow] Tile proxy started on port {}", port);
    }

    // Now load map — MapLibre GLWidget will use the tile URL
    m_mapWidget->loadMap();

    splitter->addWidget(m_mapWidget);

    // ── RIGHT: Panel ──
    auto* right = new QWidget;
    right->setStyleSheet(QStringLiteral("background-color: %1;").arg(theme::BG_SIDEBAR));
    right->setMinimumWidth(280);
    right->setMaximumWidth(800);
    auto* rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->setSpacing(0);

    rightLay->addWidget(buildPanelHeader());

    auto* sep = new QFrame;
    sep->setFixedHeight(1);
    sep->setStyleSheet(QStringLiteral("background-color: %1;").arg(theme::BORDER));
    rightLay->addWidget(sep);

    m_statusPanel = new StatusPanel(this);
    rightLay->addWidget(m_statusPanel, 1);

    auto* sep2 = new QFrame;
    sep2->setFixedHeight(1);
    sep2->setStyleSheet(QStringLiteral("background-color: %1;").arg(theme::BORDER));
    rightLay->addWidget(sep2);

    rightLay->addWidget(buildControls());
    splitter->addWidget(right);

    splitter->setSizes({900, 580});
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);

    // Status bar
    auto* sb = new QStatusBar;
    setStatusBar(sb);
    sb->showMessage(QStringLiteral("Отключено"));

    auto labelStyle = QStringLiteral("color: %1; font-size: 11px; border: none; padding: 0 6px;")
                        .arg(theme::TEXT_TERTIARY);
    auto valueStyle = QStringLiteral("color: %1; font-family: \"%2\"; font-size: 11px; "
                                     "font-weight: 600; border: none; padding: 0 4px;")
                        .arg(theme::TEXT_PRIMARY, theme::FONT_MONO);
    auto dimStyle = QStringLiteral("color: %1; font-family: \"%2\"; font-size: 11px; "
                                   "border: none; padding: 0 4px;")
                        .arg(theme::TEXT_DIM, theme::FONT_MONO);

    auto* sbZoomLbl = new QLabel(QStringLiteral("Масштаб"));
    sbZoomLbl->setStyleSheet(labelStyle);
    m_sbZoomVal = new QLabel(QString::number(m_config.gui.map_zoom));
    m_sbZoomVal->setStyleSheet(valueStyle);

    auto* sbLayerVal = new QLabel(QStringLiteral("Спутник"));
    sbLayerVal->setStyleSheet(dimStyle);

    auto* sbAcLbl = new QLabel(QStringLiteral("ЛА"));
    sbAcLbl->setStyleSheet(labelStyle);
    m_sbAcVal = new QLabel(QStringLiteral("--- , ---"));
    m_sbAcVal->setStyleSheet(valueStyle);

    auto* sbCurLbl = new QLabel(QStringLiteral("Курсор"));
    sbCurLbl->setStyleSheet(labelStyle);
    m_sbCurVal = new QLabel(QStringLiteral("--- , ---"));
    m_sbCurVal->setStyleSheet(dimStyle);

    for (auto* w : {sbZoomLbl, m_sbZoomVal, sbLayerVal, sbAcLbl, m_sbAcVal, sbCurLbl, m_sbCurVal})
        sb->addPermanentWidget(w);
}

QWidget* MainWindow::buildPanelHeader()
{
    auto* hdr = new QWidget;
    hdr->setFixedHeight(48);
    auto* lay = new QHBoxLayout(hdr);
    lay->setContentsMargins(10, 0, 10, 0);
    lay->setSpacing(8);

    m_btnConnect = new QPushButton(QStringLiteral("Подключить"));
    m_btnConnect->setFixedHeight(30);
    m_btnConnect->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; color: #fff; border: none; "
        "border-radius: 6px; padding: 0 12px; font-size: 12px; font-weight: 600; }"
        "QPushButton:hover { background-color: %2; }")
        .arg(theme::PRIMARY, theme::PRIMARY_HOVER));
    lay->addWidget(m_btnConnect);

    m_btnDisconnect = new QPushButton(QStringLiteral("Откл."));
    m_btnDisconnect->setFixedHeight(30);
    m_btnDisconnect->setEnabled(false);
    lay->addWidget(m_btnDisconnect);

    lay->addStretch();

    m_lblMode = new QLabel(QStringLiteral("---"));
    m_lblMode->setStyleSheet(QStringLiteral(
        "color: %1; font-family: \"%2\"; font-size: 12px; font-weight: 700; "
        "padding: 3px 10px; border-radius: 4px; background-color: %3;")
        .arg(theme::TEXT_TERTIARY, theme::FONT_MONO, theme::BG_INPUT));
    lay->addWidget(m_lblMode);

    m_lblStatus = new QLabel(QStringLiteral("OFF"));
    m_lblStatus->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 10px; font-weight: 700; padding: 2px 8px; "
        "border-radius: 10px; background-color: %2;")
        .arg(theme::ERROR_CLR, theme::ERROR_BG));
    lay->addWidget(m_lblStatus);

    return hdr;
}

QWidget* MainWindow::buildControls()
{
    auto* panel = new QWidget;
    auto* lay = new QVBoxLayout(panel);
    lay->setContentsMargins(10, 8, 10, 10);
    lay->setSpacing(6);

    // NAV button
    m_btnNav = new QPushButton(QStringLiteral("Навигация"));
    m_btnNav->setCheckable(true);
    m_btnNav->setEnabled(false);
    m_btnNav->setFixedHeight(36);
    m_btnNav->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border: 1px solid %3; "
        "border-radius: 8px; font-size: 13px; font-weight: 600; padding: 0 16px; }"
        "QPushButton:hover { background-color: %4; color: %5; border-color: %6; }"
        "QPushButton:checked { background-color: %7; color: #fff; border-color: %7; }"
        "QPushButton:disabled { background-color: %8; color: %9; border-color: %10; }")
        .arg(theme::BG_INPUT, theme::TEXT_SECONDARY, theme::BORDER,
             theme::BG_HOVER, theme::TEXT_PRIMARY, theme::BORDER_LIGHT,
             theme::SUCCESS, theme::BG_CARD, theme::TEXT_DIM, theme::BORDER_SUBTLE));
    lay->addWidget(m_btnNav);

    // Row 1: Position + Home + Route
    auto* r1 = new QHBoxLayout;
    r1->setSpacing(4);

    m_btnSetPos = new QPushButton(QStringLiteral("Коррекция"));
    m_btnSetPos->setCheckable(true);
    m_btnSetPos->setEnabled(false);
    m_btnSetPos->setFixedHeight(30);
    r1->addWidget(m_btnSetPos);

    m_btnSetHome = new QPushButton(QStringLiteral("Дом"));
    m_btnSetHome->setCheckable(true);
    m_btnSetHome->setEnabled(false);
    m_btnSetHome->setFixedHeight(30);
    r1->addWidget(m_btnSetHome);

    m_btnLoadRoute = new QPushButton(QStringLiteral("Маршрут"));
    m_btnLoadRoute->setEnabled(false);
    m_btnLoadRoute->setFixedHeight(30);
    r1->addWidget(m_btnLoadRoute);

    lay->addLayout(r1);

    // Row 2: Follow + RTH + Clear + Zones + Settings
    auto* r2 = new QHBoxLayout;
    r2->setSpacing(4);

    m_btnFollow = new QPushButton(QStringLiteral("Слежение"));
    m_btnFollow->setCheckable(true);
    m_btnFollow->setEnabled(false);
    m_btnFollow->setFixedHeight(30);
    r2->addWidget(m_btnFollow);

    m_btnHome = new QPushButton(QStringLiteral("Домой"));
    m_btnHome->setCheckable(true);
    m_btnHome->setEnabled(false);
    m_btnHome->setFixedHeight(30);
    m_btnHome->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border: 1px solid %3; "
        "border-radius: 6px; padding: 0 10px; font-size: 12px; font-weight: 500; }"
        "QPushButton:hover { background-color: %4; color: %5; }"
        "QPushButton:checked { background-color: %6; color: #000; border-color: %6; }"
        "QPushButton:disabled { background-color: %7; color: %8; border-color: %9; }")
        .arg(theme::BG_INPUT, theme::TEXT_SECONDARY, theme::BORDER,
             theme::BG_HOVER, theme::TEXT_PRIMARY,
             theme::WARNING, theme::BG_CARD, theme::TEXT_DIM, theme::BORDER_SUBTLE));
    r2->addWidget(m_btnHome);

    m_btnClearTrack = new QPushButton(QStringLiteral("Трек"));
    m_btnClearTrack->setEnabled(false);
    m_btnClearTrack->setFixedHeight(30);
    r2->addWidget(m_btnClearTrack);

    m_btnDrawZone = new QPushButton(QStringLiteral("Зоны"));
    m_btnDrawZone->setCheckable(true);
    m_btnDrawZone->setFixedHeight(30);
    r2->addWidget(m_btnDrawZone);

    // Gear icon — painted
    auto makeGearIcon = [](int sz, const QColor& c) {
        QPixmap pm(sz, sz);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(c, 1.4));
        p.setBrush(Qt::NoBrush);
        double cx = sz / 2.0, cy = sz / 2.0;
        p.drawEllipse(QPointF(cx, cy), sz * 0.15, sz * 0.15);
        for (int i = 0; i < 8; ++i) {
            double a = i * M_PI / 4.0;
            p.drawLine(QPointF(cx + sz * 0.22 * std::cos(a), cy + sz * 0.22 * std::sin(a)),
                       QPointF(cx + sz * 0.40 * std::cos(a), cy + sz * 0.40 * std::sin(a)));
        }
        return QIcon(pm);
    };

    m_btnSettings = new QPushButton();
    m_btnSettings->setIcon(makeGearIcon(18, QColor(theme::TEXT_SECONDARY)));
    m_btnSettings->setIconSize(QSize(18, 18));
    m_btnSettings->setFixedSize(30, 30);
    m_btnSettings->setToolTip(QStringLiteral("Настройки"));
    r2->addWidget(m_btnSettings);

    lay->addLayout(r2);

    // Waypoint nav row — painted arrow icons
    auto makeArrowIcon = [](int sz, const QColor& c, bool left) {
        QPixmap pm(sz, sz);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (left) {
            p.drawLine(QPointF(sz * 0.6, sz * 0.2), QPointF(sz * 0.3, sz * 0.5));
            p.drawLine(QPointF(sz * 0.3, sz * 0.5), QPointF(sz * 0.6, sz * 0.8));
        } else {
            p.drawLine(QPointF(sz * 0.4, sz * 0.2), QPointF(sz * 0.7, sz * 0.5));
            p.drawLine(QPointF(sz * 0.7, sz * 0.5), QPointF(sz * 0.4, sz * 0.8));
        }
        return QIcon(pm);
    };

    auto* wr = new QHBoxLayout;
    wr->setSpacing(4);

    auto* wl = new QLabel(QStringLiteral("ТЧК"));
    wl->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(theme::TEXT_TERTIARY));
    wr->addWidget(wl);

    m_spinWaypoint = new QSpinBox;
    m_spinWaypoint->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_spinWaypoint->setMinimum(1);
    m_spinWaypoint->setMaximum(1);
    m_spinWaypoint->setEnabled(false);
    m_spinWaypoint->setFixedSize(44, 28);
    wr->addWidget(m_spinWaypoint);

    m_btnWpPrev = new QPushButton();
    m_btnWpPrev->setIcon(makeArrowIcon(16, QColor(theme::TEXT_SECONDARY), true));
    m_btnWpPrev->setIconSize(QSize(16, 16));
    m_btnWpPrev->setFixedSize(28, 28);
    m_btnWpPrev->setEnabled(false);
    wr->addWidget(m_btnWpPrev);

    m_btnWpNext = new QPushButton();
    m_btnWpNext->setIcon(makeArrowIcon(16, QColor(theme::TEXT_SECONDARY), false));
    m_btnWpNext->setIconSize(QSize(16, 16));
    m_btnWpNext->setFixedSize(28, 28);
    m_btnWpNext->setEnabled(false);
    wr->addWidget(m_btnWpNext);

    wr->addStretch();
    lay->addLayout(wr);

    return panel;
}

// ═════════════════════════════════════════════════════════════════════════
//  Connections
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::setupConnections()
{
    auto* be = m_mapWidget->backend();

    // Header buttons
    connect(m_btnConnect, &QPushButton::clicked, this, &MainWindow::onConnect);
    connect(m_btnDisconnect, &QPushButton::clicked, this, &MainWindow::onDisconnect);

    // Control buttons
    connect(m_btnSetPos, &QPushButton::clicked, this, &MainWindow::onSetPositionToggle);
    connect(m_btnSetHome, &QPushButton::clicked, this, &MainWindow::onSetHomeToggle);
    connect(m_btnLoadRoute, &QPushButton::clicked, this, &MainWindow::onLoadRoute);
    connect(m_btnClearTrack, &QPushButton::clicked, this, &MainWindow::onClearTrack);
    connect(m_btnDrawZone, &QPushButton::clicked, this, &MainWindow::onDrawZoneToggle);
    connect(m_btnSettings, &QPushButton::clicked, this, &MainWindow::onSettings);
    connect(m_btnNav, &QPushButton::clicked, this, &MainWindow::onNavToggle);
    connect(m_btnFollow, &QPushButton::clicked, this, &MainWindow::onFollowToggle);
    connect(m_btnHome, &QPushButton::clicked, this, &MainWindow::onHomeToggle);
    connect(m_btnWpPrev, &QPushButton::clicked, this, &MainWindow::onWpPrev);
    connect(m_btnWpNext, &QPushButton::clicked, this, &MainWindow::onWpNext);
    connect(m_spinWaypoint, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::onWpSelect);

    // Map backend signals
    connect(be, &MapBackend::mapClicked, this, &MainWindow::onMapClicked);
    connect(be, &MapBackend::contextMenuRequested, this, &MainWindow::onContextAddWaypoint);
    connect(be, &MapBackend::drawingFinished, this, &MainWindow::onZoneDrawingFinished);
    connect(be, &MapBackend::drawingCancelled, this, &MainWindow::onZoneDrawingCancelled);
    connect(be, &MapBackend::zoneDoubleClicked, this, &MainWindow::onZoneDoubleClicked);
    connect(be, &MapBackend::zoneContextMenuRequested, this, &MainWindow::onZoneContextMenu);
    connect(be, &MapBackend::zoneVerticesUpdated, this, &MainWindow::onZoneVerticesUpdated);
    connect(be, &MapBackend::mouseMoved, this, &MainWindow::onMapMouseMove);
    connect(be, &MapBackend::zoomChanged, this, &MainWindow::onMapZoomChanged);

    // Settlement loader
    connect(&m_settlementLoader, &SettlementLoader::tileLoaded,
            this, &MainWindow::onSettlementTileLoaded);

    // Map bounds → settlement loader (skip if settlements hidden or zoom outside visibility)
    auto shouldLoadSettlements = [this]() {
        auto* be = m_mapWidget->backend();
        int z = be->currentZoom();
        return be->showSettlements() && z >= 11 && z <= 16;
    };
    connect(be, &MapBackend::boundsChanged, this, [this, shouldLoadSettlements](double s, double w, double n, double e) {
        if (shouldLoadSettlements())
            m_settlementLoader.request(s, w, n, e);
    });

    // Render area → settlement loader (fetch tiles for render area, not just viewport)
    connect(be, &MapBackend::renderAreaChanged, this,
        [this, shouldLoadSettlements](double s, double w, double n, double e) {
            if (shouldLoadSettlements())
                m_settlementLoader.request(s, w, n, e);
        });

    // Status panel
    connect(m_statusPanel, &StatusPanel::orbitRadiusChanged, this, &MainWindow::onOrbitRadiusChanged);
    connect(m_statusPanel, &StatusPanel::targetAltitudeChanged, this, &MainWindow::onTargetAltitudeChanged);
    connect(m_statusPanel, &StatusPanel::targetAirspeedChanged, this, &MainWindow::onTargetAirspeedChanged);
    connect(m_statusPanel, &StatusPanel::windOverrideRequested, this, &MainWindow::onWindOverride);

    // MavlinkConnection signals
    connect(&m_connection, &MavlinkConnection::connectionRestored,
            this, &MainWindow::onConnectionRestored);
    connect(&m_connection, &MavlinkConnection::connectionLost,
            this, &MainWindow::onConnectionLost);

    // Autopilot signals
    connect(&m_autopilot, &AutopilotManager::engaged,
            this, &MainWindow::onAutopilotEngaged);
    connect(&m_autopilot, &AutopilotManager::disengaged,
            this, &MainWindow::onAutopilotDisengaged);
    connect(&m_autopilot, &AutopilotManager::waypointReached,
            this, &MainWindow::onWaypointReached);
    connect(&m_autopilot, &AutopilotManager::avoidanceFailed,
            this, [this](const QString& reason) {
        statusBar()->showMessage(reason, 10000);
        SPDLOG_WARN("[MainWindow] Avoidance failed: {}", reason.toStdString());
    });

    // Layer visibility → save config
    connect(be, &MapBackend::showTrackChanged, this, [this] {
        m_config.gui.show_track = m_mapWidget->backend()->showTrack();
        saveConfig(m_config);
    });
    connect(be, &MapBackend::showWaypointsChanged, this, [this] {
        m_config.gui.show_waypoints = m_mapWidget->backend()->showWaypoints();
        saveConfig(m_config);
    });
    connect(be, &MapBackend::showZonesChanged, this, [this] {
        m_config.gui.show_zones = m_mapWidget->backend()->showZones();
        saveConfig(m_config);
    });
    connect(be, &MapBackend::showSettlementsChanged, this, [this] {
        m_config.gui.show_settlements = m_mapWidget->backend()->showSettlements();
        saveConfig(m_config);
    });

    // Zones loaded deferred via QTimer::singleShot in constructor
}

void MainWindow::setupTimer()
{
    m_updateTimer = new QTimer(this);
    connect(m_updateTimer, &QTimer::timeout, this, &MainWindow::updateDisplay);
    m_updateTimer->start(100);

    // No demo data — aircraft/waypoints appear on connection or user actions
}

// ═════════════════════════════════════════════════════════════════════════
//  Apply saved config to UI
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::applyConfig()
{
    m_mapWidget->backend()->setTrackMaxLength(m_config.gui.track_length);
}

// ═════════════════════════════════════════════════════════════════════════
//  Connection
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::onConnect()
{
    statusBar()->showMessage(QStringLiteral("Подключение..."));
    if (m_connection.connectToSitl()) {
        m_proxy.start();
        m_connection.requestDataStreams(4);
    }
}

void MainWindow::onDisconnect()
{
    m_connection.disconnect();
    m_proxy.stop();
    onConnectionLost();
}

void MainWindow::enableControls(bool enabled)
{
    for (auto* btn : {m_btnNav, m_btnSetPos, m_btnSetHome,
                      m_btnLoadRoute, m_btnClearTrack, m_btnFollow, m_btnHome})
        btn->setEnabled(enabled);
}

void MainWindow::onConnectionRestored()
{
    m_btnConnect->setEnabled(false);
    m_btnDisconnect->setEnabled(true);
    enableControls(true);

    m_lblStatus->setText(QStringLiteral("ON"));
    m_lblStatus->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 10px; font-weight: 700; padding: 2px 8px; "
        "border-radius: 10px; background-color: %2;")
        .arg(theme::SUCCESS, theme::SUCCESS_BG));
    statusBar()->showMessage(QStringLiteral("Подключено — порт %1")
                              .arg(m_config.mavlink.sitl_port));

    m_btnFollow->setChecked(true);
    m_mapWidget->backend()->setFollowMode(true);
}

void MainWindow::onConnectionLost()
{
    m_btnConnect->setEnabled(true);
    m_btnDisconnect->setEnabled(false);
    enableControls(false);

    m_lblStatus->setText(QStringLiteral("OFF"));
    m_lblStatus->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 10px; font-weight: 700; padding: 2px 8px; "
        "border-radius: 10px; background-color: %2;")
        .arg(theme::ERROR_CLR, theme::ERROR_BG));
    statusBar()->showMessage(QStringLiteral("Отключено"));
}

// ═════════════════════════════════════════════════════════════════════════
//  Position / Home
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::onSetPositionToggle()
{
    m_setPositionMode = m_btnSetPos->isChecked();
    updateLeftClickMode();
    statusBar()->showMessage(m_setPositionMode
        ? QStringLiteral("Кликните на карте для коррекции позиции EKF...")
        : QStringLiteral("Режим коррекции отменён"));
}

void MainWindow::onSetHomeToggle()
{
    m_setHomeMode = m_btnSetHome->isChecked();
    updateLeftClickMode();
    statusBar()->showMessage(m_setHomeMode
        ? QStringLiteral("Кликните на карте для установки точки дома...")
        : QStringLiteral("Режим установки дома отменён"));
}

void MainWindow::updateLeftClickMode()
{
    m_mapWidget->backend()->setLeftClickMode(m_setPositionMode || m_setHomeMode);
}

void MainWindow::onMapClicked(double lat, double lon)
{
    if (m_setPositionMode)
        setCorrectionPosition(lat, lon);
    else if (m_setHomeMode)
        setHomePosition(lat, lon);
}

void MainWindow::setCorrectionPosition(double lat, double lon)
{
    m_connection.sendPositionReset(lat, lon);
    m_setPositionMode = false;
    m_btnSetPos->setChecked(false);
    updateLeftClickMode();
    m_mapWidget->backend()->setAircraftPosition(lat, lon);
    statusBar()->showMessage(QStringLiteral("Коррекция позиции: %1, %2")
                              .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6));
}

void MainWindow::setHomePosition(double lat, double lon)
{
    m_homePosition = LatLon{lat, lon};
    m_autopilot.setHomePosition(m_homePosition);
    m_mapWidget->backend()->setHome(lat, lon);
    m_setHomeMode = false;
    m_btnSetHome->setChecked(false);
    updateLeftClickMode();
    statusBar()->showMessage(QStringLiteral("Дом: %1, %2")
                              .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6));
}

// ═════════════════════════════════════════════════════════════════════════
//  Route / Waypoints
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::onLoadRoute()
{
    auto routesDir = QApplication::applicationDirPath() + "/routes";
    QDir().mkpath(routesDir);

    auto path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Загрузить маршрут"), routesDir,
        QStringLiteral("JSON файлы (*.json)"));
    if (path.isEmpty()) return;

    auto route = m_routePlanner.loadRoute(path.toStdString());
    if (route) {
        refreshMapWaypoints();
        statusBar()->showMessage(QStringLiteral("Маршрут: %1 (%2 точек)")
            .arg(QString::fromStdString(route->name))
            .arg(route->waypoints.size()));
    } else {
        QMessageBox::warning(this, QStringLiteral("Ошибка"),
                             QStringLiteral("Не удалось загрузить маршрут"));
    }
}

void MainWindow::refreshMapWaypoints()
{
    auto* route = m_routePlanner.getRoute();
    if (!route) return;

    PerfLogger localPerf{"refreshMapWaypoints", 1};  // log every call
    localPerf.begin("build_list");

    int activeIdx = m_routePlanner.activeWaypointIndex();
    QVector<QVariantMap> wpList;
    for (int i = 0; i < static_cast<int>(route->waypoints.size()); ++i) {
        auto& wp = route->waypoints[static_cast<size_t>(i)];
        QVariantMap m;
        m["lat"] = wp.lat;
        m["lon"] = wp.lon;
        m["altitude"] = wp.altitude;
        m["action"] = QString::fromStdString(wp.action);
        m["wpIndex"] = i;
        wpList.append(m);
    }
    localPerf.end("build_list");

    localPerf.begin("set_waypoints");
    m_mapWidget->backend()->setWaypoints(wpList, activeIdx);
    localPerf.end("set_waypoints");

    localPerf.begin("update_controls");
    updateWaypointControls();
    localPerf.end("update_controls");

    localPerf.tick();
    checkRouteConflicts();
}

void MainWindow::updateWaypointControls()
{
    int n = m_routePlanner.waypointCount();
    for (auto* w : {m_btnWpPrev, m_btnWpNext})
        w->setEnabled(n > 0);
    m_spinWaypoint->setEnabled(n > 0);

    if (n > 0) {
        m_spinWaypoint->blockSignals(true);
        m_spinWaypoint->setMaximum(n);
        m_spinWaypoint->setValue(m_routePlanner.activeWaypointIndex() + 1);
        m_spinWaypoint->blockSignals(false);
    }
}

void MainWindow::onWpPrev()
{
    m_routePlanner.prevWaypoint();
    refreshMapWaypoints();
}

void MainWindow::onWpNext()
{
    m_routePlanner.nextWaypoint();
    refreshMapWaypoints();
}

void MainWindow::onWpSelect(int value)
{
    m_routePlanner.setActiveWaypoint(value - 1);
    refreshMapWaypoints();
}

void MainWindow::onContextAddWaypoint(double lat, double lon)
{
    SPDLOG_INFO("[MainWindow] Context menu at ({:.6f}, {:.6f})", lat, lon);

    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral(
        "QMenu { background: %1; color: %2; border: 1px solid %3; padding: 4px; }"
        "QMenu::item { padding: 6px 20px; }"
        "QMenu::item:selected { background: %4; }")
        .arg(theme::BG_CARD, theme::TEXT_SECONDARY, theme::BORDER, theme::BG_HOVER));

    auto* actSetPos = menu.addAction(QStringLiteral("Установить позицию здесь"));
    auto* actAddWp  = menu.addAction(QStringLiteral("Добавить точку маршрута"));
    auto* actSetHome = menu.addAction(QStringLiteral("Установить дом"));
    menu.addSeparator();
    auto* actCenter = menu.addAction(QStringLiteral("Центрировать карту"));
    auto* actClearTrack = menu.addAction(QStringLiteral("Очистить трек"));
    menu.addSeparator();
    auto* actDrawZone = menu.addAction(QStringLiteral("Нарисовать запретную зону"));
    auto* actClearCache = menu.addAction(QStringLiteral("Очистить кэш нас. пунктов"));

    auto* chosen = menu.exec(QCursor::pos());
    if (!chosen) return;

    if (chosen == actSetPos) {
        setCorrectionPosition(lat, lon);
        statusBar()->showMessage(QStringLiteral("Позиция: %1, %2")
                                  .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6));
    } else if (chosen == actAddWp) {
        WaypointDialog dialog(lat, lon, &m_zoneChecker, this);
        if (dialog.exec() != QDialog::Accepted) return;

        auto r = dialog.result();

        if (!m_routePlanner.getRoute())
            m_routePlanner.createRoute("Новый маршрут");

        m_routePlanner.addWaypoint(r.lat, r.lon, r.altitude, r.radius,
                                    r.action.toStdString(), r.orbitRadius, r.orbitTurns,
                                    r.climbEnroute);

        if (m_autopilot.isEngaged()) {
            auto st = m_autopilot.status();
            if (st.isOrbiting || st.returningHome)
                m_routePlanner.setActiveWaypoint(m_routePlanner.waypointCount() - 1);
        }

        refreshMapWaypoints();
        statusBar()->showMessage(QStringLiteral("Добавлена точка: %1, %2")
                                  .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6));
    } else if (chosen == actSetHome) {
        setHomePosition(lat, lon);
        statusBar()->showMessage(QStringLiteral("Home: %1, %2")
                                  .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6));
    } else if (chosen == actCenter) {
        // Center on aircraft position (not click position)
        if (m_mapWidget->backend()->aircraftVisible()) {
            auto pos = m_mapWidget->backend()->aircraftPosition();
            m_mapWidget->backend()->centerOn(pos.latitude(), pos.longitude());
        }
    } else if (chosen == actClearTrack) {
        onClearTrack();
    } else if (chosen == actDrawZone) {
        onStartZoneDrawing();
    } else if (chosen == actClearCache) {
        m_settlementLoader.clearCache();
        m_mapWidget->backend()->clearSettlements();
        statusBar()->showMessage(QStringLiteral("Кэш населённых пунктов очищен"));
    }
}

// ═════════════════════════════════════════════════════════════════════════
//  Map Controls
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::onClearTrack()
{
    m_mapWidget->backend()->clearTrack();
    statusBar()->showMessage(QStringLiteral("Трек очищен"));
}

void MainWindow::onFollowToggle()
{
    m_mapWidget->backend()->setFollowMode(m_btnFollow->isChecked());
}

void MainWindow::onMapMouseMove(double lat, double lon)
{
    m_sbCurVal->setText(QStringLiteral("%1 , %2")
                         .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6));
}

void MainWindow::onMapZoomChanged(int zoom)
{
    m_sbZoomVal->setText(QString::number(zoom));
}

// ═════════════════════════════════════════════════════════════════════════
//  Settlements
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::onSettlementTileLoaded(const QString& /*key*/, const FeatureList& features)
{
    if (!features.empty()) {
        // Convert to CachedSettlement for ZoneChecker
        std::vector<CachedSettlement> cached;
        for (auto& f : features) {
            CachedSettlement cs;
            cs.type = f.type;
            if (f.form == 'p') {
                for (auto& c : f.coords)
                    cs.polygon.push_back({c[0], c[1]});
            }
            cached.push_back(std::move(cs));
        }
        m_zoneChecker.addSettlementFeatures(cached);
    }

    // Feed to map models
    QVariantList vl;
    for (auto& f : features) {
        QVariantMap m;
        m["F"] = (f.form == 'p') ? "p" : "n";
        m["t"] = QString::fromStdString(f.type);
        if (f.form == 'p') {
            QVariantList coords;
            for (auto& c : f.coords)
                coords.append(QVariant(QVariantList{c[0], c[1]}));  // wrap pair as single QVariant
            m["c"] = coords;
        } else {
            m["lat"] = f.lat;
            m["lon"] = f.lon;
        }
        vl.append(m);
    }
    m_mapWidget->backend()->addSettlementFeatures(vl);
}

// ═════════════════════════════════════════════════════════════════════════
//  Zones
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::loadZones()
{
    m_zoneManager.load();
    auto& zones = m_zoneManager.getAllZones();
    QVariantList zoneList;
    for (auto& z : zones) {
        QVariantMap zm;
        zm["id"] = QString::fromStdString(z.id);
        zm["name"] = QString::fromStdString(z.name);
        QVariantList pts;
        for (auto& p : z.points)
            pts.append(QVariant::fromValue(QVariantList{p.lat, p.lon}));
        zm["points"] = pts;
        zoneList.append(zm);
    }
    m_mapWidget->backend()->loadAllZones(zoneList);
}

void MainWindow::onDrawZoneToggle()
{
    if (m_btnDrawZone->isChecked())
        onStartZoneDrawing();
    else
        cancelZoneDrawing();
}

void MainWindow::onStartZoneDrawing()
{
    m_setPositionMode = false;
    m_setHomeMode = false;
    m_btnSetPos->setChecked(false);
    m_btnSetHome->setChecked(false);

    m_drawingZoneMode = true;
    m_btnDrawZone->setChecked(true);
    m_mapWidget->backend()->startDrawing();
    statusBar()->showMessage(QStringLiteral(
        "Кликайте по карте для создания зоны... (двойной клик для завершения, Esc — отмена)"));
}

void MainWindow::cancelZoneDrawing()
{
    m_drawingZoneMode = false;
    m_btnDrawZone->setChecked(false);
    m_mapWidget->backend()->cancelDrawing();
    statusBar()->showMessage(QString());
}

void MainWindow::onZoneDrawingFinished(const QString& pointsJson)
{
    m_drawingZoneMode = false;
    m_btnDrawZone->setChecked(false);

    SPDLOG_INFO("[MainWindow] onZoneDrawingFinished: json='{}' (len={})",
                 pointsJson.toStdString(), pointsJson.size());

    // Parse points from JSON string
    auto json = nlohmann::json::parse(pointsJson.toStdString(), nullptr, false);
    if (json.is_discarded() || !json.is_array()) {
        SPDLOG_ERROR("[MainWindow] Failed to parse zone points JSON!");
        return;
    }

    Polygon points;
    for (auto& p : json)
        points.push_back({p[0].get<double>(), p[1].get<double>()});

    SPDLOG_INFO("[MainWindow] Parsed {} zone vertices", points.size());
    for (size_t i = 0; i < points.size(); ++i)
        SPDLOG_INFO("[MainWindow]   vertex[{}]: lat={}, lon={}", i, points[i].lat, points[i].lon);

    ZonePropertiesDialog dialog(nullptr, this);
    if (dialog.exec() != QDialog::Accepted) {
        statusBar()->showMessage(QStringLiteral("Создание зоны отменено"));
        return;
    }

    SPDLOG_INFO("[MainWindow] Points after dialog: {} vertices", points.size());

    auto r = dialog.result();
    auto zoneId = m_zoneManager.addZone(points, r.name.toStdString(),
                                        r.description.toStdString(),
                                        r.altitude > 0 ? std::optional(r.altitude) : std::nullopt);

    auto* zone = m_zoneManager.getZone(zoneId);
    SPDLOG_INFO("[MainWindow] getZone({}) returned {}, points={}",
                 zoneId, (zone ? "OK" : "null"), zone ? zone->points.size() : 0);
    if (zone) {
        QVariantList pts;
        for (auto& p : zone->points)
            pts.append(QVariant::fromValue(QVariantList{p.lat, p.lon}));
        m_mapWidget->backend()->addZone(QString::fromStdString(zone->id), pts,
                                        QString::fromStdString(zone->name));
        m_mapWidget->backend()->setLayerVisibility("restricted", true);
    }

    statusBar()->showMessage(QStringLiteral("Запретная зона создана: %1")
                              .arg(r.name.isEmpty() ? QStringLiteral("Без названия") : r.name));
}

void MainWindow::onZoneDrawingCancelled()
{
    m_drawingZoneMode = false;
    m_btnDrawZone->setChecked(false);
    statusBar()->showMessage(QStringLiteral("Рисование зоны отменено"));
}

void MainWindow::onZoneContextMenu(const QString& zoneId, int /*sx*/, int /*sy*/)
{
    auto* zone = m_zoneManager.getZone(zoneId.toStdString());
    if (!zone) return;

    QMenu menu(this);
    auto* actEdit = menu.addAction(QStringLiteral("Редактировать вершины"));
    auto* actProps = menu.addAction(QStringLiteral("Свойства"));
    menu.addSeparator();
    auto* actDelete = menu.addAction(QStringLiteral("Удалить зону"));

    auto* action = menu.exec(QCursor::pos());

    if (action == actEdit) {
        m_mapWidget->backend()->enableZoneEditing(zoneId);
        statusBar()->showMessage(QStringLiteral(
            "Перетаскивайте вершины. ПКМ — удалить. Esc — завершить."));
    } else if (action == actProps) {
        onZoneDoubleClicked(zoneId);
    } else if (action == actDelete) {
        auto name = QString::fromStdString(zone->name);
        m_zoneManager.removeZone(zoneId.toStdString());
        m_mapWidget->backend()->removeZone(zoneId);
        statusBar()->showMessage(QStringLiteral("Запретная зона удалена: %1")
                                  .arg(name.isEmpty() ? QStringLiteral("Без названия") : name));
    }
}

void MainWindow::onZoneDoubleClicked(const QString& zoneId)
{
    auto* zone = m_zoneManager.getZone(zoneId.toStdString());
    if (!zone) return;

    m_mapWidget->backend()->disableZoneEditing();

    ZonePropertiesDialog dialog(zone, this);
    int result = dialog.exec();

    if (result == ZonePropertiesDialog::DELETE_REQUESTED) {
        auto name = QString::fromStdString(zone->name);
        m_zoneManager.removeZone(zoneId.toStdString());
        m_mapWidget->backend()->removeZone(zoneId);
        statusBar()->showMessage(QStringLiteral("Запретная зона удалена: %1")
                                  .arg(name.isEmpty() ? QStringLiteral("Без названия") : name));
    } else if (result == QDialog::Accepted) {
        auto r = dialog.result();
        nlohmann::json fields;
        fields["name"] = r.name.toStdString();
        fields["description"] = r.description.toStdString();
        if (r.altitude > 0)
            fields["altitude"] = r.altitude;
        else
            fields["altitude"] = nullptr;
        if (!r.avoidMode.isEmpty())
            fields["avoid_mode"] = r.avoidMode.toStdString();
        if (r.buffer > 0)
            fields["buffer"] = r.buffer;

        std::map<std::string, nlohmann::json> fieldMap;
        for (auto it = fields.begin(); it != fields.end(); ++it)
            fieldMap[it.key()] = it.value();
        m_zoneManager.updateZone(zoneId.toStdString(), fieldMap);

        // Refresh on map
        auto* updated = m_zoneManager.getZone(zoneId.toStdString());
        if (updated) {
            m_mapWidget->backend()->removeZone(zoneId);
            QVariantList pts;
            for (auto& p : updated->points)
                pts.append(QVariant::fromValue(QVariantList{p.lat, p.lon}));
            m_mapWidget->backend()->addZone(zoneId, pts,
                                             QString::fromStdString(updated->name));
        }
        statusBar()->showMessage(QStringLiteral("Запретная зона обновлена: %1")
                                  .arg(r.name.isEmpty() ? QStringLiteral("Без названия") : r.name));
    }
}

void MainWindow::onZoneEditingFinished()
{
    statusBar()->showMessage(QString());
}

void MainWindow::onZoneVerticesUpdated(const QString& zoneId, const QVariantList& points)
{
    Polygon poly;
    for (auto& v : points) {
        auto list = v.toList();
        if (list.size() >= 2)
            poly.push_back({list[0].toDouble(), list[1].toDouble()});
    }
    m_zoneManager.updateZonePoints(zoneId.toStdString(), poly);
}

void MainWindow::onSettings()
{
    SettingsDialog dialog(m_config, SettingsDialog::PageMap, this);
    if (dialog.exec() != QDialog::Accepted) return;

    m_config = dialog.getConfig();
    saveConfig(m_config);

    // Apply map settings
    m_mapWidget->backend()->setTrackMaxLength(m_config.gui.track_length);

    // Apply zone settings
    m_zoneChecker.updateConfig(m_config.zone_avoidance);
    m_autopilot.resetZoneAvoidance();
    checkRouteConflicts();

    statusBar()->showMessage(QStringLiteral("Настройки сохранены"));
}

// ═════════════════════════════════════════════════════════════════════════
//  Autopilot
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::onNavToggle()
{
    if (m_btnNav->isChecked()) {
        if (m_autopilot.engageNav()) {
            auto* wp = m_routePlanner.activeWaypoint();
            if (wp) {
                statusBar()->showMessage(QStringLiteral("Навигация: WPT %1/%2")
                    .arg(wp->id).arg(m_routePlanner.waypointCount()));
            }
        } else {
            m_btnNav->setChecked(false);
            if (!m_routePlanner.getRoute())
                statusBar()->showMessage(QStringLiteral("Загрузите маршрут"));
            else
                statusBar()->showMessage(QStringLiteral("Не удалось включить навигацию"));
        }
    } else {
        m_autopilot.disengage(QStringLiteral("Отключено пользователем"));
    }
}

void MainWindow::onHomeToggle()
{
    if (m_btnHome->isChecked()) {
        if (!m_homePosition) {
            m_btnHome->setChecked(false);
            statusBar()->showMessage(QStringLiteral("Установите точку Дом на карте"));
            return;
        }
        if (m_autopilot.engageHome()) {
            m_btnNav->setChecked(false);
            statusBar()->showMessage(QStringLiteral("Возврат домой"));
        } else {
            m_btnHome->setChecked(false);
        }
    } else {
        m_autopilot.disengage(QStringLiteral("Отключено пользователем"));
    }
}

void MainWindow::onAutopilotEngaged(const QString& mode)
{
    if (mode == "NAV")
        m_btnNav->setChecked(true);
}

void MainWindow::onAutopilotDisengaged(const QString& /*prevMode*/, const QString& reason)
{
    m_btnNav->setChecked(false);
    m_btnHome->setChecked(false);
    statusBar()->showMessage(reason.isEmpty()
        ? QStringLiteral("АП отключен")
        : QStringLiteral("АП откл.: %1").arg(reason));
}

void MainWindow::onWaypointReached(int /*reachedId*/, int nextId)
{
    int total = m_routePlanner.waypointCount();
    m_mapWidget->backend()->updateActiveWaypoint(nextId - 1);
    m_mapWidget->backend()->clearRouteConflicts();
    m_guiAvoidanceActive = false;
    checkRouteConflicts();
    statusBar()->showMessage(QStringLiteral("WPT reached → %1/%2").arg(nextId).arg(total));
}

void MainWindow::onOrbitRadiusChanged(int r)
{
    m_autopilot.setOrbitRadius(static_cast<double>(r));
}

void MainWindow::onTargetAltitudeChanged(int a)
{
    m_autopilot.setTargetAltitude(static_cast<double>(a));
}

void MainWindow::onTargetAirspeedChanged(int s)
{
    m_autopilot.setTargetAirspeed(static_cast<double>(s));
}

void MainWindow::onWindOverride(int direction, int speed)
{
    m_connection.sendWindOverride(direction, speed);
    statusBar()->showMessage(QStringLiteral("Ветер: %1° / %2 м/с").arg(direction).arg(speed));
}

// ═════════════════════════════════════════════════════════════════════════
//  Display Loop (100ms timer)
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::updateDisplay()
{
    if (m_guiAvoidanceResult) {
        onAvoidanceComputed();
    }

    QElapsedTimer loopTimer;
    loopTimer.start();

    if (!m_connection.isConnected()) return;

    auto* tel = m_connection.telemetry();

    { PerfScope s(m_perf, "telemetry_ui");
        m_statusPanel->updateTelemetry(*tel);
        m_lblMode->setText(tel->mode().isEmpty() ? QStringLiteral("---") : tel->mode());
    }

    auto pos = tel->position();
    if (pos.lat != 0.0 || pos.lon != 0.0) {
        { PerfScope s(m_perf, "update_aircraft");
            m_mapWidget->backend()->updateAircraft(pos.lat, pos.lon, tel->heading());
            m_sbAcVal->setText(QStringLiteral("%1 , %2")
                                .arg(pos.lat, 0, 'f', 6).arg(pos.lon, 0, 'f', 6));
        }

        { PerfScope s(m_perf, "nav_calc");
            auto apStatus = m_autopilot.status();
            if (apStatus.returningHome && m_homePosition) {
                double d = nav::haversineDistance(pos.lat, pos.lon,
                                                  m_homePosition->lat, m_homePosition->lon);
                double e = nav::etaSeconds(d, tel->groundspeed());
                m_statusPanel->updateNavigation(0, 0, d, e, 0.0);
            } else if (m_routePlanner.getRoute()) {
                if (!m_autopilot.isEngaged()) {
                    if (m_routePlanner.isWaypointReached(pos)) {
                        int old = m_routePlanner.activeWaypointIndex();
                        m_routePlanner.nextWaypoint();
                        int idx = m_routePlanner.activeWaypointIndex();
                        if (idx != old) {
                            m_mapWidget->backend()->updateActiveWaypoint(idx);
                            m_mapWidget->backend()->clearRouteConflicts();
                            m_guiAvoidanceActive = false;
                            checkRouteConflicts();
                        }
                    }
                }
                double dist = m_routePlanner.distanceToWaypoint(pos);
                double eta = m_routePlanner.etaToWaypoint(pos, tel->groundspeed());
                double xtk = m_routePlanner.crossTrackError(pos);
                int idx = m_routePlanner.activeWaypointIndex();
                int total = m_routePlanner.waypointCount();
                m_statusPanel->updateNavigation(idx + 1, total, dist, eta, xtk);
            }
        }
    }

    { PerfScope s(m_perf, "autopilot_update");
        m_autopilot.update();
    }

    if (++m_avoidanceTick >= 10) {
        m_avoidanceTick = 0;
        syncLiveAvoidanceOverlay();
        checkAircraftAvoidanceThrottled();
    }

    { PerfScope s(m_perf, "ap_status_ui");
        auto apMode = m_autopilot.mode();
        auto apStatus = m_autopilot.status();
        m_mapWidget->backend()->setReturningHome(apStatus.returningHome);
        m_statusPanel->updateAutopilot(
            apMode == AutopilotMode::MANUAL ? QStringLiteral("MANUAL") : QStringLiteral("NAV"),
            apStatus);
    }

    double loopMs = loopTimer.nsecsElapsed() / 1e6;
    m_mapWidget->backend()->tickFps(loopMs);
    m_perf.tick();
}

// ═════════════════════════════════════════════════════════════════════════
//  Avoidance
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::checkRouteConflicts()
{
    m_guiAvoidanceActive = false;
    auto* route = m_routePlanner.getRoute();

    // If no route: still check aircraft→home when returning home
    if (!route || route->waypoints.empty()) {
        if (m_autopilot.status().returningHome && m_homePosition) {
            auto acPos = getAircraftPosition();
            if (acPos) {
                auto* tel = m_connection.telemetry();
                double alt = tel ? tel->altitudeAgl() : 100.0;
                if (m_zoneChecker.segmentIntersectsObstacles(
                        acPos->first, acPos->second, m_homePosition->lat, m_homePosition->lon, alt)) {
                    SPDLOG_INFO("[MainWindow] Aircraft→home conflict (no route)");
                    QVariantList intersectionPts;
                    auto pts = m_zoneChecker.findIntersectionPoints(
                        acPos->first, acPos->second, m_homePosition->lat, m_homePosition->lon, alt);
                    for (const auto& pt : pts) {
                        QVariantMap pm;
                        pm["lat"] = pt.lat;
                        pm["lon"] = pt.lon;
                        pm["reason"] = QString::fromStdString(pt.reason);
                        intersectionPts.append(pm);
                    }
                    if (!intersectionPts.isEmpty())
                        m_mapWidget->backend()->setConflictPoints(intersectionPts);
                    computeAircraftAvoidance(acPos->first, acPos->second,
                                              m_homePosition->lat, m_homePosition->lon, alt);
                    return;
                }
            }
        }
        m_mapWidget->backend()->clearRouteConflicts();
        return;
    }

    QVariantList conflicts;
    QVariantList intersectionPts;
    bool aircraftConflict = false;
    int activeIdx = m_routePlanner.activeWaypointIndex();

    // Check aircraft → active WP
    if (activeIdx >= 0 && activeIdx < static_cast<int>(route->waypoints.size())) {
        auto acPos = getAircraftPosition();
        if (acPos) {
            auto& wp = route->waypoints[static_cast<size_t>(activeIdx)];
            if (m_zoneChecker.segmentIntersectsObstacles(
                    acPos->first, acPos->second, wp.lat, wp.lon, wp.altitude)) {
                m_lastAvoidanceCheckPos = acPos;
                aircraftConflict = true;

                // Collect intersection points for aircraft→WP segment
                auto pts = m_zoneChecker.findIntersectionPoints(
                    acPos->first, acPos->second, wp.lat, wp.lon, wp.altitude);
                for (const auto& pt : pts) {
                    QVariantMap pm;
                    pm["lat"] = pt.lat;
                    pm["lon"] = pt.lon;
                    pm["reason"] = QString::fromStdString(pt.reason);
                    intersectionPts.append(pm);
                }
            }
        }
    }

    // Check ALL WP → WP segments (not just when aircraft has no conflict)
    for (size_t i = 0; i + 1 < route->waypoints.size(); ++i) {
        auto& wp1 = route->waypoints[i];
        auto& wp2 = route->waypoints[i + 1];
        if (m_zoneChecker.segmentIntersectsObstacles(
                wp1.lat, wp1.lon, wp2.lat, wp2.lon, wp2.altitude)) {
            // Find actual intersection points for each obstacle
            auto pts = m_zoneChecker.findIntersectionPoints(
                wp1.lat, wp1.lon, wp2.lat, wp2.lon, wp2.altitude);
            QString reason;
            if (!pts.empty()) {
                reason = QString::fromStdString(pts[0].reason);
                for (const auto& pt : pts) {
                    QVariantMap pm;
                    pm["lat"] = pt.lat;
                    pm["lon"] = pt.lon;
                    pm["reason"] = QString::fromStdString(pt.reason);
                    intersectionPts.append(pm);
                }
            } else {
                auto res = m_zoneChecker.isPointRestricted(
                    (wp1.lat + wp2.lat) / 2.0, (wp1.lon + wp2.lon) / 2.0, wp2.altitude);
                reason = res.restricted
                    ? QString::fromStdString(res.reason)
                    : QStringLiteral("Маршрут пересекает запретную область");
            }
            QVariantMap c;
            c["from_idx"] = static_cast<int>(i);
            c["to_idx"] = static_cast<int>(i + 1);
            c["reason"] = reason;
            conflicts.append(c);
        }
    }

    // Check lastWP → home segment
    bool homeConflict = false;
    if (m_homePosition && !route->waypoints.empty()) {
        auto& lastWp = route->waypoints.back();
        double homeAlt = lastWp.altitude;  // use last WP altitude for home segment
        if (m_zoneChecker.segmentIntersectsObstacles(
                lastWp.lat, lastWp.lon, m_homePosition->lat, m_homePosition->lon, homeAlt)) {
            homeConflict = true;
            SPDLOG_INFO("[MainWindow] Home segment conflict: lastWP({:.5f},{:.5f}) → home({:.5f},{:.5f})",
                         lastWp.lat, lastWp.lon, m_homePosition->lat, m_homePosition->lon);
            auto pts = m_zoneChecker.findIntersectionPoints(
                lastWp.lat, lastWp.lon, m_homePosition->lat, m_homePosition->lon, homeAlt);
            QString reason;
            if (!pts.empty()) {
                reason = QString::fromStdString(pts[0].reason);
                for (const auto& pt : pts) {
                    QVariantMap pm;
                    pm["lat"] = pt.lat;
                    pm["lon"] = pt.lon;
                    pm["reason"] = QString::fromStdString(pt.reason);
                    intersectionPts.append(pm);
                }
            } else {
                reason = QStringLiteral("Маршрут до дома пересекает запретную область");
            }
            QVariantMap c;
            c["from_idx"] = static_cast<int>(route->waypoints.size() - 1);
            c["to_idx"] = -1;  // special marker: destination is home
            c["reason"] = reason;
            conflicts.append(c);
        }
    }

    // Check aircraft → home (when returning home without active WP)
    bool aircraftHomeConflict = false;
    if (!aircraftConflict && m_autopilot.status().returningHome && m_homePosition) {
        auto acPos = getAircraftPosition();
        if (acPos) {
            auto* tel = m_connection.telemetry();
            double alt = tel ? tel->altitudeAgl() : 100.0;
            if (m_zoneChecker.segmentIntersectsObstacles(
                    acPos->first, acPos->second, m_homePosition->lat, m_homePosition->lon, alt)) {
                aircraftHomeConflict = true;
                SPDLOG_INFO("[MainWindow] Aircraft→home conflict detected");
                auto pts = m_zoneChecker.findIntersectionPoints(
                    acPos->first, acPos->second, m_homePosition->lat, m_homePosition->lon, alt);
                for (const auto& pt : pts) {
                    QVariantMap pm;
                    pm["lat"] = pt.lat;
                    pm["lon"] = pt.lon;
                    pm["reason"] = QString::fromStdString(pt.reason);
                    intersectionPts.append(pm);
                }
            }
        }
    }

    // Push ALL conflict markers to map
    if (!conflicts.isEmpty() || !intersectionPts.isEmpty()) {
        m_mapWidget->backend()->setRouteConflicts(conflicts);
        if (!intersectionPts.isEmpty())
            m_mapWidget->backend()->setConflictPoints(intersectionPts);

        // Compute avoidance for the most urgent segment:
        // aircraft→WP first, aircraft→home second, then WP→WP/home conflicts
        if (aircraftConflict) {
            auto acPos = getAircraftPosition();
            auto& wp = route->waypoints[static_cast<size_t>(activeIdx)];
            computeAircraftAvoidance(acPos->first, acPos->second,
                                      wp.lat, wp.lon, wp.altitude);
        } else if (aircraftHomeConflict) {
            auto acPos = getAircraftPosition();
            auto* tel = m_connection.telemetry();
            double alt = tel ? tel->altitudeAgl() : 100.0;
            computeAircraftAvoidance(acPos->first, acPos->second,
                                      m_homePosition->lat, m_homePosition->lon, alt);
        } else if (!conflicts.isEmpty()) {
            auto first = conflicts[0].toMap();
            int toIdx = first["to_idx"].toInt();
            if (toIdx == -1) {
                // Home segment conflict — compute avoidance lastWP→home
                int fromIdx = first["from_idx"].toInt();
                auto& fromWp = route->waypoints[static_cast<size_t>(fromIdx)];
                computeAircraftAvoidance(fromWp.lat, fromWp.lon,
                                          m_homePosition->lat, m_homePosition->lon, fromWp.altitude);
            } else {
                computeWpAvoidance(first["from_idx"].toInt(), toIdx,
                                    route->waypoints[static_cast<size_t>(toIdx)].altitude,
                                    first["reason"].toString());
            }
        }
    } else {
        m_mapWidget->backend()->clearRouteConflicts();
    }
}

std::optional<std::pair<double, double>> MainWindow::getAircraftPosition() const
{
    if (m_connection.isConnected()) {
        auto pos = m_connection.telemetry()->position();
        if (pos.lat != 0.0 || pos.lon != 0.0)
            return std::make_pair(pos.lat, pos.lon);
    }
    auto acPos = m_mapWidget->backend()->aircraftPosition();
    if (acPos.isValid())
        return std::make_pair(acPos.latitude(), acPos.longitude());
    return std::nullopt;
}

void MainWindow::computeAircraftAvoidance(double acLat, double acLon,
                                           double wpLat, double wpLon, double alt)
{
    SPDLOG_DEBUG("[MainWindow] Aircraft avoidance: ({:.5f},{:.5f}) → ({:.5f},{:.5f}) alt={}",
                 acLat, acLon, wpLat, wpLon, alt);

    QtConcurrent::run([this, acLat, acLon, wpLat, wpLon, alt] {
        auto result = m_pathPlanner.planPath(acLat, acLon, wpLat, wpLon, alt);
        QMetaObject::invokeMethod(this, [this, result, acLat, acLon, wpLat, wpLon] {
            AvoidanceResult r;
            r.path = result;
            r.wp1Lat = acLat; r.wp1Lon = acLon;
            r.wp2Lat = wpLat; r.wp2Lon = wpLon;
            r.reason = QStringLiteral("Траектория пересекает запретную область");
            m_guiAvoidanceResult = r;
        }, Qt::QueuedConnection);
    });
}

void MainWindow::computeWpAvoidance(int fromIdx, int toIdx, double alt,
                                     const QString& reason)
{
    auto* route = m_routePlanner.getRoute();
    if (!route) return;

    auto& wp1 = route->waypoints[static_cast<size_t>(fromIdx)];
    auto& wp2 = route->waypoints[static_cast<size_t>(toIdx)];

    double startLat = wp1.lat, startLon = wp1.lon;

    // Use aircraft position for active segment
    int activeIdx = m_routePlanner.activeWaypointIndex();
    auto acPos = getAircraftPosition();
    if (acPos && fromIdx == activeIdx) {
        startLat = acPos->first;
        startLon = acPos->second;
    }

    double endLat = wp2.lat, endLon = wp2.lon;

    QtConcurrent::run([this, startLat, startLon, endLat, endLon, alt, reason] {
        auto result = m_pathPlanner.planPath(startLat, startLon, endLat, endLon, alt);
        QMetaObject::invokeMethod(this, [this, result, startLat, startLon, endLat, endLon, reason] {
            AvoidanceResult r;
            r.path = result;
            r.wp1Lat = startLat; r.wp1Lon = startLon;
            r.wp2Lat = endLat; r.wp2Lon = endLon;
            r.reason = reason;
            m_guiAvoidanceResult = r;
        }, Qt::QueuedConnection);
    });
}

void MainWindow::onAvoidanceComputed()
{
    if (!m_guiAvoidanceResult) return;

    auto& r = *m_guiAvoidanceResult;

    // Show direct path + conflict point
    QVariantList directPath;
    QVariantMap p1, p2;
    p1["lat"] = r.wp1Lat; p1["lon"] = r.wp1Lon;
    p2["lat"] = r.wp2Lat; p2["lon"] = r.wp2Lon;
    directPath.append(p1);
    directPath.append(p2);
    m_mapWidget->backend()->setPlannedDirectPath(directPath);

    // Find actual intersection points instead of midpoint
    QVariantList conflictPts;
    auto ipts = m_zoneChecker.findIntersectionPoints(
        r.wp1Lat, r.wp1Lon, r.wp2Lat, r.wp2Lon,
        m_routePlanner.getRoute() ? m_routePlanner.getRoute()->waypoints[
            static_cast<size_t>(std::max(0, m_routePlanner.activeWaypointIndex()))].altitude : 100.0);
    if (!ipts.empty()) {
        for (const auto& pt : ipts) {
            QVariantMap cp;
            cp["lat"] = pt.lat;
            cp["lon"] = pt.lon;
            cp["reason"] = QString::fromStdString(pt.reason);
            conflictPts.append(cp);
        }
    } else {
        QVariantMap cp;
        cp["lat"] = (r.wp1Lat + r.wp2Lat) / 2.0;
        cp["lon"] = (r.wp1Lon + r.wp2Lon) / 2.0;
        cp["reason"] = r.reason;
        conflictPts.append(cp);
    }
    m_mapWidget->backend()->setConflictPoints(conflictPts);

    // Show avoidance path
    if (r.path && !r.path->empty()) {
        QVariantList pathPts;
        QVariantMap sp;
        sp["lat"] = r.wp1Lat; sp["lon"] = r.wp1Lon;
        pathPts.append(sp);
        for (auto& pt : *r.path) {
            QVariantMap m;
            m["lat"] = pt.lat; m["lon"] = pt.lon;
            pathPts.append(m);
        }
        QVariantMap ep;
        ep["lat"] = r.wp2Lat; ep["lon"] = r.wp2Lon;
        pathPts.append(ep);
        m_mapWidget->backend()->setAvoidancePath(pathPts);
        m_guiAvoidanceActive = true;
    } else {
        m_mapWidget->backend()->setAvoidancePath({});
        m_guiAvoidanceActive = false;
    }

    m_guiAvoidanceResult.reset();
}

void MainWindow::syncLiveAvoidanceOverlay()
{
    if (!m_autopilot.isEngaged()) return;

    auto* tel = m_connection.telemetry();
    auto pos = tel->position();
    if (pos.lat == 0.0 && pos.lon == 0.0) return;

    // Returning home: show avoidance path to home
    if (m_autopilot.status().returningHome && m_homePosition) {
        auto remaining = m_autopilot.remainingAvoidanceWaypoints();
        if (remaining.empty()) {
            if (!m_guiAvoidanceActive)
                m_mapWidget->backend()->setAvoidancePath({});
            return;
        }
        m_guiAvoidanceActive = false;
        QVariantList pathPts;
        QVariantMap sp;
        sp["lat"] = pos.lat; sp["lon"] = pos.lon;
        pathPts.append(sp);
        for (auto& [lat, lon] : remaining) {
            QVariantMap m;
            m["lat"] = lat; m["lon"] = lon;
            pathPts.append(m);
        }
        QVariantMap ep;
        ep["lat"] = m_homePosition->lat; ep["lon"] = m_homePosition->lon;
        pathPts.append(ep);
        m_mapWidget->backend()->setAvoidancePath(pathPts);
        return;
    }

    // Normal route: show avoidance path to active WP
    auto* wp = m_routePlanner.activeWaypoint();
    if (!wp) {
        if (!m_guiAvoidanceActive)
            m_mapWidget->backend()->setAvoidancePath({});
        return;
    }

    auto remaining = m_autopilot.remainingAvoidanceWaypoints();
    if (remaining.empty()) {
        if (!m_guiAvoidanceActive)
            m_mapWidget->backend()->setAvoidancePath({});
        return;
    }

    m_guiAvoidanceActive = false;
    QVariantList pathPts;
    QVariantMap sp;
    sp["lat"] = pos.lat; sp["lon"] = pos.lon;
    pathPts.append(sp);
    for (auto& [lat, lon] : remaining) {
        QVariantMap m;
        m["lat"] = lat; m["lon"] = lon;
        pathPts.append(m);
    }
    QVariantMap ep;
    ep["lat"] = wp->lat; ep["lon"] = wp->lon;
    pathPts.append(ep);
    m_mapWidget->backend()->setAvoidancePath(pathPts);
}

void MainWindow::checkAircraftAvoidanceThrottled()
{
    auto* tel = m_connection.telemetry();
    auto pos = tel->position();
    if ((pos.lat == 0.0 && pos.lon == 0.0) || m_autopilot.isEngaged())
        return;
    if (m_guiAvoidanceResult) return;

    // Throttle: recheck only if moved > 500m
    if (m_lastAvoidanceCheckPos) {
        double d = nav::haversineDistance(pos.lat, pos.lon,
                                          m_lastAvoidanceCheckPos->first,
                                          m_lastAvoidanceCheckPos->second);
        if (d < 500) return;
    }

    // Aircraft → home (when returning home)
    if (m_autopilot.status().returningHome && m_homePosition) {
        m_lastAvoidanceCheckPos = std::make_pair(pos.lat, pos.lon);
        double alt = tel ? tel->altitudeAgl() : 100.0;
        if (m_zoneChecker.segmentIntersectsObstacles(
                pos.lat, pos.lon, m_homePosition->lat, m_homePosition->lon, alt)) {
            SPDLOG_DEBUG("[MainWindow] Aircraft→home avoidance check: conflict detected");
            computeAircraftAvoidance(pos.lat, pos.lon, m_homePosition->lat, m_homePosition->lon, alt);
        } else if (m_guiAvoidanceActive) {
            m_guiAvoidanceActive = false;
            m_mapWidget->backend()->setAvoidancePath({});
            m_mapWidget->backend()->setPlannedDirectPath({});
            m_mapWidget->backend()->setConflictPoints({});
        }
        return;
    }

    // Aircraft → active WP (normal route mode)
    auto* route = m_routePlanner.getRoute();
    if (!route || route->waypoints.empty()) return;

    int activeIdx = m_routePlanner.activeWaypointIndex();
    if (activeIdx < 0 || activeIdx >= static_cast<int>(route->waypoints.size()))
        return;

    m_lastAvoidanceCheckPos = std::make_pair(pos.lat, pos.lon);

    auto& wp = route->waypoints[static_cast<size_t>(activeIdx)];
    if (m_zoneChecker.segmentIntersectsObstacles(
            pos.lat, pos.lon, wp.lat, wp.lon, wp.altitude)) {
        computeAircraftAvoidance(pos.lat, pos.lon, wp.lat, wp.lon, wp.altitude);
    } else if (m_guiAvoidanceActive) {
        m_guiAvoidanceActive = false;
        m_mapWidget->backend()->setAvoidancePath({});
        m_mapWidget->backend()->setPlannedDirectPath({});
        m_mapWidget->backend()->setConflictPoints({});
    }
}

// ═════════════════════════════════════════════════════════════════════════
//  Close
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::closeEvent(QCloseEvent* event)
{
    m_updateTimer->stop();
    m_autopilot.disengage();
    m_connection.disconnect();
    m_proxy.stop();
    QMainWindow::closeEvent(event);
}

} // namespace vtol

