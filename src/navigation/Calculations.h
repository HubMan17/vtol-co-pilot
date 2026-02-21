#pragma once

#include "core/Types.h"
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vtol::nav {

constexpr double EARTH_RADIUS = 6371000.0;

// --- Scalar geodesic functions ---

double haversineDistance(double lat1, double lon1, double lat2, double lon2);
double bearingTo(double lat1, double lon1, double lat2, double lon2);
double crossTrackDistance(double posLat, double posLon,
                          double wp1Lat, double wp1Lon,
                          double wp2Lat, double wp2Lon);
double alongTrackDistance(double posLat, double posLon,
                           double wp1Lat, double wp1Lon,
                           double wp2Lat, double wp2Lon);
double etaSeconds(double distanceM, double groundspeedMs);
double metersToLatOffset(double meters);
double metersToLonOffset(double meters, double latitude);
double normalizeHeading(double heading);
double headingDifference(double h1, double h2);
LatLon projectPoint(double lat, double lon, double bearingDeg, double distanceM);

// --- Zone avoidance geometry ---

bool pointInPolygon(double lat, double lon, const Polygon& polygon);
Polygon circleToPolygon(double centerLat, double centerLon, double radiusM, int nPoints = 16);
bool segmentsIntersect2d(double ax1, double ay1, double ax2, double ay2,
                          double bx1, double by1, double bx2, double by2);
bool segmentIntersectsPolygon(double p1Lat, double p1Lon, double p2Lat, double p2Lon,
                               const Polygon& polygon);
bool isVisible(double p1Lat, double p1Lon, double p2Lat, double p2Lon,
               const std::vector<Polygon>& obstacles);
bool isVisibleVg(double p1Lat, double p1Lon, double p2Lat, double p2Lon,
                  const std::vector<Polygon>& obstacles);
Polygon polygonBuffer(const Polygon& polygon, double bufferM);

} // namespace vtol::nav

// --- Batch (Eigen) geometry ---
// Declared in BatchGeometry.h
