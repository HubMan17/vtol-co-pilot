#include "RoutePlanner.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <chrono>

namespace vtol {

namespace {
std::string nowIso() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}
} // anonymous

std::optional<Route> RoutePlanner::loadRoute(const std::string& path)
{
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            SPDLOG_ERROR("Failed to open route file: {}", path);
            return std::nullopt;
        }

        nlohmann::json data;
        file >> data;

        Route route;
        route.name = data.value("name", std::filesystem::path(path).stem().string());
        route.created = data.value("created", "");
        route.modified = data.value("modified", "");

        for (const auto& wpData : data.value("waypoints", nlohmann::json::array())) {
            Waypoint wp;
            wp.id = wpData.value("id", static_cast<int>(route.waypoints.size()) + 1);
            wp.lat = wpData.at("lat").get<double>();
            wp.lon = wpData.at("lon").get<double>();
            wp.altitude = wpData.contains("altitude") ? wpData["altitude"].get<double>()
                        : wpData.value("alt", 100.0);
            wp.radius = wpData.value("radius", 150.0);
            wp.action = wpData.value("action", "FLYTHROUGH");
            wp.orbit_radius = wpData.value("orbit_radius", 150.0);
            wp.orbit_turns = wpData.value("orbit_turns", 1);
            wp.climb_enroute = wpData.value("climb_enroute", false);
            route.waypoints.push_back(wp);
        }

        m_route = route;
        m_activeIdx = 0;
        SPDLOG_INFO("Route loaded: {} ({} waypoints)", route.name, route.waypoints.size());
        return route;

    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to load route: {}", e.what());
        return std::nullopt;
    }
}

bool RoutePlanner::saveRoute(const Route& route, const std::string& path)
{
    try {
        namespace fs = std::filesystem;
        fs::create_directories(fs::path(path).parent_path());

        nlohmann::json data;
        data["name"] = route.name;
        data["created"] = route.created;
        data["modified"] = nowIso();

        auto& wps = data["waypoints"] = nlohmann::json::array();
        for (const auto& wp : route.waypoints) {
            wps.push_back({
                {"id", wp.id}, {"lat", wp.lat}, {"lon", wp.lon},
                {"altitude", wp.altitude}, {"radius", wp.radius},
                {"action", wp.action}, {"orbit_radius", wp.orbit_radius},
                {"orbit_turns", wp.orbit_turns}, {"climb_enroute", wp.climb_enroute}
            });
        }

        std::ofstream file(path);
        if (!file.is_open()) {
            SPDLOG_ERROR("Failed to write route to {}", path);
            return false;
        }
        file << data.dump(2);
        SPDLOG_INFO("Route saved to {}", path);
        return true;

    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to save route: {}", e.what());
        return false;
    }
}

Route& RoutePlanner::createRoute(const std::string& name)
{
    m_route = Route{name, {}, nowIso(), ""};
    m_activeIdx = 0;
    return *m_route;
}

Waypoint* RoutePlanner::addWaypoint(double lat, double lon, double alt,
                                      double radius, const std::string& action,
                                      double orbitRadius, int orbitTurns, bool climbEnroute)
{
    if (!m_route) return nullptr;
    int newId = static_cast<int>(m_route->waypoints.size()) + 1;
    m_route->waypoints.push_back({newId, lat, lon, alt, radius, action, orbitRadius, orbitTurns, climbEnroute});
    return &m_route->waypoints.back();
}

bool RoutePlanner::removeWaypoint(int index)
{
    if (!m_route || index < 0 || index >= static_cast<int>(m_route->waypoints.size()))
        return false;
    m_route->waypoints.erase(m_route->waypoints.begin() + index);
    // Reindex
    for (int i = 0; i < static_cast<int>(m_route->waypoints.size()); ++i)
        m_route->waypoints[i].id = i + 1;
    if (m_activeIdx >= static_cast<int>(m_route->waypoints.size()))
        m_activeIdx = std::max(0, static_cast<int>(m_route->waypoints.size()) - 1);
    return true;
}

bool RoutePlanner::updateWaypoint(int index, const Waypoint& newData)
{
    if (!m_route || index < 0 || index >= static_cast<int>(m_route->waypoints.size()))
        return false;
    int savedId = m_route->waypoints[index].id;
    m_route->waypoints[index] = newData;
    m_route->waypoints[index].id = savedId;  // keep sequential id
    return true;
}

bool RoutePlanner::moveWaypoint(int fromIndex, int toIndex)
{
    if (!m_route) return false;
    int n = static_cast<int>(m_route->waypoints.size());
    if (fromIndex < 0 || fromIndex >= n || toIndex < 0 || toIndex >= n) return false;
    if (fromIndex == toIndex) return true;

    Waypoint wp = m_route->waypoints[fromIndex];
    m_route->waypoints.erase(m_route->waypoints.begin() + fromIndex);
    m_route->waypoints.insert(m_route->waypoints.begin() + toIndex, wp);

    // Reindex ids
    for (int i = 0; i < static_cast<int>(m_route->waypoints.size()); ++i)
        m_route->waypoints[i].id = i + 1;

    // Adjust active index to follow the moved waypoint if it was active
    if (m_activeIdx == fromIndex)
        m_activeIdx = toIndex;
    else if (fromIndex < toIndex && m_activeIdx > fromIndex && m_activeIdx <= toIndex)
        --m_activeIdx;
    else if (fromIndex > toIndex && m_activeIdx >= toIndex && m_activeIdx < fromIndex)
        ++m_activeIdx;

    return true;
}

Waypoint* RoutePlanner::activeWaypoint()
{
    if (!m_route || m_route->waypoints.empty()) return nullptr;
    if (m_activeIdx >= static_cast<int>(m_route->waypoints.size())) return nullptr;
    return &m_route->waypoints[m_activeIdx];
}

const Waypoint* RoutePlanner::activeWaypoint() const
{
    if (!m_route || m_route->waypoints.empty()) return nullptr;
    if (m_activeIdx >= static_cast<int>(m_route->waypoints.size())) return nullptr;
    return &m_route->waypoints[m_activeIdx];
}

Waypoint* RoutePlanner::nextWaypoint()
{
    if (!m_route || m_route->waypoints.empty()) return nullptr;
    if (m_activeIdx < static_cast<int>(m_route->waypoints.size()) - 1)
        ++m_activeIdx;
    return activeWaypoint();
}

Waypoint* RoutePlanner::prevWaypoint()
{
    if (!m_route || m_route->waypoints.empty()) return nullptr;
    if (m_activeIdx > 0) --m_activeIdx;
    return activeWaypoint();
}

void RoutePlanner::setActiveWaypoint(int index)
{
    if (m_route && index >= 0 && index < static_cast<int>(m_route->waypoints.size()))
        m_activeIdx = index;
}

bool RoutePlanner::isWaypointReached(const LatLon& pos) const
{
    auto* wp = activeWaypoint();
    if (!wp) return false;
    return nav::haversineDistance(pos.lat, pos.lon, wp->lat, wp->lon) <= wp->radius;
}

double RoutePlanner::distanceToWaypoint(const LatLon& pos) const
{
    auto* wp = activeWaypoint();
    if (!wp) return std::numeric_limits<double>::infinity();
    return nav::haversineDistance(pos.lat, pos.lon, wp->lat, wp->lon);
}

double RoutePlanner::bearingToWaypoint(const LatLon& pos) const
{
    auto* wp = activeWaypoint();
    if (!wp) return 0.0;
    return nav::bearingTo(pos.lat, pos.lon, wp->lat, wp->lon);
}

double RoutePlanner::etaToWaypoint(const LatLon& pos, double groundspeed) const
{
    return nav::etaSeconds(distanceToWaypoint(pos), groundspeed);
}

double RoutePlanner::crossTrackError(const LatLon& pos) const
{
    if (!m_route || m_route->waypoints.size() < 2 || m_activeIdx == 0)
        return 0.0;
    const auto& wp1 = m_route->waypoints[m_activeIdx - 1];
    const auto& wp2 = m_route->waypoints[m_activeIdx];
    return nav::crossTrackDistance(pos.lat, pos.lon, wp1.lat, wp1.lon, wp2.lat, wp2.lon);
}

const Waypoint* RoutePlanner::previousWaypoint() const
{
    if (!m_route || m_activeIdx == 0) return nullptr;
    return &m_route->waypoints[m_activeIdx - 1];
}

bool RoutePlanner::isRouteComplete() const
{
    if (!m_route) return true;
    return m_activeIdx >= static_cast<int>(m_route->waypoints.size());
}

} // namespace vtol
