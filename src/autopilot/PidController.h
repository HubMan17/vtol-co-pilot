#pragma once

#include <algorithm>
#include <cmath>

namespace vtol {

struct PidState {
    double pTerm = 0.0;
    double iTerm = 0.0;
    double dTerm = 0.0;
    double output = 0.0;
    double error = 0.0;
};

class PidController {
public:
    PidController(double kp = 0, double ki = 0, double kd = 0,
                  double outputMin = -1.0, double outputMax = 1.0)
        : m_kp(kp), m_ki(ki), m_kd(kd)
        , m_outputMin(outputMin), m_outputMax(outputMax)
        , m_integralLimit(std::abs(outputMax))
    {}

    double update(double error, double dt) {
        if (dt <= 0) return m_state.output;

        double pTerm = m_kp * error;

        m_integral += error * dt;
        m_integral = std::clamp(m_integral, -m_integralLimit, m_integralLimit);
        double iTerm = m_ki * m_integral;

        double dTerm = 0.0;
        if (m_hasLastError) {
            dTerm = m_kd * (error - m_lastError) / dt;
        }
        m_lastError = error;
        m_hasLastError = true;

        double output = std::clamp(pTerm + iTerm + dTerm, m_outputMin, m_outputMax);

        m_state = {pTerm, iTerm, dTerm, output, error};
        return output;
    }

    void reset() {
        m_integral = 0;
        m_lastError = 0;
        m_hasLastError = false;
        m_state = {};
    }

    void setGains(double kp, double ki, double kd) { m_kp = kp; m_ki = ki; m_kd = kd; }
    void setOutputLimits(double min, double max) { m_outputMin = min; m_outputMax = max; }
    void setIntegralLimit(double limit) { m_integralLimit = limit; }
    [[nodiscard]] const PidState& state() const { return m_state; }

private:
    double m_kp, m_ki, m_kd;
    double m_outputMin, m_outputMax;
    double m_integralLimit;
    double m_integral = 0;
    double m_lastError = 0;
    bool   m_hasLastError = false;
    PidState m_state;
};

} // namespace vtol
