#include "Display.h"
#include "../config.h"
#include "../config/ModemConfig.h"
#include <stdio.h>
#include <string.h>

namespace {

void formatCount(char* out, size_t outLen, uint32_t value) {
    if (value < 1000u) {
        snprintf(out, outLen, "%lu", static_cast<unsigned long>(value));
    } else if (value < 100000u) {
        snprintf(out, outLen, "%lu.%luk",
                 static_cast<unsigned long>(value / 1000u),
                 static_cast<unsigned long>((value % 1000u) / 100u));
    } else if (value < 1000000u) {
        snprintf(out, outLen, "%luk",
                 static_cast<unsigned long>(value / 1000u));
    } else if (value < 100000000u) {
        snprintf(out, outLen, "%lu.%luM",
                 static_cast<unsigned long>(value / 1000000u),
                 static_cast<unsigned long>((value % 1000000u) / 100000u));
    } else {
        snprintf(out, outLen, "%luM",
                 static_cast<unsigned long>(value / 1000000u));
    }
}

void formatLinkAge(char* out, size_t outLen, uint32_t ageMs) {
    if (ageMs == 0xFFFFFFFFu) {
        snprintf(out, outLen, "--");
    } else if (ageMs < 1000u) {
        snprintf(out, outLen, "%lums", static_cast<unsigned long>(ageMs));
    } else if (ageMs < 10000u) {
        snprintf(out, outLen, "%lu.%lus",
                 static_cast<unsigned long>(ageMs / 1000u),
                 static_cast<unsigned long>((ageMs % 1000u) / 100u));
    } else if (ageMs < 100000u) {
        snprintf(out, outLen, "%lus",
                 static_cast<unsigned long>(ageMs / 1000u));
    } else {
        snprintf(out, outLen, ">99s");
    }
}

}  // namespace

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

    const uint32_t now = millis();
    if (s.txCount != _prev.txCount) {
        _flashTxUntilMs = now + 750u;
    }
    if (s.rxCount != _prev.rxCount) {
        _flashRxUntilMs = now + 750u;
    }

    _sprite.fillSprite(TFT_BLACK);

    // Header: link state, transport mode, and recent traffic indicators.
    const char* linkLabel = "DOWN";
    const bool recentError = s.lastRadioErrorMs != 0 &&
        (now - s.lastRadioErrorMs) <= RADIO_ERROR_HOLD_MS;
    if (s.radioState == RadioState::ERROR && recentError) {
        linkLabel = "ERROR";
    } else if (s.linkState == static_cast<uint8_t>(LinkState::READY)) {
        linkLabel = "READY";
    } else if (s.linkState == static_cast<uint8_t>(LinkState::PROBING)) {
        linkLabel = "PROBE";
    }
    const char mode =
        (s.transportMode == static_cast<uint8_t>(TransportMode::NATIVE_PACKET))
            ? 'N'
            : 'G';

    _sprite.fillRect(0, 0, OLED_WIDTH, 11, TFT_WHITE);
    _sprite.setTextColor(TFT_BLACK, TFT_WHITE);
    _sprite.setCursor(2, 2);
    char buf[32];
    snprintf(buf, sizeof(buf), "LINK %-5s %c  %c%c",
             linkLabel,
             mode,
             static_cast<int32_t>(_flashTxUntilMs - now) > 0 ? 'T' : '-',
             static_cast<int32_t>(_flashRxUntilMs - now) > 0 ? 'R' : '-');
    _sprite.print(buf);

    _sprite.drawFastHLine(0, 12, OLED_WIDTH, TFT_WHITE);
    _sprite.setTextColor(TFT_WHITE, TFT_BLACK);

    // PHY essentials.
    char rate[8];
    if (s.bitrateKbps >= 1000u) {
        snprintf(rate, sizeof(rate), "%lu.%luM",
                 static_cast<unsigned long>(s.bitrateKbps / 1000u),
                 static_cast<unsigned long>((s.bitrateKbps % 1000u) / 100u));
    } else {
        snprintf(rate, sizeof(rate), "%luK",
                 static_cast<unsigned long>(s.bitrateKbps));
    }
    snprintf(buf, sizeof(buf), "%.1f %s C%u P%+d",
             s.freqMHz, rate, s.codingRate, s.txPowerDbm);
    _sprite.setCursor(2, 14);
    _sprite.print(buf);

    // Signal and peer freshness.
    char age[10];
    formatLinkAge(age, sizeof(age), s.linkAgeMs);
    if (s.rssi == 0) {
        snprintf(buf, sizeof(buf), "RSSI: ---  AGE:%s", age);
    } else {
        snprintf(buf, sizeof(buf), "RSSI:%4d  AGE:%s", s.rssi, age);
    }
    _sprite.setCursor(2, 24);
    _sprite.print(buf);

    // Delivered traffic counters.
    char tx[8];
    char rx[8];
    formatCount(tx, sizeof(tx), s.txCount);
    formatCount(rx, sizeof(rx), s.rxCount);
    snprintf(buf, sizeof(buf), "TX:%s  RX:%s", tx, rx);
    _sprite.setCursor(2, 34);
    _sprite.print(buf);

    // Failure, retry, and egress-backpressure indicators.
    char errors[8];
    char retries[8];
    char deferrals[8];
    formatCount(errors, sizeof(errors), s.errorCount);
    formatCount(retries, sizeof(retries), s.arqRetryCount);
    formatCount(deferrals, sizeof(deferrals), s.rxEgressDeferrals);
    snprintf(buf, sizeof(buf), "E:%s R:%s BP:%s", errors, retries, deferrals);
    _sprite.setCursor(2, 44);
    _sprite.print(buf);

#if SERIAL_TX_WDT_DIAGNOSTICS
    // Host queue depth and USB write state for backpressure diagnosis.
    snprintf(buf, sizeof(buf), "Q:%lu/%lu W:%u S:%u",
             static_cast<unsigned long>(s.txQueueDepth),
             static_cast<unsigned long>(s.rxQueueDepth),
             s.serialWriteLockHeld,
             s.serialTxActive);
#else
    // MAC stack margin and host queue pressure.
    char hwm[8];
    char txWait[8];
    char rxWait[8];
    formatCount(hwm, sizeof(hwm), s.macStackHwm);
    formatCount(txWait, sizeof(txWait), s.txQueueWaitCount);
    formatCount(rxWait, sizeof(rxWait), s.rxQueueWaitCount);
    snprintf(buf, sizeof(buf), "MAC:%s Q:%s/%s", hwm, txWait, rxWait);
#endif
    _sprite.setCursor(2, 54);
    _sprite.print(buf);

    _sprite.pushSprite(&_lcd, 0, 0);
    _prev = s;
}

void Display::notifyTx() {
    _flashTxUntilMs = millis() + 100;
}

void Display::notifyRx() {
    _flashRxUntilMs = millis() + 100;
}
