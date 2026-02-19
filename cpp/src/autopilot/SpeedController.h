#pragma once

#include <algorithm>
#include <cmath>

namespace vtol {

class SpeedController {
public:
    static constexpr double MIN_SPEED = 15.0;
    static constexpr double MAX_SPEED = 35.0;

    explicit SpeedController(double targetSpeed = 20.0)
        : m_target(std::clamp(targetSpeed, MIN_SPEED, MAX_SPEED))
    {}

    void setTargetSpeed(double speed) {
        m_target = std::clamp(speed, MIN_SPEED, MAX_SPEED);
    }

    void update(double currentAirspeed) {
        m_error = m_target - currentAirspeed;
    }

    [[nodiscard]] bool isOnSpeed(double tolerance = 2.0) const { return std::abs(m_error) <= tolerance; }
    [[nodiscard]] double error() const { return m_error; }
    [[nodiscard]] double target() const { return m_target; }

    void reset() { m_error = 0; }

private:
    double m_target;
    double m_error = 0.0;
};

} // namespace vtol
