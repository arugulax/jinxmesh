// The declination table against the model it was made from (WMM 2025 at 2027.5, by pygeomag).
#include "util/Declination.h"
#include <cmath>
#include <doctest/doctest.h>

TEST_CASE("declination follows the model within a degree")
{
    CHECK(fabs(geo::declination(44.26, -72.58) - -13.67) < 1.0);
    CHECK(fabs(geo::declination(47.6, -122.3) - 14.77) < 1.0);
    CHECK(fabs(geo::declination(51.5, -0.1) - 1.34) < 1.0);
    CHECK(fabs(geo::declination(-33.9, 151.2) - 12.85) < 1.0);
    CHECK(fabs(geo::declination(35.7, 139.7) - -7.97) < 1.0);
    CHECK(fabs(geo::declination(64.1, -21.9) - -10.8) < 1.0);
}

TEST_CASE("declination takes any position")
{
    CHECK(geo::declination(90, 180) == geo::declination(90, 180));
    CHECK(geo::declination(-90, -180) > -181);
    CHECK(geo::declination(0, 190) == doctest::Approx(geo::declination(0, -170)));
    CHECK(geo::declination(NAN, 0) == 0);
    CHECK(geo::declination(0, 1e9) == 0);
}
