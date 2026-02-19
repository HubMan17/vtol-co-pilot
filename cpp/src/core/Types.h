#pragma once

#include <cmath>
#include <tuple>
#include <vector>

namespace vtol {

struct LatLon {
    double lat = 0.0;
    double lon = 0.0;

    constexpr LatLon() = default;
    constexpr LatLon(double lat, double lon) : lat(lat), lon(lon) {}

    [[nodiscard]] constexpr std::tuple<double, double> toTuple() const { return {lat, lon}; }
    [[nodiscard]] constexpr bool isValid() const { return lat != 0.0 || lon != 0.0; }

    constexpr bool operator==(const LatLon& o) const { return lat == o.lat && lon == o.lon; }
    constexpr bool operator!=(const LatLon& o) const { return !(*this == o); }
};

using Polygon = std::vector<LatLon>;

struct PidCoeffs {
    double p = 0.0;
    double i = 0.0;
    double d = 0.0;
};

} // namespace vtol
