#include "FastInkplate.h"

#ifdef ARDUINO_INKPLATE5V2

namespace tw {

// Same sequence as EPDDriver::vscan_start(): begin a frame and skip the two
// rows that are off screen.
void FastInkplate::startFrame() {
    CKV_SET;
    delayMicroseconds(7);
    SPV_CLEAR;
    delayMicroseconds(10);
    CKV_CLEAR;
    CKV_SET;
    delayMicroseconds(8);
    SPV_SET;
    delayMicroseconds(10);
    CKV_CLEAR;
    CKV_SET;
    delayMicroseconds(18);
    CKV_CLEAR;
    CKV_SET;
    delayMicroseconds(18);
    CKV_CLEAR;
    CKV_SET;
}

// Shift _dmaLineBuffer into the source drivers (CKV is high while it transfers,
// driving the previously latched row), then latch it (EPDDriver::vscan_end()).
void FastInkplate::sendRow() {
    sendDataI2S(myI2S, _dmaI2SDesc);
    CKV_CLEAR;
    LE_SET;
    LE_CLEAR;
}

// Advance one row without shifting new data. Only valid while the latched and
// shifted data are both no-op, so the brief CKV pulse drives nothing.
void FastInkplate::skipRow() {
    CKV_SET;
    delayMicroseconds(1);
    CKV_CLEAR;
    LE_SET;
    LE_CLEAR;
}

uint32_t FastInkplate::quickPartialUpdate() {
    if (getDisplayMode() == INKPLATE_3BIT)
        return 0;
    if (_blockPartial) {
        // The library needs one full refresh before partial updates are allowed.
        display(true);
        return E_INK_HEIGHT;
    }

    const int rowBytes = E_INK_WIDTH / 8;
    const int rowOut = E_INK_WIDTH / 4;
    static bool changed[E_INK_HEIGHT];
    uint32_t changedRows = 0;

    // Build the drive data like EPDDriver::partialUpdate(), but only for rows that
    // differ; unchanged rows get no-op data (0xFF) without the per-pixel lookups.
    for (int i = 0; i < E_INK_HEIGHT; i++) {
        const uint8_t *oldRow = DMemoryNew + i * rowBytes;
        const uint8_t *newRow = _partial + i * rowBytes;
        uint8_t *out = _pBuffer + i * rowOut;
        changed[i] = memcmp(oldRow, newRow, rowBytes) != 0;
        if (!changed[i]) {
            memset(out, 0xFF, rowOut);
            continue;
        }
        changedRows++;
        for (int j = 0; j < rowBytes; j++) {
            uint8_t diffw = oldRow[j] & ~newRow[j];
            uint8_t diffb = ~oldRow[j] & newRow[j];
            out[2 * j] = LUTW[diffw & 0x0F] & LUTB[diffb & 0x0F];
            out[2 * j + 1] = LUTW[diffw >> 4] & LUTB[diffb >> 4];
        }
    }

    if (changedRows == 0)
        return 0;

    _dmaI2SDesc->size = rowOut + 16;
    _dmaI2SDesc->length = rowOut + 16;
    _dmaI2SDesc->sosf = 1;
    _dmaI2SDesc->owner = 1;
    _dmaI2SDesc->qe.stqe_next = 0;
    _dmaI2SDesc->eof = 1;
    _dmaI2SDesc->buf = _dmaLineBuffer;
    _dmaI2SDesc->offset = 0;

    if (!einkOn())
        return 0;

    for (int k = 0; k < 4; k++) {
        uint8_t *dp = _pBuffer;
        startFrame();
        // After startFrame() the latched data is unknown, so the first row is sent.
        bool previousNoop = false;
        for (int i = 0; i < E_INK_HEIGHT; i++) {
            if (!changed[i] && previousNoop) {
                skipRow();
            } else {
                uint8_t *src = dp + rowOut - 1;
                for (int j = 0; j < rowOut; j += 4) {
                    _dmaLineBuffer[j + 2] = *(src--);
                    _dmaLineBuffer[j + 3] = *(src--);
                    _dmaLineBuffer[j] = *(src--);
                    _dmaLineBuffer[j + 1] = *(src--);
                }
                sendRow();
            }
            previousNoop = !changed[i];
            dp += rowOut;
        }
        delayMicroseconds(230);
    }

    // The library ends with two no-op frames (clean(2, 2)); keep them, but skip
    // their rows after the first since the data never changes.
    for (int i = 0; i < rowOut; i++)
        _dmaLineBuffer[i] = 0;
    for (int k = 0; k < 2; k++) {
        startFrame();
        sendRow();
        for (int i = 1; i < E_INK_HEIGHT; i++)
            skipRow();
        delayMicroseconds(230);
    }

    memcpy(DMemoryNew, _partial, E_INK_WIDTH * E_INK_HEIGHT / 8);
    return changedRows;
}

} // namespace tw

#endif
