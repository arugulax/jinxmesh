#include "configuration.h"

#if defined(USE_KEYBOARD_VIEW) && !MESHTASTIC_EXCLUDE_GPS

#include "GPS.h"

/**
 * What the keyboard view of device-ui asks the firmware about the position while it navigates. device-ui has
 * weak stand-ins for these, so it also links where there is no GPS code (the simulator, other boards).
 */
extern "C" {

/**
 * The last fix of the receiver, degrees times 1e7, and its age. 0: none yet.
 */
int jinxmesh_fix(int32_t *latitude, int32_t *longitude, uint32_t *ageMs)
{
    int32_t lat = 0, lon = 0;
    uint32_t age = 0;
    if (!gps || !GPS::lastFix(lat, lon, age))
        return 0;
    if (latitude)
        *latitude = lat;
    if (longitude)
        *longitude = lon;
    if (ageMs)
        *ageMs = age;
    return 1;
}

/**
 * Keeps the receiver on, with a fix every second, for that many seconds from now. 0 lets it go back to its
 * own schedule.
 */
void jinxmesh_track(unsigned seconds)
{
    GPS::keepTracking(seconds * 1000UL);
}
}

#endif
