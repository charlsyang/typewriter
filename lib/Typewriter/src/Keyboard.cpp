#include "Keyboard.h"

#include <NimBLEDevice.h>
#include <esp_random.h>

namespace tw {

namespace {

const NimBLEUUID HID_SERVICE((uint16_t)0x1812);
const NimBLEUUID PROTOCOL_MODE((uint16_t)0x2A4E);
const NimBLEUUID REPORT((uint16_t)0x2A4D);
const NimBLEUUID BOOT_KEYBOARD_INPUT((uint16_t)0x2A22);
const NimBLEUUID REPORT_REFERENCE((uint16_t)0x2908);
const uint16_t APPEARANCE_KEYBOARD = 0x03C1;

// HID modifier bits.
const uint8_t MOD_CTRL = 0x11, MOD_SHIFT = 0x22, MOD_ALT = 0x44, MOD_GUI = 0x88;

// HID usages.
const uint8_t KEY_A = 0x04, KEY_Z = 0x1D, KEY_ENTER = 0x28, KEY_ESC = 0x29, KEY_BACKSPACE = 0x2A,
              KEY_TAB = 0x2B, KEY_SPACE = 0x2C, KEY_CAPS = 0x39;

// US layout, usages 0x1E-0x38: unshifted and shifted.
const char *const US_PLAIN = "1234567890\0\0\0\0\0-=[]\\#;'`,./";
const char *const US_SHIFT = "!@#$%^&*()\0\0\0\0\0_+{}|~:\"~<>?";

// Mac Option-key punctuation: {usage, shifted, UTF-8}.
struct OptionKey {
    uint8_t usage;
    bool shift;
    const char *text;
};
const OptionKey OPTION_KEYS[] = {
    {0x2D, false, "–"}, {0x2D, true, "—"}, // - : en dash, em dash
    {0x2F, false, "“"}, {0x2F, true, "”"}, // [ : double quotes
    {0x30, false, "‘"}, {0x30, true, "’"}, // ] : single quotes
    {0x33, false, "…"},                    // ; : ellipsis
    {0x25, false, "•"},                    // 8 : bullet
    {0x16, false, "ß"},                    // s
    {0x2E, false, "≠"},                    // =
};

const uint32_t REPEAT_DELAY_MS = 450;
const uint32_t REPEAT_INTERVAL_MS = 45;

struct Shared {
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    Keyboard::State state = Keyboard::State::Searching;
    uint32_t passkey = 0;
    char name[32] = "";
    uint32_t revision = 0;
    // Key held down, for auto-repeat.
    uint8_t heldUsage = 0;
    uint8_t heldMods = 0;
    uint32_t heldSince = 0;
    uint32_t lastRepeat = 0;
    bool capsLock = false;
    uint8_t lastKeys[6] = {0};
};

Shared shared;
QueueHandle_t events;
TaskHandle_t task;
NimBLEClient *client;
NimBLEAddress target;
volatile bool targetFound = false;

void setState(Keyboard::State s) {
    portENTER_CRITICAL(&shared.lock);
    shared.state = s;
    shared.revision++;
    portEXIT_CRITICAL(&shared.lock);
}

bool repeats(uint8_t usage) {
    return (usage >= KEY_A && usage <= 0x38 && usage != KEY_ENTER && usage != KEY_ESC && usage != KEY_TAB) ||
           usage == KEY_BACKSPACE || (usage >= 0x4F && usage <= 0x52); // arrows
}

// Turns one key press into an event; returns false for keys that do nothing.
bool translate(uint8_t usage, uint8_t mods, bool caps, KeyEvent &e) {
    memset(&e, 0, sizeof(e));
    bool shift = mods & MOD_SHIFT;
    bool alt = mods & MOD_ALT;
    bool ctrl = mods & MOD_CTRL;
    bool gui = mods & MOD_GUI;

    if (usage == KEY_BACKSPACE) {
        e.type = alt ? KeyEvent::BackspaceWord : KeyEvent::Backspace;
        return true;
    }
    if (usage == KEY_ENTER || usage == 0x58) { // Return, keypad Enter
        e.type = KeyEvent::Enter;
        return true;
    }
    if (usage == KEY_TAB) {
        e.type = KeyEvent::Tab;
        return true;
    }
    if (usage >= 0x4F && usage <= 0x52 && !(ctrl || gui)) { // Right, Left, Down, Up
        static const KeyEvent::Type ARROWS[] = {KeyEvent::Right, KeyEvent::Left, KeyEvent::Down, KeyEvent::Up};
        e.type = ARROWS[usage - 0x4F];
        return true;
    }
    if (usage == KEY_ESC) {
        e.type = KeyEvent::Escape;
        return true;
    }
    if ((ctrl || gui) && usage >= KEY_A && usage <= KEY_Z) {
        e.type = KeyEvent::Shortcut;
        e.key = 'a' + (usage - KEY_A);
        e.ctrl = ctrl;
        e.cmd = gui;
        e.shift = shift;
        return true;
    }
    if (ctrl || gui)
        return false;

    if (alt) {
        for (const OptionKey &k : OPTION_KEYS) {
            if (k.usage == usage && k.shift == shift) {
                e.type = KeyEvent::Text;
                strlcpy(e.text, k.text, sizeof(e.text));
                return true;
            }
        }
        return false;
    }

    char c = 0;
    if (usage >= KEY_A && usage <= KEY_Z) {
        c = 'a' + (usage - KEY_A);
        if (shift != caps)
            c -= 32;
    } else if (usage == KEY_SPACE) {
        c = ' ';
    } else if (usage >= 0x1E && usage <= 0x38) {
        c = (shift ? US_SHIFT : US_PLAIN)[usage - 0x1E];
    }
    if (!c)
        return false;
    e.type = KeyEvent::Text;
    e.text[0] = c;
    return true;
}

// A keyboard report: modifier bits and up to six pressed key usages.
void onKeyboardReport(uint8_t mods, const uint8_t *keys) {
    uint32_t now = millis();
    for (int i = 0; i < 6; i++) {
        uint8_t k = keys[i];
        if (k < KEY_A)
            continue; // no key, or a rollover error
        bool wasDown = memchr(shared.lastKeys, k, 6) != nullptr;
        if (wasDown)
            continue;
        if (k == KEY_CAPS) {
            shared.capsLock = !shared.capsLock;
            continue;
        }
        KeyEvent e;
        if (translate(k, mods, shared.capsLock, e))
            xQueueSend(events, &e, 0);
        portENTER_CRITICAL(&shared.lock);
        if (repeats(k) && !(mods & (MOD_CTRL | MOD_GUI))) {
            shared.heldUsage = k;
            shared.heldMods = mods;
            shared.heldSince = now;
            shared.lastRepeat = now;
        } else {
            shared.heldUsage = 0;
        }
        portEXIT_CRITICAL(&shared.lock);
    }
    portENTER_CRITICAL(&shared.lock);
    if (shared.heldUsage && !memchr(keys, shared.heldUsage, 6))
        shared.heldUsage = 0;
    else if (shared.heldUsage)
        shared.heldMods = mods;
    portEXIT_CRITICAL(&shared.lock);
    memcpy(shared.lastKeys, keys, 6);
}

void onReport(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
    // Boot format: modifiers, reserved, six keys. The MX Keys' report-protocol
    // keyboard report leaves out the reserved byte.
    if (len == 8)
        onKeyboardReport(data[0], data + 2);
    else if (len == 7)
        onKeyboardReport(data[0], data + 1);
}

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *dev) override {
        if (targetFound)
            return;
        bool keyboard = dev->isAdvertisingService(HID_SERVICE) || dev->getAppearance() == APPEARANCE_KEYBOARD;
        bool directedAtUs = dev->haveTargetAddress() && dev->getTargetAddress() == NimBLEDevice::getAddress();
        bool bonded = NimBLEDevice::isBonded(dev->getAddress());
        if (!keyboard && !directedAtUs && !bonded)
            return;
        Serial.printf("kbd: found %s \"%s\" (hid %d, directed %d, bonded %d)\n",
                      dev->getAddress().toString().c_str(), dev->getName().c_str(), keyboard, directedAtUs, bonded);
        target = dev->getAddress();
        portENTER_CRITICAL(&shared.lock);
        strlcpy(shared.name, dev->getName().c_str(), sizeof(shared.name));
        portEXIT_CRITICAL(&shared.lock);
        targetFound = true;
        NimBLEDevice::getScan()->stop();
        xTaskNotifyGive(task);
    }
};

class ClientCallbacks : public NimBLEClientCallbacks {
    uint32_t onPassKeyDisplay(NimBLEConnInfo &info) override {
        uint32_t key = 100000 + esp_random() % 900000;
        portENTER_CRITICAL(&shared.lock);
        shared.passkey = key;
        shared.state = Keyboard::State::Pairing;
        shared.revision++;
        portEXIT_CRITICAL(&shared.lock);
        Serial.printf("kbd: type %06lu on the keyboard, then Enter\n", (unsigned long)key);
        return key;
    }

    void onConfirmPasskey(NimBLEConnInfo &info, uint32_t pin) override {
        NimBLEDevice::injectConfirmPasskey(info, true);
    }

    void onAuthenticationComplete(NimBLEConnInfo &info) override {
        Serial.printf("kbd: security %s (bonded %d)\n", info.isEncrypted() ? "on" : "FAILED", info.isBonded());
    }

    void onDisconnect(NimBLEClient *c, int reason) override {
        Serial.printf("kbd: disconnected (reason %d)\n", reason);
        memset(shared.lastKeys, 0, sizeof(shared.lastKeys));
        shared.heldUsage = 0;
        setState(Keyboard::State::Searching);
        xTaskNotifyGive(task);
    }
};

ScanCallbacks scanCallbacks;
ClientCallbacks clientCallbacks;

// Reports on channels other than the one we type from (media keys, Mac-specific
// keys): read and ignored. The MX Keys Mini sends F10-F12 here; in boot
// protocol it has nowhere to send them and stops sending anything at all.
void onOtherReport(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify) {
}

// Uses report protocol, as macOS and Windows do: the keyboard's own input
// report (report ID 1, usually the same 8-byte layout as boot) plus every other
// input report, except Logitech's HID++ channel. Falls back to boot protocol if
// no keyboard report is found.
bool subscribe() {
    NimBLERemoteService *hid = client->getService(HID_SERVICE);
    if (!hid) {
        Serial.println("kbd: no HID service");
        return false;
    }
    NimBLERemoteCharacteristic *mode = hid->getCharacteristic(PROTOCOL_MODE);
    if (mode) {
        uint8_t reportMode = 1;
        mode->writeValue(&reportMode, 1, false);
    }
    bool haveKeyboard = false;
    for (NimBLERemoteCharacteristic *chr : hid->getCharacteristics(true)) {
        if (chr->getUUID() != REPORT || !chr->canNotify())
            continue;
        // Report Reference: {report ID, type (1 input, 2 output, 3 feature)}.
        uint8_t id = 0, type = 0;
        NimBLERemoteDescriptor *ref = chr->getDescriptor(REPORT_REFERENCE);
        if (ref) {
            NimBLEAttValue v = ref->readValue();
            if (v.size() >= 2) {
                id = v[0];
                type = v[1];
            }
        }
        // Report IDs 0x10-0x12 are Logitech HID++ (their Options app's channel).
        bool hidpp = id >= 0x10 && id <= 0x12;
        bool keys = id == 1;
        bool ok = !hidpp && chr->subscribe(true, keys ? onReport : onOtherReport);
        Serial.printf("kbd: report handle %u, id 0x%02x, type %u: %s\n", chr->getHandle(), id, type,
                      hidpp ? "skipped (HID++)" : !ok ? "subscribe failed" : keys ? "keys" : "ignored");
        haveKeyboard |= keys && ok;
    }
    if (haveKeyboard) {
        Serial.println("kbd: using report protocol");
        return true;
    }
    NimBLERemoteCharacteristic *boot = hid->getCharacteristic(BOOT_KEYBOARD_INPUT);
    if (boot && boot->canNotify() && mode) {
        uint8_t bootMode = 0;
        mode->writeValue(&bootMode, 1, false);
        if (boot->subscribe(true, onReport)) {
            Serial.println("kbd: using boot protocol");
            return true;
        }
    }
    return false;
}

void connectionTask(void *) {
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCallbacks, false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCallbacks, false);
    client->setConnectionParams(6, 12, 0, 200); // 7.5-15 ms interval for low latency

    while (true) {
        if (client->isConnected()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        setState(Keyboard::State::Searching);
        targetFound = false;
        scan->start(0, false, true);
        while (!targetFound)
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        setState(Keyboard::State::Connecting);
        if (!client->connect(target)) {
            Serial.println("kbd: connect failed");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        // Encrypts with the stored bond, or pairs with a passkey.
        if (!client->secureConnection()) {
            Serial.println("kbd: pairing failed");
            client->disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!subscribe()) {
            client->disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        setState(Keyboard::State::Connected);
        Serial.printf("kbd: connected to \"%s\"\n", shared.name);
    }
}

} // namespace

void Keyboard::begin(const char *deviceName) {
    events = xQueueCreate(32, sizeof(KeyEvent));
    NimBLEDevice::init(deviceName);
    NimBLEDevice::setSecurityAuth(true, true, true); // bond, MITM protection, secure connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    xTaskCreatePinnedToCore(connectionTask, "keyboard", 6144, nullptr, 2, &task, 0);
}

bool Keyboard::poll(KeyEvent &event) {
    if (xQueueReceive(events, &event, 0))
        return true;
    uint32_t now = millis();
    portENTER_CRITICAL(&shared.lock);
    uint8_t usage = shared.heldUsage;
    uint8_t mods = shared.heldMods;
    bool due = usage && now - shared.heldSince >= REPEAT_DELAY_MS && now - shared.lastRepeat >= REPEAT_INTERVAL_MS;
    if (due)
        shared.lastRepeat = now;
    bool caps = shared.capsLock;
    portEXIT_CRITICAL(&shared.lock);
    return due && translate(usage, mods, caps, event);
}

Keyboard::State Keyboard::state() const {
    return shared.state;
}

uint32_t Keyboard::passkey() const {
    return shared.passkey;
}

String Keyboard::name() const {
    char copy[sizeof(shared.name)];
    portENTER_CRITICAL(&shared.lock);
    memcpy(copy, shared.name, sizeof(copy));
    portEXIT_CRITICAL(&shared.lock);
    return String(copy);
}

uint32_t Keyboard::revision() const {
    return shared.revision;
}

void Keyboard::forget() {
    NimBLEDevice::deleteAllBonds();
    if (client && client->isConnected())
        client->disconnect();
}

} // namespace tw
