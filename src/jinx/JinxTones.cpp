#include "configuration.h"
#include "modules/ExternalNotificationModule.h"

/**
 * Tunes between the firmware and the keyboard view of device-ui, which lets the user pick one per kind of
 * notification.
 */
extern "C" {

/**
 * The tune (RTTTL) for a text message that just came in, or null for the configured ringtone. direct: it is
 * a direct message to this node; channel: the slot of the channel it came on. The keyboard view has the real one; this stands in where it is not built in.
 */
__attribute__((weak)) const char *jinxmesh_tone(int direct, int channel, const char *text, unsigned length)
{
    (void)direct;
    (void)channel;
    (void)text;
    (void)length;
    return nullptr;
}

/**
 * Plays a tune once, for the keyboard view: to hear a choice, and for its own alerts. The text has to stay
 * where it is while it plays. 0: there will be no sound (switched off, or no buzzer).
 */
int jinxmesh_play(const char *rtttl)
{
#if !MESHTASTIC_EXCLUDE_RTTTL && !MESHTASTIC_EXCLUDE_EXTERNALNOTIFICATION
    return externalNotificationModule && externalNotificationModule->playTone(rtttl) ? 1 : 0;
#else
    (void)rtttl;
    return 0;
#endif
}
}
