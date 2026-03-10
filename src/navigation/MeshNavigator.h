#pragma once

#include <QObject>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <deque>
#include "core/Config.h"

namespace vtol {

class MeshNavigator : public QObject {
    Q_OBJECT
public:
    explicit MeshNavigator(QObject* parent = nullptr);

    void configure(const MeshConfig& config);
    void start();
    void stop();

    void setHomePosition(double lat, double lon);
    void setAircraftPosition(double lat, double lon);

signals:
    void correctedPosition(double lat, double lon, double meshDistanceM);
    void meshError(const QString& message);

private slots:
    void poll();
    void onReplyFinished(QNetworkReply* reply);

private:
    // NED conversion (meters from home)
    double toNorth(double lat) const;
    double toEast(double lon) const;
    double fromNorthToLat(double north) const;
    double fromEastToLon(double east) const;

    // Outlier rejection: returns true if sample should be rejected
    bool isOutlier(double rangeM) const;

    // Check if sample set has enough lateral spread for least squares
    bool hasGeometry() const;

    // Gauss-Newton solver: returns (north, east) in NED from home
    std::pair<double, double> solveLeastSquares() const;

    // RMS residual of a solution
    double computeResidual(double north, double east) const;

    // Fallback: bearing(INS) + avg range
    std::pair<double, double> fallbackBearingRange() const;

    struct MeshSample {
        double rangeM;       // mesh distance in meters
        double insNorth;     // INS position in NED meters from home
        double insEast;
    };

    MeshConfig m_config;
    QTimer m_pollTimer;
    QNetworkAccessManager m_nam;
    bool m_requestPending = false;
    bool m_running = false;

    double m_homeLat = 0, m_homeLon = 0;
    double m_cosHomeLat = 1.0;
    bool m_homeSet = false;
    double m_acLat = 0, m_acLon = 0;
    bool m_acSet = false;

    std::deque<MeshSample> m_samples;

    static constexpr double DEG_TO_M = 111320.0;
    static constexpr double MAX_RESIDUAL_M = 200.0;
    static constexpr double MIN_GEOMETRY_SPREAD_M = 50.0;
    static constexpr double OUTLIER_THRESHOLD_M = 100.0;
    static constexpr int MIN_SAMPLES_FOR_LS = 3;
    static constexpr int MAX_GN_ITERATIONS = 15;
};

} // namespace vtol
