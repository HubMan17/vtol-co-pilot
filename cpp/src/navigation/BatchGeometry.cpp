#include "BatchGeometry.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vtol::nav {

constexpr double BATCH_EARTH_RADIUS = 6371000.0;

Eigen::Matrix<bool, Eigen::Dynamic, Eigen::Dynamic>
batchSegmentsIntersect(const Eigen::MatrixXd& segsA, const Eigen::MatrixXd& segsB)
{
    int P = static_cast<int>(segsA.rows());
    int E = static_cast<int>(segsB.rows());

    Eigen::VectorXd ax1 = segsA.col(0), ay1 = segsA.col(1);
    Eigen::VectorXd ax2 = segsA.col(2), ay2 = segsA.col(3);
    Eigen::VectorXd bx1 = segsB.col(0), by1 = segsB.col(1);
    Eigen::VectorXd bx2 = segsB.col(2), by2 = segsB.col(3);

    Eigen::VectorXd dxA = ax2 - ax1;  // (P,)
    Eigen::VectorXd dyA = ay2 - ay1;
    Eigen::VectorXd dxB = bx2 - bx1;  // (E,)
    Eigen::VectorXd dyB = by2 - by1;

    // (P,1) * (1,E) → (P,E)
    Eigen::MatrixXd denom = dxA * dyB.transpose() - dyA * dxB.transpose();

    // (P,E) offsets
    Eigen::MatrixXd dbx = bx1.transpose().replicate(P, 1) - ax1.replicate(1, E);
    Eigen::MatrixXd dby = by1.transpose().replicate(P, 1) - ay1.replicate(1, E);

    // Safe denominator
    Eigen::MatrixXd safeDenom = denom.array().abs().max(1e-15).matrix();
    safeDenom = (denom.array() < 0).select(-safeDenom, safeDenom);

    Eigen::MatrixXd t = (dbx.array() * dyB.transpose().replicate(P, 1).array() -
                          dby.array() * dxB.transpose().replicate(P, 1).array()) / safeDenom.array();
    Eigen::MatrixXd u = (dbx.array() * dyA.replicate(1, E).array() -
                          dby.array() * dxA.replicate(1, E).array()) / safeDenom.array();

    auto absDenom = denom.array().abs();
    Eigen::Matrix<bool, Eigen::Dynamic, Eigen::Dynamic> result =
        (absDenom >= 1e-15) && (t.array() > 0) && (t.array() < 1) &&
        (u.array() > 0) && (u.array() < 1);

    return result;
}

Eigen::Matrix<bool, Eigen::Dynamic, 1>
batchPointInPolygon(const Eigen::MatrixXd& points, const Eigen::MatrixXd& polygon)
{
    int M = static_cast<int>(points.rows());
    int N = static_cast<int>(polygon.rows());

    Eigen::Matrix<bool, Eigen::Dynamic, 1> inside =
        Eigen::Matrix<bool, Eigen::Dynamic, 1>::Constant(M, false);

    if (N < 3) return inside;

    Eigen::VectorXd plat = points.col(0);
    Eigen::VectorXd plon = points.col(1);

    int j = N - 1;
    for (int i = 0; i < N; ++i) {
        double yi = polygon(i, 0), xi = polygon(i, 1);
        double yj = polygon(j, 0), xj = polygon(j, 1);

        double dy = yj - yi;
        if (std::abs(dy) < 1e-15) { j = i; continue; }

        auto cond = ((plat.array() < yi) != (plat.array() < yj));
        Eigen::VectorXd xIntersect = ((xj - xi) * (plat.array() - yi) / dy + xi).matrix();
        auto flip = cond && (plon.array() < xIntersect.array());

        for (int m = 0; m < M; ++m) {
            if (flip(m)) inside(m) = !inside(m);
        }
        j = i;
    }
    return inside;
}

Eigen::VectorXd
batchHaversine(const Eigen::VectorXd& lat1, const Eigen::VectorXd& lon1,
               const Eigen::VectorXd& lat2, const Eigen::VectorXd& lon2)
{
    Eigen::VectorXd lat1R = lat1 * M_PI / 180.0;
    Eigen::VectorXd lat2R = lat2 * M_PI / 180.0;
    Eigen::VectorXd dLat = (lat2 - lat1) * M_PI / 180.0;
    Eigen::VectorXd dLon = (lon2 - lon1) * M_PI / 180.0;

    Eigen::VectorXd a = (dLat / 2).array().sin().square() +
                         lat1R.array().cos() * lat2R.array().cos() *
                         (dLon / 2).array().sin().square();
    Eigen::VectorXd c = 2 * a.array().sqrt().binaryExpr(
        (1 - a.array()).sqrt(),
        [](double a_, double b_) { return std::atan2(a_, b_); });

    return BATCH_EARTH_RADIUS * c;
}

} // namespace vtol::nav
