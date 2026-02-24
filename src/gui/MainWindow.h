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
#include "core/BatteryMonitor.h"
#include "mavlink/MavlinkConnection.h"
#include "mavlink/MavlinkProxy.h"
#include "navigation/RoutePlanner.h"
#include "navigation/ZoneManager.h"
#include "navigation/ZoneChecker.h"
#include "navigation/PathPlanner.h"
#include "autopilot/AutopilotManager.h"
#include "gui/MapWidget.h"
#include "gui/RightPanel.h"
#include "gui/TileProxy.h"
#include "gui/SettlementLoader.h"
#include "gui/PerfLogger.h"

namespace vtol {

enum class HomeSource { NotSet, Manual, Drone };

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
    void setupConnections();
    void setupTimer();
    void applyConfig();
    void enableControls(bool enabled);

    // ── Connection ──
    void onConnect();
    void onDisconnect();
    void onConnectionRestored();
    void onConnectionLost();

    // ── Position / Home ──
    void onSetPositionToggle(bool checked);
    void onSetHomeToggle(bool checked);
    void updateLeftClickMode();
    void onMapClicked(double lat, double lon);
    void setCorrectionPosition(double lat, double lon);
    void setHomePosition(double lat, double lon);
    void onDroneHomeReceived(double lat, double lon);
    void onArmedStateChanged();
    void setHomeFromDrone(double lat, double lon);
    void processDroneHome(double lat, double lon);

    // ── Route / Waypoints ──
    void onLoadRoute();
    void refreshMapWaypoints();
    void updateWaypointControls();
    void onWpPrev();
    void onWpNext();
    void onWpSelect(int value);
    void onContextAddWaypoint(double lat, double lon);
    void onWaypointContextMenu(int wpIndex, int sx, int sy);
    void editWaypoint(int wpIndex);
    void deleteWaypoint(int wpIndex);
    void onWaypointMoved(int wpIndex, double lat, double lon);
    void onWaypointPlacementRequested(double lat, double lon);
    void onWaypointPlacementCancelled();

    // ── Map controls ──
    void onClearTrack();
    void onFollowToggle(bool checked);
    void onMapMouseMove(double lat, double lon);
    void onMapZoomChanged(int zoom);

    // ── Zones ──
    void loadZones();
    void onDrawZoneToggle(bool checked);
    void onStartZoneDrawing();
    void cancelZoneDrawing();
    void onZoneDrawingFinished(const QString& pointsJson);
    void onZoneDrawingCancelled();
    void onZoneContextMenu(const QString& zoneId, int sx, int sy);
    void onZoneDoubleClicked(const QString& zoneId);
    void onZoneEditingFinished();
    void onZoneVerticesUpdated(const QString& zoneId, const QVariantList& points);
    void onSettings();

    // ── Settlements ──
    void onSettlementTileLoaded(const QString& key, const FeatureList& features);

    // ── Autopilot ──
    void onNavToggle(bool checked);
    void onHomeToggle(bool checked);
    void onAutopilotEngaged(const QString& mode);
    void onAutopilotDisengaged(const QString& prevMode, const QString& reason);
    void onWaypointReached(int reachedId, int nextId);
    void onHomeOrbitEstablished();
    void onOrbitRadiusChanged(int r);
    void onTargetAltitudeChanged(int a);
    void onTargetAirspeedChanged(int s);
    void onWindOverride(int direction, int speed);

    // ── Battery monitor ──
    void setupBatteryMonitor();

    // ── Battery 50% decision flow ──
    void startBatt50DecisionFlow();
    void onBatt50Timeout();
    void executeBatt50AutoAction();
    void flyToWp0WithoutHome();
    void onNoHomeWarningTimeout();
    void cancelBatt50Flow();

    // ── Home decision reminder ──
    void startHomeDecisionReminder();
    void stopHomeDecisionReminder();
    void onHomeDecisionReminderTick();

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
    BatteryMonitor   m_batteryMonitor;

    // ── GUI components ──
    MapWidget*   m_mapWidget   = nullptr;
    RightPanel*  m_rightPanel  = nullptr;
    TileProxy    m_tileProxy;
    SettlementLoader m_settlementLoader;
    QTimer*      m_updateTimer = nullptr;

    // ── Status bar widgets ──
    QLabel* m_sbZoomVal = nullptr;
    QLabel* m_sbAcVal = nullptr;
    QLabel* m_sbCurVal = nullptr;

    // ── State flags ──
    bool m_setPositionMode = false;
    bool m_setHomeMode = false;
    bool m_drawingZoneMode = false;
    std::optional<LatLon> m_homePosition;
    HomeSource m_homeSource = HomeSource::NotSet;
    LatLon m_lastDroneHome;       // дедупликация (<10м)
    LatLon m_pendingDroneHome;    // последняя позиция от subscribe_home (до арма)
    bool m_prevArmed = false;

    // ── Battery 50% decision flow state ──
    QTimer* m_batt50Timer         = nullptr;
    int     m_batt50NotifCount    = 0;
    bool    m_batt50FlowActive    = false;
    bool    m_batt50Resolved      = false;   // pilot chose "continue" — downgrade subsequent warnings

    // ── No-home flow state ──
    QTimer* m_noHomeWarningTimer  = nullptr;
    int     m_noHomeWarningCount  = 0;
    bool    m_noHomeFlowActive    = false;

    // ── Home decision reminder ──
    QTimer* m_homeDecisionTimer   = nullptr;
    bool    m_homeOrbitActive     = false;

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
