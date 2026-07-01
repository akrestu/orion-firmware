#pragma once

#include <Arduino.h>
#include "HT_st7735.h"
#include "gps.h"
#include "icons.h"

// ---------------------------------------------------------------------------
// Color palette
// ---------------------------------------------------------------------------
#define CLR_BG          ST7735_BLACK
#define CLR_HEADER_BG   ST7735_COLOR565(0, 60, 160)
#define CLR_DIVIDER     ST7735_COLOR565(45, 45, 45)
#define CLR_TITLE       ST7735_WHITE
#define CLR_VALUE       ST7735_CYAN
#define CLR_LABEL       ST7735_COLOR565(160, 160, 160)
#define CLR_OK          ST7735_COLOR565(0, 230, 0)
#define CLR_WARN        ST7735_YELLOW
#define CLR_ERR         ST7735_RED
#define CLR_COUNTDOWN   ST7735_YELLOW
#define CLR_ORANGE      ST7735_COLOR565(255, 140, 0)

// ---------------------------------------------------------------------------
// Layout constants — 160×80 display, Font_7x10 (7px wide, 10px tall)
//
//  y=  0..11  Header bar (12px)
//  y= 12      Divider
//  y= 13..23  Lat row
//  y= 24      Divider
//  y= 25..35  Lon row
//  y= 36      Divider
//  y= 37..47  Speed / Sats / HDOP row
//  y= 48      Divider
//  y= 49..59  TX status row
//  y= 60      Divider
//  y= 61..79  Countdown row
// ---------------------------------------------------------------------------
#define DISP_W        160
#define DISP_H         80
#define ROW_HEADER_Y    0
#define ROW_HEADER_H   12
#define ROW_DIV1_Y     12
#define ROW_LAT_Y      13
#define ROW_DIV2_Y     24
#define ROW_LON_Y      25
#define ROW_DIV3_Y     36
#define ROW_SPEED_Y    37
#define ROW_DIV4_Y     48
#define ROW_STATUS_Y   49
#define ROW_DIV5_Y     60
#define ROW_NEXT_Y     61
#define ROW_H          11   // text row height (Font 10px + 1px gap)

// ---------------------------------------------------------------------------
// TX state — drives header dot color and status text color
// ---------------------------------------------------------------------------
enum TxStatus { TX_NONE, TX_OK, TX_FAIL, TX_SENDING };

// ---------------------------------------------------------------------------
// DisplayManager
// ---------------------------------------------------------------------------
class DisplayManager {
public:
    explicit DisplayManager(HT_st7735& disp) : _disp(disp), _isOn(true) {
        for (uint8_t i = 0; i < NUM_ROWS; i++) {
            _cache[i].text[0] = '\0';
            _cache[i].color   = 0xFFFF;  // invalid sentinel — force first draw
            _cache[i].icon    = nullptr;
        }
        _lastDotColor = 0xFFFF;
    }

    void begin() {
        _disp.st7735_init();
        _disp.st7735_fill_screen(CLR_BG);
        _isOn = true;
    }

    // Matikan layar: matikan backlight LED (display data tetap di RAM ST7735)
    void off() {
        if (!_isOn) return;
        digitalWrite(PIN_LCD_BL, LOW);
        _isOn = false;
    }

    // Hidupkan layar: nyalakan backlight, paksa full redraw agar data fresh
    void wakeup() {
        if (_isOn) return;
        digitalWrite(PIN_LCD_BL, HIGH);
        _invalidateCache();
        _isOn = true;
    }

    bool isOn() const { return _isOn; }

    // Shutdown countdown — updates only the text and bar, no full-screen clear (prevents flicker)
    void showShutdown(const char* msg, uint8_t progress) {
        if (!_shutdownInit) {
            _disp.st7735_fill_screen(CLR_BG);
            _disp.st7735_fill_rectangle(0, 0, DISP_W, 20, CLR_HEADER_BG);
            _disp.st7735_write_str(4, 5, "ORION BEACON", Font_7x10, CLR_TITLE, CLR_HEADER_BG);
            _disp.st7735_fill_rectangle(4, 48, 152, 10, CLR_DIVIDER);
            _shutdownInit = true;
            _shutdownProg = 0xFF;
            _shutdownMsg[0] = '\0';
        }
        if (strncmp(_shutdownMsg, msg, sizeof(_shutdownMsg)) != 0) {
            _disp.st7735_fill_rectangle(0, 28, DISP_W, 10, CLR_BG);
            _disp.st7735_write_str(4, 28, msg, Font_7x10, CLR_WARN, CLR_BG);
            strncpy(_shutdownMsg, msg, sizeof(_shutdownMsg) - 1);
            _shutdownMsg[sizeof(_shutdownMsg) - 1] = '\0';
        }
        if (progress != _shutdownProg) {
            _disp.st7735_fill_rectangle(5, 49, 150, 8, CLR_DIVIDER);
            if (progress > 0) {
                uint16_t barW = (uint16_t)((uint32_t)progress * 150 / 100);
                _disp.st7735_fill_rectangle(5, 49, barW, 8, CLR_ERR);
            }
            _shutdownProg = progress;
        }
    }

    void resetShutdown() { _shutdownInit = false; _invalidateCache(); }

    // Boot / init screen with progress bar (progress 0–100)
    void showBoot(const char* step, uint8_t progress = 0) {
        _disp.st7735_fill_screen(CLR_BG);
        _invalidateCache();
        // Header
        _disp.st7735_fill_rectangle(0, 0, DISP_W, 20, CLR_HEADER_BG);
        _disp.st7735_write_str(4, 5, "ORION BEACON", Font_7x10, CLR_TITLE, CLR_HEADER_BG);
        // Step label + percentage
        _disp.st7735_write_str(4, 28, step, Font_7x10, CLR_VALUE, CLR_BG);
        char pct[8];
        snprintf(pct, sizeof(pct), "%3d%%", progress);
        _disp.st7735_write_str(DISP_W - 29, 28, pct, Font_7x10, CLR_WARN, CLR_BG);
        // Progress bar
        _disp.st7735_fill_rectangle(4,  48, 152, 10, CLR_DIVIDER);
        if (progress > 0) {
            uint16_t barW = (uint16_t)((uint32_t)progress * 150 / 100);
            _disp.st7735_fill_rectangle(5, 49, barW, 8, CLR_OK);
        }
    }

    // Full-screen error
    void showError(const char* line1, const char* line2 = "") {
        _disp.st7735_fill_screen(CLR_BG);
        _invalidateCache();
        _disp.st7735_fill_rectangle(0, 0, DISP_W, ROW_HEADER_H, CLR_ERR);
        _disp.st7735_write_str(4, 1, "!! ERROR !!", Font_7x10, CLR_TITLE, CLR_ERR);
        _disp.st7735_write_str(4, ROW_LAT_Y, line1, Font_7x10, CLR_ERR,  CLR_BG);
        _disp.st7735_write_str(4, ROW_LON_Y, line2, Font_7x10, CLR_WARN, CLR_BG);
    }

    // No-fix waiting screen
    void showNoFix(uint32_t waitedSec, uint8_t satsInView = 0,
                   TxStatus lastTx = TX_NONE, const char* lastTxMsg = "") {
        _drawHeader(CLR_WARN);
        _drawRow(IDX_LAT, ROW_LAT_Y, "Cari satelit...", CLR_WARN, ICON_GPS);

        char buf[24];
        if (satsInView > 0) {
            snprintf(buf, sizeof(buf), "Terlihat: %d sat", satsInView);
            _drawRow(IDX_LON, ROW_LON_Y, buf, CLR_LABEL, ICON_GPS);
        } else {
            _drawRow(IDX_LON, ROW_LON_Y, "Terlihat: -", CLR_LABEL, ICON_GPS);
        }

        if (waitedSec >= 60) {
            snprintf(buf, sizeof(buf), "Tunggu: %um %us",
                     (unsigned)(waitedSec / 60), (unsigned)(waitedSec % 60));
        } else {
            snprintf(buf, sizeof(buf), "Tunggu: %us", waitedSec);
        }
        _drawRow(IDX_SPEED, ROW_SPEED_Y, buf, CLR_LABEL, ICON_CLOCK);

        uint16_t txClr = (lastTx == TX_OK)   ? CLR_OK
                       : (lastTx == TX_FAIL)  ? CLR_ERR
                       :                        CLR_LABEL;
        _drawRow(IDX_STATUS, ROW_STATUS_Y,
                 lastTxMsg[0] != '\0' ? lastTxMsg : "Belum ada TX", txClr, ICON_ANTENNA);

        _drawRow(IDX_NEXT, ROW_NEXT_Y, "LoRa aktif, GPS belum", CLR_LABEL, ICON_ANTENNA);
    }

    // Main tracking dashboard — called every display refresh cycle (~200ms)
    // displayOffInSec: seconds until auto-off (0 = unknown/disabled)
    void showTracking(const GpsData& gps, uint32_t nextSendSec,
                      bool moving, TxStatus txStatus, const char* txMsg,
                      uint32_t displayOffInSec = 0) {

        bool   approaching   = (nextSendSec <= 5) && (txStatus != TX_SENDING);
        bool   screenWarning = (displayOffInSec > 0) && (displayOffInSec <= 10);
        bool   blinkOn       = (millis() / 400) % 2;
        uint16_t dotColor;
        if (screenWarning) {
            dotColor = blinkOn ? CLR_ORANGE : CLR_BG;
        } else if (txStatus == TX_SENDING) {
            dotColor = blinkOn ? CLR_WARN : CLR_BG;
        } else if (approaching) {
            dotColor = blinkOn ? CLR_COUNTDOWN : CLR_LABEL;
        } else {
            dotColor = (txStatus == TX_OK)   ? CLR_OK
                     : (txStatus == TX_FAIL) ? CLR_ERR
                     :                         CLR_LABEL;
        }
        _drawHeader(dotColor);

        char buf[24];

        // ── Lat ──────────────────────────────────────────────────────────
        snprintf(buf, sizeof(buf), "Lat: %+.5f", gps.latitude);
        _drawRow(IDX_LAT, ROW_LAT_Y, buf, CLR_VALUE, ICON_GPS);

        // ── Lon ──────────────────────────────────────────────────────────
        snprintf(buf, sizeof(buf), "Lon: %+.5f", gps.longitude);
        _drawRow(IDX_LON, ROW_LON_Y, buf, CLR_VALUE, ICON_GPS);

        // ── Speed + Heading + Sats + HDOP — row color driven by HDOP quality
        // Cap HDOP display at 99 to prevent overflow on 160px wide screen
        float dispHdop = gps.hdop > 99.0f ? 99.0f : gps.hdop;
        snprintf(buf, sizeof(buf), "%3.0fk/h %-2s S:%d H:%.0f",
                 gps.speed_kmh, _cardinalDir(gps.course_deg, gps.speed_kmh),
                 gps.satellites, dispHdop);
        _drawRow(IDX_SPEED, ROW_SPEED_Y, buf, _hdopColor(gps.speed_kmh, gps.hdop),
                 moving ? ICON_ARROW : ICON_PAUSE);

        // ── TX status ────────────────────────────────────────────────────
        uint16_t statusClr = (txStatus == TX_OK)      ? CLR_OK
                           : (txStatus == TX_FAIL)    ? CLR_ERR
                           : (txStatus == TX_SENDING) ? CLR_WARN
                           :                            CLR_LABEL;
        _drawRow(IDX_STATUS, ROW_STATUS_Y, txMsg, statusClr, ICON_ANTENNA);

        // ── Countdown — display-off warning overrides TX countdown ──────
        if (txStatus == TX_SENDING) {
            _drawRow(IDX_NEXT, ROW_NEXT_Y, "Transmitting...", CLR_WARN, ICON_ANTENNA);
        } else if (screenWarning) {
            snprintf(buf, sizeof(buf), "Layar mati %lus...", displayOffInSec);
            _drawRow(IDX_NEXT, ROW_NEXT_Y, buf,
                     blinkOn ? CLR_ORANGE : CLR_LABEL, ICON_CLOCK);
        } else {
            snprintf(buf, sizeof(buf), "TX in:%lus [%s]",
                     nextSendSec, moving ? "MOV" : "IDL");
            uint16_t cntClr = approaching
                ? (blinkOn ? CLR_ERR : CLR_COUNTDOWN)
                : CLR_COUNTDOWN;
            _drawRow(IDX_NEXT, ROW_NEXT_Y, buf, cntClr, ICON_CLOCK);
        }
    }

private:
    HT_st7735& _disp;
    bool       _isOn;
    bool       _shutdownInit = false;
    uint8_t    _shutdownProg = 0xFF;
    char       _shutdownMsg[24] = {};

    // ── Fase 4: Row cache ─────────────────────────────────────────────────
    // Skip fill+write when text and color are identical to last draw.
    static constexpr uint8_t NUM_ROWS = 5;
    enum RowIdx : uint8_t { IDX_LAT=0, IDX_LON, IDX_SPEED, IDX_STATUS, IDX_NEXT };
    struct RowState { char text[24]; uint16_t color; const uint8_t* icon; };
    RowState _cache[NUM_ROWS];
    uint16_t _lastDotColor;

    // ── Fase 3: HDOP quality helpers ─────────────────────────────────────
    // Row color: prioritise HDOP warning over stationary dimming.
    uint16_t _hdopColor(float speed_kmh, float hdop) const {
        if (hdop > 5.0f)  return CLR_ERR;
        if (hdop > 2.5f)  return CLR_ORANGE;
        if (hdop > 1.5f)  return CLR_WARN;
        return speed_kmh > 2.0f ? ST7735_WHITE : CLR_LABEL;
    }
    // Cardinal direction from course_deg; returns "--" when speed too low to be reliable
    const char* _cardinalDir(float deg, float speed_kmh) const {
        if (speed_kmh < 2.0f) return "--";
        static const char* dirs[] = { "N","NE","E","SE","S","SW","W","NW" };
        uint8_t idx = (uint8_t)((deg + 22.5f) / 45.0f) & 7;
        return dirs[idx];
    }

    // Render 8×10 monochrome bitmap at (x,y) in foreground color fg.
    // Builds RGB565 pixel buffer on stack and sends as one draw_image call.
    void _drawIcon(uint16_t x, uint16_t y, const uint8_t* mask, uint16_t fg) {
        uint16_t buf[80];
        for (uint8_t row = 0; row < 10; row++) {
            uint8_t m = mask[row];
            for (uint8_t col = 0; col < 8; col++)
                buf[row * 8 + col] = (m & (0x80 >> col)) ? fg : (uint16_t)CLR_BG;
        }
        _disp.st7735_draw_image(x, y, 8, 10, buf);
    }

    // Cached row draw — only clears and redraws if text, color, or icon changed.
    // icon=nullptr → text starts at x=4; icon provided → icon at x=0, text at x=11.
    void _drawRow(RowIdx idx, uint16_t y, const char* text, uint16_t color,
                  const uint8_t* icon = nullptr) {
        RowState& c = _cache[idx];
        if (c.color == color && c.icon == icon &&
            strncmp(c.text, text, sizeof(c.text)) == 0) return;
        _disp.st7735_fill_rectangle(0, y, DISP_W, ROW_H, CLR_BG);
        if (icon) {
            _drawIcon(0, y, icon, color);
            if (text[0] != '\0')
                _disp.st7735_write_str(11, y, text, Font_7x10, color, CLR_BG);
        } else {
            if (text[0] != '\0')
                _disp.st7735_write_str(4, y, text, Font_7x10, color, CLR_BG);
        }
        strncpy(c.text, text, sizeof(c.text) - 1);
        c.text[sizeof(c.text) - 1] = '\0';
        c.color = color;
        c.icon  = icon;
    }

    void _drawHeader(uint16_t dotColor) {
        if (_lastDotColor == 0xFFFF) {
            _disp.st7735_fill_rectangle(0, ROW_HEADER_Y, DISP_W, ROW_HEADER_H, CLR_HEADER_BG);
            _disp.st7735_write_str(4, 1, UNIT_NAME, Font_7x10, CLR_TITLE, CLR_HEADER_BG);
            _disp.st7735_fill_rectangle(0, ROW_DIV1_Y, DISP_W, 1, CLR_DIVIDER);
            _disp.st7735_fill_rectangle(0, ROW_DIV2_Y, DISP_W, 1, CLR_DIVIDER);
            _disp.st7735_fill_rectangle(0, ROW_DIV3_Y, DISP_W, 1, CLR_DIVIDER);
            _disp.st7735_fill_rectangle(0, ROW_DIV4_Y, DISP_W, 1, CLR_DIVIDER);
            _disp.st7735_fill_rectangle(0, ROW_DIV5_Y, DISP_W, 1, CLR_DIVIDER);
        }
        if (dotColor != _lastDotColor) {
            _disp.st7735_fill_rectangle(DISP_W - 12, 2, 8, 8, dotColor);
            _lastDotColor = dotColor;
        }
    }

    void _invalidateCache() {
        for (uint8_t i = 0; i < NUM_ROWS; i++) {
            _cache[i].text[0] = '\0';
            _cache[i].color   = 0xFFFF;
            _cache[i].icon    = nullptr;
        }
        _lastDotColor = 0xFFFF;
    }
};
