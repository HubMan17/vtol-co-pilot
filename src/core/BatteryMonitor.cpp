#include "BatteryMonitor.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace vtol {

BatteryMonitor::BatteryMonitor(QObject* parent)
    : QObject(parent)
{
    m_lastAmperageWarning = std::chrono::steady_clock::now()
        - std::chrono::seconds(9999);  // allow first warning immediately
    rebuildThresholds();
}

void BatteryMonitor::configure(const SystemConfig& cfg)
{
    m_cfg = cfg;
    rebuildThresholds();
    SPDLOG_INFO("[BatteryMonitor::configure] v_max={:.1f}, v_op_zero={:.1f}, "
                "v_crit={:.1f}, hysteresis={:.1f}, amp_warn={:.1f}",
                cfg.v_max, cfg.v_operational_zero, cfg.v_critical,
                cfg.hysteresis_margin, cfg.amperage_warning);
}

void BatteryMonitor::reset()
{
    m_voltageBuffer.clear();
    for (auto& t : m_thresholds)
        t.triggered = false;
    m_criticalActive = false;
    m_amperageWarningPending = false;
    SPDLOG_INFO("[BatteryMonitor::reset] all thresholds cleared");
}

void BatteryMonitor::rebuildThresholds()
{
    m_thresholds.clear();

    const double range = m_cfg.v_max - m_cfg.v_operational_zero;
    if (range <= 0) return;

    // 25% consumed → 75% remaining
    const double v25 = m_cfg.v_max - 0.25 * range;
    m_thresholds.push_back({
        v25, false,
        QStringLiteral("batt-25"),
        QStringLiteral("Заряд 75%"),
        QStringLiteral("Потрачено 25% заряда батареи"),
        false
    });

    // 50% consumed → 50% remaining
    const double v50 = m_cfg.v_max - 0.50 * range;
    m_thresholds.push_back({
        v50, false,
        QStringLiteral("batt-50"),
        QStringLiteral("Заряд 50%"),
        QStringLiteral("Потрачено 50% заряда батареи"),
        false
    });

    // 75% consumed → 25% remaining
    const double v75 = m_cfg.v_max - 0.75 * range;
    m_thresholds.push_back({
        v75, false,
        QStringLiteral("batt-75"),
        QStringLiteral("Заряд 25%"),
        QString::fromUtf8("Потрачено 75% заряда \u2014 рекомендуется возврат"),
        false
    });

    // 100% consumed → operational zero
    m_thresholds.push_back({
        m_cfg.v_operational_zero, false,
        QStringLiteral("batt-limit"),
        QStringLiteral("Заряд исчерпан"),
        QString::fromUtf8("Достигнут лимит батареи \u2014 необходима посадка"),
        true  // Critical level
    });

    // Critical threshold (separate)
    m_criticalThreshold = {
        m_cfg.v_critical, false,
        QStringLiteral("batt-critical"),
        QStringLiteral("КРИТИЧЕСКИЙ ЗАРЯД"),
        QString::fromUtf8("Напряжение ниже %.1f В \u2014 риск потери тяги!").arg(m_cfg.v_critical),
        true
    };

    SPDLOG_DEBUG("[BatteryMonitor::rebuildThresholds] 25%={:.1f}V, 50%={:.1f}V, "
                 "75%={:.1f}V, limit={:.1f}V, critical={:.1f}V",
                 v25, v50, v75, m_cfg.v_operational_zero, m_cfg.v_critical);
}

double BatteryMonitor::rollingAverage() const
{
    if (m_voltageBuffer.empty()) return 0.0;
    double sum = 0;
    for (double v : m_voltageBuffer) sum += v;
    return sum / static_cast<double>(m_voltageBuffer.size());
}

void BatteryMonitor::update(double voltage, double current)
{
    if (voltage < 0.5) return;  // no data

    // Update rolling average buffer
    m_voltageBuffer.push_back(voltage);
    while (static_cast<int>(m_voltageBuffer.size()) > m_cfg.rolling_avg_samples)
        m_voltageBuffer.pop_front();

    const double avg = rollingAverage();

    // --- Voltage thresholds ---
    for (auto& t : m_thresholds) {
        if (!t.triggered && avg < t.voltage) {
            // Threshold triggered
            t.triggered = true;
            SPDLOG_WARN("[BatteryMonitor] threshold triggered: {} at {:.1f}V (avg={:.1f}V)",
                        t.tag.toStdString(), t.voltage, avg);

            if (t.tag == QStringLiteral("batt-limit")) {
                emit batteryLimit(t.title, t.message);
            } else {
                emit batteryWarning(t.title, t.message, t.tag);
            }
        }
        else if (t.triggered && avg > t.voltage + m_cfg.hysteresis_margin) {
            // Threshold reset (voltage recovered above hysteresis band)
            t.triggered = false;
            SPDLOG_INFO("[BatteryMonitor] threshold reset: {} at {:.1f}V (margin={:.1f}V)",
                        t.tag.toStdString(), avg, m_cfg.hysteresis_margin);
        }
    }

    // --- Critical threshold (below operational range) ---
    if (!m_criticalActive && avg <= m_cfg.v_critical) {
        m_criticalActive = true;
        SPDLOG_ERROR("[BatteryMonitor] CRITICAL: voltage {:.1f}V <= {:.1f}V!",
                     avg, m_cfg.v_critical);
        emit batteryCritical(
            m_criticalThreshold.title,
            QString("Напряжение %1 В — риск потери тяги!")
                .arg(avg, 0, 'f', 1));
    }
    else if (m_criticalActive && avg > m_cfg.v_critical + m_cfg.hysteresis_margin) {
        m_criticalActive = false;
        SPDLOG_INFO("[BatteryMonitor] critical reset: {:.1f}V > {:.1f}V",
                    avg, m_cfg.v_critical + m_cfg.hysteresis_margin);
    }

    // --- Amperage monitoring ---
    if (current > m_cfg.amperage_warning) {
        if (!m_autopilotEngaged) {
            SPDLOG_DEBUG("[BatteryMonitor] amperage {:.1f}A > {:.1f}A but autopilot not engaged",
                         current, m_cfg.amperage_warning);
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - m_lastAmperageWarning).count();

        if (elapsed >= m_cfg.amperage_debounce_sec) {
            m_lastAmperageWarning = now;
            SPDLOG_WARN("[BatteryMonitor] high current: {:.1f}A > {:.1f}A threshold",
                        current, m_cfg.amperage_warning);
            emit amperageWarning(
                QStringLiteral("Высокий ток"),
                QString("Потребление %1 А — риск перегрузки")
                    .arg(current, 0, 'f', 1));
        } else {
            SPDLOG_DEBUG("[BatteryMonitor] amperage warning debounced: {}s of {}s",
                         elapsed, m_cfg.amperage_debounce_sec);
        }
    }
}

} // namespace vtol
