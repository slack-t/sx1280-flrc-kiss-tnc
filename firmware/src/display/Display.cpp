#include "Display.h"
#include "../config/ModemConfig.h"
#include <stdio.h>
#include <string.h>

bool Display::begin() {
    _lcd.init();
    _lcd.setRotation(0);
    _lcd.fillScreen(TFT_BLACK);
    _lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    _lcd.setTextSize(1);
    _lcd.setFont(&fonts::Font0);

    // Draw boot screen directly to LCD so it appears instantly before the double buffer is active
    _lcd.drawFastHLine(0, 12, OLED_WIDTH, TFT_WHITE);
    _lcd.setCursor(2, 2);
    _lcd.print("SX1280 FLRC TNC BOOTING...");
    _lcd.setCursor(2, 20);
    _lcd.print("Initializing OLED: OK");
    _lcd.setCursor(2, 32);
    _lcd.print("Initializing Radio...");

    _sprite.setColorDepth(1); // 1-bit monochrome offscreen buffer
    _sprite.createSprite(OLED_WIDTH, OLED_HEIGHT);
    _sprite.setTextSize(1);
    _sprite.setFont(&fonts::Font0); // Clear 6x8 pixel font

    _initialised = true;
    return true;
}

void Display::showError(const char* context, int16_t errorCode) {
    // Fill the screen directly to bypass double buffering since we are halting
    _lcd.fillScreen(TFT_BLACK);
    _lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    _lcd.drawRect(0, 0, OLED_WIDTH, OLED_HEIGHT, TFT_WHITE);
    
    // Title
    _lcd.fillRect(2, 2, OLED_WIDTH - 4, 11, TFT_WHITE);
    _lcd.setTextColor(TFT_BLACK, TFT_WHITE);
    _lcd.setCursor(4, 4);
    _lcd.print("SYSTEM HALTED");
    
    // Message
    _lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    _lcd.setCursor(6, 20);
    _lcd.print("ERR: ");
    _lcd.print(context);
    
    char codeBuf[32];
    snprintf(codeBuf, sizeof(codeBuf), "CODE: %d", errorCode);
    _lcd.setCursor(6, 36);
    _lcd.print(codeBuf);
    
    _lcd.setCursor(6, 50);
    _lcd.print("Check HW & reset.");
}

void Display::_drawChrome() {
    // Left blank as rendering is fully handled in update() using the double buffer sprite.
}

void Display::update(const Stats& s) {
    if (!_initialised) return;

    // Clear offscreen buffer
    _sprite.fillSprite(TFT_BLACK);

    // 1. Determine header flash state (blinks on TX/RX packets)
    uint32_t now = millis();
    bool flashActive = (now < _flashTxUntilMs || now < _flashRxUntilMs);

    // 2. Draw Header Bar
    if (flashActive) {
        // Flash state: Black bar with white text
        _sprite.fillRect(0, 0, OLED_WIDTH, 11, TFT_BLACK);
        _sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    } else {
        // Normal state: White bar with black text
        _sprite.fillRect(0, 0, OLED_WIDTH, 11, TFT_WHITE);
        _sprite.setTextColor(TFT_BLACK, TFT_WHITE);
    }

    _sprite.setCursor(2, 2);
    _sprite.print("SX1280 FLRC TNC");

    // Print State in Header (right-aligned)
    const char* stateStr = "IDLE";
    switch (s.radioState) {
        case RadioState::TX:    stateStr = "TX"; break;
        case RadioState::RX:    stateStr = "RX"; break;
        case RadioState::ERROR: stateStr = "ERR"; break;
        default:                stateStr = "IDLE"; break;
    }

    // Alignment logic for 6x8 font (each char is 6px wide)
    int stateX = 102; // Default for "IDLE" (4 chars = 24px, 128 - 24 - 2)
    if (s.radioState == RadioState::TX || s.radioState == RadioState::RX) {
        stateX = 114; // "TX"/"RX" (2 chars = 12px, 128 - 12 - 2)
    } else if (s.radioState == RadioState::ERROR) {
        stateX = 108; // "ERR" (3 chars = 18px, 128 - 18 - 2)
    }

    _sprite.setCursor(stateX, 2);
    _sprite.print(stateStr);

    // 3. Draw Layout Separators (White on black)
    _sprite.drawFastHLine(0, 12, OLED_WIDTH, TFT_WHITE);
    _sprite.drawFastVLine(64, 12, OLED_HEIGHT - 12, TFT_WHITE);

    // 4. Draw Body Text
    _sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    char buf[32];

    const uint8_t page = static_cast<uint8_t>((now / 3000) % 4);

    if (page == 1) {
        snprintf(buf, sizeof(buf), "AT:%lu", s.arqAckTimeoutCount);
        _sprite.setCursor(2, 15);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "RE:%lu", s.radioRxErrors);
        _sprite.setCursor(68, 15);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "RT:%lu", s.arqRetryCount);
        _sprite.setCursor(2, 27);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "TE:%lu", s.radioTxErrors);
        _sprite.setCursor(68, 27);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "TQ:%lu", s.txQueueWaitCount);
        _sprite.setCursor(2, 39);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "RQ:%lu", s.rxQueueWaitCount);
        _sprite.setCursor(68, 39);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "ZW:%lu", s.serialTxZeroWrites);
        _sprite.setCursor(2, 51);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "ST:%lu", s.serialTxTimeouts);
        _sprite.setCursor(68, 51);
        _sprite.print(buf);

        _sprite.pushSprite(&_lcd, 0, 0);
        return;
    }

    if (page == 2) {
        snprintf(buf, sizeof(buf), "SP:%lu", s.rxSpuriousIrqCount);
        _sprite.setCursor(2, 15);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "IL:%lu", s.rxInvalidLengthCount);
        _sprite.setCursor(68, 15);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "CE:%lu", s.rxCrcErrorCount);
        _sprite.setCursor(2, 27);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "HE:%lu", s.rxHeaderErrorCount);
        _sprite.setCursor(68, 27);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "RD:%lu", s.rxReadDataErrorCount);
        _sprite.setCursor(2, 39);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "LE:%d", s.lastRadioErr);
        _sprite.setCursor(68, 39);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "IR:%04x", s.lastIrqStatus);
        _sprite.setCursor(2, 51);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "LN:%u", s.lastPacketLength);
        _sprite.setCursor(68, 51);
        _sprite.print(buf);

        _sprite.pushSprite(&_lcd, 0, 0);
        return;
    }

    if (page == 3) {
        const char* source = "DEF";
        if (s.configSource == static_cast<uint8_t>(ModemConfigSource::NVS)) {
            source = "NVS";
        } else if (s.configSource == static_cast<uint8_t>(ModemConfigSource::RESET_HELD)) {
            source = "RST";
        }

        snprintf(buf, sizeof(buf), "CR:%u P:%d", s.codingRate, s.txPowerDbm);
        _sprite.setCursor(2, 15);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "PR:%u BT:%u", s.preambleBits, s.btShaping);
        _sprite.setCursor(2, 27);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "SW:%08lx", static_cast<unsigned long>(s.syncWord));
        _sprite.setCursor(2, 39);
        _sprite.print(buf);

        snprintf(buf, sizeof(buf), "V:%u %s C:%04x", s.configVersion, source, s.configCrc16);
        _sprite.setCursor(2, 51);
        _sprite.print(buf);

        _sprite.pushSprite(&_lcd, 0, 0);
        return;
    }

    // --- Row 1 (Y = 15) ---
    snprintf(buf, sizeof(buf), "F:%.1f", s.freqMHz);
    _sprite.setCursor(2, 15);
    _sprite.print(buf);

    snprintf(buf, sizeof(buf), "TX:%lu", s.txCount);
    _sprite.setCursor(68, 15);
    _sprite.print(buf);

    // --- Row 2 (Y = 27) ---
    snprintf(buf, sizeof(buf), "R:%luK", s.bitrateKbps);
    _sprite.setCursor(2, 27);
    _sprite.print(buf);

    snprintf(buf, sizeof(buf), "RX:%lu", s.rxCount);
    _sprite.setCursor(68, 27);
    _sprite.print(buf);

    // --- Row 3 (Y = 39) ---
    snprintf(buf, sizeof(buf), "RSSI:%d", s.rssi);
    _sprite.setCursor(2, 39);
    _sprite.print(buf);

    snprintf(buf, sizeof(buf), "ER:%lu", s.errorCount);
    _sprite.setCursor(68, 39);
    _sprite.print(buf);

    // --- Row 4 (Y = 51) ---
    snprintf(buf, sizeof(buf), "RTY:%lu", s.arqRetryCount);
    _sprite.setCursor(2, 51);
    _sprite.print(buf);

    snprintf(buf, sizeof(buf), "ATO:%lu", s.arqAckTimeoutCount);
    _sprite.setCursor(68, 51);
    _sprite.print(buf);

    // 5. Push double-buffered frame to display
    _sprite.pushSprite(&_lcd, 0, 0);
}

void Display::notifyTx() {
    _flashTxUntilMs = millis() + 100;
}

void Display::notifyRx() {
    _flashRxUntilMs = millis() + 100;
}
