#include "Calculations.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace vtol::nav {

double haversineDistance(double lat1, double lon1, double lat2, double lon2)
{
    double lat1Rad = lat1 * M_PI / 180.0;
    double lat2Rad = lat2 * M_PI / 180.0;
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;

    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
               std::cos(lat1Rad) * std::cos(lat2Rad) *
               std::sin(dLon / 2) * std::sin(dLon / 2);
    double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    return EARTH_RADIUS * c;
}

double bearingTo(double lat1, double lon1, double lat2, double lon2)
{
    double lat1Rad = lat1 * M_PI / 180.0;
    double lat2Rad = lat2 * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;

    double x = std::sin(dLon) * std::cos(lat2Rad);
    double y = std::cos(lat1Rad) * std::sin(lat2Rad) -
               std::sin(lat1Rad) * std::cos(lat2Rad) * std::cos(dLon);

    double bearing = std::atan2(x, y);
    return std::fmod(bearing * 180.0 / M_PI + 360.0, 360.0);
}

double crossTrackDistance(double posLat, double posLon,
                           double wp1Lat, double wp1Lon,
                           double wp2Lat, double wp2Lon)
{
    double d13 = haversineDistance(wp1Lat, wp1Lon, posLat, posLon) / EARTH_RADIUS;
    double brng13 = bearingTo(wp1Lat, wp1Lon, posLat, posLon) * M_PI / 180.0;
    double brng12 = bearingTo(wp1Lat, wp1Lon, wp2Lat, wp2Lon) * M_PI / 180.0;

    double xtd = std::asin(std::sin(d13) * std::sin(brng13 - brng12));
    return xtd * EARTH_RADIUS;
}

double alongTrackDistance(double posLat, double posLon,
                            double wp1Lat, double wp1Lon,
                            double wp2Lat, double wp2Lon)
{
    double d13 = haversineDistance(wp1Lat, wp1Lon, posLat, posLon) / EARTH_RADIUS;
    double xtd = crossTrackDistance(posLat, posLon, wp1Lat, wp1Lon, wp2Lat, wp2Lon) / EARTH_RADIUS;

    double cosD13 = std::cos(d13);
    double cosXtd = std::cos(xtd);
    if (std::abs(cosXtd) < 1e-15) return 0.0;

    double atd = std::acos(std::clamp(cosD13 / cosXtd, -1.0, 1.0));
    return atd * EARTH_RADIUS;
}

double etaSeconds(double distanceM, double groundspeedMs)
{
    if (groundspeedMs <= 0) return std::numeric_limits<double>::infinity();
    return distanceM / groundspeedMs;
}

double metersToLatOffset(double meters)
{
    return meters / 111320.0;
}

double metersToLonOffset(double meters, double latitude)
{
    return meters / (111320.0 * std::cos(latitude * M_PI / 180.0));
}

double normalizeHeading(double heading)
{
    return std::fmod(heading, 360.0);
}

double headingDifference(double h1, double h2)
{
    double diff = h2 - h1;
    while (diff > 180)  diff -= 360;
    while (diff < -180) diff += 360;
    return diff;
}

LatLon projectPoint(double lat, double lon, double bearingDeg, double distanceM)
{
    double d = distanceM / EARTH_RADIUS;
    double brg = bearingDeg * M_PI / 180.0;
    double lat1 = lat * M_PI / 180.0;
    double lon1 = lon * M_PI / 180.0;

    double lat2 = std::asin(std::sin(lat1) * std::cos(d) +
                             std::cos(lat1) * std::sin(d) * std::cos(brg));
    double lon2 = lon1 + std::atan2(std::sin(brg) * std::sin(d) * std::cos(lat1),
                                     std::cos(d) - std::sin(lat1) * std::sin(lat2));

    return {lat2 * 180.0 / M_PI, lon2 * 180.0 / M_PI};
}

// ── Zone avoidance geometry ─────────────────────────────────────────

bool pointInPolygon(double lat, double lon, const Polygon& polygon)
{
    int n = static_cast<int>(polygon.size());
    if (n < 3) return false;

    bool inside = false;
    int j = n - 1;
    for (int i = 0; i < n; ++i) {
        double yi = polygon[i].lat, xi = polygon[i].lon;
        double yj = polygon[j].lat, xj = polygon[j].lon;
        if (((yi > lat) != (yj > lat)) &&
            (lon < (xj - xi) * (lat - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
        j = i;
    }
    return inside;
}

Polygon circleToPolygon(double centerLat, double centerLon, double radiusM, int nPoints)
{
    Polygon pts;
    pts.reserve(nPoints);
    for (int i = 0; i < nPoints; ++i) {
        double bearing = 360.0 * i / nPoints;
        pts.push_back(projectPoint(centerLat, centerLon, bearing, radiusM));
    }
    return pts;
}

bool segmentsIntersect2d(double ax1, double ay1, double ax2, double ay2,
                          double bx1, double by1, double bx2, double by2)
{
    double dxA = ax2 - ax1;
    double dyA = ay2 - ay1;
    double dxB = bx2 - bx1;
    double dyB = by2 - by1;

    double denom = dxA * dyB - dyA * dxB;
    if (std::abs(denom) < 1e-15) return false;  // parallel

    double t = ((bx1 - ax1) * dyB - (by1 - ay1) * dxB) / denom;
    double u = ((bx1 - ax1) * dyA - (by1 - ay1) * dxA) / denom;

    return t > 0.0 && t < 1.0 && u > 0.0 && u < 1.0;
}

bool segmentIntersectsPolygon(double p1Lat, double p1Lon, double p2Lat, double p2Lon,
                               const Polygon& polygon)
{
    if (pointInPolygon(p1Lat, p1Lon, polygon) || pointInPolygon(p2Lat, p2Lon, polygon))
        return true;

    int n = static_cast<int>(polygon.size());
    if (n < 3) return false;

    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        if (segmentsIntersect2d(
                p1Lat, p1Lon, p2Lat, p2Lon,
                polygon[i].lat, polygon[i].lon, polygon[j].lat, polygon[j].lon))
            return true;
    }
    return false;
}

bool isVisible(double p1Lat, double p1Lon, double p2Lat, double p2Lon,
               const std::vector<Polygon>& obstacles)
{
    for (const auto& poly : obstacles) {
        if (segmentIntersectsPolygon(p1Lat, p1Lon, p2Lat, p2Lon, poly))
            return false;
    }
    return true;
}

bool isVisibleVg(double p1Lat, double p1Lon, double p2Lat, double p2Lon,
                  const std::vector<Polygon>& obstacles)
{
    double midLat = (p1Lat + p2Lat) / 2;
    double midLon = (p1Lon + p2Lon) / 2;

    // Perpendicular offset for interior detection (~1cm)
    double dlat = p2Lat - p1Lat;
    double dlon = p2Lon - p1Lon;
    double norm = std::sqrt(dlat * dlat + dlon * dlon);
    constexpr double eps = 1e-7;
    double perpLat, perpLon;
    if (norm > 1e-15) {
        perpLat = -dlon / norm * eps;
        perpLon = dlat / norm * eps;
    } else {
        perpLat = eps;
        perpLon = 0.0;
    }

    double cl1 = midLat + perpLat, cn1 = midLon + perpLon;
    double cl2 = midLat - perpLat, cn2 = midLon - perpLon;

    for (const auto& poly : obstacles) {
        int n = static_cast<int>(poly.size());
        if (n < 3) continue;

        // Check edge crossings (strict inequality — ignores shared endpoints)
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            if (segmentsIntersect2d(
                    p1Lat, p1Lon, p2Lat, p2Lon,
                    poly[i].lat, poly[i].lon, poly[j].lat, poly[j].lon))
                return false;
        }
        // Both perpendicular sides inside → segment cuts through interior
        if (pointInPolygon(cl1, cn1, poly) && pointInPolygon(cl2, cn2, poly))
            return false;
    }
    return true;
}

Polygon polygonBuffer(const Polygon& polygon, double bufferM)
{
    if (polygon.empty() || bufferM <= 0)
        return polygon;

    int n = static_cast<int>(polygon.size());
    if (n < 3) return polygon;

    // Centroid
    double cx = 0, cy = 0;
    for (const auto& p : polygon) { cx += p.lat; cy += p.lon; }
    cx /= n; cy /= n;

    constexpr double latScale = 111320.0;
    double lonScale = 111320.0 * std::cos(cx * M_PI / 180.0);

    // Convert to flat meters
    struct FlatPt { double x, y; };
    std::vector<FlatPt> flat(n);
    for (int i = 0; i < n; ++i) {
        flat[i] = {(polygon[i].lat - cx) * latScale, (polygon[i].lon - cy) * lonScale};
    }

    // Signed area → winding direction
    double area2 = 0.0;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        area2 += flat[i].x * flat[j].y - flat[j].x * flat[i].y;
    }
    double sign = area2 > 0 ? 1.0 : -1.0;

    Polygon result;
    result.reserve(n);

    for (int i = 0; i < n; ++i) {
        int prevI = (i - 1 + n) % n;
        int nextI = (i + 1) % n;

        double dx1 = flat[i].x - flat[prevI].x;
        double dy1 = flat[i].y - flat[prevI].y;
        double L1 = std::sqrt(dx1 * dx1 + dy1 * dy1);

        double dx2 = flat[nextI].x - flat[i].x;
        double dy2 = flat[nextI].y - flat[i].y;
        double L2 = std::sqrt(dx2 * dx2 + dy2 * dy2);

        if (L1 < 0.01 || L2 < 0.01) {
            result.push_back(polygon[i]);
            continue;
        }

        // Outward normals
        double nx1 = sign * dy1 / L1, ny1 = sign * (-dx1) / L1;
        double nx2 = sign * dy2 / L2, ny2 = sign * (-dx2) / L2;

        // Bisector direction
        double bx = nx1 + nx2;
        double by = ny1 + ny2;
        double blen = std::sqrt(bx * bx + by * by);
        if (blen < 1e-10) {
            bx = nx1; by = ny1;
        } else {
            bx /= blen; by /= blen;
        }

        // Offset along bisector
        double cosHalf = std::max(nx1 * bx + ny1 * by, 0.25);
        double offset = bufferM / cosHalf;

        double newX = flat[i].x + bx * offset;
        double newY = flat[i].y + by * offset;

        result.push_back({cx + newX / latScale, cy + newY / lonScale});
    }

    return result;
}

} // namespace vtol::nav
