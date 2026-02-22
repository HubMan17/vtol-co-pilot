#pragma once

#include "core/Types.h"
#include "Calculations.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>

namespace vtol {

struct Waypoint {
    int id = 0;
    double lat = 0.0;
    double lon = 0.0;
    double altitude = 100.0;
    double radius = 150.0;
    std::string action = "FLYTHROUGH";
    double orbit_radius = 150.0;
    int orbit_turns = 1;
    bool climb_enroute = false;

    [[nodiscard]] LatLon toLatLon() const { return {lat, lon}; }

    [[nodiscard]] std::string actionName() const {
        static const std::map<std::string, std::string> names = {
            {"FLYTHROUGH", "Пролёт"},
            {"ORBIT_TURNS", "Кружить N кругов"},
            {"ORBIT_INFINITE", "Кружить бесконечно"},
            {"ALTITUDE", "Набор/смена высоты"},
        };
        auto it = names.find(action);
        return it != names.end() ? it->second : action;
    }
};

// ─── Operational waypoint (ad-hoc, outside the route) ───────────────
enum class OperationalMode { GOTO, VIA_POINT };

struct OperationalWaypoint {
    Waypoint waypoint;
    OperationalMode mode = OperationalMode::GOTO;

    [[nodiscard]] const char* modeName() const {
        return mode == OperationalMode::GOTO ? "GOTO" : "VIA_POINT";
    }
};

struct Route {
    std::string name;
    std::vector<Waypoint> waypoints;
    std::string created;
    std::string modified;
};

class RoutePlanner {
public:
    RoutePlanner() = default;

    std::optional<Route> loadRoute(const std::string& path);
    bool saveRoute(const Route& route, const std::string& path);

    Route* getRoute() { return m_route.has_value() ? &m_route.value() : nullptr; }
    const Route* getRoute() const { return m_route.has_value() ? &m_route.value() : nullptr; }
    void clearRoute() { m_route.reset(); m_activeIdx = 0; }
    void clearWaypoints() { if (m_route) { m_route->waypoints.clear(); m_activeIdx = 0; } }
    Route& createRoute(const std::string& name);

    Waypoint* addWaypoint(double lat, double lon, double alt = 100.0,
                           double radius = 50.0, const std::string& action = "FLYTHROUGH",
                           double orbitRadius = 100.0, int orbitTurns = 1,
                           bool climbEnroute = false);
    bool removeWaypoint(int index);

    Waypoint* activeWaypoint();
    const Waypoint* activeWaypoint() const;
    int activeWaypointIndex() const { return m_activeIdx; }
    int waypointCount() const { return m_route ? static_cast<int>(m_route->waypoints.size()) : 0; }

    Waypoint* nextWaypoint();
    Waypoint* prevWaypoint();
    void setActiveWaypoint(int index);

    bool isWaypointReached(const LatLon& pos) const;
    double distanceToWaypoint(const LatLon& pos) const;
    double bearingToWaypoint(const LatLon& pos) const;
    double etaToWaypoint(const LatLon& pos, double groundspeed) const;
    double crossTrackError(const LatLon& pos) const;

    const Waypoint* previousWaypoint() const;
    bool isRouteComplete() const;

private:
    std::optional<Route> m_route;
    int m_activeIdx = 0;
};

} // namespace vtol
