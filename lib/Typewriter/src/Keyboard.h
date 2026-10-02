// A Bluetooth LE keyboard, connected with the ESP32 as the host (HID over GATT).
//
// A background task scans for keyboards, connects, pairs with a passkey shown on
// screen, and reconnects to the bonded keyboard on its own. Key reports become
// KeyEvents with US-layout text, including the Mac Option-key punctuation
// (– — “ ” ‘ ’ …). Bonds persist in flash, so pairing happens once.
#pragma once

#include <Arduino.h>

namespace tw {

struct KeyEvent {
    enum Type : uint8_t {
        Text,          // text holds UTF-8
        Backspace,     // delete one character
        BackspaceWord, // Option+Backspace: delete the previous word
        Enter,
        Tab,
        Escape,
        Up,
        Down,
        Left,
        Right,
        Shortcut, // Ctrl or Cmd with a letter: key holds the lowercase letter
    };
    Type type;
    char text[8]; // Text only, NUL-terminated
    char key;     // Shortcut only
    bool ctrl;    // Shortcut only
    bool cmd;     // Shortcut only
    bool shift;   // Shortcut only
};

class Keyboard {
  public:
    enum class State : uint8_t {
        Searching,  // scanning; a new keyboard must be in pairing mode
        Connecting, // found one, connecting
        Pairing,    // waiting for the passkey to be typed on the keyboard
        Connected,
    };

    // Starts Bluetooth and the background connection task.
    void begin(const char *deviceName);

    // Next key press, including auto-repeat of held keys. Call often from loop().
    bool poll(KeyEvent &event);

    State state() const;
    // Passkey to show while state() is Pairing.
    uint32_t passkey() const;
    // Name of the connected keyboard (empty if it didn't advertise one).
    String name() const;
    // Increments whenever state, passkey or name change, so the UI can redraw.
    uint32_t revision() const;

    // Forgets every paired keyboard and disconnects.
    void forget();
};

} // namespace tw
