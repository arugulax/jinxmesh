#include "configuration.h"

#if defined(USE_KEYBOARD_VIEW) && !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && !MESHTASTIC_EXCLUDE_MAGNETOMETER

#include "main.h"
#include "motion/MagnetometerThread.h"
#include "motion/MotionSensor.h"

/**
 * What the keyboard view of device-ui asks the firmware about the compass. device-ui has weak stand-ins for
 * these, so it also links where there is no compass (the simulator, other boards).
 */
extern "C" {

/**
 * 0: no compass on this device. 1: *heading holds degrees from magnetic north, 0 to 360. 2: being calibrated.
 * 3: there is a compass, a first reading is on its way. Asking keeps the sensor reading for a few seconds.
 */
int jinxmesh_compass(float *heading)
{
    if (!magnetometerThread)
        return 0;
    float degrees = 0.0f;
    uint32_t ageMs = 0;
    const bool known = MotionSensor::getLatestCompassHeading(degrees, ageMs);
    if (MotionSensor::compassCalibrating())
        return 2;
    if (!known || ageMs > 2000)
        return 3;
    if (heading)
        *heading = degrees;
    return 1;
}

/**
 * Starts a calibration: the device is turned every way for that many seconds, then the result is stored.
 */
void jinxmesh_compass_calibrate(unsigned seconds)
{
    if (magnetometerThread)
        MotionSensor::requestCompassCalibration((uint16_t)seconds);
}
}

#endif
