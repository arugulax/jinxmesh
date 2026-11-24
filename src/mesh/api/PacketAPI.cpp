#ifdef USE_PACKET_API

// First, in its own block so the include sorter keeps it there: configuration.h supplies the
// variant defines mesh-pb-constants.h needs (portduino resolves MAX_NUM_NODES at runtime).
#include "UptimeClock.h"
#include "configuration.h"

#include "Channels.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RadioInterface.h"
#include "api/PacketAPI.h"
#include "modules/NodeInfoModule.h"

#ifdef MESHTASTIC_PHONEAPI_ACCESS_CONTROL
// receivePacket() dispatches ToRadio straight to MeshService, bypassing handleToRadioPacket and so
// the lockdown admin gate. Fail the build rather than silently ship an admin-auth bypass.
#error "USE_PACKET_API is incompatible with MESHTASTIC_PHONEAPI_ACCESS_CONTROL (PacketAPI bypasses the lockdown admin gate)"
#endif

PacketAPI *packetAPI = nullptr;

static uint32_t fromRadioId = 0; // id of the last FromRadio handed to the screen

PacketAPI *PacketAPI::create(PacketServer *_server)
{
    if (!packetAPI) {
        packetAPI = new PacketAPI(_server);
    }
    return packetAPI;
}

PacketAPI::PacketAPI(PacketServer *_server)
    : concurrency::OSThread("PacketAPI"), isConnected(false), programmingMode(false), server(_server)
{
    api_type = TYPE_PACKET;
}

int32_t PacketAPI::runOnce()
{
    bool success = false;
    // The keyboard view runs next to a phone: it has packets of its own (MeshService::copyToScreen), so
    // Bluetooth does not take the screen away as it does upstream.
#if !defined(ARCH_PORTDUINO) && !defined(USE_KEYBOARD_VIEW)
    if (config.bluetooth.enabled) {
        if (!programmingMode) {
            // in programmingMode we don't send any packets to the client except this one notify
            programmingMode = true;
            success = notifyProgrammingMode();
        }
    } else
#endif
    {
        success = sendPacket();
        success |= sendChanges();
    }
    success |= sendPasskey(); // also in programming mode: that is when a phone pairs
    success |= receivePacket();
    return success ? 10 : 50;
}

bool PacketAPI::receivePacket(void)
{
    bool data_received = false;
    while (server->hasData()) {
        isConnected = true;
        data_received = true;

        powerFSM.trigger(EVENT_INPUT);
        lastContactMsec = Time::skipZero(Time::getMillis());

        meshtastic_ToRadio *mr;
        auto p = server->receivePacket()->move();
        int id = p->getPacketId();
        LOG_DEBUG("Received packet id=%u", id);
        mr = (meshtastic_ToRadio *)&static_cast<DataPacket<meshtastic_ToRadio> *>(p.get())->getData();

        switch (mr->which_payload_variant) {
        case meshtastic_ToRadio_packet_tag: {
            meshtastic_MeshPacket *mp = &mr->packet;
            mp->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_API;
            printPacket("PACKET FROM QUEUE", mp);
            service->handleToRadio(*mp);
            service->mirrorToPhone(*mp); // the phone shows what was sent from the screen
            break;
        }
        case meshtastic_ToRadio_want_config_id_tag: {
            uint32_t config_nonce = mr->want_config_id;
            LOG_INFO("Screen wants config, nonce=%u", config_nonce);
            // the replay sends what is set now, sendChanges() everything that differs later
            for (uint8_t i = 0; i < MAX_NUM_CHANNELS; i++)
                memcpy(&sentChannels[i], &channels.getByIndex(i), sizeof(sentChannels[i]));
            memcpy(&sentLora, &config.lora, sizeof(sentLora));
            watchChanges = true;
            handleStartConfig();
            break;
        }
        case meshtastic_ToRadio_heartbeat_tag:
            if (mr->heartbeat.nonce == 1) {
                if (nodeInfoModule) {
                    LOG_INFO("Broadcasting nodeinfo ping");
                    nodeInfoModule->sendOurNodeInfo(NODENUM_BROADCAST, true, 0, true);
                }
            } else {
                LOG_DEBUG("Got client heartbeat");
            }
            break;
        default:
            LOG_ERROR("Unhandled meshtastic_ToRadio variant: %d", mr->which_payload_variant);
            break;
        }
    }
    return data_received;
}

bool PacketAPI::sendPacket(void)
{
    if (server->available()) {
        // fill dummy buffer; we don't use it, we directly send the fromRadio structure
        uint32_t len = getFromRadio(txBuf);
        if (len != 0) {
            fromRadioScratch.id = ++fromRadioId;
            bool result = server->sendPacket(DataPacket<meshtastic_FromRadio>(fromRadioId, fromRadioScratch));
            if (!result) {
                LOG_ERROR("send queue full");
            }
            return result;
        }
    }
    return false;
}

/**
 * The screen reads the channels and the LoRa settings once, in the config replay. A phone or a
 * serial client can change both without a restart and the firmware tells no other client, which
 * left the screen showing (and, on its next edit, writing back) the channels from boot. Pass on
 * one change per call.
 */
bool PacketAPI::sendChanges(void)
{
    if (!watchChanges || !isSendingPackets() || !server->available())
        return false;

    meshtastic_FromRadio fromRadio = meshtastic_FromRadio_init_zero;
    for (uint8_t i = 0; i < MAX_NUM_CHANNELS; i++) {
        const meshtastic_Channel &channel = channels.getByIndex(i);
        if (memcmp(&sentChannels[i], &channel, sizeof(channel)) == 0)
            continue;
        LOG_INFO("Channel %u changed, tell the screen", i);
        fromRadio.which_payload_variant = meshtastic_FromRadio_channel_tag;
        fromRadio.channel = channel;
        fromRadio.id = ++fromRadioId;
        if (!server->sendPacket(DataPacket<meshtastic_FromRadio>(fromRadioId, fromRadio)))
            return false;
        memcpy(&sentChannels[i], &channel, sizeof(channel));
        return true;
    }
    if (memcmp(&sentLora, &config.lora, sizeof(sentLora)) != 0) {
        LOG_INFO("LoRa config changed, tell the screen");
        fromRadio.which_payload_variant = meshtastic_FromRadio_config_tag;
        fromRadio.config.which_payload_variant = meshtastic_Config_lora_tag;
        fromRadio.config.payload_variant.lora = config.lora;
        fromRadio.id = ++fromRadioId;
        if (!server->sendPacket(DataPacket<meshtastic_FromRadio>(fromRadioId, fromRadio)))
            return false;
        memcpy(&sentLora, &config.lora, sizeof(sentLora));
        return true;
    }
    return false;
}

/**
 * Tell the screen the passkey of a pairing that is under way, or that the pairing is over.
 * The keyboard view reads the "pairing:" note and shows the digits; they go to the screen's queue
 * only, not to the log and not to other clients.
 */
bool PacketAPI::sendPasskey(void)
{
    if (pendingPasskey.load() == c_noPasskey || !server->available())
        return false;
    const int32_t passkey = pendingPasskey.exchange(c_noPasskey);
    if (passkey == c_noPasskey)
        return false;
    meshtastic_FromRadio fromRadio = meshtastic_FromRadio_init_zero;
    fromRadio.which_payload_variant = meshtastic_FromRadio_clientNotification_tag;
    fromRadio.clientNotification.level = meshtastic_LogRecord_Level_INFO;
    if (passkey >= 0)
        snprintf(fromRadio.clientNotification.message, sizeof(fromRadio.clientNotification.message), "pairing:%06u",
                 (unsigned)passkey);
    else
        snprintf(fromRadio.clientNotification.message, sizeof(fromRadio.clientNotification.message), "pairing:");
    fromRadio.id = ++fromRadioId;
    if (!server->sendPacket(DataPacket<meshtastic_FromRadio>(fromRadioId, fromRadio))) {
        // keep it for the next round, unless the Bluetooth task has left something newer meanwhile
        int32_t none = c_noPasskey;
        pendingPasskey.compare_exchange_strong(none, passkey);
        return false;
    }
    return true;
}

bool PacketAPI::notifyProgrammingMode(void)
{
    // tell the client we are in programming mode by sending only the bluetooth config state
    LOG_INFO("force client into programmingMode");
    // its own settings first (brightness, when the screen goes dark, ...): the config that would bring them
    // is not played to the screen in this mode, and without them it runs on what it starts with
    memset(&fromRadioScratch, 0, sizeof(fromRadioScratch));
    fromRadioScratch.which_payload_variant = meshtastic_FromRadio_deviceuiConfig_tag;
    fromRadioScratch.deviceuiConfig = uiconfig;
    server->sendPacket(DataPacket<meshtastic_FromRadio>(0, fromRadioScratch));
    memset(&fromRadioScratch, 0, sizeof(fromRadioScratch));
    fromRadioScratch.id = nodeDB->getNodeNum();
    fromRadioScratch.which_payload_variant = meshtastic_FromRadio_config_tag;
    fromRadioScratch.config.which_payload_variant = meshtastic_Config_bluetooth_tag;
    fromRadioScratch.config.payload_variant.bluetooth = config.bluetooth;
    return server->sendPacket(DataPacket<meshtastic_FromRadio>(0, fromRadioScratch));
}

/**
 * return true if we got (once!) contact from our client and the server send queue is not full
 */
bool PacketAPI::checkIsConnected()
{
    isConnected |= server->hasData();
    return isConnected && server->available();
}

#endif