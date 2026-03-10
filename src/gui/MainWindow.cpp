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

    // ── Battery monitor ──
    m_batteryMonitor.configure(m_config.system);
    setupBatteryMonitor();

    // ── Mesh navigator ──
    m_meshNavigator.configure(m_config.mesh);
    connect(&m_meshNavigator, &MeshNavigator::correctedPosition, this, [this](double lat, double lon, double dist) {
        int durationMs = m_config.mesh.display_duration_sec * 1000;
        m_mapWidget->backend()->addMeshPoint(lat, lon, durationMs);
        SPDLOG_DEBUG("[Mesh] corrected pos: {:.6f}, {:.6f}, dist={:.1f}m", lat, lon, dist);
    });
    connect(&m_meshNavigator, &MeshNavigator::meshError, this, [](const QString& msg) {
        SPDLOG_WARN("[Mesh] error: {}", msg.toStdString());
    });
    if (m_config.mesh.enabled)
        m_meshNavigator.start();

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

    // ── RIGHT: unified panel ──
    m_rightPanel = new RightPanel(this);
    m_rightPanel->notificationManager()->setDefaultDurations(
        m_config.notifications.info_duration_sec,
        m_config.notifications.warning_duration_sec,
        m_config.notifications.critical_duration_sec);
    m_rightPanel->notificationManager()->setCurtailPercent(
        m_config.notifications.interrupt_curtail_pct);
    splitter->addWidget(m_rightPanel);

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

// ═════════════════════════════════════════════════════════════════════════
//  Connections
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::setupConnections()
{
    auto* be = m_mapWidget->backend();

    // ── RightPanel signals ──
    connect(m_rightPanel, &RightPanel::connectRequested,    this, &MainWindow::onConnect);
    connect(m_rightPanel, &RightPanel::disconnectRequested, this, &MainWindow::onDisconnect);
    connect(m_rightPanel, &RightPanel::navToggled,          this, &MainWindow::onNavToggle);
    connect(m_rightPanel, &RightPanel::homeToggled,         this, &MainWindow::onHomeToggle);
    connect(m_rightPanel, &RightPanel::followToggled,       this, &MainWindow::onFollowToggle);
    connect(m_rightPanel, &RightPanel::setPositionToggled,  this, &MainWindow::onSetPositionToggle);
    connect(m_rightPanel, &RightPanel::setHomeToggled,      this, &MainWindow::onSetHomeToggle);
    connect(m_rightPanel, &RightPanel::clearTrackRequested, this, &MainWindow::onClearTrack);
    connect(m_rightPanel, &RightPanel::drawZoneToggled,     this, &MainWindow::onDrawZoneToggle);
    connect(m_rightPanel, &RightPanel::settingsRequested,   this, &MainWindow::onSettings);
    connect(m_rightPanel, &RightPanel::wpPrevRequested,     this, &MainWindow::onWpPrev);
    connect(m_rightPanel, &RightPanel::wpNextRequested,     this, &MainWindow::onWpNext);
    connect(m_rightPanel, &RightPanel::wpSelected,          this, &MainWindow::onWpSelect);
    connect(m_rightPanel, &RightPanel::loadRouteRequested,  this, &MainWindow::onLoadRoute);
    connect(m_rightPanel, &RightPanel::editWaypointRequested,   this, &MainWindow::editWaypoint);
    connect(m_rightPanel, &RightPanel::deleteWaypointRequested, this, &MainWindow::deleteWaypoint);
    connect(m_rightPanel, &RightPanel::orbitRadiusChanged,  this, &MainWindow::onOrbitRadiusChanged);
    connect(m_rightPanel, &RightPanel::targetAltitudeChanged, this, &MainWindow::onTargetAltitudeChanged);
    connect(m_rightPanel, &RightPanel::targetAirspeedChanged, this, &MainWindow::onTargetAirspeedChanged);
    connect(m_rightPanel, &RightPanel::windOverrideRequested, this, &MainWindow::onWindOverride);

    connect(m_rightPanel, &RightPanel::resumeRouteRequested, this, [this]() {
        SPDLOG_INFO("[MainWindow] Resume route button clicked");
        m_autopilot.cancelOperational();
    });
    connect(m_rightPanel, &RightPanel::reorderWaypointRequested, this, [this](int from, int to) {
        m_routePlanner.moveWaypoint(from, to);
        refreshMapWaypoints();
    });
    connect(m_rightPanel, &RightPanel::centerOnWaypointRequested, this, [this](int idx) {
        auto* route = m_routePlanner.getRoute();
        if (!route || idx < 0 || idx >= static_cast<int>(route->waypoints.size())) return;
        const auto& wp = route->waypoints[idx];
        m_mapWidget->backend()->centerOn(wp.lat, wp.lon);
    });
    connect(m_rightPanel, &RightPanel::clearRouteRequested, this, [this] {
        m_routePlanner.clearWaypoints();
        refreshMapWaypoints();
        statusBar()->showMessage(QStringLiteral("Маршрут очищен"), 3000);
    });
    connect(m_rightPanel, &RightPanel::saveRouteRequested, this, [this] {
        auto* route = m_routePlanner.getRoute();
        if (!route) return;
        QString path = QFileDialog::getSaveFileName(this,
            QStringLiteral("Сохранить маршрут"), "routes/",
            QStringLiteral("JSON (*.json)"));
        if (path.isEmpty()) return;
        m_routePlanner.saveRoute(*route, path.toStdString());
        statusBar()->showMessage(QStringLiteral("Маршрут сохранён: %1").arg(path), 4000);
    });
    connect(m_rightPanel, &RightPanel::addWaypointToggled, this, [this](bool checked) {
        if (checked) {
            m_mapWidget->startWaypointPlacement();
            statusBar()->showMessage(QStringLiteral("Кликните на карте для добавления точки (Esc — отмена)"), 0);
        } else {
            m_mapWidget->cancelWaypointPlacement();
        }
    });

    // ── Map backend signals ──
    connect(be, &MapBackend::mapClicked, this, &MainWindow::onMapClicked);
    connect(be, &MapBackend::contextMenuRequested, this, &MainWindow::onContextAddWaypoint);
    connect(be, &MapBackend::waypointContextMenuRequested, this, &MainWindow::onWaypointContextMenu);
    connect(m_mapWidget, &MapWidget::waypointMoved, this, &MainWindow::onWaypointMoved);
    connect(m_mapWidget, &MapWidget::waypointPlacementRequested,
            this, &MainWindow::onWaypointPlacementRequested);
    connect(m_mapWidget, &MapWidget::waypointPlacementCancelled,
            this, &MainWindow::onWaypointPlacementCancelled);
    connect(be, &MapBackend::drawingFinished,  this, &MainWindow::onZoneDrawingFinished);
    connect(be, &MapBackend::drawingCancelled, this, &MainWindow::onZoneDrawingCancelled);
    connect(be, &MapBackend::zoneDoubleClicked,         this, &MainWindow::onZoneDoubleClicked);
    connect(be, &MapBackend::zoneContextMenuRequested,  this, &MainWindow::onZoneContextMenu);
    connect(be, &MapBackend::zoneVerticesUpdated,       this, &MainWindow::onZoneVerticesUpdated);
    connect(be, &MapBackend::mouseMoved,   this, &MainWindow::onMapMouseMove);
    connect(be, &MapBackend::zoomChanged,  this, &MainWindow::onMapZoomChanged);

    // ── Settlement loader ──
    connect(&m_settlementLoader, &SettlementLoader::tileLoaded,
            this, &MainWindow::onSettlementTileLoaded);

    auto shouldLoadSettlements = [this]() {
        auto* be = m_mapWidget->backend();
        int z = be->currentZoom();
        return be->showSettlements() && z >= 11 && z <= 16;
    };
    connect(be, &MapBackend::boundsChanged, this, [this, shouldLoadSettlements](double s, double w, double n, double e) {
        if (shouldLoadSettlements()) m_settlementLoader.request(s, w, n, e);
    });
    connect(be, &MapBackend::renderAreaChanged, this,
        [this, shouldLoadSettlements](double s, double w, double n, double e) {
            if (shouldLoadSettlements()) m_settlementLoader.request(s, w, n, e);
        });

    // ── MavlinkConnection signals ──
    connect(&m_connection, &MavlinkConnection::connectionRestored,
            this, &MainWindow::onConnectionRestored);
    connect(&m_connection, &MavlinkConnection::connectionLost,
            this, &MainWindow::onConnectionLost);
    connect(&m_connection, &MavlinkConnection::droneHomeReceived,
            this, &MainWindow::onDroneHomeReceived);
    connect(m_connection.telemetry(), &TelemetryState::armedChanged,
            this, &MainWindow::onArmedStateChanged);

    // ── Autopilot signals ──
    connect(&m_autopilot, &AutopilotManager::engaged,         this, &MainWindow::onAutopilotEngaged);
    connect(&m_autopilot, &AutopilotManager::disengaged,      this, &MainWindow::onAutopilotDisengaged);
    connect(&m_autopilot, &AutopilotManager::waypointReached, this, &MainWindow::onWaypointReached);
    connect(&m_autopilot, &AutopilotManager::homeOrbitEstablished,
            this, &MainWindow::onHomeOrbitEstablished);

    connect(&m_autopilot, &AutopilotManager::avoidanceFailed,
            this, [this](const QString& reason) {
        statusBar()->showMessage(reason, 10000);
        SPDLOG_WARN("[MainWindow] Avoidance failed: {}", reason.toStdString());

        m_rightPanel->notificationManager()->pushOrReplace(Notification{
            NotificationLevel::Critical,
            QStringLiteral("Обход невозможен"),
            reason,
            -1,
            {
                {QStringLiteral("Лететь напрямую"), [this] {
                    SPDLOG_INFO("[Notification] action: fly direct — suppressing avoidance rechecks");
                    m_autopilot.suppressAvoidanceRecheck();
                    statusBar()->showMessage(QStringLiteral("Прямой полёт — обход подавлен"));
                }},
                {QStringLiteral("Пропустить WP"), [this] {
                    SPDLOG_INFO("[Notification] action: skip waypoint");
                    auto* wp = m_routePlanner.nextWaypoint();
                    statusBar()->showMessage(wp
                        ? QStringLiteral("WP пропущена → WP%1").arg(wp->id)
                        : QStringLiteral("WP пропущена — маршрут завершён"));
                }},
            },
            {}
        }, QStringLiteral("avoidance"));
    });

    // ── Operational WP signals ──
    connect(&m_autopilot, &AutopilotManager::operationalEngaged, this, [this]() {
        bool hasRoute = m_routePlanner.getRoute()
            && !m_routePlanner.getRoute()->waypoints.empty();
        m_rightPanel->setResumeRouteVisible(true, hasRoute);
        SPDLOG_INFO("[MainWindow] Operational WP engaged — hasRoute={}", hasRoute);
        m_rightPanel->notificationManager()->pushOrReplace(Notification{
            NotificationLevel::Info,
            QStringLiteral("Оперативная точка"),
            QStringLiteral("Навигация к оперативной точке"),
            -1, {}, {}
        }, QStringLiteral("operational"));
    });

    connect(&m_autopilot, &AutopilotManager::operationalReached, this, [this]() {
        auto* opWp = m_autopilot.operationalWaypoint();
        bool isVia = opWp && opWp->mode == OperationalMode::VIA_POINT;
        SPDLOG_INFO("[MainWindow] Operational WP reached, mode={}",
                    isVia ? "VIA" : "GOTO");
        if (isVia) {
            m_rightPanel->setResumeRouteVisible(false);
            m_mapWidget->backend()->clearOperationalWp();
            refreshMapWaypoints();
            m_rightPanel->notificationManager()->pushOrReplace(Notification{
                NotificationLevel::Info,
                QStringLiteral("Промежуточная пройдена"),
                QStringLiteral("Возврат к маршруту"),
                -1, {}, {}
            }, QStringLiteral("operational"));
        } else {
            m_rightPanel->notificationManager()->pushOrReplace(Notification{
                NotificationLevel::Warning,
                QStringLiteral("Точка достигнута"),
                QStringLiteral("Нажмите «Продолжить маршрут» для возврата"),
                -1, {}, {}
            }, QStringLiteral("operational"));
        }
    });

    connect(&m_autopilot, &AutopilotManager::operationalCancelled, this, [this]() {
        m_rightPanel->setResumeRouteVisible(false);
        m_mapWidget->backend()->clearOperationalWp();
        refreshMapWaypoints();
        bool hasRoute = m_autopilot.isEngaged()
            && m_routePlanner.getRoute()
            && !m_routePlanner.getRoute()->waypoints.empty();
        SPDLOG_INFO("[MainWindow] Operational WP cancelled — hasRoute={}", hasRoute);
        m_rightPanel->notificationManager()->pushOrReplace(Notification{
            NotificationLevel::Info,
            hasRoute ? QStringLiteral("Маршрут") : QStringLiteral("Оперативная точка"),
            hasRoute ? QStringLiteral("Навигация по маршруту возобновлена")
                     : QStringLiteral("Точка отменена"),
            -1, {}, {}
        }, QStringLiteral("operational"));
    });

    // ── Layer visibility → save config ──
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
    m_rightPanel->enableControls(enabled);
}

void MainWindow::onConnectionRestored()
{
    // Reset battery decision state on new connection
    m_batt50Resolved = false;
    cancelBatt50Flow();
    stopHomeDecisionReminder();
    m_homeOrbitActive = false;

    m_rightPanel->setConnected(true);
    enableControls(true);

    statusBar()->showMessage(QStringLiteral("Подключено — порт %1")
                              .arg(m_config.mavlink.sitl_port));

    m_rightPanel->setFollowActive(true);
    m_mapWidget->backend()->setFollowMode(true);
    m_mapWidget->backend()->setZoom(13);

    m_rightPanel->notificationManager()->pushOrReplace(Notification{
        NotificationLevel::Info,
        QStringLiteral("Связь восстановлена"),
        QStringLiteral("MAVLink соединение активно"),
        -1, {}, {}
    }, QStringLiteral("connection"));
}

void MainWindow::onConnectionLost()
{
    m_rightPanel->setConnected(false);
    enableControls(false);
    statusBar()->showMessage(QStringLiteral("Отключено"));

    m_rightPanel->notificationManager()->pushOrReplace(Notification{
        NotificationLevel::Critical,
        QStringLiteral("Связь потеряна"),
        QStringLiteral("MAVLink соединение прервано"),
        -1, {}, {}
    }, QStringLiteral("connection"));
}

// ═════════════════════════════════════════════════════════════════════════
//  Position / Home
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::onSetPositionToggle(bool checked)
{
    m_setPositionMode = checked;
    updateLeftClickMode();
    statusBar()->showMessage(m_setPositionMode
        ? QStringLiteral("Кликните на карте для коррекции позиции EKF...")
        : QStringLiteral("Режим коррекции отменён"));
}

void MainWindow::onSetHomeToggle(bool checked)
{
    m_setHomeMode = checked;
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
    m_rightPanel->setPositionMode(false);
    updateLeftClickMode();
    m_mapWidget->backend()->setAircraftPosition(lat, lon);
    // Force autopilot to recompute avoidance from new position
    m_autopilot.resetZoneAvoidance();
    statusBar()->showMessage(QStringLiteral("Коррекция позиции: %1, %2")
                              .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6));
}

void MainWindow::setHomePosition(double lat, double lon)
{
    m_homePosition = LatLon{lat, lon};
    m_homeSource = HomeSource::Manual;
    m_autopilot.setHomePosition(m_homePosition);
    m_meshNavigator.setHomePosition(lat, lon);
    m_mapWidget->backend()->setHome(lat, lon);
    m_setHomeMode = false;
    m_rightPanel->setHomePlacementMode(false);
    updateLeftClickMode();
    statusBar()->showMessage(QStringLiteral("Дом: %1, %2")
                              .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6));

    // Cancel no-home flow if home was set during it
    if (m_noHomeFlowActive) {
        SPDLOG_INFO("[MainWindow] Home set during no-home flow — cancelling no-home warnings");
        m_noHomeWarningTimer->stop();
        m_noHomeFlowActive = false;
        m_noHomeWarningCount = 0;
    }
}

void MainWindow::setHomeFromDrone(double lat, double lon)
{
    m_homePosition = LatLon{lat, lon};
    m_homeSource = HomeSource::Drone;
    m_lastDroneHome = LatLon{lat, lon};
    m_autopilot.setHomePosition(m_homePosition);
    m_meshNavigator.setHomePosition(lat, lon);
    m_mapWidget->backend()->setHome(lat, lon);
    SPDLOG_INFO("[Home] Set from drone: {:.6f}, {:.6f}", lat, lon);
}

void MainWindow::onDroneHomeReceived(double lat, double lon)
{
    // Filter invalid coordinates (0,0 or very close)
    if (std::abs(lat) < 0.01 && std::abs(lon) < 0.01) return;

    SPDLOG_INFO("[Home] droneHomeReceived: {:.6f}, {:.6f} (armed={}, auto={})",
                lat, lon, m_connection.telemetry()->armed(),
                m_config.home.auto_from_drone);

    // Always store the latest position from drone (for processing on arm)
    m_pendingDroneHome = LatLon{lat, lon};

    if (!m_config.home.auto_from_drone) return;

    // If not armed yet, just store — will process in onArmedStateChanged
    if (!m_connection.telemetry()->armed()) return;

    processDroneHome(lat, lon);
}

void MainWindow::processDroneHome(double lat, double lon)
{
    // Dedup: skip if <10m from last drone home
    if (m_lastDroneHome.lat != 0.0 || m_lastDroneHome.lon != 0.0) {
        double d = nav::haversineDistance(lat, lon, m_lastDroneHome.lat, m_lastDroneHome.lon);
        if (d < 10.0) return;
    }

    const auto& mode = m_config.home.overwrite_mode;

    if (m_homeSource == HomeSource::NotSet) {
        setHomeFromDrone(lat, lon);
        if (m_config.home.notify_auto_set) {
            m_rightPanel->notificationManager()->pushOrReplace(Notification{
                NotificationLevel::Info,
                QStringLiteral("Точка дома установлена"),
                QStringLiteral("Получена с дрона: %1, %2")
                    .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6),
                -1, {}, QStringLiteral("home-point")
            }, QStringLiteral("home-point"));
        }
    } else if (m_homeSource == HomeSource::Drone) {
        setHomeFromDrone(lat, lon);
        if (m_config.home.notify_auto_set) {
            m_rightPanel->notificationManager()->pushOrReplace(Notification{
                NotificationLevel::Info,
                QStringLiteral("Точка дома обновлена"),
                QStringLiteral("Обновлена с дрона: %1, %2")
                    .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6),
                -1, {}, QStringLiteral("home-point")
            }, QStringLiteral("home-point"));
        }
    } else {
        // HomeSource::Manual — conflict
        if (mode == "force") {
            setHomeFromDrone(lat, lon);
            m_rightPanel->notificationManager()->pushOrReplace(Notification{
                NotificationLevel::Warning,
                QStringLiteral("Точка дома заменена"),
                QStringLiteral("Ручная точка заменена на дрон: %1, %2")
                    .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6),
                -1, {}, QStringLiteral("home-point")
            }, QStringLiteral("home-point"));
        } else if (mode == "keep") {
            m_lastDroneHome = LatLon{lat, lon};
            SPDLOG_INFO("[Home] Drone home received ({:.6f}, {:.6f}) — keeping manual", lat, lon);
        } else if (mode == "notify") {
            m_lastDroneHome = LatLon{lat, lon};
            m_rightPanel->notificationManager()->pushOrReplace(Notification{
                NotificationLevel::Info,
                QStringLiteral("Дрон прислал точку дома"),
                QStringLiteral("Дрон: %1, %2 — ручная точка сохранена")
                    .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6),
                -1, {}, QStringLiteral("home-point")
            }, QStringLiteral("home-point"));
        } else {
            // "ask"
            double droneLat = lat, droneLon = lon;
            m_rightPanel->notificationManager()->pushOrReplace(Notification{
                NotificationLevel::Warning,
                QStringLiteral("Дрон прислал точку дома"),
                QStringLiteral("Дрон: %1, %2\nЗаменить ручную точку?")
                    .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6),
                -1,
                {
                    {QStringLiteral("Заменить"), [this, droneLat, droneLon] {
                        setHomeFromDrone(droneLat, droneLon);
                        SPDLOG_INFO("[Home] User chose: replace manual with drone home");
                    }},
                    {QStringLiteral("Оставить"), [this, droneLat, droneLon] {
                        m_lastDroneHome = LatLon{droneLat, droneLon};
                        SPDLOG_INFO("[Home] User chose: keep manual home");
                    }},
                },
                QStringLiteral("home-point")
            }, QStringLiteral("home-point"));
        }
    }
}

void MainWindow::onArmedStateChanged()
{
    bool armed = m_connection.telemetry()->armed();
    SPDLOG_INFO("[Home] armedChanged: {} → {} (homeSource={}, pending={:.6f},{:.6f})",
                m_prevArmed, armed,
                static_cast<int>(m_homeSource),
                m_pendingDroneHome.lat, m_pendingDroneHome.lon);

    if (!m_prevArmed && armed) {
        // false → true: arming transition
        m_lastDroneHome = LatLon{};  // reset dedup

        // Process pending drone home (may have arrived before armed flag)
        if (m_config.home.auto_from_drone &&
            (std::abs(m_pendingDroneHome.lat) > 0.01 || std::abs(m_pendingDroneHome.lon) > 0.01)) {
            processDroneHome(m_pendingDroneHome.lat, m_pendingDroneHome.lon);
        } else if (m_config.home.notify_no_home && m_homeSource == HomeSource::NotSet) {
            m_rightPanel->notificationManager()->pushOrReplace(Notification{
                NotificationLevel::Warning,
                QStringLiteral("Точка дома не установлена"),
                QStringLiteral("Дрон заармлен, но точка дома не получена с дрона"),
                -1, {}, QStringLiteral("home-point")
            }, QStringLiteral("home-point"));
        }
    }

    m_prevArmed = armed;
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
        m["orbitTurns"] = wp.orbit_turns;
        m["orbitRadius"] = wp.orbit_radius;
        m["radius"] = wp.radius;
        wpList.append(m);
    }
    localPerf.end("build_list");

    localPerf.begin("set_waypoints");
    m_mapWidget->backend()->setWaypoints(wpList, activeIdx);
    localPerf.end("set_waypoints");

    localPerf.begin("update_controls");
    updateWaypointControls();
    m_rightPanel->refreshRoute(wpList, activeIdx);
    localPerf.end("update_controls");

    localPerf.tick();
    checkRouteConflicts();
}

void MainWindow::updateWaypointControls()
{
    int n = m_routePlanner.waypointCount();
    if (n > 0) {
        m_rightPanel->setWaypointRange(1, n);
        m_rightPanel->setWaypointValue(m_routePlanner.activeWaypointIndex() + 1);
    } else {
        m_rightPanel->setWaypointRange(1, 1);
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

    auto* actAddOp = menu.addAction(QStringLiteral("Оперативная точка"));
    bool canOperational = m_connection.isConnected()
        && !(m_autopilot.isEngaged() && m_autopilot.status().returningHome);
    actAddOp->setEnabled(canOperational);

    auto* actSetHome = menu.addAction(QStringLiteral("Установить дом"));
    menu.addSeparator();
    auto* actCenter = menu.addAction(QStringLiteral("Центрировать карту"));
    auto* actClearTrack = menu.addAction(QStringLiteral("Очистить трек"));
    menu.addSeparator();
    auto* actDrawZone = menu.addAction(QStringLiteral("Нарисовать запретную зону"));

    auto* chosen = menu.exec(QCursor::pos());
    if (!chosen) return;

    if (chosen == actSetPos) {
        setCorrectionPosition(lat, lon);
        statusBar()->showMessage(QStringLiteral("Позиция: %1, %2")
                                  .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6));
    } else if (chosen == actAddWp) {
        WaypointDialog dialog(lat, lon, &m_zoneChecker, false, true, this);
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
    } else if (chosen == actAddOp) {
        // VIA only makes sense when autopilot is engaged with an active route
        bool hasActiveRoute = m_autopilot.isEngaged()
            && m_routePlanner.getRoute()
            && !m_routePlanner.getRoute()->waypoints.empty();
        WaypointDialog dialog(lat, lon, &m_zoneChecker, true, hasActiveRoute, this);
        if (dialog.exec() != QDialog::Accepted) return;

        auto r = dialog.result();
        Waypoint wp;
        wp.lat = r.lat;
        wp.lon = r.lon;
        wp.altitude = r.altitude;
        wp.radius = r.radius;
        wp.action = r.action.toStdString();
        wp.orbit_radius = r.orbitRadius;
        wp.orbit_turns = r.orbitTurns;
        wp.climb_enroute = r.climbEnroute;

        auto mode = (r.operationalMode == "VIA_POINT")
            ? OperationalMode::VIA_POINT : OperationalMode::GOTO;

        if (m_autopilot.engageOperational(wp, mode)) {
            m_mapWidget->backend()->setOperationalWp(r.lat, r.lon,
                r.operationalMode.isEmpty() ? QStringLiteral("GOTO") : r.operationalMode);
            SPDLOG_INFO("[MainWindow] Operational WP placed: mode={}, lat={:.6f}, lon={:.6f}",
                        r.operationalMode.toStdString(), r.lat, r.lon);
        }
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
    }
}

// ═════════════════════════════════════════════════════════════════════════
//  Waypoint Context Menu (right-click on waypoint marker)
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::onWaypointContextMenu(int wpIndex, int sx, int sy)
{
    SPDLOG_INFO("[MainWindow] waypointContextMenu: wpIndex={} pos=({},{})", wpIndex, sx, sy);

    auto* route = m_routePlanner.getRoute();
    if (!route || wpIndex < 0 || wpIndex >= static_cast<int>(route->waypoints.size())) return;

    int total = static_cast<int>(route->waypoints.size());
    const auto& wp = route->waypoints[wpIndex];

    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral(
        "QMenu { background: %1; color: %2; border: 1px solid %3; padding: 4px; }"
        "QMenu::item { padding: 6px 20px; }"
        "QMenu::item:selected { background: %4; }"
        "QMenu::item:disabled { color: %5; }")
        .arg(theme::BG_CARD, theme::TEXT_SECONDARY, theme::BORDER, theme::BG_HOVER, theme::BORDER_LIGHT));

    QString wpLabel = QStringLiteral("Точка %1").arg(wpIndex + 1);
    auto* titleAct = menu.addAction(wpLabel);
    titleAct->setEnabled(false);
    menu.addSeparator();

    auto* actEdit   = menu.addAction(QStringLiteral("Редактировать параметры..."));
    auto* actMove   = menu.addAction(QStringLiteral("Переместить на карте"));

    // Reorder submenu
    QMenu* reorderMenu = nullptr;
    if (total > 1) {
        reorderMenu = menu.addMenu(QStringLiteral("Поменять номер"));
        reorderMenu->setStyleSheet(menu.styleSheet());
        for (int i = 0; i < total; ++i) {
            if (i == wpIndex) continue;
            auto* act = reorderMenu->addAction(QStringLiteral("→ Точка %1").arg(i + 1));
            act->setData(i);
        }
    }

    menu.addSeparator();
    auto* actUp   = menu.addAction(QStringLiteral("Переместить вверх"));
    auto* actDown = menu.addAction(QStringLiteral("Переместить вниз"));
    actUp->setEnabled(wpIndex > 0);
    actDown->setEnabled(wpIndex < total - 1);
    menu.addSeparator();
    auto* actDelete = menu.addAction(QStringLiteral("Удалить точку"));

    auto* chosen = menu.exec(QPoint(sx, sy));
    if (!chosen) return;

    if (chosen == actEdit) {
        editWaypoint(wpIndex);
    } else if (chosen == actMove) {
        // Drag mode — implemented in Task 4; for now just inform
        statusBar()->showMessage(QStringLiteral("Перетащите точку %1 в нужное место").arg(wpIndex + 1), 3000);
        m_mapWidget->startWaypointDrag(wpIndex);
    } else if (chosen == actUp) {
        SPDLOG_INFO("[MainWindow] moveWaypoint up: {} → {}", wpIndex, wpIndex - 1);
        m_routePlanner.moveWaypoint(wpIndex, wpIndex - 1);
        refreshMapWaypoints();
    } else if (chosen == actDown) {
        SPDLOG_INFO("[MainWindow] moveWaypoint down: {} → {}", wpIndex, wpIndex + 1);
        m_routePlanner.moveWaypoint(wpIndex, wpIndex + 1);
        refreshMapWaypoints();
    } else if (reorderMenu && chosen->parent() == reorderMenu) {
        int toIdx = chosen->data().toInt();
        SPDLOG_INFO("[MainWindow] moveWaypoint reorder: {} → {}", wpIndex, toIdx);
        m_routePlanner.moveWaypoint(wpIndex, toIdx);
        refreshMapWaypoints();
    } else if (chosen == actDelete) {
        deleteWaypoint(wpIndex);
    }
    Q_UNUSED(wp);
}

void MainWindow::editWaypoint(int wpIndex)
{
    auto* route = m_routePlanner.getRoute();
    if (!route || wpIndex < 0 || wpIndex >= static_cast<int>(route->waypoints.size())) return;

    const auto& existing = route->waypoints[wpIndex];
    SPDLOG_INFO("[MainWindow] editWaypoint: index={} pos=({:.5f},{:.5f}) action={}",
                wpIndex, existing.lat, existing.lon, existing.action);

    WaypointDialog dialog(existing.lat, existing.lon, &m_zoneChecker, false, false, this);
    if (dialog.exec() != QDialog::Accepted) return;

    auto r = dialog.result();
    Waypoint updated;
    updated.lat          = r.lat;
    updated.lon          = r.lon;
    updated.altitude     = r.altitude;
    updated.radius       = r.radius;
    updated.action       = r.action.toStdString();
    updated.orbit_radius = r.orbitRadius;
    updated.orbit_turns  = r.orbitTurns;
    updated.climb_enroute = r.climbEnroute;

    m_routePlanner.updateWaypoint(wpIndex, updated);
    SPDLOG_INFO("[MainWindow] editWaypoint done: index={} newPos=({:.5f},{:.5f}) action={}",
                wpIndex, r.lat, r.lon, r.action.toStdString());
    refreshMapWaypoints();
}

void MainWindow::deleteWaypoint(int wpIndex)
{
    auto* route = m_routePlanner.getRoute();
    if (!route || wpIndex < 0 || wpIndex >= static_cast<int>(route->waypoints.size())) return;

    int wpId = route->waypoints[wpIndex].id;
    SPDLOG_INFO("[MainWindow] deleteWaypoint: index={} id={}", wpIndex, wpId);

    m_routePlanner.removeWaypoint(wpIndex);
    refreshMapWaypoints();
    statusBar()->showMessage(QStringLiteral("Точка %1 удалена").arg(wpId), 3000);
}

void MainWindow::onWaypointPlacementRequested(double lat, double lon)
{
    SPDLOG_INFO("[MainWindow] placementRequested: ({:.5f},{:.5f})", lat, lon);

    // Uncheck the add button
    m_rightPanel->setAddWaypointMode(false);

    WaypointDialog dialog(lat, lon, &m_zoneChecker, false, false, this);
    if (dialog.exec() != QDialog::Accepted) return;

    auto r = dialog.result();
    if (!m_routePlanner.getRoute())
        m_routePlanner.createRoute("Новый маршрут");

    m_routePlanner.addWaypoint(r.lat, r.lon, r.altitude, r.radius,
                                r.action.toStdString(), r.orbitRadius, r.orbitTurns,
                                r.climbEnroute);
    SPDLOG_INFO("[MainWindow] addWaypoint from placement: lat={:.5f} lon={:.5f} action={}",
                r.lat, r.lon, r.action.toStdString());
    refreshMapWaypoints();
    statusBar()->showMessage(QStringLiteral("Добавлена точка: %1, %2")
                              .arg(lat, 0, 'f', 5).arg(lon, 0, 'f', 5));
}

void MainWindow::onWaypointPlacementCancelled()
{
    m_rightPanel->setAddWaypointMode(false);
    statusBar()->showMessage(QStringLiteral("Добавление точки отменено"), 2000);
}

void MainWindow::onWaypointMoved(int wpIndex, double lat, double lon)
{
    auto* route = m_routePlanner.getRoute();
    if (!route || wpIndex < 0 || wpIndex >= static_cast<int>(route->waypoints.size())) return;

    route->waypoints[wpIndex].lat = lat;
    route->waypoints[wpIndex].lon = lon;
    SPDLOG_INFO("[MainWindow] waypointMoved finalized: idx={} newPos=({:.5f},{:.5f})", wpIndex, lat, lon);
    refreshMapWaypoints();
    checkRouteConflicts();
    statusBar()->showMessage(QStringLiteral("Точка %1 перемещена").arg(wpIndex + 1), 3000);
}

// ═════════════════════════════════════════════════════════════════════════
//  Map Controls
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::onClearTrack()
{
    m_mapWidget->backend()->clearTrack();
    statusBar()->showMessage(QStringLiteral("Трек очищен"));
}

void MainWindow::onFollowToggle(bool checked)
{
    m_mapWidget->backend()->setFollowMode(checked);
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
        if (z.altitude.has_value())
            zm["altitude"] = z.altitude.value();
        if (z.avoid_mode.has_value())
            zm["avoid_mode"] = QString::fromStdString(z.avoid_mode.value());
        if (z.buffer.has_value())
            zm["buffer"] = z.buffer.value();
        zm["description"] = QString::fromStdString(z.description);
        zoneList.append(zm);
    }
    m_mapWidget->backend()->loadAllZones(zoneList);
}

void MainWindow::onDrawZoneToggle(bool checked)
{
    if (checked)
        onStartZoneDrawing();
    else
        cancelZoneDrawing();
}

void MainWindow::onStartZoneDrawing()
{
    m_setPositionMode = false;
    m_setHomeMode = false;
    m_rightPanel->setPositionMode(false);
    m_rightPanel->setHomePlacementMode(false);

    m_drawingZoneMode = true;
    m_rightPanel->setDrawZoneMode(true);
    m_mapWidget->backend()->startDrawing();
    statusBar()->showMessage(QStringLiteral(
        "Кликайте по карте для создания зоны... (двойной клик для завершения, Esc — отмена)"));
}

void MainWindow::cancelZoneDrawing()
{
    m_drawingZoneMode = false;
    m_rightPanel->setDrawZoneMode(false);
    m_mapWidget->backend()->cancelDrawing();
    statusBar()->showMessage(QString());
}

void MainWindow::onZoneDrawingFinished(const QString& pointsJson)
{
    m_drawingZoneMode = false;
    m_rightPanel->setDrawZoneMode(false);

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
    m_rightPanel->setDrawZoneMode(false);
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
        SPDLOG_INFO("[MainWindow] Context menu: deleting zone '{}' ({})", zoneId.toStdString(), name.toStdString());
        bool removed = m_zoneManager.removeZone(zoneId.toStdString());
        SPDLOG_INFO("[MainWindow] ZoneManager::removeZone returned {}", removed);
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

    SPDLOG_INFO("[MainWindow] ZonePropertiesDialog result={} (DELETE_REQUESTED={})", result, ZonePropertiesDialog::DELETE_REQUESTED);

    if (result == ZonePropertiesDialog::DELETE_REQUESTED) {
        auto name = QString::fromStdString(zone->name);
        SPDLOG_INFO("[MainWindow] Dialog: deleting zone '{}' ({})", zoneId.toStdString(), name.toStdString());
        bool removed = m_zoneManager.removeZone(zoneId.toStdString());
        SPDLOG_INFO("[MainWindow] ZoneManager::removeZone returned {}", removed);
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

    connect(&dialog, &SettingsDialog::clearSettlementsRequested, this, [this]{
        SPDLOG_INFO("[MainWindow] Clearing settlements cache");
        m_settlementLoader.clearCache();
        m_mapWidget->backend()->clearSettlements();
        m_zoneChecker.clearSettlements();
        statusBar()->showMessage(QStringLiteral("Кэш населённых пунктов очищен"));
    });

    connect(&dialog, &SettingsDialog::clearZonesRequested, this, [this]{
        SPDLOG_INFO("[MainWindow] Clearing all zones");
        m_zoneManager.clearAll();
        m_mapWidget->backend()->loadAllZones({});
        m_zoneChecker.updateConfig(m_config.zone_avoidance);
        m_autopilot.resetZoneAvoidance();
        checkRouteConflicts();
        statusBar()->showMessage(QStringLiteral("Все запретные зоны удалены"));
    });

    connect(&dialog, &SettingsDialog::clearAllRequested, this, [this]{
        SPDLOG_INFO("[MainWindow] Clearing all data");
        m_settlementLoader.clearCache();
        m_mapWidget->backend()->clearSettlements();
        m_zoneChecker.clearSettlements();
        m_zoneManager.clearAll();
        m_mapWidget->backend()->loadAllZones({});
        m_zoneChecker.updateConfig(m_config.zone_avoidance);
        m_autopilot.resetZoneAvoidance();
        checkRouteConflicts();
        statusBar()->showMessage(QStringLiteral("Все данные очищены"));
    });

    if (dialog.exec() != QDialog::Accepted) return;

    m_config = dialog.getConfig();
    saveConfig(m_config);

    // Apply map settings
    m_mapWidget->backend()->setTrackMaxLength(m_config.gui.track_length);

    // Apply zone settings
    m_zoneChecker.updateConfig(m_config.zone_avoidance);
    m_autopilot.resetZoneAvoidance();
    checkRouteConflicts();

    // Apply notification timings
    m_rightPanel->notificationManager()->setDefaultDurations(
        m_config.notifications.info_duration_sec,
        m_config.notifications.warning_duration_sec,
        m_config.notifications.critical_duration_sec);
    m_rightPanel->notificationManager()->setCurtailPercent(m_config.notifications.interrupt_curtail_pct);

    // Apply system / battery monitor settings
    m_batteryMonitor.configure(m_config.system);

    // Apply mesh navigator settings
    m_meshNavigator.stop();
    m_meshNavigator.configure(m_config.mesh);
    if (m_config.mesh.enabled)
        m_meshNavigator.start();

    statusBar()->showMessage(QStringLiteral("Настройки сохранены"));
}

// ═════════════════════════════════════════════════════════════════════════
//  Battery monitor
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::setupBatteryMonitor()
{
    auto* nm = m_rightPanel->notificationManager();

    // ── Batt-50 timer (single-shot, re-armed in onBatt50Timeout) ──
    m_batt50Timer = new QTimer(this);
    m_batt50Timer->setSingleShot(true);
    connect(m_batt50Timer, &QTimer::timeout, this, &MainWindow::onBatt50Timeout);

    // ── No-home warning timer ──
    m_noHomeWarningTimer = new QTimer(this);
    m_noHomeWarningTimer->setSingleShot(true);
    connect(m_noHomeWarningTimer, &QTimer::timeout, this, &MainWindow::onNoHomeWarningTimeout);

    // ── Home decision reminder (repeating) ──
    m_homeDecisionTimer = new QTimer(this);
    m_homeDecisionTimer->setSingleShot(false);
    connect(m_homeDecisionTimer, &QTimer::timeout, this, &MainWindow::onHomeDecisionReminderTick);

    // 25% / 50% / 75% consumed
    connect(&m_batteryMonitor, &BatteryMonitor::batteryWarning,
            this, [this, nm](const QString& title, const QString& message, const QString& tag) {
        SPDLOG_INFO("[MainWindow] battery warning: {} ({})", title.toStdString(), tag.toStdString());

        if (tag == QStringLiteral("batt-50")) {
            // 50% consumed — start decision flow (if not already active/resolved)
            if (!m_batt50FlowActive && !m_batt50Resolved) {
                startBatt50DecisionFlow();
            }
        } else if (tag == QStringLiteral("batt-75")) {
            if (m_batt50Resolved) {
                // Pilot chose "continue" at 50% — downgrade: info-only, no buttons
                nm->pushOrReplace(Notification{
                    NotificationLevel::Warning, title,
                    message + QStringLiteral("\nПилот продолжает маршрут"),
                    -1, {}, {}
                }, tag);
            } else {
                // Normal 75% behavior — suggest return home
                Notification n;
                n.level = NotificationLevel::Warning;
                n.title = title;
                n.message = message;
                if (m_config.system.action_on_limit != "notify") {
                    n.durationSec = 0;  // infinite — pilot must decide
                    n.actions = {
                        {QStringLiteral("Возврат домой"), [this]{
                            SPDLOG_WARN("[MainWindow] battery 75%: user chose RTH");
                            m_rightPanel->setHomeActive(true);
                            onHomeToggle(true);
                        }},
                        {QStringLiteral("Остаться"), []{}},
                    };
                } else {
                    n.durationSec = -1;  // auto (7s) — no action available
                }
                nm->pushOrReplace(std::move(n), tag);
            }
        } else {
            // batt-25 — just notify
            nm->pushOrReplace(Notification{
                NotificationLevel::Info, title, message, -1, {}, {}
            }, tag);
        }
    });

    // Operational zero (limit reached — need to land)
    connect(&m_batteryMonitor, &BatteryMonitor::batteryLimit,
            this, [this, nm](const QString& title, const QString& message) {
        const auto& action = m_config.system.action_on_limit;
        SPDLOG_WARN("[MainWindow] battery limit reached, action={}, resolved={}",
                    action, m_batt50Resolved);

        if (m_batt50Resolved) {
            // Pilot chose "continue" at 50% — downgrade: critical info-only, no auto-action
            nm->pushOrReplace(Notification{
                NotificationLevel::Critical, title,
                message + QStringLiteral("\nПилот продолжает маршрут"),
                -1, {}, {}  // auto 10s — informational, pilot consciously chose to continue
            }, QStringLiteral("batt-limit"));
            return;
        }

        if (action == "auto_rtl") {
            m_autopilot.activateRtl();
            nm->pushOrReplace(Notification{
                NotificationLevel::Critical, title,
                message + QStringLiteral("\nRTL активирован автоматически"),
                -1, {}, {}  // auto 10s — RTL already active, informational
            }, QStringLiteral("batt-limit"));
        } else {
            Notification n;
            n.level = NotificationLevel::Critical;
            n.title = title;
            n.message = message;
            if (action == "suggest_rth") {
                n.durationSec = 0;  // infinite — pilot must decide
                n.actions.push_back({QStringLiteral("Возврат домой"), [this]{
                    SPDLOG_WARN("[MainWindow] battery limit: user chose RTH");
                    m_rightPanel->setHomeActive(true);
                    onHomeToggle(true);
                }});
            } else {
                n.durationSec = -1;  // auto 10s — "notify" mode, no action
            }
            nm->pushOrReplace(std::move(n), QStringLiteral("batt-limit"));
        }
    });

    // Critical voltage — thrust loss risk — ALWAYS show RTL button (safety override)
    connect(&m_batteryMonitor, &BatteryMonitor::batteryCritical,
            this, [this, nm](const QString& title, const QString& message) {
        const auto& action = m_config.system.action_on_critical;
        SPDLOG_ERROR("[MainWindow] BATTERY CRITICAL, action={}", action);

        if (action == "auto_rtl") {
            m_autopilot.activateRtl();
            nm->pushOrReplace(Notification{
                NotificationLevel::Critical, title,
                message + QStringLiteral("\nRTL активирован автоматически!"),
                -1, {}, {}  // auto 10s — RTL already active, informational
            }, QStringLiteral("batt-critical"));
        } else {
            // ALWAYS show RTL button at critical — regardless of m_batt50Resolved
            Notification n;
            n.level = NotificationLevel::Critical;
            n.title = title;
            n.message = message;
            n.durationSec = 0;
            n.actions.push_back({QStringLiteral("Активировать RTL"), [this]{
                SPDLOG_ERROR("[MainWindow] battery critical: user chose RTL");
                m_autopilot.activateRtl();
            }});
            nm->pushOrReplace(std::move(n), QStringLiteral("batt-critical"));
        }
    });

    // High amperage
    connect(&m_batteryMonitor, &BatteryMonitor::amperageWarning,
            this, [nm](const QString& title, const QString& message) {
        nm->pushOrReplace(Notification{
            NotificationLevel::Warning, title, message, -1, {}, {}
        }, QStringLiteral("amp-high"));
    });

    SPDLOG_INFO("[MainWindow] battery monitor initialized");
}

// ─── Battery 50% decision flow ──────────────────────────────────────────

void MainWindow::startBatt50DecisionFlow()
{
    if (m_batt50FlowActive) return;

    m_batt50FlowActive = true;
    m_batt50NotifCount = 0;
    SPDLOG_WARN("[MainWindow] Starting batt-50 decision flow");

    // Show first notification immediately
    onBatt50Timeout();
}

void MainWindow::onBatt50Timeout()
{
    if (!m_batt50FlowActive) return;

    m_batt50NotifCount++;
    int maxNotif = m_config.system.batt50_max_notifications;

    SPDLOG_WARN("[MainWindow] batt-50 notification {}/{}", m_batt50NotifCount, maxNotif);

    if (m_batt50NotifCount > maxNotif) {
        // All notifications exhausted — auto-action
        SPDLOG_WARN("[MainWindow] batt-50: max notifications reached — executing auto-action");
        m_batt50FlowActive = false;
        executeBatt50AutoAction();
        return;
    }

    // Show decision notification
    auto* nm = m_rightPanel->notificationManager();
    Notification n;
    n.level = NotificationLevel::Warning;
    n.title = QStringLiteral("50%% заряда израсходовано (%1/%2)")
                  .arg(m_batt50NotifCount).arg(maxNotif);
    n.message = QStringLiteral("Рекомендуется вернуться домой. Продолжить маршрут?");
    n.durationSec = m_config.system.batt50_timeout_sec;  // countdown matches timer
    n.actions = {
        {QStringLiteral("Продолжить маршрут"), [this]{
            SPDLOG_INFO("[MainWindow] batt-50: pilot chose CONTINUE");
            m_batt50Resolved = true;
            cancelBatt50Flow();
        }},
        {QStringLiteral("Вернуться домой"), [this]{
            SPDLOG_WARN("[MainWindow] batt-50: pilot chose RETURN HOME");
            cancelBatt50Flow();
            executeBatt50AutoAction();
        }},
    };
    nm->pushOrReplace(std::move(n), QStringLiteral("batt-50-decision"));

    // Schedule next timeout
    m_batt50Timer->start(m_config.system.batt50_timeout_sec * 1000);
}

void MainWindow::executeBatt50AutoAction()
{
    SPDLOG_WARN("[MainWindow] executeBatt50AutoAction: home={}",
                m_homePosition.has_value());

    if (m_homePosition.has_value()) {
        // Home exists — engage home mode
        m_rightPanel->setHomeActive(true);
        onHomeToggle(true);
    } else {
        // No home — fly to WP[0]
        flyToWp0WithoutHome();
    }
}

void MainWindow::flyToWp0WithoutHome()
{
    auto* route = m_routePlanner.getRoute();
    if (!route || route->waypoints.empty()) {
        SPDLOG_ERROR("[MainWindow] flyToWp0WithoutHome: no route/waypoints!");
        m_rightPanel->notificationManager()->pushOrReplace(Notification{
            NotificationLevel::Critical,
            QStringLiteral("Нет маршрута!"),
            QStringLiteral("Нет точки дома и нет маршрута — невозможно вернуться"),
            0, {{QStringLiteral("Принял"), []{}}}, {}
        }, QStringLiteral("no-home-no-route"));
        return;
    }

    SPDLOG_WARN("[MainWindow] No home — flying to WP[0] and starting no-home flow");
    m_routePlanner.setActiveWaypoint(0);
    if (!m_autopilot.engageNav()) {
        SPDLOG_ERROR("[MainWindow] Failed to engage nav for WP[0]");
    }

    // Start no-home warning flow
    m_noHomeFlowActive = true;
    m_noHomeWarningCount = 0;
    onNoHomeWarningTimeout();  // first warning immediately
}

void MainWindow::onNoHomeWarningTimeout()
{
    if (!m_noHomeFlowActive) return;

    m_noHomeWarningCount++;
    int maxWarnings = m_config.system.no_home_max_warnings;

    SPDLOG_WARN("[MainWindow] no-home warning {}/{}", m_noHomeWarningCount, maxWarnings);

    auto* nm = m_rightPanel->notificationManager();

    if (m_noHomeWarningCount > maxWarnings) {
        // All warnings exhausted — orbit at WP[0]
        m_noHomeFlowActive = false;
        SPDLOG_WARN("[MainWindow] no-home: max warnings reached — orbiting at WP[0]");
        nm->pushOrReplace(Notification{
            NotificationLevel::Critical,
            QStringLiteral("Нет точки дома!"),
            QStringLiteral("Кружение над первой точкой маршрута. Установите дом!"),
            0, {{QStringLiteral("Принял"), []{}}}, {}
        }, QStringLiteral("no-home"));
        return;
    }

    nm->pushOrReplace(Notification{
        NotificationLevel::Warning,
        QStringLiteral("Точка дома не задана! (%1/%2)")
            .arg(m_noHomeWarningCount).arg(maxWarnings),
        QStringLiteral("Летим к WP1. Установите точку дома!"),
        m_config.system.batt50_timeout_sec,
        {{QStringLiteral("Принял"), []{}}},
        {}
    }, QStringLiteral("no-home"));

    // Schedule next warning
    m_noHomeWarningTimer->start(m_config.system.batt50_timeout_sec * 1000);
}

void MainWindow::cancelBatt50Flow()
{
    if (m_batt50Timer) m_batt50Timer->stop();
    if (m_noHomeWarningTimer) m_noHomeWarningTimer->stop();
    m_batt50FlowActive = false;
    m_batt50NotifCount = 0;
    m_noHomeFlowActive = false;
    m_noHomeWarningCount = 0;
}

// ─── Home decision reminder ─────────────────────────────────────────────

void MainWindow::startHomeDecisionReminder()
{
    int sec = m_config.system.home_decision_reminder_sec;
    SPDLOG_INFO("[MainWindow] Starting home decision reminder every {}s", sec);
    m_homeDecisionTimer->start(sec * 1000);
}

void MainWindow::stopHomeDecisionReminder()
{
    if (m_homeDecisionTimer && m_homeDecisionTimer->isActive()) {
        m_homeDecisionTimer->stop();
        SPDLOG_INFO("[MainWindow] Home decision reminder stopped");
    }
}

void MainWindow::onHomeDecisionReminderTick()
{
    if (!m_homeOrbitActive) {
        stopHomeDecisionReminder();
        return;
    }

    auto* tel = m_connection.telemetry();
    double voltage = tel->batteryVoltage();
    double urgencyThreshold = m_config.system.v_operational_zero + 2.0;
    bool gpsOk = tel->gpsFix() >= 2 && tel->satellites() > 10;

    NotificationLevel level = (voltage <= urgencyThreshold)
        ? NotificationLevel::Critical
        : NotificationLevel::Warning;

    QString msg = QStringLiteral("Самолёт кружит над домом. Напряжение: %1В, GPS: %2 спутников.")
                      .arg(voltage, 0, 'f', 1)
                      .arg(tel->satellites());

    if (voltage <= urgencyThreshold) {
        msg += QStringLiteral("\nНапряжение приближается к лимиту — решение необходимо!");
    }

    Notification n;
    n.level = level;
    n.title = QStringLiteral("Ожидание решения");
    n.message = msg;
    n.durationSec = 0;
    if (gpsOk) {
        n.actions = {
            {QStringLiteral("Включить RTL"), [this] {
                SPDLOG_INFO("[MainWindow] Home reminder: user chose RTL");
                stopHomeDecisionReminder();
                m_homeOrbitActive = false;
                m_autopilot.activateRtl();
            }},
            {QStringLiteral("Продолжить ожидание"), []{}},
        };
    } else {
        n.actions = {
            {QStringLiteral("Принял"), []{}},
        };
    }

    m_rightPanel->notificationManager()->pushOrReplace(std::move(n), QStringLiteral("home-orbit"));
    SPDLOG_INFO("[MainWindow] Home decision reminder: V={:.1f}, level={}",
                voltage, level == NotificationLevel::Critical ? "CRITICAL" : "WARNING");
}

// ═════════════════════════════════════════════════════════════════════════
//  Autopilot
// ═════════════════════════════════════════════════════════════════════════

void MainWindow::onNavToggle(bool checked)
{
    if (checked) {
        if (m_autopilot.engageNav()) {
            auto* wp = m_routePlanner.activeWaypoint();
            if (wp) {
                statusBar()->showMessage(QStringLiteral("Навигация: WPT %1/%2")
                    .arg(wp->id).arg(m_routePlanner.waypointCount()));
            }
        } else {
            m_rightPanel->setNavActive(false);
            if (!m_routePlanner.getRoute())
                statusBar()->showMessage(QStringLiteral("Загрузите маршрут"));
            else
                statusBar()->showMessage(QStringLiteral("Не удалось включить навигацию"));
        }
    } else {
        m_autopilot.disengage(QStringLiteral("Отключено пользователем"));
    }
}

void MainWindow::onHomeToggle(bool checked)
{
    if (checked) {
        if (!m_homePosition) {
            m_rightPanel->setHomeActive(false);
            statusBar()->showMessage(QStringLiteral("Установите точку Дом на карте"));
            return;
        }
        if (m_autopilot.engageHome()) {
            m_rightPanel->setNavActive(false);
            statusBar()->showMessage(QStringLiteral("Возврат домой"));
        } else {
            m_rightPanel->setHomeActive(false);
        }
    } else {
        m_autopilot.disengage(QStringLiteral("Отключено пользователем"));
    }
}

void MainWindow::onAutopilotEngaged(const QString& mode)
{
    if (mode == "NAV")
        m_rightPanel->setNavActive(true);
}

void MainWindow::onAutopilotDisengaged(const QString& /*prevMode*/, const QString& reason)
{
    cancelBatt50Flow();
    stopHomeDecisionReminder();
    m_homeOrbitActive = false;

    m_rightPanel->setNavActive(false);
    m_rightPanel->setHomeActive(false);
    m_rightPanel->setResumeRouteVisible(false);
    m_mapWidget->backend()->clearOperationalWp();
    statusBar()->showMessage(reason.isEmpty()
        ? QStringLiteral("АП отключен")
        : QStringLiteral("АП откл.: %1").arg(reason));

    if (!reason.isEmpty() && reason != QStringLiteral("Отключено пользователем")) {
        auto level = NotificationLevel::Warning;
        auto title = QStringLiteral("Автопилот отключён");
        if (reason.contains(QStringLiteral("потерян"), Qt::CaseInsensitive)) {
            level = NotificationLevel::Critical;
            title = QStringLiteral("Связь потеряна");
        } else if (reason.contains(QStringLiteral("пилот"), Qt::CaseInsensitive)) {
            title = QStringLiteral("Ручной режим");
        }
        m_rightPanel->notificationManager()->push(Notification{level, title, reason, -1, {}, {}});
    }
}

void MainWindow::onWaypointReached(int reachedId, int nextId)
{
    int total = m_routePlanner.waypointCount();
    m_mapWidget->backend()->updateActiveWaypoint(nextId - 1);
    m_mapWidget->backend()->clearRouteConflicts();
    m_guiAvoidanceActive = false;
    checkRouteConflicts();
    statusBar()->showMessage(QStringLiteral("WPT reached → %1/%2").arg(nextId).arg(total));

    m_rightPanel->notificationManager()->push(Notification{
        NotificationLevel::Info,
        QStringLiteral("Точка достигнута"),
        QStringLiteral("WP%1 пройдена, следующая WP%2").arg(reachedId).arg(nextId),
        -1, {}, {}
    });
}

void MainWindow::onHomeOrbitEstablished()
{
    auto* tel = m_connection.telemetry();
    bool gpsOk = tel->gpsFix() >= 2 && tel->satellites() > 10;
    const auto& orbitAction = m_config.system.home_orbit_action;

    SPDLOG_INFO("[MainWindow] HOME ORBIT: gps_ok={}, fix={}, sats={}, action={}",
                gpsOk, tel->gpsFix(), tel->satellites(), orbitAction);

    m_homeOrbitActive = true;

    // ── Auto-RTL modes ──
    if (orbitAction == "auto_rtl") {
        SPDLOG_INFO("[MainWindow] auto_rtl — activating RTL immediately");
        m_autopilot.activateRtl();
        m_rightPanel->notificationManager()->pushOrReplace(Notification{
            NotificationLevel::Warning,
            QStringLiteral("Самолёт над домом"),
            QStringLiteral("RTL активирован автоматически"),
            -1, {}, {}
        }, QStringLiteral("home-orbit"));
        statusBar()->showMessage(QStringLiteral("Дом — авто-RTL"));
        return;
    }

    if (orbitAction == "auto_rtl_gps") {
        if (gpsOk) {
            SPDLOG_INFO("[MainWindow] auto_rtl_gps — GPS OK, activating RTL");
            m_autopilot.activateRtl();
            m_rightPanel->notificationManager()->pushOrReplace(Notification{
                NotificationLevel::Warning,
                QStringLiteral("Самолёт над домом"),
                QStringLiteral("GPS: %1 спутников — RTL активирован автоматически")
                    .arg(tel->satellites()),
                -1, {}, {}
            }, QStringLiteral("home-orbit"));
            statusBar()->showMessage(QStringLiteral("Дом — авто-RTL (GPS ОК)"));
            return;
        }
        SPDLOG_WARN("[MainWindow] auto_rtl_gps — GPS insufficient (sats={}), fallback to wait",
                    tel->satellites());
        // Fall through to "wait" behavior
    }

    // ── Wait mode (default) — operator decides ──
    Notification n;
    n.level = NotificationLevel::Warning;
    n.title = QStringLiteral("Самолёт над домом");
    n.tag = QStringLiteral("home-orbit");
    n.durationSec = 0;

    if (gpsOk) {
        n.message = QStringLiteral(
            "Самолёт встал в круг над точкой дома на высоте %1м.\n"
            "GPS: %2 спутников. Ожидаем решение оператора.")
            .arg(static_cast<int>(tel->altitudeAgl()))
            .arg(tel->satellites());
        n.actions = {
            {QStringLiteral("Включить RTL"), [this] {
                SPDLOG_INFO("[MainWindow] USER ACTIVATED RTL from notification");
                stopHomeDecisionReminder();
                m_homeOrbitActive = false;
                m_autopilot.activateRtl();
            }},
            {QStringLiteral("Продолжить ожидание"), [this] {
                SPDLOG_INFO("[MainWindow] USER CHOSE TO CONTINUE ORBITING");
            }},
        };
    } else {
        n.message = QStringLiteral(
            "Самолёт встал в круг над точкой дома на высоте %1м.\n"
            "GPS: %2 спутников — недостаточно для RTL.\n"
            "Ожидаем решение оператора.")
            .arg(static_cast<int>(tel->altitudeAgl()))
            .arg(tel->satellites());
        n.actions = {
            {QStringLiteral("Принял"), [this] {
                SPDLOG_INFO("[MainWindow] USER ACKNOWLEDGED home orbit (no RTL)");
            }},
        };
    }

    m_rightPanel->notificationManager()->pushOrReplace(std::move(n), QStringLiteral("home-orbit"));
    statusBar()->showMessage(QStringLiteral("Самолёт в круге над домом — ожидаем решение"));

    // Start periodic reminder
    startHomeDecisionReminder();
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
        m_rightPanel->updateTelemetry(*tel);
        m_rightPanel->setFlightMode(tel->mode().isEmpty() ? QStringLiteral("---") : tel->mode());

        // Feed battery monitor
        m_batteryMonitor.setAutopilotEngaged(m_autopilot.isEngaged());
        m_batteryMonitor.update(tel->batteryVoltage(), tel->batteryCurrent());
    }

    auto pos = tel->position();
    if (pos.lat != 0.0 || pos.lon != 0.0) {
        { PerfScope s(m_perf, "update_aircraft");
            m_mapWidget->backend()->updateAircraft(pos.lat, pos.lon, tel->heading());
            m_meshNavigator.setAircraftPosition(pos.lat, pos.lon);
            m_sbAcVal->setText(QStringLiteral("%1 , %2")
                                .arg(pos.lat, 0, 'f', 6).arg(pos.lon, 0, 'f', 6));
        }

        { PerfScope s(m_perf, "nav_calc");
            auto apStatus = m_autopilot.status();
            if (apStatus.returningHome && m_homePosition) {
                double d = nav::haversineDistance(pos.lat, pos.lon,
                                                  m_homePosition->lat, m_homePosition->lon);
                double e = nav::etaSeconds(d, tel->groundspeed());
                m_rightPanel->updateNavigation(0, 0, d, e, 0.0);
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
                m_rightPanel->updateNavigation(idx + 1, total, dist, eta, xtk);
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
        m_rightPanel->updateAutopilot(
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

