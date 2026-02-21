#include "ZoneChecker.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>

namespace vtol {

const std::map<std::string, double> ZoneChecker::SETTLEMENT_FALLBACK_RADII = {
    {"city", 5000.0}, {"town", 2000.0}, {"village", 700.0}, {"hamlet", 300.0}
};

ZoneChecker::ZoneChecker(const ZoneManager& zoneManager, const ZoneAvoidanceConfig& config)
    : m_zoneManager(zoneManager)
    , m_config(config)
{}

bool ZoneChecker::isNoFlyActive(const NoFlyZone& zone, double altitude) const
{
    // Per-zone override
    std::string mode = zone.avoid_mode.value_or(m_config.nofly_mode);
    if (mode == "disabled") return false;
    if (mode == "always") return true;
    // "below_altitude"
    if (zone.altitude.has_value()) return altitude < zone.altitude.value();
    return true;
}

bool ZoneChecker::isSettlementActive(double altitude) const
{
    if (m_config.settlement_mode == "disabled") return false;
    if (m_config.settlement_mode == "always") return true;
    // "below_altitude"
    return altitude < m_config.settlement_min_altitude;
}

RestrictionResult ZoneChecker::isPointRestricted(double lat, double lon, double altitude) const
{
    // Check no-fly zones
    for (const auto& zone : m_zoneManager.getAllZones()) {
        if (!isNoFlyActive(zone, altitude)) continue;
        if (nav::pointInPolygon(lat, lon, zone.points)) {
            return {true, "Запретная зона: " + zone.name};
        }
    }

    // Check settlements
    if (isSettlementActive(altitude)) {
        for (const auto& s : m_settlements) {
            // Quick bbox check
            if (lat < s.minLat || lat > s.maxLat || lon < s.minLon || lon > s.maxLon)
                continue;
            if (nav::pointInPolygon(lat, lon, s.polygon)) {
                return {true, "Населённый пункт: " + s.name + " (" + s.type + ")"};
            }
        }
    }

    return {false, ""};
}

bool ZoneChecker::segmentIntersectsObstacles(double lat1, double lon1,
                                               double lat2, double lon2, double altitude) const
{
    // Check no-fly zones
    for (const auto& zone : m_zoneManager.getAllZones()) {
        if (!isNoFlyActive(zone, altitude)) continue;
        double buffer = zone.buffer.value_or(m_config.nofly_buffer);
        Polygon buffered = buffer > 0 ? nav::polygonBuffer(zone.points, buffer) : zone.points;
        if (nav::segmentIntersectsPolygon(lat1, lon1, lat2, lon2, buffered))
            return true;
    }

    // Check settlements
    if (isSettlementActive(altitude)) {
        // Segment bbox for spatial filtering
        double minLat = std::min(lat1, lat2) - 0.05;
        double maxLat = std::max(lat1, lat2) + 0.05;
        double minLon = std::min(lon1, lon2) - 0.05;
        double maxLon = std::max(lon1, lon2) + 0.05;

        for (const auto& s : settlementsInBbox(minLat, maxLat, minLon, maxLon)) {
            Polygon buffered = m_config.settlement_buffer > 0
                ? nav::polygonBuffer(s.polygon, m_config.settlement_buffer)
                : s.polygon;
            if (nav::segmentIntersectsPolygon(lat1, lon1, lat2, lon2, buffered))
                return true;
        }
    }

    return false;
}

std::vector<Polygon> ZoneChecker::getActiveObstacles(double altitude,
                                                       double minLat, double maxLat,
                                                       double minLon, double maxLon) const
{
    std::vector<Polygon> obstacles;

    for (const auto& zone : m_zoneManager.getAllZones()) {
        if (!isNoFlyActive(zone, altitude)) continue;
        obstacles.push_back(zone.points);
    }

    if (isSettlementActive(altitude)) {
        for (const auto& s : settlementsInBbox(minLat, maxLat, minLon, maxLon)) {
            obstacles.push_back(s.polygon);
        }
    }

    return obstacles;
}

std::vector<Polygon> ZoneChecker::getBufferedObstacles(double altitude,
                                                         double minLat, double maxLat,
                                                         double minLon, double maxLon) const
{
    // Lock: this method runs in background threads (via PathPlanner),
    // while updateConfig/addSettlementFeatures modify m_config/m_settlements on main thread
    std::lock_guard lock(m_mutex);

    std::vector<Polygon> obstacles;
    int zoneCount = 0;

    for (const auto& zone : m_zoneManager.getAllZones()) {
        if (!isNoFlyActive(zone, altitude)) {
            SPDLOG_DEBUG("[ZoneChecker] Zone '{}' skipped (not active at alt={:.0f})", zone.name, altitude);
            continue;
        }
        double buffer = zone.buffer.value_or(m_config.nofly_buffer);
        auto buffered = buffer > 0 ? nav::polygonBuffer(zone.points, buffer) : zone.points;
        SPDLOG_DEBUG("[ZoneChecker] Zone '{}' included: {} pts, buffer={:.0f}m → {} buffered pts",
                     zone.name, zone.points.size(), buffer, buffered.size());
        obstacles.push_back(std::move(buffered));
        ++zoneCount;
    }

    int settlementCount = 0;
    if (isSettlementActive(altitude)) {
        for (const auto& s : settlementsInBbox(minLat, maxLat, minLon, maxLon)) {
            obstacles.push_back(m_config.settlement_buffer > 0
                ? nav::polygonBuffer(s.polygon, m_config.settlement_buffer)
                : s.polygon);
            ++settlementCount;
        }
    }

    SPDLOG_INFO("[ZoneChecker] getBufferedObstacles(alt={:.0f}): {} zones + {} settlements = {} obstacles",
                altitude, zoneCount, settlementCount, obstacles.size());
    return obstacles;
}

std::vector<Polygon> ZoneChecker::getBufferedZones(double altitude) const
{
    std::lock_guard lock(m_mutex);
    std::vector<Polygon> zones;
    for (const auto& zone : m_zoneManager.getAllZones()) {
        if (!isNoFlyActive(zone, altitude)) continue;
        double buffer = zone.buffer.value_or(m_config.nofly_buffer);
        zones.push_back(buffer > 0 ? nav::polygonBuffer(zone.points, buffer) : zone.points);
    }
    return zones;
}

std::vector<ZoneChecker::IntersectionPoint> ZoneChecker::findIntersectionPoints(
    double lat1, double lon1, double lat2, double lon2, double altitude) const
{
    std::vector<IntersectionPoint> result;

    auto findEntry = [&](const Polygon& poly, const std::string& reason) {
        // Find the first intersection of segment with polygon edges
        int n = static_cast<int>(poly.size());
        double bestT = 2.0;
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            // Segment-segment intersection parameter t on (lat1,lon1)→(lat2,lon2)
            double dx1 = lat2 - lat1, dy1 = lon2 - lon1;
            double dx2 = poly[j].lat - poly[i].lat, dy2 = poly[j].lon - poly[i].lon;
            double denom = dx1 * dy2 - dy1 * dx2;
            if (std::abs(denom) < 1e-15) continue;
            double dbx = poly[i].lat - lat1, dby = poly[i].lon - lon1;
            double t = (dbx * dy2 - dby * dx2) / denom;
            double u = (dbx * dy1 - dby * dx1) / denom;
            if (t > 0.01 && t < 0.99 && u > 0.01 && u < 0.99) {
                if (t < bestT) bestT = t;
            }
        }
        if (bestT < 2.0) {
            double iLat = lat1 + bestT * (lat2 - lat1);
            double iLon = lon1 + bestT * (lon2 - lon1);
            result.push_back({iLat, iLon, reason});
        } else {
            // Endpoint inside polygon — use midpoint as marker
            result.push_back({(lat1 + lat2) / 2.0, (lon1 + lon2) / 2.0, reason});
        }
    };

    // Check no-fly zones
    for (const auto& zone : m_zoneManager.getAllZones()) {
        if (!isNoFlyActive(zone, altitude)) continue;
        double buffer = zone.buffer.value_or(m_config.nofly_buffer);
        Polygon buffered = buffer > 0 ? nav::polygonBuffer(zone.points, buffer) : zone.points;
        if (nav::segmentIntersectsPolygon(lat1, lon1, lat2, lon2, buffered)) {
            std::string reason = "Запретная зона";
            if (!zone.name.empty()) reason += ": " + zone.name;
            findEntry(buffered, reason);
        }
    }

    // Check settlements
    if (isSettlementActive(altitude)) {
        double minLat = std::min(lat1, lat2) - 0.05;
        double maxLat = std::max(lat1, lat2) + 0.05;
        double minLon = std::min(lon1, lon2) - 0.05;
        double maxLon = std::max(lon1, lon2) + 0.05;

        for (const auto& s : settlementsInBbox(minLat, maxLat, minLon, maxLon)) {
            Polygon buffered = m_config.settlement_buffer > 0
                ? nav::polygonBuffer(s.polygon, m_config.settlement_buffer)
                : s.polygon;
            if (nav::segmentIntersectsPolygon(lat1, lon1, lat2, lon2, buffered)) {
                std::string reason = "Населённый пункт";
                if (!s.name.empty()) reason += ": " + s.name;
                findEntry(buffered, reason);
            }
        }
    }

    return result;
}

void ZoneChecker::addSettlementFeatures(const std::vector<CachedSettlement>& features)
{
    std::lock_guard lock(m_mutex);
    for (const auto& f : features) {
        m_settlements.push_back(f);
    }
    SPDLOG_DEBUG("Added {} settlement features (total: {})", features.size(), m_settlements.size());
}

void ZoneChecker::clearSettlements()
{
    std::lock_guard lock(m_mutex);
    auto count = m_settlements.size();
    m_settlements.clear();
    SPDLOG_INFO("[ZoneChecker] Cleared {} settlements", count);
}

void ZoneChecker::setSettlementCacheDir(const std::string& dir)
{
    namespace fs = std::filesystem;
    if (!fs::exists(dir)) {
        SPDLOG_WARN("Settlement cache directory not found: {}", dir);
        return;
    }

    int loaded = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".json") continue;
        try {
            std::ifstream file(entry.path());
            nlohmann::json data;
            file >> data;

            for (const auto& feat : data) {
                CachedSettlement s;
                std::string form = feat.value("F", "p");
                s.type = feat.value("t", "village");
                s.name = feat.value("n", "");

                if (form == "p" && feat.contains("c")) {
                    for (const auto& pt : feat["c"])
                        s.polygon.push_back({pt[0].get<double>(), pt[1].get<double>()});
                } else if (form == "c" || feat.contains("lat")) {
                    double lat = feat.value("lat", 0.0);
                    double lon = feat.value("lon", 0.0);
                    auto it = SETTLEMENT_FALLBACK_RADII.find(s.type);
                    double r = it != SETTLEMENT_FALLBACK_RADII.end() ? it->second : 500.0;
                    s.polygon = nav::circleToPolygon(lat, lon, r, 16);
                }

                if (s.polygon.size() < 3) continue;

                // Precompute bbox
                s.minLat = s.maxLat = s.polygon[0].lat;
                s.minLon = s.maxLon = s.polygon[0].lon;
                for (const auto& p : s.polygon) {
                    s.minLat = std::min(s.minLat, p.lat);
                    s.maxLat = std::max(s.maxLat, p.lat);
                    s.minLon = std::min(s.minLon, p.lon);
                    s.maxLon = std::max(s.maxLon, p.lon);
                }

                m_settlements.push_back(std::move(s));
                ++loaded;
            }
        } catch (const std::exception& e) {
            SPDLOG_WARN("Failed to load settlement file {}: {}", entry.path().string(), e.what());
        }
    }

    SPDLOG_INFO("Loaded {} settlements from cache dir {}", loaded, dir);
}

std::vector<CachedSettlement> ZoneChecker::settlementsInBbox(double minLat, double maxLat,
                                                               double minLon, double maxLon) const
{
    std::vector<CachedSettlement> result;
    for (const auto& s : m_settlements) {
        if (s.maxLat < minLat || s.minLat > maxLat ||
            s.maxLon < minLon || s.minLon > maxLon)
            continue;
        result.push_back(s);
    }
    return result;
}

} // namespace vtol
