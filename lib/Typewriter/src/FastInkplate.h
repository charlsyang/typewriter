// Inkplate 5 Gen2 with a faster partial update for typing.
//
// The library's partialUpdate() clocks all 720 rows through the panel on each of
// its four drive passes, even rows where nothing changed. While typing, only one
// line of text and the status bar change, so most of that time drives no-op data.
// quickPartialUpdate() sends data only for changed rows (plus the row after each,
// so latched data always gets a full-length drive) and just advances the gate
// driver past the rest, the same way the EPDiy driver skips rows.
//
// Skipping rows also shortens each drive pass, and pixels hold their drive
// voltage for the whole pass, so shorter passes mean lighter text. passUs sets a
// minimum pass length to trade latency for darkness; the library's passes take
// about 55 ms.
#pragma once

#include <Inkplate.h>

namespace tw {

class FastInkplate : public Inkplate {
  public:
    // Adafruit_GFX is a virtual base, so the most-derived class must construct it.
    explicit FastInkplate(uint8_t mode) : Adafruit_GFX(E_INK_WIDTH, E_INK_HEIGHT), Inkplate(mode) {}

    // Like partialUpdate(false, true): the panel stays powered.
    // skipUnchanged = false sends every row (library-length passes).
    // passUs pads each of the four drive passes to at least this long.
    // Returns the number of rows that changed.
    uint32_t quickPartialUpdate(bool skipUnchanged = true, uint32_t passUs = 0);

  private:
    void startFrame();
    void sendRow();
    void skipRow();
};

} // namespace tw
