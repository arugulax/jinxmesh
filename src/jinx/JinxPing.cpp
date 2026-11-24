#include "configuration.h"
#include "mesh/NodeDB.h"
#include "modules/PositionModule.h"

/**
 * The ping of the keyboard view's wardrive log: the own position (as coarse as the channel has it set) to
 * everyone, asking for theirs, without hops. So only the nodes that hear this device itself get it, and each
 * answers straight back (at most once in three minutes, as their firmware has it): who answers can hear us.
 * 0: nothing was sent (no position since the start, no room to send).
 */
extern "C" int jinxmesh_ping(int channel)
{
#if !MESHTASTIC_EXCLUDE_GPS || 1
    if (!positionModule || channel < 0 || channel > 7)
        return 0;
    return positionModule->sendOurPosition(NODENUM_BROADCAST, true, (uint8_t)channel, true) ? 1 : 0;
#else
    return 0;
#endif
}
