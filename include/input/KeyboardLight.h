#pragma once

/**
 * Keypad backlight on boards that wire it to a host GPIO (KB_LIGHT_PIN, active high).
 * The simulator reports a light as well and only logs the changes.
 */
class KeyboardLight
{
  public:
    static bool available(void);
    static void set(bool on);
    static bool isOn(void);
};
