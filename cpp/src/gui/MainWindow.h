#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QStatusBar>
#include <QPushButton>
#include <QSpinBox>
#include <QLabel>
#include <QTimer>
#include <QFrame>
#include <optional>
#include <tuple>
#include <vector>

#include "core/Config.h"
#include "core/Types.h"
#include "mavlink/MavlinkConnection.h"
#include "mavlink/MavlinkProxy.h"
#include "navigation/RoutePlanner.h"
#include "navigation/ZoneManager.h"
#include "navigation/ZoneChecker.h"
#include "navigation/PathPlanner.h"
#include "autopilot/AutopilotManager.h"
#include "gui/MapWidget.h"
#include "gui/StatusPanel.h"
#include "gui/TileProxy.h"
#include "gui/SettlementLoader.h"
#include "gui/PerfLogger.h"

namespace vtol {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(AppConfig config, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    // ── UI construction ──
    void setupUi();
    QWidget* buildPanelHeader();
    QWidget* buildControls();
    void setupConnections();
    void setupTimer();
    void enableControls(bool enabled);

    // ── Connection ──
    void onConnect();
    void onDisconnect();
    void onConnectionRestored();
    void onConnectionLost();

    // ── Position / Home ──
    void onSetPositionToggle();
    void onSetHomeToggle();
    void updateLeftClickMode();
    void onMapClicked(double lat, double lon);
    void setCorrectionPosition(double lat, double lon);
    void setHomePosition(double lat, double lon);

    // ── Route / Waypoints ──
    void onLoadRoute();
    void refreshMapWaypoints();
    void updateWaypointControls();
    void onWpPrev();
    void onWpNext();
    void onWpSelect(int value);
    void onContextAddWaypoint(double lat, double lon);

    // ── Map controls ──
    void onClearTrack();
    void onFollowToggle();
    void onMapMouseMove(double lat, double lon);
    void onMapZoomChanged(int zoom);

    // ── Zones ──
    void loadZones();
    void onDrawZoneToggle();
    void onStartZoneDrawing();
    void cancelZoneDrawing();
    void onZoneDrawingFinished(const QString& pointsJson);
    void onZoneDrawingCancelled();
    void onZoneContextMenu(const QString& zoneId, int sx, int sy);
    void onZoneDoubleClicked(const QString& zoneId);
    void onZoneEditingFinished();
    void onZoneVerticesUpdated(const QString& zoneId, const QVariantList& points);
    void onZoneSettings();
    void onSettings();

    // ── Settlements ──
    void onSettlementTileLoaded(const QString& key, const FeatureList& features);

    // ── Autopilot ──
    void onNavToggle();
    void onHomeToggle();
    void onAutopilotEngaged(const QString& mode);
    void onAutopilotDisengaged(const QString& prevMode, const QString& reason);
    void onWaypointReached(int reachedId, int nextId);
    void onOrbitRadiusChanged(int r);
    void onTargetAltitudeChanged(int a);
    void onTargetAirspeedChanged(int s);
    void onWindOverride(int direction, int speed);

    // ── Display loop ──
    void updateDisplay();
    void checkRouteConflicts();
    std::optional<std::pair<double, double>> getAircraftPosition() const;
    void computeAircraftAvoidance(double acLat, double acLon,
                                   double wpLat, double wpLon, double alt);
    void computeWpAvoidance(int fromIdx, int toIdx, double alt,
                            const QString& reason);
    void onAvoidanceComputed();
    void syncLiveAvoidanceOverlay();
    void checkAircraftAvoidanceThrottled();

    // ── Config ──
    AppConfig m_config;

    // ── Core subsystems ──
    MavlinkConnection m_connection;
    MavlinkProxy m_proxy;
    RoutePlanner m_routePlanner;
    ZoneManager m_zoneManager;
    ZoneChecker m_zoneChecker;
    PathPlanner m_pathPlanner;
    AutopilotManager m_autopilot;

    // ── GUI components ──
    MapWidget* m_mapWidget = nullptr;
    StatusPanel* m_statusPanel = nullptr;
    TileProxy m_tileProxy;
    SettlementLoader m_settlementLoader;
    QTimer* m_updateTimer = nullptr;

    // ── Header widgets ──
    QPushButton* m_btnConnect = nullptr;
    QPushButton* m_btnDisconnect = nullptr;
    QLabel* m_lblMode = nullptr;
    QLabel* m_lblStatus = nullptr;

    // ── Control buttons ──
    QPushButton* m_btnNav = nullptr;
    QPushButton* m_btnSetPos = nullptr;
    QPushButton* m_btnSetHome = nullptr;
    QPushButton* m_btnLoadRoute = nullptr;
    QPushButton* m_btnClearTrack = nullptr;
    QPushButton* m_btnFollow = nullptr;
    QPushButton* m_btnHome = nullptr;
    QPushButton* m_btnDrawZone = nullptr;
    QPushButton* m_btnZoneSettings = nullptr;
    QPushButton* m_btnSettings = nullptr;
    QPushButton* m_btnWpPrev = nullptr;
    QPushButton* m_btnWpNext = nullptr;
    QSpinBox* m_spinWaypoint = nullptr;

    // ── Status bar widgets ──
    QLabel* m_sbZoomVal = nullptr;
    QLabel* m_sbAcVal = nullptr;
    QLabel* m_sbCurVal = nullptr;

    // ── State flags ──
    bool m_setPositionMode = false;
    bool m_setHomeMode = false;
    bool m_drawingZoneMode = false;
    std::optional<LatLon> m_homePosition;

    // ── Performance logger ──
    PerfLogger m_perf{"updateDisplay", 50};  // log every 50 ticks (~5 sec)

    // ── Avoidance state ──
    struct AvoidanceResult {
        std::optional<std::vector<LatLon>> path;
        double wp1Lat, wp1Lon;
        double wp2Lat, wp2Lon;
        QString reason;
    };
    std::optional<AvoidanceResult> m_guiAvoidanceResult;
    bool m_guiAvoidanceActive = false;
    std::optional<std::pair<double, double>> m_lastAvoidanceCheckPos;
    int m_avoidanceTick = 0;
};

} // namespace vtol
