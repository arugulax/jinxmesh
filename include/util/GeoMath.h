#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

/**
 * Distances and directions between positions as the mesh carries them (degrees times 1e7).
 * No screen code in here, so it can be tested without a display.
 */
namespace geo
{

struct Leg {
    double meters;  // along the great circle
    double bearing; // at the start, degrees clockwise from true north, 0 up to 360
};

inline Leg leg(int32_t fromLat, int32_t fromLon, int32_t toLat, int32_t toLon)
{
    const double rad = M_PI / 180.0;
    const double lat1 = fromLat * 1e-7 * rad, lat2 = toLat * 1e-7 * rad;
    // the difference as a double: across half the globe it does not fit the integers the positions come in
    const double dLat = lat2 - lat1, dLon = ((double)toLon - (double)fromLon) * 1e-7 * rad;
    const double a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1) * cos(lat2) * sin(dLon / 2) * sin(dLon / 2);
    Leg result;
    result.meters = 6371000.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
    result.bearing = atan2(sin(dLon) * cos(lat2), cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon)) / rad;
    if (result.bearing < 0)
        result.bearing += 360.0;
    return result;
}

inline const char *cardinal(double bearing)
{
    static const char *const names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return names[(int)lround(bearing / 45.0) & 7];
}

/**
 * How far a position shared with reduced precision can be off, in meters; 0 for an exact one.
 * A sender keeps the upper `bits` bits of latitude and longitude and puts the position in the
 * middle of what is left, so the error is up to half a cell.
 */
inline double precisionRadius(uint32_t bits)
{
    if (bits == 0 || bits >= 32)
        return 0.0;
    return ldexp(1.0, 32 - (int)bits) * 1e-7 * 111320.0 / 2.0;
}

/**
 * "850 m", "3.4 km", "12 km"; "900 ft", "2.1 mi", "12 mi".
 */
inline void distanceText(double meters, bool imperial, char *buf, size_t len)
{
    const double small = imperial ? meters * 3.28084 : meters;
    const double large = imperial ? meters / 1609.344 : meters / 1000.0;
    if (small < 1000)
        snprintf(buf, len, "%d %s", (int)lround(small), imperial ? "ft" : "m");
    else if (large < 100)
        snprintf(buf, len, "%.1f %s", large, imperial ? "mi" : "km");
    else
        snprintf(buf, len, "%d %s", (int)lround(large), imperial ? "mi" : "km");
}

} // namespace geo
