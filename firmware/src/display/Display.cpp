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

    _sprite.fillSprite(TFT_BLACK);

    // Header bar: title + transport mode
    _sprite.fillRect(0, 0, OLED_WIDTH, 11, TFT_WHITE);
    _sprite.setTextColor(TFT_BLACK, TFT_WHITE);
    _sprite.setCursor(2, 2);
    {
        char titleBuf[20];
        snprintf(titleBuf, sizeof(titleBuf), "SX1280 FLRC %s",
                 (s.transportMode == static_cast<uint8_t>(TransportMode::NATIVE_PACKET)) ? "[N]" : "[G]");
        _sprite.print(titleBuf);
    }

    // Dividers
    _sprite.drawFastHLine(0, 12, OLED_WIDTH, TFT_WHITE);
    _sprite.drawFastVLine(64, 12, OLED_HEIGHT - 12, TFT_WHITE);

    _sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    char buf[32];

    // Row 1: frequency | bitrate
    snprintf(buf, sizeof(buf), "F:%.1f", s.freqMHz);
    _sprite.setCursor(2, 15);
    _sprite.print(buf);

    snprintf(buf, sizeof(buf), "R:%luK", s.bitrateKbps);
    _sprite.setCursor(68, 15);
    _sprite.print(buf);

    // Row 2: coding rate + power | preamble + BT shaping
    snprintf(buf, sizeof(buf), "CR:%u P:%d", s.codingRate, s.txPowerDbm);
    _sprite.setCursor(2, 27);
    _sprite.print(buf);

    snprintf(buf, sizeof(buf), "PR:%u BT:%u", s.preambleBits, s.btShaping);
    _sprite.setCursor(68, 27);
    _sprite.print(buf);

    // Row 3: sync word (spans full width)
    snprintf(buf, sizeof(buf), "SW:%08lx", static_cast<unsigned long>(s.syncWord));
    _sprite.setCursor(2, 39);
    _sprite.print(buf);

    // Row 4: LBT threshold
    if (s.lbtRssiThresholdDbm == 0) {
        snprintf(buf, sizeof(buf), "LBT:OFF");
    } else {
        snprintf(buf, sizeof(buf), "LBT:%d", s.lbtRssiThresholdDbm);
    }
    _sprite.setCursor(2, 51);
    _sprite.print(buf);

    _sprite.pushSprite(&_lcd, 0, 0);
}

void Display::notifyTx() {
    _flashTxUntilMs = millis() + 100;
}

void Display::notifyRx() {
    _flashRxUntilMs = millis() + 100;
}
