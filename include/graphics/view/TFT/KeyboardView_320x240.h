#pragma once

#include "util/RouteGraph.h"
#include "util/TownIndex.h"
#include "graphics/common/MeshtasticView.h"
#include "graphics/common/NodeStore.h"
#include <array>
#include <map>
#include <set>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

class IStorage;
class KeyboardMapPanel;

/**
 * @brief Keyboard-first GUI view for 320x240 devices without touch (e.g. ThinkNode M9).
 * Focus is moved with the d-pad, never with a pointer. One invisible object receives
 * all key events; the view keeps its own navigation state and redraws from it.
 * The implementation is spread over KeyboardView_320x240.cpp (frame, keys, chats, nodes),
 * KeyboardViewSettings.cpp (settings, channels), KeyboardViewMenus.cpp (popups, node and
 * chat actions), KeyboardViewMap.cpp and KeyboardViewApps.cpp (Home tiles, the Apps list).
 * Note: due to static callbacks in lvgl this class is modeled as
 *       a singleton with static callback members
 */
class KeyboardView_320x240 : public MeshtasticView
{
  public:
    void init(IClientBase *client) override;
    void task_handler(void) override;

    // methods to update view
    using MeshtasticView::addOrUpdateNode;
    bool setupUIConfig(const meshtastic_DeviceUIConfig &uiconfig) override;
    void setMyInfo(uint32_t nodeNum, meshtastic_MyNodeInfo_device_id_t device_id) override;
    void setDeviceMetaData(int hw_model, const char *version, bool has_bluetooth, bool has_wifi, bool has_eth,
                           bool can_shutdown) override;
    void addOrUpdateNode(uint32_t nodeNum, uint8_t channel, uint32_t lastHeard, const meshtastic_User &cfg) override;
    void updateNodeFlags(uint32_t nodeNum, bool favorite, bool ignored, bool muted) override;
    void updateRegionPresets(const meshtastic_LoRaRegionPresetMap &map) override;
    void notifyClient(const meshtastic_ClientNotification &notification) override;
    void updatePosition(uint32_t nodeNum, int32_t lat, int32_t lon, int32_t alt, uint32_t sats, uint32_t precision) override;
    void updateMetrics(uint32_t nodeNum, const meshtastic_DeviceMetrics &metrics) override;
    void updateSignalStrength(uint32_t nodeNum, int32_t rssi, float snr) override;
    void updateLocalStats(uint32_t nodeNum, const meshtastic_LocalStats &stats) override;
    void updateHopsAway(uint32_t nodeNum, uint8_t hopsAway) override;
    void updateLastHeard(uint32_t nodeNum) override;
    void updateTime(uint32_t time) override;
    void removeNode(uint32_t nodeNum) override;
    bool hasKnownNodeForPacket(uint32_t nodeNum) const override;
    void packetReceived(const meshtastic_MeshPacket &p) override;

    // methods to update device config
    void updateChannelConfig(const meshtastic_Channel &ch) override;
    void updateDeviceConfig(const meshtastic_Config_DeviceConfig &cfg) override;
    void updatePositionConfig(const meshtastic_Config_PositionConfig &cfg) override;
    void updateDisplayConfig(const meshtastic_Config_DisplayConfig &cfg) override;
    void updateLoRaConfig(const meshtastic_Config_LoRaConfig &cfg) override;
    void updateBluetoothConfig(const meshtastic_Config_BluetoothConfig &cfg, uint32_t id = 0) override;
    void configCompleted(void) override;

    void handleResponse(uint32_t from, uint32_t id, const meshtastic_Routing &routing, const meshtastic_MeshPacket &p) override;
    void handleResponse(uint32_t from, uint32_t id, const meshtastic_RouteDiscovery &route) override;
    void handlePositionResponse(uint32_t from, uint32_t request_id, int32_t rx_rssi, float rx_snr, bool isNeighbor) override;
    void textMessageStatus(uint32_t requestId, LogMessage::MsgStatus status) override;
    void textMessageError(uint32_t requestId, uint8_t reason) override;
    void newMessage(uint32_t from, uint32_t to, uint8_t ch, const char *msg, uint32_t &msgtime, bool restore = false) override;
    void newTextMessage(uint32_t from, uint32_t to, uint8_t ch, const char *msg, uint32_t &msgtime, uint32_t id,
                        uint32_t replyId) override;
    void restoreMessage(const LogMessage &msg) override;
    void newReaction(uint32_t from, uint32_t to, uint8_t ch, const char *emoji, uint32_t &msgtime, uint32_t id,
                     uint32_t replyId) override;
    void newWaypoint(uint32_t from, uint8_t ch, const meshtastic_Waypoint &waypoint) override;
    void updateNetworkConfig(const meshtastic_Config_NetworkConfig &cfg) override;
    void updateRingtone(const char rtttl[231]) override;
    void screenSaving(bool enabled) override;

    void notifyConnected(const char *info) override;
    void notifyDisconnected(const char *info) override;
    void notifyReboot(bool show) override;
    void notifyShutdown(void) override;

  protected:
    static constexpr uint8_t c_viewAll = 0xFF;
    static constexpr uint8_t c_viewFavorites = 0xFE;

    enum class Tab : uint8_t { Home, Chats, Nodes, Map, Apps, Settings, Count };
    enum class Menu : uint8_t { None, Pick, Edit, Info };
    enum class Page : uint8_t { Root, Radio, Channels, Device, Display, Position, System, Notifications, Wifi };
    // a screen on top of the Home tab
    enum class App : uint8_t { None, List, Arrange, Nearby, Pointer, Radio, Mecp, MecpNew, Town, Trip, Wardrive, Compass, Stopwatch, Timer, Navigate };
    // what a slot of Home can show; the order is that of the catalog in KeyboardViewApps.cpp
    // Empty is the one tile that may stand on Home more than once
    enum class Tile : uint8_t {
        Time, Unread, Radio, Gps, Battery, Nodes, Nearby, Airtime, Town, Trip,
        Mecp, Compass, Wardrive, Stopwatch, Timer, Navigate, Empty, Count
    };
    static constexpr size_t c_homeSlots = 12; // four wide, three high
    static constexpr size_t c_homeColumns = 4;
    static constexpr size_t c_arrangeReset = c_homeSlots; // focus of the Home manager on its "Default layout" row
    static constexpr std::array<Tile, c_homeSlots> c_defaultHome = {Tile::Time,    Tile::Unread, Tile::Radio,  Tile::Gps,
                                                                    Tile::Battery, Tile::Nodes,  Tile::Nearby, Tile::Town,
                                                                    Tile::Mecp,    Tile::Compass, Tile::Trip,  Tile::Timer};

    // everything a settings row, a picker or a text editor can stand for
    enum class Setting : uint8_t {
        None,
        // pages
        PageRadio,
        PageChannels,
        PageDevice,
        PageDisplay,
        PagePosition,
        PageSystem,
        PageNotifications,
        // radio
        Region,
        Preset,
        Bandwidth,
        SpreadFactor,
        CodingRate,
        Slot,
        OverrideFrequency,
        TxPower,
        HopLimit,
        TxEnabled,
        // channels
        Channel,
        AddChannel,
        ChannelMenu,
        ChannelName,
        ChannelKey,
        ChannelKeyText,
        ChannelDelete,
        ChannelMute,     // row on the notifications page
        ChannelPosition, // row on the position page and the precision picker behind it
        // device
        LongName,
        ShortName,
        Role,
        TimeZone,
        // display
        Brightness,
        ScreenTimeout,
        Units,
        Clock,
        // position
        Gps,
        PositionInterval,
        SendPosition,
        OwnPosition,
        // notifications
        SoundMode,
        // bluetooth
        Bluetooth,
        // system
        Reboot,
        Shutdown,
        ResetNodes,
        ClearChats,
        FactoryReset,
        About,
        // menus outside of the settings
        NodeMenu,
        NodeRemove,
        ChatMenu,
        QuickReply,
        // control center, keyboard light and lock screen
        Control,
        KeyLight,
        AlertMode,
        WakeOnMessage,
        LockOnSleep,
        // messages
        MessageMenu,
        React,
        Mentions,
        Search,
        SearchResults,
        NodeSignal,
        // map
        MapMenu,
        MapStyle,
        MapView,
        MapLayers,
        WaypointName,
        WaypointShare,
        Waypoints,
        WaypointMenu,
        // Wi-Fi and files
        PageWifi,
        WifiEnabled,
        WifiNetwork,
        WifiSsid,
        WifiPassword,
        WifiStatus,
        WifiSaved,
        WifiSavedMenu,
        TileDownload,
        Upload,
        Files,
        FileMenu,
        // Home tiles and apps
        HomeMenu,
        TilePick,
        AppMenu,  // OK held on a row of the Apps tab
        TileSlot, // which tile of a full Home makes room
        // sounds
        NotifySound,
        SystemSounds,
        AlertTone,
        ScreenSound,
        ChatNames,
        BorderWarning,
        ToneDirect,
        ToneEmergency,
        ToneTimer,
        ChannelTone, // the tune of one channel, or muted
        QuickList,   // the quick replies, to change them
        QuickItem,   // what to do with one of them
        QuickText,   // the text of one
        // node actions
        NodeArrow,
        NodeNavigate,
        RouteMenu,
        RouteGo,          // where to: coordinates, a waypoint, a saved route
        RouteCoordinates, // typed: latitude, longitude
        RouteMode,        // by road or on foot, to what was chosen
        RouteWaypoint,
        RoutePlans,       // the saved routes of several stops
        RoutePlanMenu,    // one of them, or the one being put together
        RoutePlanStop,    // a stop of it
        RoutePlanName,
        RouteStopName,
        RouteJoin,
        // looks
        Theme,
        TileColor,
        NodeColor,
        DeclinationAuto,
        // MECP
        MecpPeople,
        MecpCallSign,
        MecpReference,
        MecpDuration,
        MecpNote,
        MecpTo,
        // Trip
        TripMenu,
        WardriveMenu,
        WardriveMode,    // listen only, or a ping every so often
        WardriveChannel, // the channel the ping goes out on
        OdometerReset,
        // Compass
        CompassMenu,
        Declination,
        CompassTurn,
        // Stopwatch and timer
        StopwatchMenu,
        TimerMenu,
        TimerLength,
    };

    /**
     * Message text that lives in external RAM where there is some: a long history as ordinary strings
     * would use up the internal heap, which small allocations are served from.
     */
    class Text
    {
      public:
        Text(void) = default;
        Text(const char *text) { assign(text, text ? strlen(text) : 0); }
        Text(const std::string &text) { assign(text.data(), text.size()); }
        Text(Text &&other) noexcept : data(other.data) { other.data = nullptr; }
        Text &operator=(Text &&other) noexcept
        {
            if (this != &other) {
                release();
                data = other.data;
                other.data = nullptr;
            }
            return *this;
        }
        Text(const Text &) = delete;
        Text &operator=(const Text &) = delete;
        ~Text() { release(); }
        const char *c_str(void) const { return data ? data : ""; }
        bool empty(void) const { return !data || !*data; }

      private:
        void assign(const char *text, size_t length);
        void release(void);
        char *data = nullptr;
    };

    struct Message {
        uint32_t from;
        uint32_t time;
        uint32_t requestId; // 0 for received and restored messages
        LogMessage::MsgStatus status;
        bool own;
        Text text;
        uint32_t id = 0;      // packet id, what a reply refers to; 0 when it was logged without one
        uint32_t replyId = 0; // packet id of the message this one answers
        uint8_t reason = 0;   // why it failed, a meshtastic_Routing_Error; known until the device restarts
        bool heard = false;   // another node was heard relaying it
    };

    struct Reaction {
        uint32_t target; // packet id of the message it is for
        uint32_t from;
        uint32_t emoji; // code point
    };

    struct Chat {
        std::vector<Message> messages;
        std::vector<Reaction> reactions;
        std::string draft;    // kept per conversation while the user is elsewhere
        uint32_t replyTo = 0; // packet id the draft answers
        uint32_t unread = 0;
        uint32_t lastTime = 0;
    };

    struct ChannelInfo {
        std::string name;
        bool enabled = false;
        bool primary = false;
        bool muted = false;
        uint8_t precision = 0; // bits of the position shared on the channel, 0: none
    };

    struct SettingRow {
        Setting id;
        std::string title;
        std::string value;
        uint32_t arg = 0;   // channel index for Setting::Channel
        bool alert = false; // value needs attention
    };

    // one popup at a time: a list to pick from, a line of text to edit or lines to read
    struct Popup {
        Setting what = Setting::None;
        uint32_t target = 0; // channel index or node number the popup is about
        std::string title;
        std::string note;
        std::vector<std::string> items;
        std::vector<uint32_t> values;
        size_t focus = 0;
        size_t top = 0;
        int32_t current = -1; // item that is in effect now
        std::string text;
        size_t caret = 0; // byte offset into text
        size_t maxLen = 0;
        std::string placeholder;
        bool secret = false;    // a password: stars, and only the letter typed last can be read for a moment
        bool peek = false;      // that letter is on show
        uint32_t peekStart = 0; // since when
        std::vector<std::string> lines;
        std::string qr;
        std::vector<uint8_t> bars; // chart above the lines of an info popup, 0..100 each
    };

    struct SignalSample {
        uint32_t time;
        float snr;
        int16_t rssi;
    };

    struct Waypoint {
        uint32_t id;
        uint32_t from;
        int32_t latitude;
        int32_t longitude;
        uint32_t expire;
        std::string name;
    };

    // a packet that came in without a relay
    struct Heard {
        uint32_t time;
        uint32_t node;
        float snr;
        int16_t rssi;
    };

    // a node of the Nearby list, as seen from this device
    struct Near {
        const NodeRecord *node;
        double meters = 0;  // never less than the radius
        double bearing = 0; // degrees from true north
        double radius = 0;  // how far a position of reduced precision can be off; 0: exact
        bool ranged = false; // distance and bearing are known, which takes an own position
    };

    // a Wi-Fi network the view has connected to before; the one in use is prefs.ssid
    struct KnownNetwork {
        std::string ssid;
        std::string password;
        std::string sealed, sealedFrom; // the password as it stands in the settings file, and what that was made from
        bool lost = false;              // the stored password cannot be opened: it has to be typed again
    };
    static constexpr size_t c_maxNetworks = 8;

    // what a Home tile shows below its name
    struct TileView {
        std::string value;
        std::string sub;
        bool accent = false; // the value is news, drawn in blue
    };

    // where a mention or a search hit is
    struct Jump {
        uint32_t key;
        uint32_t id;
        uint32_t time;
    };

    // what the view keeps for itself, in a file next to the message log
    struct Prefs {
        uint8_t keyLight = 2;   // 0 off, 1 on, 2 on while keys are in use
        uint8_t soundMode = 0;  // buzzer mode to go back to when sound is switched on again
        bool keyFlash = false;  // pulse the keyboard light on a new message
        bool wake = true;       // wake the screen on a new message
        bool lockOnSleep = false;
        bool breadcrumbs = true;
        bool tileDownload = true;
        bool wifi = false;
        std::string ssid;
        std::string password;
        std::string mapStyle;
        std::string mapView = "all"; // "all", "fav" or '#' and the name of a channel
        bool nearbyVectors = false;  // Nearby shows the plot of lines and not the list
        std::string callSign;        // goes into emergency messages, remembered from the last one
        bool wardrive = false;       // the signal log is being written
        uint16_t wardrivePing = 0;   // seconds between two pings; 0: it only listens
        uint8_t wardriveChannel = 0xFF; // the channel of the ping; 0xFF: the public one, or else the first
        int16_t declination = 0;     // degrees to add to a magnetic heading for a true one, east positive
        bool declinationSet = false; // without it headings are labeled as magnetic
        bool declinationAuto = true; // taken from the position, by the magnetic model
        uint16_t compassTurn = 0;    // degrees to add because of how the sensor sits in the case
        bool light = false;          // the light look
        bool screenSound = false;    // a short sound when the screen goes on or off
        bool longNames = false;      // chats name who wrote by the long name
        uint8_t mapLayersOff = 0;    // what a vector map leaves out: a bit each for contours, water, roads, trails, boundaries, names
        bool borderWarn = true;      // a warning when the border of a country comes near
        // tunes by their names in Ringtones.h; empty: the notification tone (the timer: its beeps)
        std::string toneDirect, toneEmergency, toneTimer;
        std::vector<std::string> quick; // the user's quick replies, once the built-in ones were changed
        bool quickOwn = false;
        std::string toneChannel[c_max_channels]; // per channel (its slot); empty: the notification tone
        uint32_t timerLength = 300;  // seconds, the length the timer was last set to
        bool timerRepeat = false;
    };

    // navigation
    virtual void switchTab(Tab to);
    virtual void walkTabs(int delta);
    virtual void goHome(void);
    virtual void back(void);
    virtual void openChat(uint32_t key);
    virtual void openUnread(void);
    virtual void openSettings(Page to, Setting focus = Setting::None);
    virtual void activate(void);
    virtual void holdAction(void);
    virtual void mapKey(void);
    virtual void quickKey(void);
    virtual void handleKey(uint32_t key);
    virtual void moveListFocus(int delta);
    virtual void sendDraft(void);
    virtual void sendText(uint32_t key, const std::string &text, uint32_t replyId = 0);
    virtual void toast(const char *text, uint32_t ms = 2000);

    // popups
    virtual void openPicker(Setting what, const char *title, std::vector<std::string> &&items,
                            std::vector<uint32_t> &&values = {}, int32_t current = -1, const char *note = "",
                            uint32_t target = 0);
    virtual void openEditor(Setting what, const char *title, const std::string &text, size_t maxLen, const char *placeholder = "",
                            const char *note = "", uint32_t target = 0);
    virtual void openInfo(const char *title, std::vector<std::string> &&lines, const char *qr = "");
    virtual void closeMenu(void);
    virtual void handleMenuKey(uint32_t key);
    virtual void activateMenuItem(void);
    virtual void previewPick(void);
    virtual void applyPick(Setting what, uint32_t value, uint32_t target);
    virtual bool applyMenuPick(Setting what, uint32_t value, uint32_t target);
    virtual void applyEdit(Setting what, const std::string &text, uint32_t target);

    // settings
    virtual std::vector<SettingRow> settingRows(Page of) const;
    virtual void activateSetting(const SettingRow &row);
    virtual void handleSettingsKey(uint32_t key);
    virtual bool loaded(bool has);
    virtual bool blocked(const char *what);
    virtual void sendLora(void);
    virtual void setRegion(meshtastic_Config_LoRaConfig_RegionCode region);
    // the presets the radio accepts in a region, as far as it told us
    virtual const meshtastic_LoRaPresetGroup *presetGroup(meshtastic_Config_LoRaConfig_RegionCode region) const;
    virtual bool presetAllowed(meshtastic_Config_LoRaConfig_RegionCode region,
                               meshtastic_Config_LoRaConfig_ModemPreset preset) const;
    virtual uint32_t slotCount(void) const;
    virtual uint32_t activeSlot(void) const;
    virtual float slotFrequency(uint32_t slot) const;
    virtual void toggleGps(void);
    virtual void setSounds(uint8_t notify, bool system);
    virtual void applyTimeZone(void);
    virtual void leaveBluetoothMode(void);
    virtual void useOneTimePins(void);
    virtual void endPairing(void);

    // channels
    virtual void storeChannel(const meshtastic_Channel &ch);
    virtual void sendChannel(uint8_t index);
    virtual void openChannelMenu(uint8_t index);
    virtual void toggleMute(uint8_t index);
    virtual void openPositionPicker(uint8_t index);
    virtual bool publicKey(uint8_t index) const;
    virtual int positionChannel(void) const;
    virtual std::string precisionName(uint32_t bits) const;
    virtual bool silent(void) const;
    virtual void shareChannel(uint8_t index);
    virtual void saveChannelKey(uint32_t target, const uint8_t *key, size_t size);
    virtual std::string keyName(const meshtastic_Channel &ch) const;

    // nodes and chats
    virtual uint32_t focusedNode(void) const;
    virtual void openNodeMenu(uint32_t nodeNum);
    virtual void openNodeDetail(uint32_t nodeNum);
    virtual void openChatMenu(uint32_t key);
    // the quick replies for a chat; without one, for the chat that is open
    virtual void openQuickReplies(uint32_t key = UINT32_MAX);
    std::vector<std::string> quickReplies(void) const;
    void openQuickList(int32_t focus = -1);
    static constexpr size_t c_maxQuick = 16, c_quickLength = 60;
    virtual void startTraceRoute(uint32_t nodeNum);
    virtual bool traceInfo(uint32_t nodeNum, std::vector<std::string> &&lines);
    virtual void startPing(uint32_t nodeNum);
    virtual std::string distanceTo(const NodeRecord &node) const;

    // control center, keyboard light and lock screen
    void loadPrefs(void);
    void savePrefs(void);
    void openControl(size_t focus = 0);
    void controlStep(uint32_t row, int32_t step);
    void lightTask(void);
    void alertMessage(uint32_t key, bool evenMuted = false);
    void lockScreen(void);
    void screenOffNow(void);
    void renderLock(void);
    uint8_t alertMode(void) const;
    const char *alertName(void) const;
    bool twelveHour(void) const { return hasDisplay && display.use_12h_clock; }
    bool imperialUnits(void) const
    {
        return hasDisplay && display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL;
    }
    void setAlertMode(uint8_t mode);
    bool extraPick(Setting what, uint32_t value, uint32_t target);
    bool extraEdit(Setting what, const std::string &text, uint32_t target);
    bool extraSetting(const SettingRow &row);
    void extraRows(Page of, std::vector<SettingRow> &rows) const;

    // messages
    void openMessageMenu(void);
    void addReaction(uint32_t key, uint32_t target, uint32_t from, const char *emoji);
    std::string reactionsOf(const Chat &chat, uint32_t id) const;
    bool mentionsMe(const Message &msg) const;
    void openMentions(void);
    void openSearch(const std::string &text = "");
    void openJumps(Setting what, const char *title, const char *none);
    void openChatAt(const Jump &to);
    int32_t conversationTop(int32_t count);
    bool chatMuted(uint32_t key) const;
    void openSignalHistory(uint32_t nodeNum);

    // pinned chats
    std::string pinName(uint32_t key) const;
    uint32_t pinKey(const std::string &pin) const;
    void pinsTask(void);
    std::vector<uint32_t> pinnedChats(void) const;
    bool pinned(uint32_t key) const;
    void setPinned(uint32_t key, bool on);
    void movePin(uint32_t key, int delta);
    void channelChanged(uint8_t index, const std::string &oldName, bool same);

    // Home tiles and apps
    bool inApp(void) const { return (tab == Tab::Home || tab == Tab::Apps) && app != App::None && !inConversation; }
    void openApp(App which, bool fromList = false);
    void closeApp(void);
    const char *appTitle(void) const;
    void handleAppKey(uint32_t key);
    void activateApp(void);
    void holdApp(void);
    void renderApp(void);
    void renderAppList(void);
    void renderArrange(void);
    void openHomeMenu(void);
    TileView tileView(Tile tile);
    void openTile(Tile tile);
    void renderTiles(int32_t focus, bool carried);
    void setHomeTiles(const std::string &line);
    std::string homeLine(void) const;
    bool appsPick(Setting what, uint32_t value, uint32_t target);
    std::string appState(App which) const;
    std::pair<uint32_t, uint32_t> colorOf(uint32_t nodeNum);
    void appsKey(uint32_t key);
    void openAppMenu(void);
    void addTile(Tile tile);
    void screenSound(bool on);
    // tells the radio side which tunes the kinds of messages have (see KeyboardViewTones.cpp)
    void applyTones(void);
    // plays a tune by its name once; false when there is no sound
    bool playTone(const std::string &name);
    std::string &toneOf(Setting which);
    // the list of what a channel sounds like: the notification tone, a tune of its own, or muted
    void openChannelTone(uint8_t index);
    static constexpr uint32_t c_pickMuted = 0xFFFF; // the value of "Muted" in that list
    // a map of lines is drawn in the colors of the look
    void mapThemeChanged(void);
    // the name of the tune behind a value of a list of tunes; empty for "same" and "beeps"
    std::string toneNameAt(Setting list, uint32_t value) const;
    uint32_t transferShown = 0; // when the notice of a file coming in over the cable was last put up
    std::vector<std::string> ownTones(void);
    std::string tuneText(const std::string &name);
    std::vector<std::string> pickTones; // the own tunes the open list offers
    bool tonesStale = true; // the tunes of the kinds of messages are read once the card is there

    // Nearby and its pointer page (KeyboardViewAppsNav.cpp)
    std::vector<Near> nearbyList(void) const;
    Near nearTo(const NodeRecord &node) const;
    void positionAge(uint32_t nodeNum, uint32_t lastHeard, char *buf, size_t len, bool brief) const;
    void nearbyKey(uint32_t key);
    void nearbyActivate(void);
    void nearbyHold(void);
    bool nearbyBack(void);
    void renderNearby(void);
    void renderVectors(const std::vector<Near> &list, size_t focus);
    void renderPointer(void);
    TileView nearbyTile(void) const;

    // RF monitor (KeyboardViewAppsRadio.cpp)
    void radioTask(void);
    void radioSample(float channelUse, float txUse);
    std::vector<SettingRow> radioRows(void) const;
    void radioKey(uint32_t key);
    void radioActivate(void);
    void renderRadio(void);
    TileView airtimeTile(void) const;

    // MECP: emergency messages in plain words (KeyboardViewAppsMecp.cpp)
    void renderMecpCard(lv_obj_t *parent, const char *text, int32_t width);
    std::string mecpSummary(const char *text) const;
    bool mecpUrgent(const char *text) const;
    std::vector<Jump> mecpMessages(void) const;
    void openMecpComposer(uint32_t fromChat);
    std::string mecpLine(bool *cut = nullptr) const;
    void mecpKey(uint32_t key);
    void mecpActivate(void);
    void mecpHold(void);
    bool mecpBack(void);
    void mecpSend(void);
    void renderMecpList(void);
    void renderMecpComposer(void);
    bool mecpPick(Setting what, uint32_t value, uint32_t target);
    bool mecpEdit(Setting what, const std::string &text, uint32_t target);
    void mecpMessageMenu(const Message &msg, std::vector<std::string> &items, std::vector<uint32_t> &values) const;
    bool mecpMessagePick(uint32_t value, const Message &msg);
    // Stopwatch and timer (KeyboardViewAppsClock.cpp)
    struct Split {
        uint32_t lap;   // ms
        uint32_t total; // ms
        uint32_t clock; // epoch, 0 without the time
        bool placed;
        int32_t latitude, longitude;
        double tripMeters;
    };
    uint32_t stopwatchNow(void) const;
    void stopwatchSplit(void);
    bool stopwatchSave(void);
    int32_t timerLeft(void) const;
    void timerStart(void);
    void clockTask(void);
    void clockKey(uint32_t key);
    void clockActivate(void);
    void clockHold(void);
    bool clockPick(Setting what, uint32_t value, uint32_t target);
    void renderStopwatch(void);
    void renderTimer(void);
    // Compass (KeyboardViewAppsCompass.cpp)
    void compassTask(void);
    bool compassHeading(float &degrees) const;
    bool declinationNow(float &degrees) const;
    void compassKey(uint32_t key);
    void compassHold(void);
    bool compassPick(Setting what, uint32_t value, uint32_t target);
    bool compassEdit(Setting what, const std::string &text, uint32_t target);
    void renderCompass(void);
    // Trip odometer and Wardrive (KeyboardViewAppsTrip.cpp)
    void tripFix(int32_t latitude, int32_t longitude, uint32_t satellites);
    void borderFix(int32_t latitude, int32_t longitude);
    void playAlarm(void);
    void tripTask(void);
    uint32_t tripElapsedNow(void) const;
    void tripToggle(void);
    void tripKey(uint32_t key);
    void tripHold(void);
    bool tripPick(Setting what, uint32_t value, uint32_t target);
    void renderTrip(void);
    TileView tripTile(void) const;
    TileView clockTile(bool timer) const;
    std::string tripLine(void) const;
    void setTripLine(const std::string &line);
    void wardriveHeard(uint32_t nodeNum, int32_t rssi, float snr);
    void wardriveTask(void);
    // event: 0 a node was heard, 1 a quiet minute, 2 a ping went out, 3 a ping could not be sent, 4 logging starts
    bool wardriveWrite(uint32_t nodeNum, int32_t rssi, float snr, uint8_t event = 0);
    void wardriveHold(void);
    void wardrivePingNow(void);
    std::string channelWarning(uint8_t index) const;
    void openMapLayers(int32_t focus = 0);
    void applyMapLayers(void);
    void offerFile(const char *path);
    void tripTrack(uint32_t time, int32_t latitude, int32_t longitude, double speed);
    uint8_t wardrivePingChannel(void) const;
    void wardriveToggle(void);
    void wardriveStop(const char *why);
    void renderWardrive(void);
    // Town locator (KeyboardViewAppsTown.cpp): where a position is, in words
    bool openTowns(void);
    const TownIndex::Place &ownPlace(void);
    std::string townOf(int32_t latitude, int32_t longitude);
    std::vector<TownIndex::Entry> townRows(void);
    void townKey(uint32_t key);
    void townActivate(void);
    bool townBack(void);
    void renderTown(void);
    TileView townTile(void);
    // Navigate: a route over roads and trails (KeyboardViewAppsRoute.cpp)
    bool openRoads(void);
    void navigateTo(int32_t latitude, int32_t longitude, const std::string &name, uint8_t mode, uint32_t node);
    void routeStart(void);
    void routeEnd(void);
    void routeTask(void);
    void routeFix(int32_t latitude, int32_t longitude);
    std::string stepText(size_t step) const;
    void routeKey(uint32_t key);
    void routeActivate(void);
    void routeHold(void);
    bool routeBack(void);
    bool routePick(Setting what, uint32_t value, uint32_t target);
    bool routeEdit(Setting what, const std::string &text, uint32_t target);
    void routeCommand(const std::string &line);
    void openRouteGo(void);
    void openPlan(void);
    void openPlans(void);
    void planGo(size_t index, uint8_t mode);
    void planBegin(uint8_t mode);
    bool planSave(void);
    bool planLoad(const std::string &name);
    std::string guideText(void) const;
    void renderRoute(void);
    void renderRouteLine(void);
    void followOwn(void);
    TileView routeTile(void) const;
    // a place that is not a node: on the map and on the pointer page
    void showPlace(double latitude, double longitude);
    void pointTo(double latitude, double longitude, const std::string &name);
    // map options
    void openMapMenu(void);
    void openMapStyles(void);
    void selectMapStyle(const std::string &style);
    void openWaypoints(void);
    void placeWaypoints(void);
    void addBreadcrumb(int32_t latitude, int32_t longitude);
    void placeBreadcrumbs(void);
    void applyTileDownload(void);
    // map views: all nodes, the favorites or the nodes heard on one channel
    void markHeard(uint32_t nodeNum, uint8_t channel);
    bool inMapView(uint32_t nodeNum, uint8_t view) const;
    void resolveMapView(void);
    void setMapView(uint8_t view);
    void refreshMarkers(void);
    void openMapViews(void);
    std::string mapViewName(uint8_t view) const;
    size_t mapViewCount(uint8_t view) const;
    bool mapPick(Setting what, uint32_t value, uint32_t target);
    bool mapEdit(Setting what, const std::string &text, uint32_t target);

    // Wi-Fi and files
    bool ensureStorage(void);
    void wifiTask(void);
    void applyWifi(void);
    bool wifiConnected(void) const;
    std::string wifiAddress(void) const;
    void toggleUpload(void);
    void showUpload(void);
    void openFiles(const std::string &path);
    void openNetworks(void);
    void rememberNetwork(void);
    void useNetwork(size_t index);
    void openSavedNetworks(void);
    void openWifiPassword(bool keepIfEmpty);
    bool filesPick(Setting what, uint32_t value, uint32_t target);
    bool filesEdit(Setting what, const std::string &text, uint32_t target);
    bool filesSetting(const SettingRow &row);
    void filesRows(std::vector<SettingRow> &rows) const;

    // map
    virtual void showMap(bool show);
    virtual void mapTask(void);
    virtual void loadMap(void);
    virtual void placeMarker(uint32_t nodeNum);
    virtual void dropMarker(uint32_t nodeNum);
    virtual void showOnMap(uint32_t nodeNum);
    virtual void handleMapKey(uint32_t key);

    // model
    virtual std::vector<const NodeRecord *> visibleNodes(void) const;
    virtual std::vector<uint32_t> chatOrder(void) const;
    virtual Chat &chatFor(uint32_t key);
    virtual void appendMessage(uint32_t key, Message &&msg);
    virtual void setMessageStatus(uint32_t key, uint32_t requestId, LogMessage::MsgStatus status);
    virtual const Message *findMessage(uint32_t key, uint32_t id) const;
    virtual std::string messageState(uint32_t key, const Message &msg, bool brief) const;
    virtual std::string quoteOf(uint32_t key, uint32_t id, bool withName) const;
    virtual std::string chatName(uint32_t key) const;
    virtual std::string shortName(uint32_t nodeNum) const;
    // who wrote, the way chats show it: the short or the long name, as set
    virtual std::string senderName(uint32_t nodeNum) const;
    virtual uint32_t totalUnread(void) const;
    virtual uint32_t now(void) const;

    // rendering
    virtual void createFrame(void);
    virtual void render(void);
    virtual void renderStatusBar(void);
    virtual void renderHome(void);
    virtual void renderChats(void);
    virtual void renderNodes(void);
    virtual void renderMap(void);
    virtual void renderSettings(void);
    virtual void renderConversation(void);
    virtual void renderMenu(void);
    virtual void renderToast(void);
    virtual void renderTakeover(void);

#if defined(ARCH_PORTDUINO)
    virtual void loadDemoData(void);
    virtual void stressStep(void);
#endif

  private:
    // view creation only via ViewFactory
    friend class ViewFactory;
    static KeyboardView_320x240 *instance(void);
    static KeyboardView_320x240 *instance(const DisplayDriverConfig &cfg);
    KeyboardView_320x240();
    KeyboardView_320x240(const DisplayDriverConfig *cfg, DisplayDriver *driver);

    static void onKeyEvent(lv_event_t *e);

    static KeyboardView_320x240 *gui;

    NodeStore nodeStore;
    std::map<uint32_t, Chat> conversations; // key: channel index (< c_max_channels) or node number
    std::array<ChannelInfo, c_max_channels> channelInfo;
    std::array<meshtastic_Channel, c_max_channels> channels{};
    // as storeChannel() saw them last: an edit changes channels[] in place before it is stored, and what a
    // channel was before is told by this copy
    std::array<meshtastic_Channel, c_max_channels> channelsStored{};
    std::set<uint32_t> favorites;
    std::set<uint32_t> ignoredNodes;

    // navigation state
    Tab tab = Tab::Home;
    std::vector<Tab> trail;           // tabs visited before the current one; Back walks it
    bool barFocus = false;            // Home and Map: the d-pad walks the tab bar instead of the content
    bool inConversation = false;      // conversation is a layer on top of the Chats tab
    uint32_t convKey = 0;             // conversation shown while inConversation
    size_t caret = 0;                 // byte offset into the draft
    int32_t convSelect = -1;          // message picked with up and down, -1: the composer has the focus
    uint8_t homeFocus = 1;            // focused tile, row-major in the grid of four by three
    uint32_t chatsFocus = UINT32_MAX; // key of the focused chat row; an unknown key means the first row
    uint32_t nodesFocus = 0;          // node number of the focused node row; an unknown node means the first row
    size_t chatsTop = 0;              // first visible row
    size_t nodesTop = 0;              // first visible row
    std::string nodeFilter;           // live filter typed on the Nodes tab
    Page page = Page::Root;           // settings page shown on the Settings tab
    size_t settingFocus = 0;          // focused row of the page
    size_t settingTop = 0;            // first visible row
    size_t rootFocus = 0;             // row to return to on the settings root
    Menu menu = Menu::None;           // kind of popup that is open
    Popup popup;

    // radio and device state
    meshtastic_Config_LoRaConfig lora{};
    meshtastic_Config_DeviceConfig device{};
    meshtastic_Config_PositionConfig position{};
    meshtastic_Config_DisplayConfig display{};
    meshtastic_Config_BluetoothConfig bluetooth{};
    meshtastic_DeviceUIConfig uiConfig{};
    meshtastic_User ownUser{};
    bool hasLora = false;                           // a config is only sent back after it was received
    meshtastic_LoRaRegionPresetMap regionPresets{}; // empty with a radio that does not send it
    bool hasDevice = false;
    bool hasPosition = false;
    bool hasDisplay = false;
    bool hasBluetooth = false;
    bool hasOwnUser = false;
    std::string firmwareVersion;
    std::string alertTone; // the tune the radio plays for a message, as it told us
    bool canShutdown = false;
    float lastSnr = 0.0f;
    bool hasSnr = false;
    bool connected = true;
    bool regionPrompted = false; // the region picker opens by itself once
    bool bluetoothMode = false;  // the Bluetooth screen is up: state, the PIN of a pairing, off
    bool pairing = false;        // a phone is pairing: the radio made a PIN for this one attempt
    char pairingPin[8] = "";     // never stored and never logged
    uint32_t pairingStart = 0;
    bool restarting = false;
    uint32_t restartStart = 0;
    bool splash = true;        // the start screen with the logo is up
    uint32_t splashStart = 0;
    uint32_t splashSeen = 0;   // ms the start screen has been up while the screen was being drawn
    uint32_t splashHold = 0;   // simulator: show the start screen this long, ms
    bool safeMode = false;     // simulator: skip everything that restarts the radio
    std::string pendingName;   // channel name typed before its key is chosen
    uint32_t traceRequest = 0; // request id of the running trace route
    bool traceShown = false;   // the popup on screen belongs to it
    uint32_t pingTarget = 0;   // node a ping is out to
    uint32_t pingRequest = 0;

    // map
    KeyboardMapPanel *map = nullptr;
    IStorage *storage = nullptr;
    std::map<uint32_t, lv_obj_t *> markers;
    bool mapShown = false;
    bool mapTiles = false;  // a tile source was found on the card
    uint8_t mapZoomMin = 2; // an archive without a download source narrows these to what it holds
    uint8_t mapZoomMax = 18;
    uint8_t mapStretch = 0; // zoom levels the tiles on screen are stretched over, for the heads-up display
    uint32_t mapTarget = 0; // node the map was centered on last
    std::vector<std::string> mapStyles; // what the style picker offers
    std::vector<Waypoint> waypoints;
    std::vector<std::pair<int32_t, int32_t>> breadcrumbs; // own track of this session
    int32_t dropLatitude = 0; // where the waypoint being named goes
    int32_t dropLongitude = 0;
    std::string dropName;
    bool mounted = false; // the card of the map is in use
    uint8_t mapView = c_viewAll;         // a channel index, c_viewFavorites or c_viewAll
    std::map<uint32_t, uint8_t> heardOn; // per node the channels it was heard on, one bit each

    // control center, keyboard light and lock screen
    Prefs prefs;
    bool locked = false;
    bool screenOff = false;
    bool offDrawn = false;    // the one drawing behind a dark screen is done
    uint32_t screenOffAt = 0; // lv tick
    bool ownMoved = false;    // the map has not been told of pendingLat/pendingLon yet
    float pendingLat = 0, pendingLon = 0;
    uint8_t pulseLeft = 0; // light changes left of a message alert
    uint32_t pulseTime = 0;

    // Home tiles, apps and pinned chats
    App app = App::None;      // screen shown on top of the Home tab
    bool appFromList = false; // Back leads to the Apps list and not to the tiles
    size_t appsFocus = 0;     // focused row of the Apps list
    size_t appsTop = 0;       // first visible row
    std::array<Tile, c_homeSlots> homeTiles = c_defaultHome;
    std::array<uint8_t, (size_t)Tile::Count> tileColors{}; // per tile an index into the palette; 0: none
    std::map<uint32_t, uint8_t> nodeColors;                // per node the same, for its chip and its marker
    std::set<uint32_t> ownedNodes;                         // nodes the user calls their own: a ring around the chip
    size_t arrangeFocus = 0;        // Home manager: focused slot, or c_arrangeReset

    // Nearby
    uint32_t nearbyFocus = 0;                    // focused node
    size_t nearbyTop = 0;                        // first visible row
    std::string nearbyFilter;                    // typed into the list
    uint32_t pointerNode = 0;                    // whom the pointer page points at
    std::map<uint32_t, uint32_t> positionTimes;  // when a position came in during this session
    bool positionsLive = false;                  // the stored nodes are through, positions now are news
    lv_point_precise_t pointerPoints[5] = {};    // the arrow; a line keeps a pointer to its points
    std::vector<std::array<lv_point_precise_t, 2>> vectorPoints; // the lines of the plot, for the same reason

    // RF monitor
    static constexpr size_t c_radioHistory = 30; // samples, one every 20 seconds: ten minutes
    static constexpr size_t c_heardKept = 20;
    meshtastic_LocalStats localStats = meshtastic_LocalStats_init_zero;
    bool hasLocalStats = false;
    bool hasAirtime = false;      // channelUse and txUse are known
    float channelUse = 0.0f;      // percent, of the radio this screen belongs to
    float txUse = 0.0f;           // percent of the last hour this radio transmitted
    std::array<uint8_t, c_radioHistory> useHistory{}; // in half percent, oldest first
    std::array<uint8_t, c_radioHistory> txHistory{};
    size_t historyCount = 0;
    uint32_t historyTime = 0;     // tick of the newest sample
    uint32_t statsAsked = 0;      // tick of the last request for the counters
    std::vector<Heard> heard;     // newest last
    size_t radioFocus = 0;
    size_t radioTop = 0;
    bool carrying = false;          // Home manager: the focused tile is picked up and moves with the arrows
    // the pointer page on a place instead of a node
    bool pointerPlace = false;
    int32_t placeLatitude = 0, placeLongitude = 0; // degrees times 1e7
    std::string placeName;
    App pointerFrom = App::Nearby;     // where Back leads from the pointer page
    uint32_t pointerChat = UINT32_MAX; // or the chat it leads back to
    // Stopwatch: all times in ms of the processor's running clock
    bool stopwatchRunning = false;
    uint32_t stopwatchStart = 0;  // tick the running stretch began at
    uint32_t stopwatchBase = 0;   // of the stretches that are over
    std::vector<Split> splits;    // oldest first
    size_t splitsTop = 0;         // rows scrolled down in the list
    // Timer
    bool timerRunning = false;
    uint32_t timerEnd = 0;        // tick it reaches zero at
    uint32_t timerPaused = 0;     // ms left while it stands; 0: at its full length
    uint32_t timerRounds = 0;     // times it has reached zero since it was started
    bool timerRinging = false;    // it reached zero and nobody has pressed a key since
    uint32_t timerRang = 0;       // tick of the last beep
    uint32_t timerRingStart = 0;  // tick it reached zero at
    uint8_t timerBeeps = 0;       // beeps still to come
    std::string timerEntry;       // digits being typed for a new length
    uint32_t clockDrawn = 0;      // tick of the last drawing of a running clock
    // Compass
    uint8_t compassState = 0;   // 0 none, 1 heading known, 2 being calibrated, 3 waiting for a reading
    float compassNorth = 0, compassEast = 0; // the smoothed magnetic heading as a vector
    float compassShown = -1000; // heading of the last drawing, degrees
    uint32_t compassPolled = 0; // tick of the last look at the sensor
    lv_point_precise_t compassPoints[2] = {}; // the mark of what the pointer page points at
    // Trip odometer: distances in meters, times in seconds
    bool tripRunning = false;
    double tripMeters = 0, odometerMeters = 0;
    uint32_t tripElapsed = 0;     // of the stretches that are over
    uint32_t tripResumed = 0;     // epoch the running stretch began at
    uint32_t tripMoving = 0;
    float tripTop = 0;            // m/s, between two fixes
    bool tripAnchored = false;    // there is a fix to measure from
    int32_t tripLatitude = 0, tripLongitude = 0; // the last fix that counted
    uint32_t tripFixTime = 0;     // epoch of the last fix, counted or not
    uint32_t borderChecked = 0;   // when the distance to the border was last looked at (ticks; 0: never)
    uint8_t borderStep = 0;       // how near the border was said to be: 1 within 500 m, 2 within 250, 3 within 100
    bool tripUnsaved = false;
    uint32_t tripSaved = 0;       // tick of the last save
    // Wardrive
    uint32_t wardriveRows = 0;                     // written since the start of the device
    std::vector<std::string> wardriveLast;         // the newest samples in words, newest last
    std::map<uint32_t, uint32_t> wardriveNodeTick; // when a node was logged last
    uint32_t wardriveRowTick = 0;                  // when any row was written last
    uint32_t wardrivePingTick = 0;                 // when a ping was last tried (0: not yet)
    uint32_t wardrivePingSent = 0;                 // when the last one went out (0: none yet)
    uint32_t wardrivePings = 0;                    // sent since the start of the device
    uint32_t wardriveChecked = 0;                  // when the free space was looked at last
    uint32_t wardriveFreeMb = 0;
    uint8_t wardriveFailures = 0;                  // writes that failed in a row
    std::string wardriveFile;                      // the file of the last row
    uint32_t wardriveMarks = 0;                    // marks set since the start of the device
    std::set<uint32_t> wardriveNodes, wardriveAnswered; // heard while logging; and of them within a minute of a ping
    uint32_t wardriveFarNode = 0;                  // the one heard from farthest away, by where it says it is
    double wardriveFarMeters = 0;
    std::string tripFile;                          // the track of the trip: the file of its last row
    uint32_t tripTracked = 0;                      // epoch of that row
    std::string wardriveNote;                      // why it is not logging
    // Navigate
    RouteGraph roads;
    uint32_t roadsTried = 0;       // tick of the last attempt to open the road data; 0: never
    RouteGraph::Route route;
    bool routeActive = false;      // there is a route and it is being followed
    bool routeSearching = false;
    uint8_t routeMode = 0;         // 0 by car, 1 on foot
    int32_t routeLatitude = 0, routeLongitude = 0; // where it leads
    std::string routeName;
    uint32_t routeNode = 0;        // the node it leads to; 0: a place
    std::string routeNote;         // why there is no route
    uint32_t routeStarted = 0;     // tick the search began at
    size_t routeStep = 0;          // the step this device is on; the next one is what comes up
    size_t routeNearest = 0;       // the stretch of the line this device is on
    uint32_t routeAlong = 0;       // meters of the route that are done
    uint32_t routeOff = 0;         // meters this device is away from the line
    uint32_t routeOffSince = 0;    // tick since when it has been off the route; 0: it is on it
    uint8_t routeOffFixes = 0;
    bool routeArrived = false;
    bool routeList = false;        // the list of turns is on screen, not the next turn
    size_t routeFocus = 0, routeTop = 0;
    uint32_t routePolled = 0;      // tick of the last look at the receiver
    uint32_t routeDrawn = 0;
    lv_point_precise_t turnPoints[8] = {};       // the arrow of the next turn
    std::vector<lv_point_precise_t> routeLine;   // the route on the map, in pixels
    std::vector<lv_point_precise_t> planLine;    // the line through all stops of a route of several, the same
    // a route of several stops: put together here or sent from the page, kept in /apps/routes, followed stop by stop
    struct Stop {
        int32_t latitude;
        int32_t longitude;
        std::string name;
    };
    std::vector<Stop> planStops;
    std::string planName;
    std::vector<std::string> planFiles; // the saved ones, as the list shows them
    size_t planAt = 0;                  // the stop the route leads to
    bool planRunning = false;           // the stops are followed one after the other
    uint32_t planNext = 0;              // tick at which the next stop is taken up after an arrival
    bool planAdding = false;            // what is chosen becomes a stop, not the destination
    Stop routeChosen{};                 // the destination that still waits for "by road" or "on foot"
    uint32_t inboxPolled = 0;
    // Town locator
    TownIndex towns;
    uint32_t townsTried = 0;        // tick of the last attempt to open the town data; 0: never
    TownIndex::Place ownTown;       // where this device is
    bool ownTownKnown = false;
    int32_t ownTownLatitude = 0, ownTownLongitude = 0; // where that was looked up
    std::vector<TownIndex::Entry> townsAround;         // of that place
    std::vector<std::pair<uint64_t, std::string>> townCache; // towns of other positions, newest last
    std::string townSearch;
    std::vector<TownIndex::Entry> townHits;
    size_t townFocus = 0, townTop = 0;
    // MECP
    size_t mecpListFocus = 0, mecpListTop = 0;
    uint8_t mecpStep = 0;               // composer: 0 how serious, 1 what, 2 details, 3 review and send
    size_t mecpFocus = 0, mecpTop = 0;  // focused and first visible row of the step
    uint8_t mecpSeverity = 3;
    bool mecpDrill = false;
    std::vector<std::string> mecpCodes; // picked, in that order
    std::string mecpSearch;
    uint32_t mecpPeople = 0;
    bool mecpPosition = true, mecpTime = true;
    std::string mecpReference, mecpNote;
    uint32_t mecpMinutes = 0;
    uint32_t mecpTo = 0;                // chat the message goes to
    uint32_t mecpFrom = UINT32_MAX;     // chat the composer was opened from; none: the Apps list
    // pinned chats in order: '#' and a channel name, '=' and the slot of a channel without a name, or '!' and a
    // node number in hex
    std::vector<std::string> pins;
    // pins whose channel changed, with the pin of what the slot holds now if that is the same channel renamed;
    // looked at once the channels have settled, since a set of channels arrives one by one
    std::vector<std::pair<std::string, std::string>> pinChanges;
    uint32_t pinChangeTime = 0;

    // messages
    std::set<uint32_t> mutedNodes;
    std::map<uint32_t, std::vector<SignalSample>> signalLog;
    std::vector<Jump> jumps; // what the mention and search lists lead to
    int32_t convTop = 0;     // first message drawn in the open conversation

    // Wi-Fi and files
    meshtastic_Config_NetworkConfig network{};
    std::vector<KnownNetwork> wifiKnown; // saved networks; out of reach of one, the strongest of the others is used
    bool wifiStarted = false;
    bool wifiParked = false; // switched off behind a dark screen, back on with the screen
    bool passwordLost = false; // the stored Wi-Fi password could not be opened and has to be typed again
    bool wifiUp = false;
    bool uploading = false; // the upload page is being served
    uint32_t wifiChecked = 0;
    std::string filesPath;
    std::vector<std::string> fileNames;

    // rendering state
    lv_obj_t *root = nullptr;
    lv_obj_t *statusBar = nullptr;
    lv_obj_t *content = nullptr;
    lv_obj_t *mapLayer = nullptr; // kept between redraws, the map panel draws into it
    lv_obj_t *mapHud = nullptr;
    lv_obj_t *tabBar = nullptr;
    lv_obj_t *overlay = nullptr;
    lv_obj_t *keySink = nullptr;
    lv_group_t *group = nullptr;
    std::string toastText;
    uint32_t toastStart = 0;
    uint32_t toastTime = 0;
    uint32_t lastRender = 0;
    uint32_t actTime = 0;
    uint32_t shownMinute = 0;
    bool demoData = false; // sample mesh loaded, ignore the simulated radio's channels
    bool dirty = true;     // model changed, redraw when the throttle allows
    bool urgent = true;    // redraw on the next task_handler run (key presses)

#if defined(ARCH_PORTDUINO)
    uint32_t stressNodes = 0; // synthetic mesh traffic for load tests
    uint32_t stressLast = 0;
    uint32_t stressCount = 0;
#endif
};
