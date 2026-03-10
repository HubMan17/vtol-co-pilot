#include "MeshNavigator.h"
#include "Calculations.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QUrl>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    m_samples.clear();
    m_requestPending = false;
    m_running = true;
    m_pollTimer.start();
    SPDLOG_INFO("[Mesh] started polling {}:{}", m_config.air_modem_ip, m_config.poll_interval_ms);
}

void MeshNavigator::stop()
{
    m_running = false;
    m_pollTimer.stop();
    m_samples.clear();
    SPDLOG_INFO("[Mesh] stopped");
}

void MeshNavigator::setHomePosition(double lat, double lon)
{
    m_homeLat = lat;
    m_homeLon = lon;
    m_cosHomeLat = std::cos(lat * M_PI / 180.0);
    m_homeSet = true;
}

void MeshNavigator::setAircraftPosition(double lat, double lon)
{
    m_acLat = lat;
    m_acLon = lon;
    m_acSet = true;
}

// ═══════════════════════ NED conversion ═══════════════════════

double MeshNavigator::toNorth(double lat) const
{
    return (lat - m_homeLat) * DEG_TO_M;
}

double MeshNavigator::toEast(double lon) const
{
    return (lon - m_homeLon) * DEG_TO_M * m_cosHomeLat;
}

double MeshNavigator::fromNorthToLat(double north) const
{
    return m_homeLat + north / DEG_TO_M;
}

double MeshNavigator::fromEastToLon(double east) const
{
    return m_homeLon + east / (DEG_TO_M * m_cosHomeLat);
}

// ═══════════════════════ Outlier rejection ═══════════════════════

bool MeshNavigator::isOutlier(double rangeM) const
{
    if (m_samples.size() < 3) return false;

    // Compute median of existing ranges
    std::vector<double> ranges;
    ranges.reserve(m_samples.size());
    for (const auto& s : m_samples)
        ranges.push_back(s.rangeM);
    std::sort(ranges.begin(), ranges.end());
    double median = ranges[ranges.size() / 2];

    return std::abs(rangeM - median) > OUTLIER_THRESHOLD_M;
}

// ═══════════════════════ Geometry check ═══════════════════════

bool MeshNavigator::hasGeometry() const
{
    if (static_cast<int>(m_samples.size()) < MIN_SAMPLES_FOR_LS) return false;

    // Bounding box of INS positions in window
    double minN = m_samples[0].insNorth, maxN = minN;
    double minE = m_samples[0].insEast, maxE = minE;
    for (const auto& s : m_samples) {
        minN = std::min(minN, s.insNorth); maxN = std::max(maxN, s.insNorth);
        minE = std::min(minE, s.insEast);  maxE = std::max(maxE, s.insEast);
    }

    double spreadN = maxN - minN;
    double spreadE = maxE - minE;
    double spread = std::sqrt(spreadN * spreadN + spreadE * spreadE);

    return spread >= MIN_GEOMETRY_SPREAD_M;
}

// ═══════════════════════ Least squares solver ═══════════════════════

std::pair<double, double> MeshNavigator::solveLeastSquares() const
{
    // Solve for corrected position (N, E) in NED from home at latest sample time.
    //
    // Each sample i gives constraint:
    //   sqrt((N + dn_i)^2 + (E + de_i)^2) = range_i
    //
    // where (dn_i, de_i) = INS offset from latest sample to sample i
    // (relative INS is accurate over short intervals).
    //
    // Gauss-Newton minimizes sum of (predicted_dist - range_i)^2.

    const auto& latest = m_samples.back();

    // Initial guess: current INS position in NED
    double x = latest.insNorth;
    double y = latest.insEast;

    for (int iter = 0; iter < MAX_GN_ITERATIONS; ++iter) {
        // J^T J (2x2) and J^T r (2x1)
        double JtJ00 = 0, JtJ01 = 0, JtJ11 = 0;
        double Jtr0 = 0, Jtr1 = 0;

        for (const auto& s : m_samples) {
            // Position of aircraft at sample time in corrected frame
            double dn = s.insNorth - latest.insNorth;
            double de = s.insEast  - latest.insEast;
            double px = x + dn;
            double py = y + de;

            double dist = std::sqrt(px * px + py * py);
            if (dist < 1.0) dist = 1.0;

            double residual = dist - s.rangeM;
            double jx = px / dist;
            double jy = py / dist;

            JtJ00 += jx * jx;
            JtJ01 += jx * jy;
            JtJ11 += jy * jy;
            Jtr0  += jx * residual;
            Jtr1  += jy * residual;
        }

        // Solve 2x2: (J^T J) delta = J^T r
        double det = JtJ00 * JtJ11 - JtJ01 * JtJ01;
        if (std::abs(det) < 1e-10) break;

        double dx = ( JtJ11 * Jtr0 - JtJ01 * Jtr1) / det;
        double dy = (-JtJ01 * Jtr0 + JtJ00 * Jtr1) / det;

        x -= dx;
        y -= dy;

        if (dx * dx + dy * dy < 0.01) break;  // converged (<0.1m step)
    }

    return {x, y};
}

double MeshNavigator::computeResidual(double north, double east) const
{
    const auto& latest = m_samples.back();
    double sumSq = 0;
    for (const auto& s : m_samples) {
        double dn = s.insNorth - latest.insNorth;
        double de = s.insEast  - latest.insEast;
        double px = north + dn;
        double py = east  + de;
        double dist = std::sqrt(px * px + py * py);
        double err = dist - s.rangeM;
        sumSq += err * err;
    }
    return std::sqrt(sumSq / m_samples.size());
}

// ═══════════════════════ Fallback ═══════════════════════

std::pair<double, double> MeshNavigator::fallbackBearingRange() const
{
    // Average range
    double sum = 0;
    for (const auto& s : m_samples)
        sum += s.rangeM;
    double avgRange = sum / m_samples.size();

    // Bearing from INS
    double bearing = nav::bearingTo(m_homeLat, m_homeLon, m_acLat, m_acLon);
    double bearingRad = bearing * M_PI / 180.0;

    double north = avgRange * std::cos(bearingRad);
    double east  = avgRange * std::sin(bearingRad);
    return {north, east};
}

// ═══════════════════════ Polling ═══════════════════════

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

    // Outlier rejection (median filter)
    if (isOutlier(distanceM)) {
        SPDLOG_WARN("[Mesh] Outlier rejected: {:.1f}m", distanceM);
        return;
    }

    // Store sample with INS position in NED
    MeshSample sample;
    sample.rangeM   = distanceM;
    sample.insNorth = toNorth(m_acLat);
    sample.insEast  = toEast(m_acLon);
    m_samples.push_back(sample);

    // Trim to window size
    while (static_cast<int>(m_samples.size()) > m_config.sliding_window_size)
        m_samples.pop_front();

    if (m_samples.empty()) return;

    // Average range for min_reliable_distance check
    double avgRange = 0;
    for (const auto& s : m_samples) avgRange += s.rangeM;
    avgRange /= m_samples.size();

    if (avgRange < m_config.min_reliable_distance) {
        SPDLOG_DEBUG("[Mesh] Distance {:.1f}m < min {:.1f}m, skipping",
                     avgRange, m_config.min_reliable_distance);
        return;
    }

    // Choose solver
    double corrN, corrE;
    bool usedLS = false;

    if (hasGeometry()) {
        auto [n, e] = solveLeastSquares();
        double residual = computeResidual(n, e);

        if (residual > MAX_RESIDUAL_M) {
            SPDLOG_WARN("[Mesh] LS residual too high: {:.1f}m, falling back", residual);
            auto [fn, fe] = fallbackBearingRange();
            corrN = fn;
            corrE = fe;
        } else {
            corrN = n;
            corrE = e;
            usedLS = true;
            SPDLOG_DEBUG("[Mesh] LS solution: N={:.1f} E={:.1f}, residual={:.1f}m, samples={}",
                         n, e, residual, m_samples.size());
        }
    } else {
        auto [fn, fe] = fallbackBearingRange();
        corrN = fn;
        corrE = fe;
        SPDLOG_DEBUG("[Mesh] Fallback bearing+range, samples={}, avgRange={:.1f}m",
                     m_samples.size(), avgRange);
    }

    // Convert NED back to lat/lon
    double corrLat = fromNorthToLat(corrN);
    double corrLon = fromEastToLon(corrE);

    // Distance from home for the signal
    double corrDist = std::sqrt(corrN * corrN + corrE * corrE);

    SPDLOG_INFO("[Mesh] corrected: {:.6f}, {:.6f} dist={:.1f}m method={} samples={}",
                corrLat, corrLon, corrDist, usedLS ? "LS" : "bearing", m_samples.size());

    emit correctedPosition(corrLat, corrLon, corrDist);
}

} // namespace vtol
