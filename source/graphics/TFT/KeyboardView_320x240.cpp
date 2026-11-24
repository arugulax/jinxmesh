#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "graphics/common/LoRaPresets.h"
#include "graphics/common/ViewController.h"
#include "graphics/driver/DisplayDriver.h"
#include "graphics/driver/DisplayDriverFactory.h"
#include "graphics/view/TFT/KeyboardViewKit.h"
#include "input/DiacriticRotation.h"
#include "input/policy/InputContextState.h"
#include "input/policy/UICommandDispatcher.h"
#include "util/ILog.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

using namespace kbview;

namespace
{

constexpr uint32_t c_restartTime = 30000;     // ms after which a restart is taken as not having happened
constexpr uint32_t c_noKey = 0xFF;            // what the M9's keyboard reports when it has nothing to say
constexpr uint32_t c_splashGuard = 300;       // ms of the start screen in which keys are not yet taken
constexpr uint32_t c_splashStep = 50;         // ms: the most one round of the task counts towards that time
constexpr uint32_t c_splashTime = 2000;       // ms the start screen stays, whether the radio has answered or not
constexpr uint32_t c_defaultBrightness = 153; // 60 %, the stock default
constexpr uint32_t c_minBrightness = 26;      // 10 %, anything below reads as a dead screen
#if defined(ARCH_PORTDUINO)
constexpr uint32_t c_stressInterval = 100; // ms between two steps of made-up traffic
#endif

} // namespace

KeyboardView_320x240 *KeyboardView_320x240::gui = nullptr;

KeyboardView_320x240 *KeyboardView_320x240::instance(void)
{
    if (!gui)
        gui = new KeyboardView_320x240(nullptr, DisplayDriverFactory::create(c_width, c_height));
    return gui;
}

KeyboardView_320x240 *KeyboardView_320x240::instance(const DisplayDriverConfig &cfg)
{
    if (!gui)
        gui = new KeyboardView_320x240(&cfg, DisplayDriverFactory::create(cfg));
    return gui;
}

KeyboardView_320x240::KeyboardView_320x240(const DisplayDriverConfig *cfg, DisplayDriver *driver)
    : MeshtasticView(cfg, driver, new ViewController)
{
}

void KeyboardView_320x240::init(IClientBase *client)
{
    ILOG_DEBUG("KeyboardView_320x240 init...");
    MeshtasticView::init(client);
    createFrame();

    // every keypad feeds the key sink; nothing else is ever focused
    group = lv_group_create();
    lv_group_add_obj(group, keySink);
    lv_group_focus_obj(keySink);
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        const lv_indev_type_t type = lv_indev_get_type(indev);
        if (type == LV_INDEV_TYPE_KEYPAD || type == LV_INDEV_TYPE_ENCODER)
            lv_indev_set_group(indev, group);
        else if (type == LV_INDEV_TYPE_POINTER && lv_indev_get_cursor(indev))
            lv_obj_set_hidden(lv_indev_get_cursor(indev), true); // nothing here can be pointed at
    }
    // the view moves focus itself, so UP/DOWN must arrive as keys and not as group traversal
    input_policy::InputContextState::instance().setFocusSemantic(input_policy::FocusSemantic::Scrollable);

    auto &dispatcher = input_policy::UICommandDispatcher::instance();
    auto bind = [this, &dispatcher](input_policy::UICommand command, std::function<void(void)> action) {
        dispatcher.registerHandler(command, [this, action](const input_policy::CommandPayload &) {
            if (splash || restarting || bluetoothMode || locked)
                return; // a takeover screen owns the keyboard
            action();
            dirty = urgent = true;
        });
    };
    // Home on Home, with nothing open: the screen goes off
    bind(input_policy::UICommand::GoHome, [this]() {
        if (tab == Tab::Home && !inApp() && !inConversation && menu == Menu::None)
            screenOffNow();
        else
            goHome();
    });
    bind(input_policy::UICommand::OpenChats, [this]() { switchTab(Tab::Chats); });
    bind(input_policy::UICommand::OpenMap, [this]() { mapKey(); });
    bind(input_policy::UICommand::QuickChat, [this]() { quickKey(); });
    bind(input_policy::UICommand::ToggleGps, [this]() { activateSetting(SettingRow{Setting::Gps, "", ""}); });
    bind(input_policy::UICommand::SendPing, [this]() {
        controller->sendPing();
        toast("Ping sent");
    });

#if defined(ARCH_PORTDUINO)
    if (getenv("KBVIEW_DEMO"))
        loadDemoData();
    safeMode = getenv("KBVIEW_SAFE") != nullptr;
    if (const char *count = getenv("KBVIEW_STRESS"))
        stressNodes = (uint32_t)std::max(0, atoi(count));
    // KBVIEW_SPLASH=<ms> holds the start screen that long, 0 skips it
    if (const char *ms = getenv("KBVIEW_SPLASH")) {
        splashHold = (uint32_t)std::max(0, atoi(ms));
        splash = splashHold > 0;
    }
#endif

    loadPrefs();
    splashStart = lv_tick_get();
    state = MeshtasticView::eBootScreenDone;
    render();
}

void KeyboardView_320x240::task_handler(void)
{
    MeshtasticView::task_handler();
    mapTask();
    lightTask();
    wifiTask();
    pinsTask();
    radioTask();
    tripTask();
    wardriveTask();
    compassTask();
    clockTask();
    routeTask();
#if defined(ARCH_PORTDUINO)
    if (stressNodes && lv_tick_elaps(stressLast) >= c_stressInterval) {
        stressLast = lv_tick_get();
        stressStep();
    }
#endif

    // the clock and all "x min" ages move once a minute
    const uint32_t minute = now() / 60;
    if (minute != shownMinute) {
        shownMinute = minute;
        dirty = true;
    }
    if (!toastText.empty() && lv_tick_elaps(toastStart) > toastTime) {
        toastText.clear();
        dirty = urgent = true;
    }
    // what must not stay readable: the letter typed last in a password, a pairing PIN nobody took off
    if (menu == Menu::Edit && popup.peek && lv_tick_elaps(popup.peekStart) >= c_peekTime) {
        popup.peek = false;
        dirty = urgent = true;
    }
    if (pairing && lv_tick_elaps(pairingStart) >= c_pairingTime)
        endPairing();
    // The start screen stays for a fixed time; what the radio has not sent by then fills in on the screens.
    // Only time in which the screen is being drawn counts: while the firmware is still starting up this task
    // does not run for seconds, and with a clock that runs from init() the logo was gone as soon as it showed.
    if (splash) {
        const uint32_t step = lv_tick_elaps(splashStart);
        splashStart = lv_tick_get();
        splashSeen += std::min<uint32_t>(step, c_splashStep);
        if (splashSeen >= (splashHold ? splashHold : c_splashTime)) {
            splash = false;
            dirty = urgent = true;
        }
    }
    // a restart that never happened must not lock the keyboard for good
    if (restarting && lv_tick_elaps(restartStart) > c_restartTime) {
        restarting = false;
        dirty = urgent = true;
    }
    // Behind a dark screen there is one more drawing (so that waking never shows what was open before the lock)
    // and then none: what changes meanwhile is drawn when the screen comes back, which asks for it itself.
    if (dirty && screenOff && offDrawn)
        return;
    if (dirty && (urgent || lv_tick_elaps(lastRender) >= c_renderInterval)) {
        offDrawn = screenOff;
        render();
    }
}

// -------- input --------

void KeyboardView_320x240::onKeyEvent(lv_event_t *e)
{
    KeyboardView_320x240 *view = static_cast<KeyboardView_320x240 *>(lv_event_get_user_data(e));
    if (view->splash) {
        // Any key ends the start screen and does nothing else. Any real key: the M9's keyboard reports 0xFF
        // once as it starts, and that took the logo off the moment it showed.
        const lv_event_code_t code = lv_event_get_code(e);
        const bool key = code == LV_EVENT_KEY && lv_event_get_key(e) != c_noKey;
        if ((key || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED) && view->splashSeen >= c_splashGuard) {
            view->splash = false;
            view->dirty = view->urgent = true;
        }
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_KEY && lv_event_get_key(e) == c_noKey)
        return; // not a key
    switch (lv_event_get_code(e)) {
    case LV_EVENT_KEY:
        view->handleKey(lv_event_get_key(e));
        break;
    case LV_EVENT_SHORT_CLICKED:
        view->activate();
        break;
    case LV_EVENT_LONG_PRESSED:
        view->holdAction();
        break;
    default:
        return;
    }
    view->dirty = view->urgent = true;
}

void KeyboardView_320x240::handleKey(uint32_t key)
{
#if defined(ARCH_PORTDUINO)
    // stand-ins on a desktop keyboard: Ctrl+E for the CTRL key, Ctrl+G for the @ key
    if (key == 5)
        key = LV_KEY_CUSTOM_DIACRITIC;
    else if (key == 7)
        key = '@';
#endif
    if (locked) {
        // only holding OK gets past the lock screen
        if (key == LV_KEY_DEL)
            holdAction();
        return;
    }
    if (restarting || bluetoothMode) {
        // Back leaves the Bluetooth screen and keeps Bluetooth on; a restart is waited out
        if (key == LV_KEY_ESC && !restarting)
            bluetoothMode = false;
#if defined(ARCH_PORTDUINO)
        // no Bluetooth to pair with here: P plays the radio announcing a PIN, O the radio taking it off
        if (bluetoothMode && !restarting && (key == 'p' || key == 'o')) {
            meshtastic_ClientNotification note = meshtastic_ClientNotification_init_zero;
            note.level = meshtastic_LogRecord_Level_INFO;
            if (key == 'p')
                snprintf(note.message, sizeof(note.message), "pairing:%06u", (unsigned)(lv_tick_get() % 1000000));
            else
                snprintf(note.message, sizeof(note.message), "pairing:");
            notifyClient(note);
        }
#endif
        return;
    }
    if (key == LV_KEY_CUSTOM_DIACRITIC) {
        // CTRL: the control center, from anywhere; again closes it
        if (menu == Menu::Pick && popup.what == Setting::Control)
            closeMenu();
        else
            openControl();
        return;
    }
    if (menu != Menu::None) {
        handleMenuKey(key);
        return;
    }

    switch (key) {
    case LV_KEY_ENTER:
        return; // OK is handled on release, so that holding it can mean something else
    case LV_KEY_ESC:
        back();
        return;
    case LV_KEY_HOME:
        goHome();
        return;
    case LV_KEY_END: // stand-in for the tab keys on a desktop keyboard
        switchTab((Tab)(((uint8_t)tab + 1) % (uint8_t)Tab::Count));
        return;
    case LV_KEY_DEL: // stand-in for the Triangle key on a desktop keyboard
        holdAction();
        return;
    default:
        break;
    }

    if (inConversation) {
        Chat &chat = chatFor(convKey);
        caret = std::min(caret, chat.draft.size());
        const int32_t count = (int32_t)chat.messages.size();
        if (printable(key)) {
            if (chat.draft.size() < c_maxDraft) {
                chat.draft.insert(caret, 1, (char)key);
                caret++;
            }
            convSelect = -1;
        } else if (key == LV_KEY_BACKSPACE) {
            if (caret > 0) {
                chat.draft.erase(caret - 1, 1);
                caret--;
            } else if (chat.draft.empty()) {
                chat.replyTo = 0; // nothing left to delete but the quote
            }
        } else if (key == LV_KEY_LEFT) {
            if (caret > 0)
                caret--;
        } else if (key == LV_KEY_RIGHT) {
            if (caret < chat.draft.size())
                caret++;
        } else if (key == LV_KEY_UP) {
            // up from the composer walks the messages, newest first; OK on one quotes it
            if (convSelect < 0)
                convSelect = count - 1;
            else if (convSelect > 0)
                convSelect--;
        } else if (key == LV_KEY_DOWN) {
            if (convSelect >= 0)
                convSelect = convSelect + 1 < count ? convSelect + 1 : -1;
        }
        return;
    }

    // on the tab bar left and right change the tab, up or OK go back into the screen
    if (barFocus) {
        if (key == LV_KEY_LEFT)
            walkTabs(-1);
        else if (key == LV_KEY_RIGHT)
            walkTabs(1);
        else if (key == LV_KEY_UP)
            barFocus = false;
        return;
    }

    // a screen on top of Home has its own keys, letters included
    if (inApp()) {
        handleAppKey(key);
        return;
    }

    if (key == '@' && tab != Tab::Nodes) {
        openMentions();
        return;
    }
    // a key the keyboard reports without a meaning: say which, so that it can be given one
    if (key >= 0x80 && key < 0x100) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Key 0x%02X", (unsigned)key);
        toast(buf);
        return;
    }

    switch (tab) {
    case Tab::Home:
        if (key == LV_KEY_LEFT && homeFocus % c_homeColumns > 0)
            homeFocus--;
        else if (key == LV_KEY_RIGHT && homeFocus % c_homeColumns + 1 < c_homeColumns)
            homeFocus++;
        else if (key == LV_KEY_UP && homeFocus >= c_homeColumns)
            homeFocus -= c_homeColumns;
        else if (key == LV_KEY_DOWN && homeFocus + c_homeColumns < c_homeSlots)
            homeFocus += c_homeColumns;
        else if (key == LV_KEY_DOWN)
            barFocus = true; // below the tiles is the tab bar
        break;
    case Tab::Chats:
        // lists only use up and down, so left and right walk the tab bar; typing searches the messages
        if (printable(key))
            openSearch(std::string(1, (char)key));
        else if (key == LV_KEY_UP)
            moveListFocus(-1);
        else if (key == LV_KEY_DOWN)
            moveListFocus(1);
        else if (key == LV_KEY_LEFT)
            walkTabs(-1);
        else if (key == LV_KEY_RIGHT)
            walkTabs(1);
        break;
    case Tab::Nodes:
        if (key == LV_KEY_UP) {
            moveListFocus(-1);
        } else if (key == LV_KEY_DOWN) {
            moveListFocus(1);
        } else if (key == LV_KEY_LEFT) {
            walkTabs(-1);
        } else if (key == LV_KEY_RIGHT) {
            walkTabs(1);
        } else if (key == LV_KEY_BACKSPACE) {
            if (!nodeFilter.empty())
                nodeFilter.pop_back();
        } else if (printable(key) && key != '@' && nodeFilter.size() < 16) {
            nodeFilter.push_back((char)key);
            nodesTop = 0;
        }
        break;
    case Tab::Map:
        handleMapKey(key);
        break;
    case Tab::Apps:
        appsKey(key);
        break;
    case Tab::Settings:
        handleSettingsKey(key);
        break;
    default:
        break;
    }
}

void KeyboardView_320x240::activate(void)
{
    if (bluetoothMode && !restarting && !locked)
        useOneTimePins();
    if (restarting || bluetoothMode || locked)
        return;
    if (menu != Menu::None) {
        activateMenuItem();
        return;
    }
    if (inConversation) {
        Chat &chat = chatFor(convKey);
        if (convSelect < 0 || convSelect >= (int32_t)chat.messages.size()) {
            sendDraft();
        } else if (chat.messages[convSelect].id == 0) {
            toast("Stored without an id, cannot be quoted");
        } else {
            chat.replyTo = chat.messages[convSelect].id;
            convSelect = -1;
        }
        return;
    }
    if (barFocus) {
        barFocus = false;
        return;
    }
    if (inApp()) {
        activateApp();
        return;
    }
    switch (tab) {
    case Tab::Home:
        openTile(homeTiles[std::min<size_t>(homeFocus, c_homeSlots - 1)]);
        break;
    case Tab::Chats:
        if (!chatOrder().empty())
            openChat(chatsFocus);
        break;
    case Tab::Nodes:
        if (focusedNode())
            openNodeDetail(focusedNode());
        break;
    case Tab::Map:
        handleMapKey(LV_KEY_ENTER);
        break;
    case Tab::Apps:
        appsKey(LV_KEY_ENTER);
        break;
    case Tab::Settings: {
        const std::vector<SettingRow> rows = settingRows(page);
        if (settingFocus < rows.size())
            activateSetting(rows[settingFocus]);
        break;
    }
    default:
        break;
    }
}

void KeyboardView_320x240::holdAction(void)
{
    if (restarting)
        return;
    if (locked) {
        locked = false;
        return;
    }
    if (bluetoothMode) {
        leaveBluetoothMode();
        return;
    }
    if (menu != Menu::None)
        return;
    if (inConversation) {
        // on a picked message: what can be done with it; in the composer: the quick replies
        if (convSelect >= 0)
            openMessageMenu();
        else
            openQuickReplies();
        return;
    }
    if (barFocus)
        return;
    if (inApp()) {
        holdApp();
        return;
    }
    switch (tab) {
    case Tab::Home:
        openHomeMenu();
        break;
    case Tab::Chats:
        if (!chatOrder().empty())
            openChatMenu(chatsFocus);
        break;
    case Tab::Nodes:
        if (focusedNode())
            openNodeMenu(focusedNode());
        break;
    case Tab::Map:
        openMapMenu();
        break;
    case Tab::Apps:
        openAppMenu();
        break;
    case Tab::Settings: {
        const std::vector<SettingRow> rows = settingRows(page);
        if (page == Page::Channels && settingFocus < rows.size() && rows[settingFocus].id == Setting::Channel)
            openChannelMenu((uint8_t)rows[settingFocus].arg);
        break;
    }
    default:
        break;
    }
}

/**
 * MAP key: opens the map ready to be moved; on the map it switches between moving it and the tab bar.
 */
void KeyboardView_320x240::mapKey(void)
{
    if (tab == Tab::Map && !inConversation) {
        closeMenu();
        barFocus = !barFocus;
    } else {
        switchTab(Tab::Map);
    }
}

/**
 * Preset key: quick replies while in a conversation, otherwise the newest unread message.
 */
void KeyboardView_320x240::quickKey(void)
{
    if (inConversation)
        openQuickReplies();
    else
        openMentions();
}

/**
 * One tab to the left or right. Screens that use left and right themselves keep the focus on the tab bar.
 */
void KeyboardView_320x240::walkTabs(int delta)
{
    const int to = (int)tab + delta;
    if (to < 0 || to >= (int)Tab::Count)
        return;
    switchTab((Tab)to);
    barFocus = tab == Tab::Home || tab == Tab::Map;
}

void KeyboardView_320x240::moveListFocus(int delta)
{
    if (tab == Tab::Chats) {
        const std::vector<uint32_t> order = chatOrder();
        if (order.empty())
            return;
        auto it = std::find(order.begin(), order.end(), chatsFocus);
        int index = it == order.end() ? 0 : (int)(it - order.begin()) + delta;
        chatsFocus = order[std::clamp(index, 0, (int)order.size() - 1)];
    } else if (tab == Tab::Nodes) {
        const std::vector<const NodeRecord *> list = visibleNodes();
        if (list.empty())
            return;
        auto it = std::find_if(list.begin(), list.end(), [this](const NodeRecord *node) { return node->id == nodesFocus; });
        int index = it == list.end() ? 0 : (int)(it - list.begin()) + delta;
        nodesFocus = list[std::clamp(index, 0, (int)list.size() - 1)]->id;
    }
}

// -------- navigation --------

void KeyboardView_320x240::switchTab(Tab to)
{
    // CHAT inside a conversation goes back to the list
    inConversation = false;
    barFocus = false;
    closeMenu();
    if (to == tab)
        return;
    if (to == Tab::Home) {
        goHome();
        return;
    }
    trail.push_back(tab);
    if (trail.size() > c_maxTrail)
        trail.erase(trail.begin());
    tab = to;
    if (to == Tab::Apps)
        app = App::None; // the list
    if (to == Tab::Settings) {
        page = Page::Root;
        settingFocus = settingTop = rootFocus = 0;
    }
}

/**
 * Home is the root: it clears the trail, so Back on Home does nothing.
 */
void KeyboardView_320x240::goHome(void)
{
    inConversation = false;
    barFocus = false;
    closeMenu();
    trail.clear();
    tab = Tab::Home;
    app = App::None;
}

/**
 * Back closes the top layer, one per press, and then walks the tab trail.
 */
void KeyboardView_320x240::back(void)
{
    if (menu != Menu::None) {
        closeMenu();
    } else if (inConversation && convSelect >= 0) {
        convSelect = -1; // back to the composer
    } else if (inConversation && chatFor(convKey).replyTo) {
        chatFor(convKey).replyTo = 0; // an ordinary message again
    } else if (inConversation) {
        inConversation = false; // the draft stays with the conversation
    } else if (inApp()) {
        closeApp();
    } else if (tab == Tab::Home && barFocus) {
        barFocus = false;
    } else if (tab == Tab::Map && !barFocus) {
        barFocus = true; // first out of moving the map, then on along the trail
    } else if (tab == Tab::Settings && page != Page::Root) {
        page = Page::Root;
        settingFocus = rootFocus;
        settingTop = 0;
    } else if (tab == Tab::Nodes && !nodeFilter.empty()) {
        nodeFilter.clear();
        nodesTop = 0;
    } else if (!trail.empty()) {
        tab = trail.back();
        trail.pop_back();
        barFocus = false;
    } else if (tab != Tab::Home) {
        tab = Tab::Home;
        barFocus = false;
    }
}

/**
 * A conversation is a layer on the Chats tab; opening it from elsewhere records where we came from.
 */
void KeyboardView_320x240::openChat(uint32_t key)
{
    closeMenu();
    barFocus = false;
    if (tab != Tab::Chats) {
        trail.push_back(tab);
        if (trail.size() > c_maxTrail)
            trail.erase(trail.begin());
        tab = Tab::Chats;
    }
    Chat &chat = chatFor(key);
    chat.unread = 0;
    inConversation = true;
    convKey = key;
    chatsFocus = key;
    caret = chat.draft.size();
    convSelect = -1;
}

/**
 * Newest unread conversation, else the most recent one, else the chat list.
 */
void KeyboardView_320x240::openUnread(void)
{
    const Chat *best = nullptr;
    uint32_t bestKey = 0;
    for (const auto &[key, chat] : conversations) {
        if (chat.messages.empty())
            continue;
        bool better = true;
        if (best) {
            const bool unread = chat.unread > 0, bestUnread = best->unread > 0;
            better = unread != bestUnread ? unread : chat.lastTime > best->lastTime;
        }
        if (better) {
            best = &chat;
            bestKey = key;
        }
    }
    if (best)
        openChat(bestKey);
    else
        switchTab(Tab::Chats);
}

void KeyboardView_320x240::sendDraft(void)
{
    Chat &chat = chatFor(convKey);
    if (chat.draft.empty())
        return;
    const std::string text = chat.draft;
    const uint32_t replyId = chat.replyTo;
    chat.draft.clear();
    chat.replyTo = 0;
    caret = 0;
    convSelect = -1;
    sendText(convKey, text, replyId);
}

/**
 * Sends a text to a chat: a channel or a node.
 */
void KeyboardView_320x240::sendText(uint32_t key, const std::string &text, uint32_t replyId)
{
    if (text.empty())
        return;

    uint32_t to = UINT32_MAX;
    uint8_t ch = 0;
    const uint8_t maxHops = lora.hop_limit ? lora.hop_limit : 3;
    uint8_t hopLimit = maxHops;
    bool usePkc = false;
    uint32_t requestId;
    auto callback = [this](const ResponseHandler::Request &req, ResponseHandler::EventType evt, int32_t) {
        if (evt == ResponseHandler::timeout) {
            controller->setTextMessageStatus(req.packetId, LogMessage::eNoResponse, true);
            setMessageStatus((uint32_t)(uintptr_t)req.cookie, req.packetId, LogMessage::eNoResponse);
        }
    };

    if (key < c_max_channels) {
        ch = (uint8_t)key;
        requestId = requests.addRequest(ch, ResponseHandler::TextMessageRequest, (void *)(uintptr_t)key, callback);
    } else {
        const NodeRecord *node = nodeStore.find(key);
        to = key;
        ch = node ? node->channel : 0;
        usePkc = node && node->hasKey;
        if (node && node->hopsAway >= 0)
            hopLimit = node->hopsAway < maxHops ? node->hopsAway + 1 : node->hopsAway;
        requestId = requests.addRequest(to, ResponseHandler::TextMessageRequest, (void *)(uintptr_t)key, callback);
    }

    const uint32_t msgTime = now();
    // shown at once with a pending mark; the status follows in textMessageStatus()
    appendMessage(key, Message{ownNode, msgTime, requestId, LogMessage::eDefault, true, text, requestId, replyId});
    // the controller copies a whole payload, whatever the length of the text
    char payload[meshtastic_Constants_DATA_PAYLOAD_LEN] = {0};
    strncpy(payload, text.c_str(), sizeof(payload) - 1);
    controller->sendTextMessage(to, ch, hopLimit, msgTime, requestId, usePkc, payload, replyId);
}

void KeyboardView_320x240::toast(const char *text, uint32_t ms)
{
    toastText = text;
    toastStart = lv_tick_get();
    toastTime = ms;
    dirty = urgent = true;
}

// -------- model --------

/**
 * Nodes that pass the live filter, most recently heard first.
 * TODO: favorites first, once the node store carries the favorite flag
 */
std::vector<const NodeRecord *> KeyboardView_320x240::visibleNodes(void) const
{
    const std::string filter = folded(nodeFilter.c_str());
    std::vector<const NodeRecord *> list;
    list.reserve(nodeStore.size());
    for (const auto &[id, node] : nodeStore.records()) {
        if (id == ownNode)
            continue;
        if (!filter.empty()) {
            char hex[12];
            snprintf(hex, sizeof(hex), "!%08x", (unsigned)id);
            if (folded(node.user.long_name).find(filter) == std::string::npos &&
                folded(node.user.short_name).find(filter) == std::string::npos &&
                std::string(hex).find(filter) == std::string::npos)
                continue;
        }
        list.push_back(&node);
    }
    // favorites lead the list, ignored nodes close it
    auto rank = [this](const NodeRecord *node) { return favorites.count(node->id) ? 0 : ignoredNodes.count(node->id) ? 2 : 1; };
    std::sort(list.begin(), list.end(), [&rank](const NodeRecord *a, const NodeRecord *b) {
        if (rank(a) != rank(b))
            return rank(a) < rank(b);
        return a->lastHeard != b->lastHeard ? a->lastHeard > b->lastHeard : a->id < b->id;
    });
    return list;
}

/**
 * Channels and direct messages in one list: the pinned chats in the order they were given, then unread first,
 * then most recent.
 */
std::vector<uint32_t> KeyboardView_320x240::chatOrder(void) const
{
    const std::vector<uint32_t> top = pinnedChats();
    auto isPinned = [&top](uint32_t key) { return std::find(top.begin(), top.end(), key) != top.end(); };
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < c_max_channels; i++) {
        if (channelInfo[i].enabled && !isPinned(i))
            order.push_back(i);
    }
    for (const auto &[key, chat] : conversations) {
        const bool listed = (key < c_max_channels && channelInfo[key].enabled) || isPinned(key);
        const bool open = inConversation && convKey == key;
        if (!listed && (open || !chat.messages.empty() || !chat.draft.empty()))
            order.push_back(key);
    }
    std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b) {
        const auto ia = conversations.find(a), ib = conversations.find(b);
        const uint32_t unreadA = ia == conversations.end() ? 0 : ia->second.unread;
        const uint32_t unreadB = ib == conversations.end() ? 0 : ib->second.unread;
        if ((unreadA > 0) != (unreadB > 0))
            return unreadA > 0;
        const uint32_t timeA = ia == conversations.end() ? 0 : ia->second.lastTime;
        const uint32_t timeB = ib == conversations.end() ? 0 : ib->second.lastTime;
        return timeA != timeB ? timeA > timeB : a < b;
    });
    order.insert(order.begin(), top.begin(), top.end());
    return order;
}

KeyboardView_320x240::Chat &KeyboardView_320x240::chatFor(uint32_t key)
{
    return conversations[key];
}

void KeyboardView_320x240::appendMessage(uint32_t key, Message &&msg)
{
    if (key < c_max_channels && !msg.own)
        markHeard(msg.from, (uint8_t)key); // whoever writes on a channel has its key
    Chat &chat = chatFor(key);
    chat.lastTime = msg.time;
    chat.messages.push_back(std::move(msg));
    // the oldest message goes: of this chat when it is full, of the longest chat when all together are too many
    uint32_t victim = UINT32_MAX;
    if (chat.messages.size() > c_maxMessages) {
        victim = key;
    } else {
        size_t total = 0, most = 0;
        uint32_t longest = key;
        for (const auto &[other, entry] : conversations) {
            total += entry.messages.size();
            if (entry.messages.size() > most) {
                most = entry.messages.size();
                longest = other;
            }
        }
        if (total > c_maxTotalMessages)
            victim = longest;
    }
    if (victim != UINT32_MAX) {
        Chat &from = chatFor(victim);
        const uint32_t gone = from.messages.front().id;
        from.messages.erase(from.messages.begin());
        if (gone) {
            from.reactions.erase(std::remove_if(from.reactions.begin(), from.reactions.end(),
                                                [gone](const Reaction &reaction) { return reaction.target == gone; }),
                                 from.reactions.end());
        }
        // the picked message moved up with the rest, and so did the one a menu is open about
        if (inConversation && convKey == victim) {
            if (convSelect >= 0)
                convSelect--;
            if (convTop > 0)
                convTop--;
            if (menu == Menu::Pick && popup.what == Setting::MessageMenu) {
                if (popup.target == 0)
                    closeMenu(); // its message is the one that went
                else
                    popup.target--;
            }
        }
        // A direct chat that lost its last message this way is gone: left in the list of chats it could not be
        // opened or read, and every new sender would leave one behind for good.
        if (victim != key && victim >= c_max_channels && from.messages.empty() && from.draft.empty() &&
            !(inConversation && convKey == victim) && !pinned(victim))
            conversations.erase(victim);
    }
    // Chats are bounded as well: of the direct chats beyond the limit the one that has been quiet longest goes.
    if (conversations.size() > c_maxChats) {
        uint32_t oldest = UINT32_MAX, oldestTime = UINT32_MAX;
        for (const auto &[other, entry] : conversations) {
            if (other < c_max_channels || other == key || (inConversation && other == convKey) || !entry.draft.empty() ||
                pinned(other))
                continue;
            if (entry.lastTime < oldestTime) {
                oldestTime = entry.lastTime;
                oldest = other;
            }
        }
        if (oldest != UINT32_MAX)
            conversations.erase(oldest);
    }
    dirty = true;
}

const KeyboardView_320x240::Message *KeyboardView_320x240::findMessage(uint32_t key, uint32_t id) const
{
    const auto it = conversations.find(key);
    if (id == 0 || it == conversations.end())
        return nullptr;
    for (auto msg = it->second.messages.rbegin(); msg != it->second.messages.rend(); ++msg) {
        if (msg->id == id)
            return &*msg;
    }
    return nullptr;
}

/**
 * One line that stands for a quoted message: who wrote it and how it starts.
 */
std::string KeyboardView_320x240::quoteOf(uint32_t key, uint32_t id, bool withName) const
{
    const Message *msg = findMessage(key, id);
    if (!msg)
        return "Earlier message";
    size_t length = strnlen(msg->text.c_str(), c_quoteChars);
    // not in the middle of a character: a label that has to shorten such a text reads past its end
    while (length > 0 && ((uint8_t)msg->text.c_str()[length] & 0xC0) == 0x80)
        length--;
    std::string line(msg->text.c_str(), length);
    std::replace(line.begin(), line.end(), '\n', ' ');
    if (!withName)
        return line;
    return (msg->own ? std::string("You") : senderName(msg->from)) + ": " + line;
}

void KeyboardView_320x240::setMessageStatus(uint32_t key, uint32_t requestId, LogMessage::MsgStatus status)
{
    auto it = conversations.find(key);
    if (it == conversations.end())
        return;
    std::vector<Message> &list = it->second.messages;
    for (auto msg = list.rbegin(); msg != list.rend(); ++msg) {
        if (msg->requestId == requestId) {
            msg->status = status;
            dirty = true;
            break;
        }
    }
}

std::string KeyboardView_320x240::chatName(uint32_t key) const
{
    if (key < c_max_channels) {
        const ChannelInfo &info = channelInfo[key];
        if (!info.name.empty())
            return info.name;
        if (info.primary && lora.use_preset)
            return LoRaPresets::modemPresetToString(lora.modem_preset);
        char buf[16];
        snprintf(buf, sizeof(buf), "Channel %u", (unsigned)key);
        return buf;
    }
    const NodeRecord *node = nodeStore.find(key);
    if (node && node->user.long_name[0])
        return node->user.long_name;
    char buf[16];
    snprintf(buf, sizeof(buf), "!%08x", (unsigned)key);
    return buf;
}

std::string KeyboardView_320x240::senderName(uint32_t nodeNum) const
{
    if (prefs.longNames) {
        const NodeRecord *node = nodeStore.find(nodeNum);
        if (node && node->user.long_name[0])
            return std::string(node->user.long_name, strnlen(node->user.long_name, sizeof(node->user.long_name)));
    }
    return shortName(nodeNum);
}

std::string KeyboardView_320x240::shortName(uint32_t nodeNum) const
{
    const NodeRecord *node = nodeStore.find(nodeNum);
    if (node && node->user.short_name[0])
        return std::string(node->user.short_name, strnlen(node->user.short_name, sizeof(node->user.short_name)));
    char buf[8];
    snprintf(buf, sizeof(buf), "%04x", (unsigned)(nodeNum & 0xffff));
    return buf;
}

uint32_t KeyboardView_320x240::totalUnread(void) const
{
    uint32_t count = 0;
    for (const auto &[key, chat] : conversations) {
        if (chatMuted(key))
            continue; // a muted chat keeps its own badge and stays out of the total
        count += chat.unread;
    }
    return count;
}

uint32_t KeyboardView_320x240::now(void) const
{
    time_t t;
    time(&t);
    return validTime((uint32_t)t) ? (uint32_t)t : actTime;
}

// -------- updates from the radio --------

bool KeyboardView_320x240::setupUIConfig(const meshtastic_DeviceUIConfig &uiconfig)
{
    if (uiconfig.version == 1) {
        uiConfig = uiconfig;
    } else {
        // nothing stored on this device yet
        uiConfig = meshtastic_DeviceUIConfig{};
        uiConfig.version = 1;
        uiConfig.screen_brightness = c_defaultBrightness;
        uiConfig.screen_timeout = 30;
        uiConfig.alert_enabled = true;
        uiConfig.banner_enabled = true;
        controller->storeUIConfig(uiConfig);
    }
    // there is no finding the way back out of a black screen with a keyboard
    if (uiConfig.screen_brightness < c_minBrightness)
        uiConfig.screen_brightness = c_defaultBrightness;
    if (displaydriver->hasLight())
        displaydriver->setBrightness((uint8_t)uiConfig.screen_brightness);
    displaydriver->setScreenTimeout(uiConfig.screen_timeout);

    if (state == MeshtasticView::eBootScreenDone)
        state = MeshtasticView::eInitDone;
    dirty = true;
    return true;
}

void KeyboardView_320x240::setMyInfo(uint32_t nodeNum, meshtastic_MyNodeInfo_device_id_t device_id)
{
    ownNode = nodeNum;
    if (state == MeshtasticView::eBootScreenDone)
        state = MeshtasticView::eInitDone;
    dirty = true;
}

void KeyboardView_320x240::addOrUpdateNode(uint32_t nodeNum, uint8_t channel, uint32_t lastHeard, const meshtastic_User &cfg)
{
    if (!nodeStore.find(nodeNum) && nodeStore.size() >= MAX_NUM_NODES_VIEW) {
        const NodeId purge = nodeStore.selectPurgeCandidate(nodeNum, ownNode, now());
        if (!purge)
            return;
        dropMarker(purge);
        heardOn.erase(purge);
        positionTimes.erase(purge);
        signalLog.erase(purge);
        nodeStore.remove(purge);
    }
    nodeStore.upsertUser(nodeNum, channel, lastHeard, cfg, false);
    markHeard(nodeNum, channel); // the channel its node info came in on
    if (nodeNum == ownNode) {
        ownUser = cfg;
        hasOwnUser = true;
#if defined(ARCH_PORTDUINO)
        // the sample mesh needs somebody to be near to: the simulated radio has no GPS
        const NodeRecord *own = nodeStore.find(ownNode);
        if (demoData && own && !own->position.hasCoordinates()) {
            NodePosition position;
            position.known = true;
            position.latitude = 442600000;
            position.longitude = -725600000;
            position.altitude = 240;
            position.satellites = 9;
            position.precision = 32;
            nodeStore.updatePosition(ownNode, position);
        }
#endif
    }
    placeMarker(nodeNum); // the name on the marker may have changed
    dirty = true;
}

void KeyboardView_320x240::updatePosition(uint32_t nodeNum, int32_t lat, int32_t lon, int32_t alt, uint32_t sats,
                                          uint32_t precision)
{
    NodePosition position;
    position.known = true;
    position.latitude = lat;
    position.longitude = lon;
    position.altitude = alt;
    position.satellites = sats;
    position.precision = precision;
    nodeStore.updatePosition(nodeNum, position);
    // the positions the radio had stored come without their age; what arrives after that is news
    if (positionsLive && nodeStore.find(nodeNum))
        positionTimes[nodeNum] = now();
    if (positionsLive && nodeNum == ownNode) {
        tripFix(lat, lon, sats);
        borderFix(lat, lon);
    }
    if (nodeNum == ownNode)
        routeFix(lat, lon);
    placeMarker(nodeNum);
    if (pingRequest && nodeNum == pingTarget) {
        // the answer to a ping is the position itself
        requests.removeRequest(pingRequest, ResponseHandler::PositionRequest);
        pingTarget = pingRequest = 0;
        toast((chatName(nodeNum) + " answered with a position").c_str(), 3000);
    }
    dirty = true;
}

void KeyboardView_320x240::updateMetrics(uint32_t nodeNum, const meshtastic_DeviceMetrics &metrics)
{
    nodeStore.updateDeviceMetrics(nodeNum, metrics);
    if (nodeNum == ownNode && metrics.has_channel_utilization && metrics.has_air_util_tx && !demoData) {
        channelUse = metrics.channel_utilization;
        txUse = metrics.air_util_tx;
        hasAirtime = true;
    }
    dirty = true;
}

/**
 * Signal is only reported for packets heard directly, so the node is a neighbor.
 */
void KeyboardView_320x240::updateSignalStrength(uint32_t nodeNum, int32_t rssi, float snr)
{
    nodeStore.updateSignal(nodeNum, rssi, snr);
    nodeStore.updateHops(nodeNum, 0);
    if (nodeNum != ownNode) {
        lastSnr = snr;
        hasSnr = true;
        // a short history per node, for the signal chart
        auto log = signalLog.find(nodeNum);
        if (log == signalLog.end()) {
            // room for a new neighbor is made by the one that has been silent longest
            if (signalLog.size() >= c_signalNodes) {
                auto stale = signalLog.begin();
                for (auto it = signalLog.begin(); it != signalLog.end(); ++it) {
                    if (it->second.empty() || (!stale->second.empty() && it->second.back().time < stale->second.back().time))
                        stale = it;
                }
                signalLog.erase(stale);
            }
            log = signalLog.emplace(nodeNum, std::vector<SignalSample>()).first;
        }
        if (log != signalLog.end()) {
            if (log->second.size() >= c_signalSamples)
                log->second.erase(log->second.begin());
            log->second.push_back(SignalSample{now(), snr, (int16_t)rssi});
        }
        if (heard.size() >= c_heardKept)
            heard.erase(heard.begin());
        heard.push_back(Heard{now(), nodeNum, snr, (int16_t)rssi});
        wardriveHeard(nodeNum, rssi, snr);
    }
    dirty = true;
}

void KeyboardView_320x240::updateHopsAway(uint32_t nodeNum, uint8_t hopsAway)
{
    nodeStore.updateHops(nodeNum, (int8_t)hopsAway);
    dirty = true;
}

void KeyboardView_320x240::updateLastHeard(uint32_t nodeNum)
{
    nodeStore.updateLastHeard(nodeNum, now());
    dirty = true;
}

void KeyboardView_320x240::updateTime(uint32_t timeVal)
{
    time_t t;
    time(&t);
    if (validTime((uint32_t)t))
        actTime = (uint32_t)t;
    else if (timeVal > actTime)
        actTime = timeVal;
}

void KeyboardView_320x240::removeNode(uint32_t nodeNum)
{
    dropMarker(nodeNum);
    heardOn.erase(nodeNum);
    positionTimes.erase(nodeNum);
    nodeStore.remove(nodeNum);
    favorites.erase(nodeNum);
    ignoredNodes.erase(nodeNum);
    mutedNodes.erase(nodeNum);
    signalLog.erase(nodeNum);
    dirty = true;
}

bool KeyboardView_320x240::hasKnownNodeForPacket(uint32_t nodeNum) const
{
    return nodeStore.find(nodeNum) != nullptr;
}

/**
 * A packet that was decoded with a channel's key says that its sender has that key. Direct messages are
 * encrypted to the node and say nothing about a channel.
 */
void KeyboardView_320x240::packetReceived(const meshtastic_MeshPacket &p)
{
    MeshtasticView::packetReceived(p);
    if (!p.pki_encrypted)
        markHeard(p.from, p.channel);
}

void KeyboardView_320x240::updateChannelConfig(const meshtastic_Channel &ch)
{
    if (demoData)
        return;
    storeChannel(ch);
}

void KeyboardView_320x240::updateLoRaConfig(const meshtastic_Config_LoRaConfig &cfg)
{
    if (demoData)
        return;
    lora = cfg;
    hasLora = true;
    dirty = true;
}

void KeyboardView_320x240::textMessageStatus(uint32_t requestId, LogMessage::MsgStatus status)
{
    const auto request = requests.findRequest(requestId, ResponseHandler::TextMessageRequest);
    if (request.type != ResponseHandler::TextMessageRequest)
        return;
    const uint32_t key = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(request.cookie));
    // nobody acknowledges a channel message; hearing it relayed is as good as it gets
    if (key < c_max_channels && status == LogMessage::eHeard)
        status = LogMessage::eAcked;
    if (status == LogMessage::eHeard || (key < c_max_channels && status == LogMessage::eAcked)) {
        auto chat = conversations.find(key);
        if (chat != conversations.end()) {
            for (auto msg = chat->second.messages.rbegin(); msg != chat->second.messages.rend(); ++msg) {
                if (msg->requestId == requestId) {
                    msg->heard = true;
                    break;
                }
            }
        }
    }
    setMessageStatus(key, requestId, status);
    if (status == LogMessage::eAcked || status == LogMessage::eFailed || status == LogMessage::eNoResponse)
        requests.removeRequest(requestId);
}

/**
 * Why the radio could not send a message; the status that follows says that it failed.
 */
void KeyboardView_320x240::textMessageError(uint32_t requestId, uint8_t reason)
{
    const auto request = requests.findRequest(requestId, ResponseHandler::TextMessageRequest);
    if (request.type != ResponseHandler::TextMessageRequest)
        return;
    auto chat = conversations.find(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(request.cookie)));
    if (chat == conversations.end())
        return;
    for (auto msg = chat->second.messages.rbegin(); msg != chat->second.messages.rend(); ++msg) {
        if (msg->requestId == requestId) {
            msg->reason = reason;
            break;
        }
    }
}

/**
 * How far a message of this device got, in words. Brief for the line below a message, else a sentence.
 * What the mesh can tell: that a neighbor relayed it ("sent to mesh"), and for a direct message that the radio
 * of the other side confirmed it ("received"). Nobody confirms a channel message.
 */
std::string KeyboardView_320x240::messageState(uint32_t key, const Message &msg, bool brief) const
{
    const bool direct = key >= c_max_channels;
    switch (msg.status) {
    case LogMessage::eDefault:
        if (msg.requestId)
            return brief ? "sending" : "Being sent";
        return brief ? "sent" : "Sent; what became of it is not known";
    case LogMessage::eHeard:
        return brief ? "sent to mesh" : "Sent to the mesh: another node relayed it";
    case LogMessage::eAcked:
        if (direct)
            return brief ? "received" : "Received: the radio of the other side confirmed it";
        return brief ? "sent to mesh" : "Sent to the mesh: another node relayed it";
    case LogMessage::eNoResponse:
        if (direct)
            return msg.heard ? (brief ? "sent to mesh, not confirmed" : "Sent to the mesh, but the other side did not confirm it")
                             : (brief ? "not confirmed" : "The other side did not confirm it");
        return brief ? "no relay heard" : "No other node was heard relaying it";
    case LogMessage::eFailed:
        switch (msg.reason) {
        case meshtastic_Routing_Error_NO_CHANNEL:
            return brief ? "failed: no such channel" : "Failed: the other side does not have this channel or its key";
        case meshtastic_Routing_Error_TOO_LARGE:
            return brief ? "failed: too long" : "Failed: too long for one packet";
        case meshtastic_Routing_Error_DUTY_CYCLE_LIMIT:
            return brief ? "failed: airtime limit" : "Failed: this radio has used up its legal airtime for now";
        case meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
            return brief ? "failed: too many messages" : "Failed: too many messages in a short time";
        case meshtastic_Routing_Error_NO_INTERFACE:
            return brief ? "failed: radio not ready" : "Failed: the radio was not ready to send";
        case meshtastic_Routing_Error_PKI_FAILED:
        case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY:
        case meshtastic_Routing_Error_PKI_SEND_FAIL_PUBLIC_KEY:
            return brief ? "failed: key not known" : "Failed: the key of the other side is not known or has changed";
        case meshtastic_Routing_Error_NOT_AUTHORIZED:
        case meshtastic_Routing_Error_BAD_REQUEST:
            return brief ? "failed: refused" : "Failed: the other side refused it";
        case meshtastic_Routing_Error_NO_ROUTE:
            return brief ? "failed: no route" : "Failed: no route to the other side";
        case meshtastic_Routing_Error_GOT_NAK:
            return brief ? "failed: turned down" : "Failed: a node on the way turned it down";
        case meshtastic_Routing_Error_TIMEOUT:
            return brief ? "failed: timed out" : "Failed: timed out";
        default:
            break;
        }
        // all tries are used up, or the reason was lost with a restart
        if (direct && msg.heard)
            return brief ? "sent to mesh, not confirmed" : "Sent to the mesh, but the other side did not confirm it";
        return brief ? "failed to send to mesh" : "Failed to send to the mesh: no other node was heard relaying it";
    default:
        return brief ? "sent" : "";
    }
}

void KeyboardView_320x240::newMessage(uint32_t from, uint32_t to, uint8_t ch, const char *msg, uint32_t &msgtime, bool restore)
{
    newTextMessage(from, to, ch, msg, msgtime, 0, 0);
}

void KeyboardView_320x240::newTextMessage(uint32_t from, uint32_t to, uint8_t ch, const char *msg, uint32_t &msgtime, uint32_t id,
                                          uint32_t replyId)
{
    if (to == UINT32_MAX && ch >= c_max_channels)
        return;
    if (!validTime(msgtime))
        msgtime = now();
    const bool own = from == ownNode;
    const uint32_t key = to == UINT32_MAX ? ch : own ? to : from;
    if (to != UINT32_MAX && key < c_max_channels)
        return; // no node has a number that small: filed under it, the answer would go out to a whole channel
    appendMessage(key, Message{from, msgtime, 0, LogMessage::eNone, own, msg, id, replyId});
    if (!own && !(inConversation && convKey == key && !locked && !screenOff))
        chatFor(key).unread++;
    if (!own)
        alertMessage(key, mecpUrgent(msg)); // a Mayday or Urgent message gets through a muted chat
}

void KeyboardView_320x240::restoreMessage(const LogMessage &msg)
{
    const bool own = msg.from == ownNode;
    const uint32_t key = msg.to == UINT32_MAX ? msg.ch : own ? msg.to : msg.from;
    if ((msg.to == UINT32_MAX) != (key < c_max_channels))
        return; // a broadcast on a channel that does not exist, or a direct message of a node that cannot
    if (msg.trashFlag) {
        // the chat was deleted after these messages were logged
        conversations.erase(key);
        dirty = true;
        return;
    }
    // the controller logs the packet id and the id replied to behind the text
    // a record whose length is nonsense is read no further than its buffer
    const size_t size = std::min<size_t>(msg._size, sizeof(msg.bytes));
    const size_t length = strnlen((const char *)msg.bytes, size);
    uint32_t ids[2] = {0, 0};
    uint8_t kind = 0;
    if (size >= length + 1 + sizeof(ids))
        memcpy(ids, msg.bytes + length + 1, sizeof(ids));
    if (size > length + 1 + sizeof(ids))
        kind = msg.bytes[length + 1 + sizeof(ids)];
    if (kind == 1) {
        // a reaction: it belongs to the message it answers
        addReaction(key, ids[1], msg.from, std::string((const char *)msg.bytes, length).c_str());
        return;
    }
    appendMessage(key, Message{msg.from, (uint32_t)msg.time, 0, msg.status, own, std::string((const char *)msg.bytes, length),
                               ids[0], ids[1]});
    if (!own && msg.status == LogMessage::eUnread)
        chatFor(key).unread++;
}

void KeyboardView_320x240::notifyConnected(const char *info)
{
    connected = true;
    restarting = false;
    dirty = urgent = true;
}

void KeyboardView_320x240::notifyDisconnected(const char *info)
{
    connected = false;
    dirty = true;
}

// -------- rendering --------

void KeyboardView_320x240::createFrame(void)
{
    root = lv_obj_create(nullptr);
    lv_obj_remove_style_all(root);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_style_bg_color(root, lv_color_hex(c_ground), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

    const int32_t contentHeight = c_height - c_statusHeight - c_tabHeight;
    statusBar = box(root, 0, 0, c_width, c_statusHeight, c_surface);
    content = plain(root, 0, c_statusHeight, c_width, contentHeight);
    // the map keeps its tiles between two renders, so it lives beside the content and not in it
    mapLayer = box(root, 0, c_statusHeight, c_width, contentHeight, c_ground);
    lv_obj_set_hidden(mapLayer, true);
    mapHud = plain(root, 0, c_statusHeight, c_width, contentHeight);
    lv_obj_set_hidden(mapHud, true);
    tabBar = box(root, 0, c_height - c_tabHeight, c_width, c_tabHeight, c_surface);
    overlay = plain(root, 0, 0, c_width, c_height);

    keySink = plain(root, 0, 0, 1, 1);
    lv_obj_add_event_cb(keySink, onKeyEvent, LV_EVENT_ALL, this);

    lv_screen_load(root);
}

/**
 * The screen is small and static between events, so it is redrawn from the model
 * instead of keeping widgets and model in sync.
 */
void KeyboardView_320x240::render(void)
{
    // what was made once takes the colors of the look that is on now
    lv_obj_set_style_bg_color(root, lv_color_hex(c_ground), LV_PART_MAIN);
    lv_obj_set_style_bg_color(statusBar, lv_color_hex(c_surface), LV_PART_MAIN);
    lv_obj_set_style_bg_color(tabBar, lv_color_hex(c_surface), LV_PART_MAIN);
    lv_obj_set_style_bg_color(mapLayer, lv_color_hex(c_ground), LV_PART_MAIN);
    lv_obj_clean(statusBar);
    lv_obj_clean(content);
    lv_obj_clean(mapHud);
    lv_obj_clean(tabBar);
    lv_obj_clean(overlay);

    const bool takeover = splash || restarting || bluetoothMode || locked;
    renderStatusBar();
    // a conversation takes the tab bar's space as well, and so does a screen on top of Home
    const bool full = inConversation || inApp();
    lv_obj_set_hidden(tabBar, full);
    lv_obj_set_height(content, c_height - c_statusHeight - (full ? 0 : c_tabHeight));
    lv_obj_update_layout(content); // the screens below measure the content area
    showMap(tab == Tab::Map && !inConversation && !takeover);
    if (inConversation) {
        renderConversation();
    } else {
        switch (tab) {
        case Tab::Chats:
            renderChats();
            break;
        case Tab::Nodes:
            renderNodes();
            break;
        case Tab::Map:
            renderMap();
            break;
        case Tab::Settings:
            renderSettings();
            break;
        case Tab::Apps:
            if (inApp())
                renderApp();
            else
                renderAppList();
            break;
        default:
            if (inApp())
                renderApp();
            else
                renderHome();
            break;
        }
        box(tabBar, 0, 0, c_width, 1, c_border);
        const int32_t w = c_width / (int32_t)Tab::Count;
        for (uint8_t i = 0; i < (uint8_t)Tab::Count; i++) {
            const bool active = i == (uint8_t)tab;
            if (active && barFocus) {
                box(tabBar, i * w, 0, w, c_tabHeight, c_focusTint);
                box(tabBar, i * w, 0, w, 2, c_focus);
            } else if (active) {
                box(tabBar, i * w, 0, w, 2, c_blue);
            }
            label(tabBar, i * w, 5, w, font12, active ? c_text : c_muted, c_tabNames[i], LV_TEXT_ALIGN_CENTER);
        }
    }
    if (takeover)
        renderTakeover();
    else
        renderMenu();
    renderToast();

    lastRender = lv_tick_get();
    dirty = urgent = false;
}

void KeyboardView_320x240::renderStatusBar(void)
{
    box(statusBar, 0, c_statusHeight - 1, c_width, 1, c_border);

    // the marks on the right take what they need, the title gets the rest
    lv_obj_t *right = plain(statusBar, 0, 0, LV_SIZE_CONTENT, c_statusHeight - 1);
    flexRow(right, LV_FLEX_ALIGN_END, 8);
    char buf[16];
    lv_obj_t *unreadMark = nullptr, *alertMark = nullptr, *wifiMark = nullptr, *gpsMark = nullptr;
    const uint32_t unread = totalUnread();
    if (unread) {
        snprintf(buf, sizeof(buf), "%u new", (unsigned)unread);
        unreadMark = label(right, 0, 0, 0, font12, c_blue, buf);
    }
    if (!connected)
        label(right, 0, 0, 0, font12, c_focus, "No radio");
    else if (hasLora && lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
        label(right, 0, 0, 0, font12, c_focus, "No region");
    else if (state != MeshtasticView::eRunning && !demoData)
        label(right, 0, 0, 0, font12, c_muted, "Syncing");
    if (silent())
        alertMark = label(right, 0, 0, 0, font12, c_muted, prefs.keyFlash ? "Flash" : "Mute");
    if (prefs.wardrive)
        label(right, 0, 0, 0, font12, c_focus, "Log"); // the signal log is being written
    // symbols for the radios that are on: bright when they have what they are for, dim while they look for it
    const lv_font_t *symbols = &lv_font_montserrat_16;
    if (hasBluetooth && bluetooth.enabled)
        label(right, 0, 0, 0, symbols, c_blue, LV_SYMBOL_BLUETOOTH);
    if (prefs.wifi || network.wifi_enabled) {
        lv_obj_t *mark = label(right, 0, 0, 0, symbols, uploading ? c_focus : wifiUp || network.wifi_enabled ? c_blue : c_muted,
                               LV_SYMBOL_WIFI);
        if (!uploading)
            wifiMark = mark;
    }
    const NodeRecord *own = nodeStore.find(ownNode);
    const bool fix = own && own->position.hasCoordinates();
    if (fix || (hasPosition && position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED))
    {
        // a map pin, drawn: the fonts have none
        static const lv_point_precise_t pin[] = {{5, 14}, {1, 7}, {1, 4}, {3, 2}, {7, 2}, {9, 4}, {9, 7}, {5, 14}};
        const uint32_t color = fix ? c_status : c_muted;
        gpsMark = plain(right, 0, 0, 11, 16);
        lv_obj_t *outline = lv_line_create(gpsMark);
        lv_line_set_points(outline, pin, sizeof(pin) / sizeof(pin[0]));
        lv_obj_set_style_line_width(outline, 2, LV_PART_MAIN);
        lv_obj_set_style_line_color(outline, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_line_rounded(outline, true, LV_PART_MAIN);
        lv_obj_set_style_radius(box(gpsMark, 4, 4, 3, 3, color), 1, LV_PART_MAIN);
    }
    if (own && own->hasDeviceMetrics) {
        if (own->deviceMetrics.battery_level > 100)
            snprintf(buf, sizeof(buf), "PWR");
        else
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)own->deviceMetrics.battery_level);
        label(right, 0, 0, 0, font12, c_status, buf);
    }
    if (validTime(now())) {
        clockString(now(), buf, sizeof(buf), twelveHour(), false);
        label(right, 0, 0, 0, font12, c_status, buf);
    }
    lv_obj_update_layout(right);
    int32_t taken = lv_obj_get_width(right);

    int32_t fixed = 0; // what stands next to the name on the left
    lv_obj_t *left = plain(statusBar, 6, 0, c_width - 20 - taken, c_statusHeight - 1);
    flexRow(left, LV_FLEX_ALIGN_START, 6);
    // the tab bar says where one is, so the top of a tab says who one is: short name, then long name;
    // a page of the settings has its own title, and a direct chat shows the names of the other side
    std::string title = c_tabNames[(uint8_t)tab];
    std::string chip;
    if (inConversation) {
        title = convKey < c_max_channels ? "# " + chatName(convKey) : chatName(convKey);
        if (convKey >= c_max_channels)
            chip = shortName(convKey);
    } else if (inApp()) {
        title = appTitle();
    } else if (tab == Tab::Settings && page != Page::Root) {
        title = c_pageNames[(uint8_t)page];
    } else if (hasOwnUser && ownUser.long_name[0]) {
        title = ownUser.long_name;
        chip = ownUser.short_name;
    }
    lv_obj_t *tag = nullptr;
    if (!chip.empty()) {
        tag = box(left, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT, c_raised);
        lv_obj_set_style_radius(tag, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(tag, 4, LV_PART_MAIN);
        label(tag, 0, 0, 0, font12, c_text, chip.c_str());
        lv_obj_update_layout(tag);
        fixed += lv_obj_get_width(tag) + 6;
    }
    lv_obj_t *nodes = nullptr;
    if (!inConversation && tab == Tab::Nodes) {
        const uint32_t time = now();
        size_t total = 0, online = 0;
        for (const auto &[id, node] : nodeStore.records()) {
            if (id == ownNode)
                continue;
            total++;
            if (node.lastHeard && time - node.lastHeard <= c_onlineSecs)
                online++;
        }
        char count[24];
        snprintf(count, sizeof(count), "%u of %u", (unsigned)(nodeFilter.empty() ? online : visibleNodes().size()),
                 (unsigned)total);
        nodes = label(left, 0, 0, 0, font12, c_muted, count);
        lv_obj_update_layout(nodes);
        fixed += lv_obj_get_width(nodes) + 6;
    }
    lv_obj_t *titleLabel = label(left, 0, 0, 0, font14, c_text, title.c_str());
    lv_obj_update_layout(titleLabel);
    // a name that does not fit comes before the marks one can do without
    const int32_t wanted = fixed + lv_obj_get_width(titleLabel);
    for (lv_obj_t *mark : {gpsMark, wifiMark, alertMark, unreadMark}) {
        if (c_width - 20 - taken >= wanted)
            break;
        if (!mark)
            continue;
        lv_obj_delete(mark);
        lv_obj_update_layout(right);
        taken = lv_obj_get_width(right);
    }
    const int32_t room = c_width - 20 - taken;
    lv_obj_set_x(right, c_width - 6 - taken);
    lv_obj_set_width(left, room);
    lv_obj_set_style_max_width(titleLabel, std::max<int32_t>(room - fixed, 20), LV_PART_MAIN);
    lv_obj_set_height(titleLabel, lv_font_get_line_height(font14));
    lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_MODE_DOTS);
    // the long name first, the short one behind it, and then what was measured for them
    if (tag)
        lv_obj_move_to_index(tag, -1);
    if (nodes)
        lv_obj_move_to_index(nodes, -1);
}

void KeyboardView_320x240::renderHome(void)
{
    homeFocus = std::min<uint8_t>(homeFocus, c_homeSlots - 1);
    renderTiles(barFocus ? -1 : homeFocus, false);
}

void KeyboardView_320x240::renderChats(void)
{
    const std::vector<uint32_t> order = chatOrder();
    if (order.empty()) {
        emptyNote(content, "No channels yet");
        return;
    }
    auto it = std::find(order.begin(), order.end(), chatsFocus);
    const size_t focus = it == order.end() ? 0 : (size_t)(it - order.begin());
    chatsFocus = order[focus];
    const size_t rows = (lv_obj_get_height(content) - 4) / c_rowHeight;
    chatsTop = scrollTop(chatsTop, focus, order.size(), rows);

    const uint32_t time = now();
    const size_t pinCount = pinnedChats().size(); // they lead the list
    for (size_t i = chatsTop; i < order.size() && i < chatsTop + rows; i++) {
        const uint32_t key = order[i];
        const bool isChannel = key < c_max_channels;
        const auto chat = conversations.find(key);
        const std::string name = chatName(key);
        const std::string chip = isChannel ? "#" : shortName(key);
        std::string last = "No messages yet";
        char when[16] = "";
        uint32_t unread = 0;
        if (chat != conversations.end() && !chat->second.messages.empty()) {
            const Message &msg = chat->second.messages.back();
            last = msg.own ? "You: " : isChannel ? senderName(msg.from) + ": " : "";
            const std::string emergency = mecpSummary(msg.text.c_str());
            last += emergency.empty() ? oneLine(msg.text.c_str()) : emergency;
            chatTimeString(msg.time, time, when, sizeof(when), twelveHour());
            unread = chat->second.unread;
        }
        if (chatMuted(key))
            last = "Muted · " + last;
        uint32_t chipBg = c_raised, chipFg = c_text;
        if (!isChannel)
            std::tie(chipBg, chipFg) = colorOf(key);
        // the unread count takes the place of the mark; a pinned chat is known by where it stands as well
        listRow(content, 2 + (int32_t)(i - chatsTop) * c_rowHeight, i == focus,
                RowSpec{chip.c_str(), chipBg, chipFg, isChannel, name.c_str(), last.c_str(), when, i < pinCount ? "Pinned" : "",
                        unread});
    }
    listScrollbar(content, 2, chatsTop, order.size(), rows);
}

void KeyboardView_320x240::renderNodes(void)
{
    const std::vector<const NodeRecord *> list = visibleNodes();
    int32_t top = 2;
    if (!nodeFilter.empty()) {
        // the filter is typed straight into the list; the chip shows what is being matched
        lv_obj_t *chip = box(content, 6, 3, LV_SIZE_CONTENT, c_filterHeight - 4, c_raised);
        outline(chip, c_focus, 1, 4);
        lv_obj_set_style_pad_hor(chip, 6, LV_PART_MAIN);
        flexRow(chip, LV_FLEX_ALIGN_START, 4);
        label(chip, 0, 0, 0, font12, c_muted, "Filter");
        label(chip, 0, 0, 0, font12, c_text, nodeFilter.c_str());
        top += c_filterHeight;
    }
    if (list.empty()) {
        emptyNote(content, nodeFilter.empty() ? "No nodes heard yet" : "No node matches");
        return;
    }
    auto it = std::find_if(list.begin(), list.end(), [this](const NodeRecord *node) { return node->id == nodesFocus; });
    const size_t focus = it == list.end() ? 0 : (size_t)(it - list.begin());
    nodesFocus = list[focus]->id;
    const size_t rows = (lv_obj_get_height(content) - top - 2) / c_rowHeight;
    nodesTop = scrollTop(nodesTop, focus, list.size(), rows);

    const uint32_t time = now();
    for (size_t i = nodesTop; i < list.size() && i < nodesTop + rows; i++) {
        const NodeRecord &node = *list[i];
        const std::string chip = shortName(node.id);
        const std::string name = chatName(node.id);
        char sub[48], hops[16], heard[16], snr[16] = "";
        hopString(node, hops, sizeof(hops));
        snprintf(sub, sizeof(sub), "%s%s · %s",
                 favorites.count(node.id)     ? "Fav · "
                 : ignoredNodes.count(node.id) ? "Ignored · "
                                               : "",
                 roleText(node.user.role), hops);
        agoString(node.lastHeard, time, heard, sizeof(heard));
        if (node.hopsAway == 0 && node.lastHeard)
            snprintf(snr, sizeof(snr), "%.1f dB", node.snr);
        const auto [chipBg, chipFg] = colorOf(node.id);
        listRow(content, top + (int32_t)(i - nodesTop) * c_rowHeight, i == focus,
                RowSpec{chip.c_str(), chipBg, chipFg, false, name.c_str(), sub, heard, snr, 0, ownedNodes.count(node.id) ? c_owned : 0});
    }
    listScrollbar(content, top, nodesTop, list.size(), rows);
}

void KeyboardView_320x240::renderConversation(void)
{
    Chat &chat = chatFor(convKey);
    const bool isChannel = convKey < c_max_channels;
    const int32_t height = lv_obj_get_height(content);
    caret = std::min(caret, chat.draft.size());
    const int32_t count = (int32_t)chat.messages.size();
    if (convSelect >= count)
        convSelect = -1;
    const int32_t first = conversationTop(count);
    const int32_t end = std::min<int32_t>(count, first + (int32_t)c_shownMessages);

    // a quoted message: an accent bar and one line that ends in dots
    auto quoteLine = [](lv_obj_t *parent, const std::string &text, int32_t width) {
        lv_obj_t *quote = plain(parent, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        flexRow(quote, LV_FLEX_ALIGN_START, 5);
        lv_obj_set_style_radius(box(quote, 0, 0, 2, lv_font_get_line_height(font12), c_blue), 1, LV_PART_MAIN);
        lv_obj_t *line = label(quote, 0, 0, 0, font12, c_muted, text.c_str());
        lv_obj_set_style_max_width(line, width, LV_PART_MAIN);
        lv_obj_set_height(line, lv_font_get_line_height(font12));
        lv_label_set_long_mode(line, LV_LABEL_LONG_MODE_DOTS);
        return quote;
    };

    // composer: one line that grows to three
    lv_obj_t *composer = box(content, 6, 0, c_width - 12, LV_SIZE_CONTENT, c_surface);
    outline(composer, c_border, 2, 5);
    lv_obj_set_style_pad_hor(composer, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(composer, 3, LV_PART_MAIN);
    flexRow(composer, LV_FLEX_ALIGN_START, 6);

    lv_obj_t *input = lv_textarea_create(composer);
    lv_group_remove_obj(input); // keys arrive through the key sink
    lv_obj_remove_style_all(input);
    lv_obj_set_clickable(input, false);
    lv_obj_set_scrollbar_mode(input, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_grow(input, 1);
    lv_obj_set_height(input, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(input, 3 * lv_font_get_line_height(font14), LV_PART_MAIN);
    lv_obj_set_style_text_font(input, font14, LV_PART_MAIN);
    lv_obj_set_style_text_color(input, lv_color_hex(c_text), LV_PART_MAIN);
    lv_obj_set_style_text_color(input, lv_color_hex(c_focusHint), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_side(input, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_border_color(input, lv_color_hex(c_focus), LV_PART_CURSOR);
    lv_obj_set_style_border_opa(input, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_width(input, 2, LV_PART_CURSOR);
    lv_textarea_set_placeholder_text(input, "Message");
    lv_textarea_set_text(input, chat.draft.c_str());
    lv_textarea_set_cursor_pos(input, (int32_t)caret);

    const size_t left = c_maxDraft - std::min(c_maxDraft, chat.draft.size());
    char hint[8];
    if (convSelect >= 0)
        snprintf(hint, sizeof(hint), "Reply"); // what OK does to the picked message
    else if (left < c_draftWarn)
        snprintf(hint, sizeof(hint), "%u", (unsigned)left);
    else
        snprintf(hint, sizeof(hint), "Enter");
    lv_obj_t *hintChip = plain(composer, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    outline(hintChip, c_focusHint, 1, 3);
    lv_obj_set_style_pad_hor(hintChip, 5, LV_PART_MAIN);
    label(hintChip, 0, 0, 0, font12, left < c_draftWarn ? c_focus : c_focusMuted, hint);

    // the composer has the focus unless the user has scrolled up into the messages
    auto focusComposer = [composer, input](bool focused) {
        if (!focused)
            return;
        lv_obj_set_style_bg_color(composer, lv_color_hex(c_focusTint), LV_PART_MAIN);
        lv_obj_set_style_border_color(composer, lv_color_hex(c_focus), LV_PART_MAIN);
        lv_obj_set_style_border_opa(input, LV_OPA_COVER, LV_PART_CURSOR);
    };

    lv_obj_update_layout(composer);
    const int32_t composerTop = height - 5 - lv_obj_get_height(composer);
    lv_obj_set_y(composer, composerTop);

    // what the draft answers sits on the composer
    int32_t listBottom = composerTop - 4;
    if (chat.replyTo) {
        listBottom -= lv_font_get_line_height(font12) + 3;
        lv_obj_set_pos(quoteLine(content, "Reply to " + quoteOf(convKey, chat.replyTo, true), c_width - 36), 12, listBottom + 3);
    }

    // messages, newest at the bottom
    lv_obj_t *list = plain(content, 0, 0, c_width, listBottom);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(list, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_top(list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 4, LV_PART_MAIN);

    if (chat.messages.empty()) {
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        label(list, 0, 0, 0, font14, c_muted, "No messages yet");
        focusComposer(true);
        return;
    }

    lv_obj_t *last = nullptr;
    lv_obj_t *picked = nullptr;
    for (int32_t i = first; i < end; i++) {
        const Message &msg = chat.messages[i];
        char when[16] = "";
        if (validTime(msg.time))
            clockString(msg.time, when, sizeof(when), twelveHour());

        lv_obj_t *row = plain(list, 0, 0, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_t *bubble = box(row, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT, msg.own ? c_ownBubble : c_raised);
        lv_obj_set_style_max_width(bubble, c_bubbleWidth, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(bubble, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(bubble, 2, LV_PART_MAIN);
        lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);

        if (msg.own) {
            outline(bubble, c_ownBorder, 1, 6);
            lv_obj_set_align(bubble, LV_ALIGN_TOP_RIGHT);
            lv_obj_set_flex_align(bubble, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
        } else {
            lv_obj_set_style_radius(bubble, 6, LV_PART_MAIN);
            lv_obj_t *header = plain(bubble, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            flexRow(header, LV_FLEX_ALIGN_START, 8);
            if (isChannel) {
                // the node color is made for chip backgrounds; lift dark ones so they read as text
                lv_color_t color = lv_color_hex(std::get<0>(colorOf(msg.from)));
                if (lv_color_brightness(color) < 150)
                    color = lv_color_mix(lv_color_white(), color, 150);
                lv_obj_set_style_text_color(label(header, 0, 0, 0, font12, c_text, senderName(msg.from).c_str()), color,
                                            LV_PART_MAIN);
            }
            label(header, 0, 0, 0, font12, c_muted, when);
        }

        if (msg.replyId)
            quoteLine(bubble, quoteOf(convKey, msg.replyId, true), c_bubbleWidth - 22);
        if (strncmp(msg.text.c_str(), "MECP/", 5) == 0 && !mecpSummary(msg.text.c_str()).empty()) {
            renderMecpCard(bubble, msg.text.c_str(), c_bubbleWidth - 14); // an emergency message in plain words
        } else {
            lv_obj_t *text = label(bubble, 0, 0, 0, font14, c_text, msg.text.c_str());
            lv_obj_set_style_max_width(text, c_bubbleWidth - 14, LV_PART_MAIN);
        }

        if (msg.own) {
            const std::string status = messageState(convKey, msg, true);
            const bool bad = msg.status == LogMessage::eFailed || msg.status == LogMessage::eNoResponse;
            char meta[64];
            snprintf(meta, sizeof(meta), "%s%s%s", when, when[0] ? "  " : "", status.c_str());
            lv_obj_t *state = label(bubble, 0, 0, 0, font12, bad ? c_focus : c_ownMeta, meta);
            lv_obj_set_style_max_width(state, c_bubbleWidth - 14, LV_PART_MAIN);
        }
        if (msg.id && !chat.reactions.empty()) {
            const std::string reactions = reactionsOf(chat, msg.id);
            if (!reactions.empty()) {
                lv_obj_t *line = label(bubble, 0, 0, 0, font14, c_focusMuted, reactions.c_str());
                lv_obj_set_style_max_width(line, c_bubbleWidth - 14, LV_PART_MAIN);
            }
        }
        if (!msg.own && mentionsMe(msg))
            outline(bubble, c_blue, 1, 6); // it names this node
        if (i == convSelect) {
            outline(bubble, c_focus, 2, 6);
            lv_obj_set_style_bg_color(bubble, lv_color_hex(c_focusTint), LV_PART_MAIN);
            picked = row;
        }
        last = row;
    }

    // short conversations sit on the composer, long ones scroll
    lv_obj_update_layout(list);
    const int32_t used = lv_obj_get_y(last) + lv_obj_get_height(last);
    const int32_t room = lv_obj_get_content_height(list);
    if (used <= room) {
        lv_obj_set_style_pad_top(list, 4 + room - used, LV_PART_MAIN);
    } else {
        // the newest message sits on the composer unless the picked one is further up
        lv_obj_scroll_to_y(list, picked ? std::min(used - room, lv_obj_get_y(picked)) : used - room, LV_ANIM_OFF);
    }
    focusComposer(convSelect < 0);
}

void KeyboardView_320x240::renderToast(void)
{
    if (toastText.empty())
        return;
    lv_obj_t *panel = box(overlay, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT, c_raised);
    outline(panel, c_border, 1, 5);
    lv_obj_set_style_pad_hor(panel, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(panel, 5, LV_PART_MAIN);
    label(panel, 0, 0, 0, font12, c_text, toastText.c_str());
    // below an open popup, where nothing but the dimmed tab bar is
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, menu != Menu::None ? -2 : -(c_tabHeight + 8));
}

#if defined(ARCH_PORTDUINO)
/**
 * Sample mesh for working on the layout in the simulator (KBVIEW_DEMO=1); never built for a device.
 */
void KeyboardView_320x240::loadDemoData(void)
{
    struct DemoNode {
        uint32_t id;
        const char *shortName;
        const char *longName;
        meshtastic_Config_DeviceConfig_Role role;
        int8_t hops;
        float snr;
        uint32_t age; // seconds since last heard
    };
    const DemoNode demoNodes[] = {
        {0x4b7fd1ae, "RDG1", "Ridge Repeater", meshtastic_Config_DeviceConfig_Role_ROUTER, 1, 7.2f, 5},
        {0x4bf2a7b8, "MPL1", "Maple Hill", meshtastic_Config_DeviceConfig_Role_CLIENT, 0, 6.5f, 2 * 60},
        {0x4b9cc0ff, "HIKE", "Hiker Kit", meshtastic_Config_DeviceConfig_Role_CLIENT, 2, -3.0f, 4 * 60},
        {0x4be6c36a, "BARN", "Barn Solar", meshtastic_Config_DeviceConfig_Role_SENSOR, 1, 2.5f, 11 * 60},
        {0x4bb9a0e8, "LAKE", "Lake Camp", meshtastic_Config_DeviceConfig_Role_CLIENT, 3, -9.5f, 38 * 60},
        {0x4b8fcb6b, "SGB1", "Sugarbush Relay", meshtastic_Config_DeviceConfig_Role_ROUTER, 2, 0.0f, 52 * 60},
        {0x4bf0a070, "MRB1", "Mad River Base", meshtastic_Config_DeviceConfig_Role_CLIENT, 0, 4.0f, 95 * 60},
        {0x4b6fc7d9, "TRK1", "Old Truck", meshtastic_Config_DeviceConfig_Role_TRACKER, 4, 0.0f, 3 * 3600},
        {0x4bd9d27a, "CMH1", "Camels Hump Summit", meshtastic_Config_DeviceConfig_Role_ROUTER, 3, 0.0f, 26 * 3600},
    };
    const uint32_t time = now();
    uint32_t index = 0;
    for (const DemoNode &demo : demoNodes) {
        meshtastic_User user{};
        strncpy(user.short_name, demo.shortName, sizeof(user.short_name) - 1);
        strncpy(user.long_name, demo.longName, sizeof(user.long_name) - 1);
        user.role = demo.role;
        nodeStore.upsertUser(demo.id, 0, time - demo.age, user, false);
        markHeard(demo.id, 0);
        nodeStore.updateLastHeard(demo.id, time - demo.age);
        nodeStore.updateHops(demo.id, demo.hops);
        if (demo.hops == 0) {
            nodeStore.updateSignal(demo.id, -90 - (int32_t)(demo.id % 17), demo.snr);
            heard.push_back(Heard{time - demo.age, demo.id, demo.snr, (int16_t)(-90 - (int32_t)(demo.id % 17))});
        }
        if (index++ % 3 != 2) {
            // two of three share a position, scattered around the valley; every other one only roughly
            NodePosition position;
            position.known = true;
            position.latitude = 442600000 + ((int32_t)((demo.id >> 4) % 2000) - 1000) * 600;
            position.longitude = -725600000 + ((int32_t)((demo.id >> 12) % 2000) - 1000) * 900;
            position.altitude = 180 + (int32_t)(demo.id % 700);
            position.satellites = 0;
            position.precision = index % 4 == 0 ? 15 : 32;
            nodeStore.updatePosition(demo.id, position);
            if (index % 2)
                positionTimes[demo.id] = time - demo.age;
        }
    }

    const char *const channelNames[] = {"LongFast", "Family", "Weather"};
    for (uint8_t i = 0; i < 3; i++) {
        meshtastic_Channel ch{};
        ch.index = i;
        ch.has_settings = true;
        ch.role = i == 0 ? meshtastic_Channel_Role_PRIMARY : meshtastic_Channel_Role_SECONDARY;
        strncpy(ch.settings.name, channelNames[i], sizeof(ch.settings.name) - 1);
        ch.settings.psk.size = 1;
        ch.settings.psk.bytes[0] = 1;
        storeChannel(ch);
    }
    lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    lora.tx_enabled = true;
    hasLora = true;
    lora.use_preset = true;
    lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    lora.hop_limit = 3;
    lastSnr = 6.5f;
    hasSnr = true;

    // a radio that has been up for a day on a moderately busy channel
    for (size_t i = 0; i < c_radioHistory; i++)
        radioSample(6.0f + 4.0f * sinf(i * 0.45f) + (i % 7 == 3 ? 9.0f : 0.0f), 0.4f + 0.3f * cosf(i * 0.3f));
    historyTime = lv_tick_get();
    channelUse = 7.4f;
    txUse = 0.6f;
    hasAirtime = true;
    localStats.uptime_seconds = 26 * 3600 + 14 * 60;
    localStats.num_packets_tx = 412;
    localStats.num_packets_rx = 3871;
    localStats.num_packets_rx_bad = 57;
    localStats.num_rx_dupe = 1204;
    localStats.num_tx_relay = 233;
    localStats.num_tx_relay_canceled = 96;
    localStats.num_tx_dropped = 2;
    localStats.num_online_nodes = 7;
    localStats.num_total_nodes = (uint16_t)(sizeof(demoNodes) / sizeof(demoNodes[0]));
    localStats.heap_total_bytes = 2 * 1024 * 1024;
    localStats.heap_free_bytes = 1340 * 1024;
    localStats.noise_floor = -112;
    hasLocalStats = true;
    std::sort(heard.begin(), heard.end(), [](const Heard &a, const Heard &b) { return a.time < b.time; });

    uint32_t demoId = 0x1000;
    auto add = [this, time, &demoId](uint32_t key, uint32_t from, uint32_t age, const char *text, bool own = false,
                                     LogMessage::MsgStatus status = LogMessage::eNone, uint32_t requestId = 0,
                                     uint32_t replyId = 0) {
        appendMessage(key, Message{from, time - age, requestId, status, own, text, ++demoId, replyId});
        return demoId;
    };
    add(2, 0x4be6c36a, 2 * 86400, "Frost advisory tonight, low of 28F");
    add(0x4b9cc0ff, 0x4b9cc0ff, 86400, "Boots are on the porch");
    add(0, 0x4b7fd1ae, 50 * 60, "Ridge repeater back on solar");
    add(0, 0x4bf2a7b8, 31 * 60, "MECP/1/M01 P01 2pax 44.27310,-72.57140 @1432 ~W1ABC on the lower trail");
    add(0, 0x4b9cc0ff, 29 * 60, "MECP/3/R01 R03 20min");
    const uint32_t asked = add(1, 0x4bf2a7b8, 11 * 60, "Anyone copy from the ridge?");
    add(1, 0, 10 * 60, "Loud and clear, SNR 6.5 here.", true, LogMessage::eAcked, 0, asked);
    add(0x4bf2a7b8, 0x4bf2a7b8, 9 * 60, "Left the gate open for you");
    add(0x4bf2a7b8, 0x4bf2a7b8, 8 * 60, "Bring the chainsaw?");
    const uint32_t heading = add(1, 0x4b9cc0ff, 4 * 60, "Heading down, back by 5");
    add(1, 0x4bf2a7b8, 3 * 60, "Take the lower trail, the upper one is iced over", false, LogMessage::eNone, 0, heading);
    add(1, 0, 60, "Copy. Kettle is on.", true, LogMessage::eDefault, 1);
    markHeard(0x4bb9a0e8, 1); // Lake Camp sends its position on Family and has not written yet
    conversations[1].unread = 1;
    conversations[1].draft = "See you at the tr";
    conversations[0x4bf2a7b8].unread = 2;

    demoData = true;
}

/**
 * Simulator only (KBVIEW_STRESS=<nodes>): one step of made-up mesh traffic, to see how the view holds up under load.
 * More nodes than the view keeps also exercises the purge of the oldest ones.
 */
void KeyboardView_320x240::stressStep(void)
{
    stressCount++;
    const uint32_t nodeNum = 0x51000000 + (uint32_t)rand() % stressNodes;
    const uint32_t time = now();
    if (!nodeStore.find(nodeNum) || rand() % 10 == 0) {
        meshtastic_User user{};
        snprintf(user.short_name, sizeof(user.short_name), "%04x", (unsigned)(nodeNum & 0xffff));
        snprintf(user.long_name, sizeof(user.long_name), "Stress node %u", (unsigned)(nodeNum & 0xffffff));
        user.role = (meshtastic_Config_DeviceConfig_Role)(rand() % 3);
        addOrUpdateNode(nodeNum, 0, time, user);
    }
    if (!nodeStore.find(nodeNum))
        return; // the view is full and had nobody to let go
    updateLastHeard(nodeNum);
    if (rand() % 2)
        updateSignalStrength(nodeNum, -60 - rand() % 60, (rand() % 240 - 120) / 10.0f);
    else
        updateHopsAway(nodeNum, (uint8_t)(1 + rand() % 5));
    if (rand() % 3 == 0)
        updatePosition(nodeNum, 442600000 + (rand() % 4000 - 2000) * 500, -725600000 + (rand() % 4000 - 2000) * 700,
                       200 + rand() % 800, 5 + rand() % 10, 32);
    if (stressCount % 5 == 0) {
        char text[120];
        if (stressCount % 35 == 0)
            snprintf(text, sizeof(text), "@%s can you hear stress message %u?", shortName(ownNode).c_str(), (unsigned)stressCount);
        else
            snprintf(text, sizeof(text), "Stress message %u from %04x, long enough to wrap over a second line of the bubble",
                     (unsigned)stressCount, (unsigned)(nodeNum & 0xffff));
        uint32_t msgTime = time;
        const bool direct = rand() % 3 == 0;
        const uint8_t ch = direct ? 0 : (uint8_t)(rand() % 2);
        // every other message answers the newest one of its chat
        const auto chat = conversations.find(direct ? nodeNum : ch);
        const uint32_t replyId =
            stressCount % 10 == 0 && chat != conversations.end() && !chat->second.messages.empty()
                ? chat->second.messages.back().id
                : 0;
        newTextMessage(nodeNum, direct ? ownNode : UINT32_MAX, ch, text, msgTime, 0x52000000 + stressCount, replyId);
    }
    if (stressCount % 20 == 0) {
        meshtastic_DeviceMetrics metrics{};
        metrics.has_battery_level = true;
        metrics.battery_level = (uint32_t)(rand() % 102);
        metrics.has_voltage = true;
        metrics.voltage = 3.3f + (rand() % 90) / 100.0f;
        metrics.has_channel_utilization = true;
        metrics.channel_utilization = (rand() % 300) / 10.0f;
        metrics.has_air_util_tx = true;
        metrics.air_util_tx = (rand() % 50) / 10.0f;
        updateMetrics(nodeNum, metrics);
    }
    if (stressCount % 7 == 0) {
        // a tapback on the newest message of a channel
        static const char *const emoji[] = {"\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x98\x82", "\xF0\x9F\x8C\xB2"};
        const uint8_t ch = (uint8_t)(rand() % 2);
        const auto chat = conversations.find(ch);
        if (chat != conversations.end() && !chat->second.messages.empty()) {
            uint32_t msgTime = time;
            newReaction(nodeNum, UINT32_MAX, ch, emoji[rand() % 4], msgTime, 0x53000000 + stressCount,
                        chat->second.messages.back().id);
        }
    }
    if (stressCount % 4 == 0) {
        // this device wanders off, which leaves a trail
        const double angle = stressCount / 40.0;
        updatePosition(ownNode, 442600000 + (int32_t)(stressCount * 600 + 20000 * sin(angle)),
                       -725600000 + (int32_t)(stressCount * 900 + 20000 * cos(angle)), 300, 9, 32);
    }
    if (stressCount % 60 == 0) {
        meshtastic_Waypoint waypoint = meshtastic_Waypoint_init_default;
        waypoint.id = 0x54000000 + stressCount / 60 % 40; // more than are kept
        waypoint.has_latitude_i = waypoint.has_longitude_i = true;
        waypoint.latitude_i = 442600000 + (rand() % 400 - 200) * 500;
        waypoint.longitude_i = -725600000 + (rand() % 400 - 200) * 700;
        waypoint.expire = stressCount % 180 == 0 ? 1 : 0; // every third one takes an old one back
        snprintf(waypoint.name, sizeof(waypoint.name), "Camp %u", (unsigned)(stressCount / 60));
        newWaypoint(nodeNum, 0, waypoint);
    }
    if (stressCount % 50 == 0)
        removeNode(0x51000000 + (uint32_t)rand() % stressNodes);
}
#endif

#endif
