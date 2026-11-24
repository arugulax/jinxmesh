#pragma once

#include "MeshService.h"
#include "PhoneAPI.h"
#include "comms/PacketServer.h"
#include "concurrency/OSThread.h"
#include <atomic>

/**
 * A version of the phone API used for inter task communication based on protobuf packets, e.g.
 * between two tasks running on CPU0 and CPU1, respectively.
 *
 */
class PacketAPI : public PhoneAPI, public concurrency::OSThread
{
  public:
    static PacketAPI *create(PacketServer *_server);
    virtual ~PacketAPI() {};
    virtual int32_t runOnce();
    // Check the current underlying physical queue to see if the client is fetching packets
    bool checkIsConnected() override;
    // A phone is pairing over Bluetooth and has to be given this passkey: the screen shows it.
    // Called from the Bluetooth task, so the value is only left here and runOnce() hands it on.
    void showPasskey(uint32_t passkey) { pendingPasskey.store((int32_t)passkey); }
    // The pairing is over, one way or the other: the passkey goes from the screen.
    void hidePasskey(void) { pendingPasskey.store(c_hidePasskey); }

  protected:
    explicit PacketAPI(PacketServer *_server);

    void onNowHasData(uint32_t fromRadioNum) override {}
    void onConnectionChanged(bool connected) override {}

    // the screen's own copies (MeshService::copyToScreen): a phone or a serial client next to it loses nothing
    meshtastic_MeshPacket *takeMeshPacket() override { return service->getForScreen(); }
    meshtastic_QueueStatus *takeQueueStatus() override { return service->getQueueStatusForScreen(); }
    meshtastic_ClientNotification *takeClientNotification() override { return service->getClientNotificationForScreen(); }
    // those are for a phone to carry to an MQTT server
    meshtastic_MqttClientProxyMessage *takeMqttClientProxyMessage() override { return nullptr; }

  private:
    bool receivePacket(void);
    bool sendPacket(void);
    bool sendChanges(void);
    bool notifyProgrammingMode(void);
    bool sendPasskey(void);

    static constexpr int32_t c_noPasskey = -1, c_hidePasskey = -2;
    std::atomic<int32_t> pendingPasskey{c_noPasskey}; // a passkey (0..999999) the screen has not been told yet

    bool isConnected;
    bool programmingMode;
    PacketServer *server;
    // what the screen was last told, to pass on changes another client makes (see sendChanges)
    bool watchChanges = false;
    meshtastic_Channel sentChannels[MAX_NUM_CHANNELS] = {};
    meshtastic_Config_LoRaConfig sentLora = meshtastic_Config_LoRaConfig_init_zero;
    uint8_t txBuf[MAX_TO_FROM_RADIO_SIZE] = {0}; // dummy buf to obey PhoneAPI
};

extern PacketAPI *packetAPI;