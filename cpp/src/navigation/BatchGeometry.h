#pragma once

#include <Eigen/Dense>

namespace vtol::nav {

/// Vectorized intersection test between two sets of line segments.
/// segsA: (P, 4) — [lat1, lon1, lat2, lon2] per row
/// segsB: (E, 4) — [lat1, lon1, lat2, lon2] per row
/// Returns: (P, E) bool matrix — result(i,j) true when segA[i] crosses segB[j].
Eigen::Matrix<bool, Eigen::Dynamic, Eigen::Dynamic>
batchSegmentsIntersect(const Eigen::MatrixXd& segsA, const Eigen::MatrixXd& segsB);

/// Ray-casting for M points against one polygon (vectorized over points).
/// points: (M, 2) — [lat, lon] per row
/// polygon: (N, 2) — [lat, lon] per vertex
/// Returns: (M,) bool vector — true if point is inside polygon.
Eigen::Matrix<bool, Eigen::Dynamic, 1>
batchPointInPolygon(const Eigen::MatrixXd& points, const Eigen::MatrixXd& polygon);

/// Vectorized haversine distance in meters.
Eigen::VectorXd
batchHaversine(const Eigen::VectorXd& lat1, const Eigen::VectorXd& lon1,
               const Eigen::VectorXd& lat2, const Eigen::VectorXd& lon2);

} // namespace vtol::nav
