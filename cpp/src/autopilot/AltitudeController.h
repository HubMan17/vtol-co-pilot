#pragma once

#include <cmath>

namespace vtol {

class AltitudeController {
public:
    void setTargetAltitude(double altitude) { m_target = altitude; }

    void update(double currentAltitude) {
        m_error = m_target - currentAltitude;
    }

    [[nodiscard]] bool isOnAltitude(double tolerance = 5.0) const { return std::abs(m_error) <= tolerance; }
    [[nodiscard]] double error() const { return m_error; }
    [[nodiscard]] double target() const { return m_target; }

    void reset() { m_error = 0; m_target = 0; }

private:
    double m_target = 0.0;
    double m_error = 0.0;
};

} // namespace vtol
