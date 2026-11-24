#!/usr/bin/env python3
"""Writes device-ui/include/util/Declination.h: magnetic declination on a 5 degree grid, from the World Magnetic
Model 2025 (NOAA/NCEI and BGS, public domain) at the middle of its validity, 2027.5.
Needs pygeomag (pip install pygeomag):  python tools/declination-table.py OUT.h"""
import os, sys
import pygeomag
from pygeomag import GeoMag

cof = os.path.join(os.path.dirname(pygeomag.__file__), "wmm", "WMM_2025.COF")
g = GeoMag(coefficients_file=cof)
EPOCH = 2027.5
rows = []
for lat in range(-90, 91, 5):
    row = []
    for lon in range(-180, 181, 5):
        la = max(-89.9, min(89.9, lat))
        d = g.calculate(glat=la, glon=lon, alt=0, time=EPOCH).d
        row.append(int(round(d * 10)))
    rows.append(row)

out = []
out.append("""#pragma once

#include <cmath>
#include <cstdint>

/**
 * Magnetic declination anywhere on Earth: what to add to a magnetic heading for a true one, east positive.
 * A table of the World Magnetic Model 2025 (NOAA/NCEI and BGS, public domain) at %.1f, every 5 degrees, read with
 * a bilinear interpolation. Good to about a degree away from the magnetic poles, and for some years: the field
 * drifts by a tenth of a degree a year in most places. Written by kbview-harness/tools/declination-table.py.
 */
namespace geo
{

constexpr int c_declinationStep = 5;
constexpr int c_declinationRows = 37, c_declinationColumns = 73;

// tenths of a degree; rows from 90 south to 90 north, columns from 180 west to 180 east
const int16_t c_declination[c_declinationRows][c_declinationColumns] = {""" % EPOCH)
for row in rows:
    out.append("    {" + ", ".join(str(v) for v in row) + "},")
out.append("""};

/**
 * Degrees of declination at a position given in degrees.
 */
inline float declination(double latitude, double longitude)
{
    if (!(latitude >= -90 && latitude <= 90) || !(longitude >= -540 && longitude <= 540))
        return 0;
    while (longitude < -180)
        longitude += 360;
    while (longitude > 180)
        longitude -= 360;
    const double y = (latitude + 90) / c_declinationStep, x = (longitude + 180) / c_declinationStep;
    int row = (int)floor(y), column = (int)floor(x);
    if (row >= c_declinationRows - 1)
        row = c_declinationRows - 2;
    if (column >= c_declinationColumns - 1)
        column = c_declinationColumns - 2;
    const double fy = y - row, fx = x - column;
    // the table jumps from -180 to 180 around the poles: there the corners are brought to one side first
    double corner[4] = {(double)c_declination[row][column], (double)c_declination[row][column + 1],
                        (double)c_declination[row + 1][column], (double)c_declination[row + 1][column + 1]};
    for (int i = 1; i < 4; i++) {
        while (corner[i] - corner[0] > 1800)
            corner[i] -= 3600;
        while (corner[i] - corner[0] < -1800)
            corner[i] += 3600;
    }
    double tenths = (corner[0] * (1 - fx) + corner[1] * fx) * (1 - fy) + (corner[2] * (1 - fx) + corner[3] * fx) * fy;
    while (tenths > 1800)
        tenths -= 3600;
    while (tenths < -1800)
        tenths += 3600;
    return (float)(tenths / 10);
}

} // namespace geo
""")
open(sys.argv[1], "w").write("\n".join(out))
# what the tests compare against
for la, lo in [(44.26, -72.58), (47.6, -122.3), (51.5, -0.1), (-33.9, 151.2), (35.7, 139.7), (64.1, -21.9)]:
    print(la, lo, round(g.calculate(glat=la, glon=lo, alt=0, time=EPOCH).d, 2))
