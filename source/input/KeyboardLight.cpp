#include "input/KeyboardLight.h"
#include "util/ILog.h"

#if defined(KB_LIGHT_PIN) && !defined(ARCH_PORTDUINO)
#include <Arduino.h>
#endif

namespace
{
bool lightOn = false;
bool lightReady = false;
} // namespace

bool KeyboardLight::available(void)
{
#if defined(KB_LIGHT_PIN) || defined(ARCH_PORTDUINO)
    return true;
#else
    return false;
#endif
}

void KeyboardLight::set(bool on)
{
    if (lightReady && on == lightOn)
        return;
#if defined(KB_LIGHT_PIN) && !defined(ARCH_PORTDUINO)
    if (!lightReady)
        pinMode(KB_LIGHT_PIN, OUTPUT);
    digitalWrite(KB_LIGHT_PIN, on ? HIGH : LOW);
#elif defined(ARCH_PORTDUINO)
    ILOG_DEBUG("keyboard light %s", on ? "on" : "off");
#endif
    lightReady = true;
    lightOn = on;
}

bool KeyboardLight::isOn(void)
{
    return lightOn;
}
