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
#include "HeadingController.h"
#include "AltitudeController.h"
#include "SpeedController.h"

namespace vtol {

class MavlinkConnection;
class RoutePlanner;
class ZoneChecker;
class PathPlanner;
struct Waypoint;

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
    void disengage(const QString& reason = "");

    // --- Query ---
    [[nodiscard]] AutopilotMode mode() const { return m_mode; }
    [[nodiscard]] bool isEngaged() const { return m_mode != AutopilotMode::MANUAL; }
    [[nodiscard]] double targetHeading() const { return m_headingCtrl.target(); }
    [[nodiscard]] double headingError() const { return m_headingCtrl.error(); }
    [[nodiscard]] AutopilotStatus status() const;
    [[nodiscard]] std::vector<std::tuple<double, double>> remainingAvoidanceWaypoints() const;

    // --- Live adjustments ---
    void setTargetAltitude(double altitude);
    void setOrbitRadius(double radius);
    void setTargetAirspeed(double speed);

signals:
    void engaged(const QString& mode);
    void disengaged(const QString& previousMode, const QString& reason);
    void waypointReached(int reachedId, int nextId);

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
    bool chooseOrbitDirectionCcw(const LatLon& position, double centerLat, double centerLon, double heading);
    void updateOrbitProgress(double currentHeading);
    void finishOrbitAndAdvance(Waypoint& oldWp);
    void handleRouteCompletion();

    // Zone avoidance
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

    // GUIDED target throttling
    double m_guidedSendTime = 0.0;
    static constexpr double GUIDED_RESEND_INTERVAL = 2.0;

    int m_logCounter = 0;

    // Zone avoidance
    std::vector<std::tuple<double, double>> m_avoidanceWaypoints;
    int m_avoidanceWpIdx = 0;
    int m_avoidanceForWpId = -1;
    int m_avoidanceGaveUp = -1;
    std::atomic<bool> m_avoidanceComputing{false};

    struct AvoidancePendingResult {
        std::vector<std::tuple<double, double>> path;
        int wpId = -1;
        bool hasResult = false;
        bool isNull = false;  // true if plan_path returned nullopt
    };
    AvoidancePendingResult m_avoidancePending;
};

} // namespace vtol
