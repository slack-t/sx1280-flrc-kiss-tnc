#pragma once
#include <LovyanGFX.hpp>
#include "../config.h"
#include "../stats/Stats.h"

// ── LovyanGFX hardware configuration for T3S3 SSD1306 (128x64, I2C) ──────────
// Defined here so both Display.cpp and the compiler can see the concrete type
// used as the _lcd member below.
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_SSD1306 _panel;
    lgfx::Bus_I2C       _bus;

public:
    LGFX() {
        {
            auto cfg       = _bus.config();
            cfg.i2c_port   = 0;
            cfg.freq_write = 400000;
            cfg.freq_read  = 400000;
            cfg.pin_sda    = OLED_SDA;
            cfg.pin_scl    = OLED_SCL;
            cfg.i2c_addr   = OLED_ADDR;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg             = _panel.config();
            cfg.pin_cs           = -1;
            cfg.pin_rst          = -1;
            cfg.panel_width      = OLED_WIDTH;
            cfg.panel_height     = OLED_HEIGHT;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.readable         = false;
            cfg.invert           = false;
            _panel.config(cfg);
        }
        setPanel(&_panel);
    }
};

// ── Display controller ────────────────────────────────────────────────────────
class Display {
public:
    // Initialise SSD1306 and draw the static chrome (title bar, dividers)
    bool begin();

    // Redraw the dynamic fields from the given Stats snapshot.
    // Only redraws values that have changed to avoid flicker.
    void update(const Stats& stats);

    // Draw a critical error screen on the display and halt
    void showError(const char* context, int16_t errorCode);

    // Non-blocking: schedule a 100 ms title-bar flash. Safe to call from any task.
    // The colour is applied on the next update() tick — no delay() anywhere.
    void notifyTx();
    void notifyRx();

private:
    LGFX        _lcd;
    LGFX_Sprite _sprite;      // offscreen buffer to eliminate tearing

    Stats       _prev;        // last drawn values — diff against incoming
    bool        _initialised    = false;
    uint32_t    _flashTxUntilMs  = 0;
    uint32_t    _flashRxUntilMs  = 0;
    uint16_t    _prevTitleColour = 0;   // tracks last drawn title bar colour

    void _drawChrome();
};
