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
    MeshConfig m_config;
    QTimer m_pollTimer;
    QNetworkAccessManager m_nam;
    bool m_requestPending = false;
    bool m_running = false;

    double m_homeLat = 0, m_homeLon = 0;
    bool m_homeSet = false;
    double m_acLat = 0, m_acLon = 0;
    bool m_acSet = false;

    std::deque<double> m_distanceBuffer;
};

} // namespace vtol
