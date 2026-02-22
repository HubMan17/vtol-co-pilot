#pragma once

#include <QObject>
#include <QTimer>
#include <QString>
#include <optional>
#include <vector>
#include <tuple>
#include <atomic>

#include "core/Types.h"
#include "core/Config.h"
#include "navigation/RoutePlanner.h"
#include "HeadingController.h"
#include "AltitudeController.h"
#include "SpeedController.h"

namespace vtol {

class MavlinkConnection;
class ZoneChecker;
class PathPlanner;

enum class AutopilotMode { MANUAL, NAV };

struct AutopilotStatus {
    QString mode;
    double targetHeading = 0;
    double headingError = 0;
    double targetAltitude = 0;
    double altitudeError = 0;
    double targetAirspeed = 0;
    double airspeedError = 0;
    double lastUpdate = 0;
    QString disengageReason;
    QString action = "IDLE";
    bool isOrbiting = false;
    int orbitTurnsCompleted = 0;
    double orbitRadius = 0;
    bool returningHome = false;
    bool operationalActive = false;
    QString operationalMode;    // "GOTO" or "VIA_POINT"
};

class AutopilotManager : public QObject {
    Q_OBJECT
public:
    explicit AutopilotManager(MavlinkConnection& connection,
                               const AutopilotConfig& config,
                               QObject* parent = nullptr);
    ~AutopilotManager() override = default;

    // --- Setters for subsystems ---
    void setRoutePlanner(RoutePlanner* planner) { m_routePlanner = planner; }
    void setZoneChecker(ZoneChecker* checker) { m_zoneChecker = checker; }
    void setPathPlanner(PathPlanner* planner) { m_pathPlanner = planner; }
    void setHomePosition(const std::optional<LatLon>& pos);
    void resetZoneAvoidance();

    // --- Engage / disengage ---
    bool engageNav();
    bool engageHome();
    bool engageOperational(const Waypoint& wp, OperationalMode mode);
    void cancelOperational();
    void disengage(const QString& reason = "");
    void activateRtl();

    // --- Query ---
    [[nodiscard]] AutopilotMode mode() const { return m_mode; }
    [[nodiscard]] bool isEngaged() const { return m_mode != AutopilotMode::MANUAL; }
    [[nodiscard]] double targetHeading() const { return m_headingCtrl.target(); }
    [[nodiscard]] double headingError() const { return m_headingCtrl.error(); }
    [[nodiscard]] AutopilotStatus status() const;
    [[nodiscard]] std::vector<std::tuple<double, double>> remainingAvoidanceWaypoints() const;
    [[nodiscard]] bool isOperationalActive() const { return m_operationalActive; }
    [[nodiscard]] bool hasOperationalWaypoint() const { return m_operationalWp.has_value(); }
    [[nodiscard]] const OperationalWaypoint* operationalWaypoint() const;

    // --- Live adjustments ---
    void setTargetAltitude(double altitude);
    void setOrbitRadius(double radius);
    void setTargetAirspeed(double speed);

    // Suppress periodic avoidance rechecks for current WP (user chose "fly direct")
    void suppressAvoidanceRecheck();

signals:
    void engaged(const QString& mode);
    void disengaged(const QString& previousMode, const QString& reason);
    void waypointReached(int reachedId, int nextId);
    void avoidanceFailed(const QString& reason);
    void homeOrbitEstablished();
    void operationalEngaged();
    void operationalReached();
    void operationalCancelled();

public slots:
    void update();

private:
    // RC stick override
    static constexpr int CH_ROLL = 0;
    static constexpr int CH_PITCH = 1;
    static constexpr int CH_THROTTLE = 2;
    static constexpr int CH_YAW = 3;
    static constexpr int PWM_CENTER = 1500;
    static constexpr double GUIDED_PROJECTION_DISTANCE = 2000.0;

    bool checkStickOverride(const std::array<int, 8>& rc);

    // Navigation
    void sendGuidedCommands(double targetBearing, const LatLon& position);
    void exitOrbit();
    void startOrbit(Waypoint& wp, double currentHeading);
    void onOperationalReached();
    bool chooseOrbitDirectionCcw(const LatLon& position, double centerLat, double centerLon, double heading);
    void updateOrbitProgress(double currentHeading);
    void finishOrbitAndAdvance(Waypoint& oldWp);
    void handleRouteCompletion();

    // Zone avoidance
    static constexpr int HOME_WP_ID = -999;
    static constexpr int OPERATIONAL_WP_ID = -998;
    std::optional<std::tuple<double, double>> getAvoidanceTarget(
        const LatLon& position, const Waypoint& wp, double altitude);
    void startAvoidanceComputation(double startLat, double startLon,
                                    double endLat, double endLon,
                                    double altitude, int wpId);

    // Subsystems (non-owning)
    MavlinkConnection& m_connection;
    AutopilotConfig m_config;
    RoutePlanner* m_routePlanner = nullptr;
    ZoneChecker* m_zoneChecker = nullptr;
    PathPlanner* m_pathPlanner = nullptr;

    // Controllers
    HeadingController m_headingCtrl;
    AltitudeController m_altitudeCtrl;
    SpeedController m_speedCtrl;

    // State
    AutopilotMode m_mode = AutopilotMode::MANUAL;
    double m_lastUpdateTime = 0.0;
    double m_engageTime = 0.0;
    QString m_disengageReason;
    int m_activeWaypointId = -1;
    int m_stickOverrideCount = 0;
    static constexpr int STICK_OVERRIDE_THRESHOLD = 3;
    int m_throttleBaseline = 0;

    // Orbit
    bool m_isOrbiting = false;
    int m_orbitTurnsCompleted = 0;
    double m_orbitLastHeading = 0.0;
    double m_orbitHeadingAccumulated = 0.0;
    bool m_waitingForAltitude = false;
    std::optional<LatLon> m_homePosition;
    bool m_returningHome = false;
    std::optional<double> m_savedAirspeedCruise;
    bool m_loiterAltTransition = false;
    bool m_orbitAdvanceHandled = false;
    bool m_orbitRepositionSent = false;
    bool m_orbitCcw = false;
    int m_tangentApproachWpId = -1;
    bool m_homeOrbitNotified = false;

    // Operational waypoint (ad-hoc, outside the route)
    std::optional<OperationalWaypoint> m_operationalWp;
    bool m_operationalActive = false;
    bool m_operationalStandalone = false; // auto-engaged without route

    // GUIDED target throttling
    double m_guidedSendTime = 0.0;
    static constexpr double GUIDED_RESEND_INTERVAL = 2.0;

    int m_logCounter = 0;

    // Zone avoidance
    std::vector<std::tuple<double, double>> m_avoidanceWaypoints;
    int m_avoidanceWpIdx = 0;
    int m_avoidanceForWpId = -1;
    std::atomic<bool> m_avoidanceComputing{false};
    double m_avoidanceLastCheckTime = 0.0;
    static constexpr double AVOIDANCE_RECHECK_INTERVAL = 10.0; // seconds

    // GaveUp with distance-based retry
    static constexpr double AVOIDANCE_RETRY_DISTANCE = 1000.0; // retry after 1km
    struct AvoidanceGaveUp {
        int wpId = -1;
        double lat = 0, lon = 0;  // position where we gave up
    };
    AvoidanceGaveUp m_avoidanceGaveUp;
    int m_avoidanceSuppressedWpId = -1; // user chose "fly direct" — skip rechecks
    int m_avoidanceNotifiedWpId = -1;  // already showed "no path" notification for this WP

    struct AvoidancePendingResult {
        std::vector<std::tuple<double, double>> path;
        int wpId = -1;
        bool hasResult = false;
        bool isNull = false;  // true if plan_path returned nullopt
    };
    AvoidancePendingResult m_avoidancePending;
};

} // namespace vtol
