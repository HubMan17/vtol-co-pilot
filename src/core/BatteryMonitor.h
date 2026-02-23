#pragma once

#include "Config.h"
#include <QObject>
#include <QString>
#include <deque>
#include <vector>
#include <chrono>

namespace vtol {

class BatteryMonitor : public QObject {
    Q_OBJECT
public:
    explicit BatteryMonitor(QObject* parent = nullptr);

    void configure(const SystemConfig& cfg);
    void update(double voltage, double current);
    void reset();

    void setAutopilotEngaged(bool engaged) { m_autopilotEngaged = engaged; }

signals:
    // Battery threshold notifications (25%, 50%, 75%)
    void batteryWarning(const QString& title, const QString& message, const QString& tag);
    // Operational zero reached (40.5V default)
    void batteryLimit(const QString& title, const QString& message);
    // Critical voltage (≤38V default)
    void batteryCritical(const QString& title, const QString& message);
    // High amperage
    void amperageWarning(const QString& title, const QString& message);

private:
    struct Threshold {
        double voltage;
        bool   triggered = false;
        QString tag;
        QString title;
        QString message;
        bool   isCritical = false;  // Critical level (red) vs Warning (orange)
    };

    void rebuildThresholds();
    double rollingAverage() const;

    SystemConfig m_cfg;

    // Rolling average buffer
    std::deque<double> m_voltageBuffer;

    // Voltage thresholds (sorted descending by voltage)
    std::vector<Threshold> m_thresholds;

    // Critical threshold (separate — below operational range)
    Threshold m_criticalThreshold;
    bool m_criticalActive = false;

    // Amperage state
    bool m_autopilotEngaged = false;
    std::chrono::steady_clock::time_point m_lastAmperageWarning;
    bool m_amperageWarningPending = false;
};

} // namespace vtol
