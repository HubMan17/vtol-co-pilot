#pragma once

#include "navigation/Calculations.h"
#include <cmath>

namespace vtol {

class HeadingController {
public:
    void setTargetHeading(double heading) {
        m_target = nav::normalizeHeading(heading);
    }

    void update(double currentHeading) {
        m_error = nav::headingDifference(currentHeading, m_target);
    }

    [[nodiscard]] int turnDirection() const { return m_error >= 0 ? 1 : -1; }
    [[nodiscard]] bool isOnHeading(double tolerance = 5.0) const { return std::abs(m_error) <= tolerance; }
    [[nodiscard]] double error() const { return m_error; }
    [[nodiscard]] double target() const { return m_target; }

    void reset() { m_error = 0; m_target = 0; }

private:
    double m_target = 0.0;
    double m_error = 0.0;
};

} // namespace vtol
