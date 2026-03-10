#include "MeshNavigator.h"
#include "Calculations.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QUrl>
#include <numeric>
#include <spdlog/spdlog.h>

namespace vtol {

MeshNavigator::MeshNavigator(QObject* parent)
    : QObject(parent)
{
    connect(&m_nam, &QNetworkAccessManager::finished,
            this, &MeshNavigator::onReplyFinished);
    connect(&m_pollTimer, &QTimer::timeout, this, &MeshNavigator::poll);
}

void MeshNavigator::configure(const MeshConfig& config)
{
    m_config = config;
    m_pollTimer.setInterval(m_config.poll_interval_ms);
    SPDLOG_INFO("[Mesh] configured: enabled={}, air={}, ground={}, poll={}ms, window={}, minDist={}",
                m_config.enabled, m_config.air_modem_ip,
                m_config.ground_modem_ip, m_config.poll_interval_ms,
                m_config.sliding_window_size, m_config.min_reliable_distance);
}

void MeshNavigator::start()
{
    if (!m_config.enabled) return;
    m_distanceBuffer.clear();
    m_requestPending = false;
    m_running = true;
    m_pollTimer.start();
    SPDLOG_INFO("[Mesh] started polling {}:{}", m_config.air_modem_ip, m_config.poll_interval_ms);
}

void MeshNavigator::stop()
{
    m_running = false;
    m_pollTimer.stop();
    m_distanceBuffer.clear();
    SPDLOG_INFO("[Mesh] stopped");
}

void MeshNavigator::setHomePosition(double lat, double lon)
{
    m_homeLat = lat;
    m_homeLon = lon;
    m_homeSet = true;
}

void MeshNavigator::setAircraftPosition(double lat, double lon)
{
    m_acLat = lat;
    m_acLon = lon;
    m_acSet = true;
}

void MeshNavigator::poll()
{
    if (m_requestPending) return;
    if (!m_homeSet || !m_acSet) return;

    QUrl url(QString("http://%1/status")
                 .arg(QString::fromStdString(m_config.air_modem_ip)));
    QNetworkRequest req(url);
    req.setTransferTimeout(m_config.poll_interval_ms * 2);
    m_nam.get(req);
    m_requestPending = true;
}

void MeshNavigator::onReplyFinished(QNetworkReply* reply)
{
    m_requestPending = false;
    reply->deleteLater();

    if (!m_running) return;

    if (reply->error() != QNetworkReply::NoError) {
        SPDLOG_WARN("[Mesh] HTTP error: {}", reply->errorString().toStdString());
        emit meshError(reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        SPDLOG_WARN("[Mesh] JSON parse error: {}", parseErr.errorString().toStdString());
        emit meshError(parseErr.errorString());
        return;
    }

    // Parse transmissionDelay[0].delay (nanoseconds)
    auto root = doc.object();
    auto delays = root["transmissionDelay"].toArray();
    if (delays.isEmpty()) {
        SPDLOG_WARN("[Mesh] No transmissionDelay in response");
        emit meshError(QStringLiteral("No transmissionDelay in response"));
        return;
    }

    double delayNs = delays[0].toObject()["delay"].toDouble();
    if (delayNs <= 0) {
        SPDLOG_WARN("[Mesh] Invalid delay value: {}", delayNs);
        emit meshError(QStringLiteral("Invalid delay value"));
        return;
    }

    // Convert nanoseconds to meters (speed of light / 2 for round-trip)
    double distanceM = delayNs * 0.299792458 / 2.0;

    // Sliding window average
    m_distanceBuffer.push_back(distanceM);
    while (static_cast<int>(m_distanceBuffer.size()) > m_config.sliding_window_size)
        m_distanceBuffer.pop_front();

    double avgDistance = std::accumulate(m_distanceBuffer.begin(), m_distanceBuffer.end(), 0.0)
                         / m_distanceBuffer.size();

    // Skip if too close — bearing unreliable
    if (avgDistance < m_config.min_reliable_distance) {
        SPDLOG_DEBUG("[Mesh] Distance {:.1f}m < min {:.1f}m, skipping",
                     avgDistance, m_config.min_reliable_distance);
        emit meshError(QString("Distance %.0fm < min %.0fm")
                           .arg(avgDistance).arg(m_config.min_reliable_distance));
        return;
    }

    // Compute corrected position: bearing from INS + range from mesh
    double bearing = nav::bearingTo(m_homeLat, m_homeLon, m_acLat, m_acLon);
    LatLon corrected = nav::projectPoint(m_homeLat, m_homeLon, bearing, avgDistance);

    emit correctedPosition(corrected.lat, corrected.lon, avgDistance);
}

} // namespace vtol
