#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QMap>
#include <spdlog/spdlog.h>
#include <chrono>

namespace vtol {

/// Lightweight per-section performance logger.
/// Usage: call begin("section"), end("section") around code,
/// then every N calls to report() it logs all section stats.
class PerfLogger {
public:
    explicit PerfLogger(const char* context, int reportEveryN = 100)
        : m_context(context), m_reportEvery(reportEveryN) {}

    void begin(const char* section) {
        m_starts[section] = nowUs();
    }

    void end(const char* section) {
        auto it = m_starts.find(section);
        if (it == m_starts.end()) return;
        double elapsed = nowUs() - it->second;
        m_starts.erase(it);
        auto& stat = m_stats[section];
        stat.totalUs += elapsed;
        stat.calls++;
        if (elapsed > stat.maxUs) stat.maxUs = elapsed;
    }

    /// Call once per update loop. Logs every reportEveryN calls.
    void tick() {
        m_tickCount++;
        if (m_tickCount < m_reportEvery) return;
        m_tickCount = 0;

        // Build log line
        std::string msg = m_context;
        msg += " [per-tick avg/max μs]:";
        for (auto& [name, stat] : m_stats) {
            if (stat.calls == 0) continue;
            double avg = stat.totalUs / stat.calls;
            msg += "  ";
            msg += name;
            msg += "=";
            msg += fmt::format("{:.0f}", avg);
            msg += "/";
            msg += fmt::format("{:.0f}", stat.maxUs);
            // Reset
            stat.totalUs = 0;
            stat.calls = 0;
            stat.maxUs = 0;
        }
        SPDLOG_INFO("{}", msg);
    }

private:
    static double nowUs() {
        using clock = std::chrono::steady_clock;
        static auto epoch = clock::now();
        return std::chrono::duration<double, std::micro>(clock::now() - epoch).count();
    }

    struct Stat { double totalUs = 0; double maxUs = 0; int calls = 0; };

    const char* m_context;
    int m_reportEvery;
    int m_tickCount = 0;
    std::unordered_map<const char*, double> m_starts;
    std::map<const char*, Stat> m_stats;
};

/// RAII scope guard that calls begin/end automatically.
struct PerfScope {
    PerfLogger& logger;
    const char* name;
    PerfScope(PerfLogger& l, const char* n) : logger(l), name(n) { logger.begin(n); }
    ~PerfScope() { logger.end(name); }
};

} // namespace vtol
