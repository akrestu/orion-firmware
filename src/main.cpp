#include <Arduino.h>
#include <esp_sleep.h>
#include "config.h"
#include "gps.h"
#include "lorawan.h"
#include "display.h"

// Fix #9: RTC_DATA_ATTR harus didefinisikan di satu TU saja.
// lorawan.h hanya extern-declare; definisi ada di sini.
RTC_DATA_ATTR uint8_t  lwNonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE]  = { 0 };
RTC_DATA_ATTR uint8_t  lwSession[RADIOLIB_LORAWAN_SESSION_BUF_SIZE] = { 0 };
RTC_DATA_ATTR bool     lwNoncesValid = false;
RTC_DATA_ATTR uint32_t bootCount     = 0;

// Posisi terakhir di RTC agar distance calc tetap valid setelah deep sleep
RTC_DATA_ATTR double   rtcLastSentLat  = 0.0;
RTC_DATA_ATTR double   rtcLastSentLon  = 0.0;
// Timestamp (ms) saat masuk deep sleep — untuk deteksi long sleep > 4 jam
RTC_DATA_ATTR uint64_t rtcSleepEpochMs = 0;

GpsManager     gpsManager;
LoRaWANManager loraManager;
HT_st7735      display;
DisplayManager displayMgr(display);

// Last transmitted position
static double   lastSentLat   = 0.0;
static double   lastSentLon   = 0.0;
static uint32_t lastSendTime  = 0;
static uint32_t lastSuccessTx = 0;

// Persistent display state
static TxStatus txStatus = TX_NONE;
static char     txMsg[24] = "Nyala nih...";

// Display auto-off timer
static uint32_t displayWakeTime = 0;

// Button debounce state
static uint8_t  btnDisplayPrev      = HIGH;
static uint32_t btnDisplayEdge      = 0;
static uint32_t btnDisplayHoldStart = 0;
static bool     btnDisplayHolding   = false;
static uint8_t  btnPowerPrev        = HIGH;
static uint32_t btnPowerEdge        = 0;

static void powerOff() {
    if (DEBUG_ENABLED) DEBUG_SERIAL.println("[SYS] Powering off...");
    displayMgr.wakeup();
    displayMgr.showBoot("Sampai jumpa, bos!", 100);
    delay(1000);

    rtcSleepEpochMs = esp_timer_get_time() / 1000ULL;
    loraManager.sleep();
    digitalWrite(PIN_GPS_PWR, LOW);
    digitalWrite(PIN_LCD_BL, LOW);

    // Deep sleep tanpa wakeup timer — hanya RST atau tombol PRG yang bisa bangunkan
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_DISPLAY, 0);
    esp_deep_sleep_start();
}

static void handleButtons() {
    uint32_t now = millis();

    uint8_t btnDisplay = digitalRead(PIN_BTN_DISPLAY);
    if (btnDisplay != btnDisplayPrev && (now - btnDisplayEdge) > 50) {
        btnDisplayEdge = now;
        if (btnDisplay == LOW) {
            btnDisplayHoldStart = now;
            btnDisplayHolding   = true;
            displayWakeTime = now;
            if (!displayMgr.isOn()) {
                displayMgr.wakeup();
                if (DEBUG_ENABLED) DEBUG_SERIAL.println("[BTN] Display ON");
            }
        } else {
            btnDisplayHolding = false;
            displayMgr.resetShutdown();
        }
        btnDisplayPrev = btnDisplay;
    }
    if (btnDisplayHolding) {
        uint32_t held = now - btnDisplayHoldStart;
        if (held >= 3000) {
            btnDisplayHolding = false;
            if (DEBUG_ENABLED) DEBUG_SERIAL.println("[BTN] PRG long press — sleep");
            powerOff();
        } else if (held >= 500) {
            // tampilkan countdown setiap 200ms agar tidak spam redraw
            static uint32_t lastCountdownDraw = 0;
            if (now - lastCountdownDraw >= 200) {
                lastCountdownDraw = now;
                uint32_t remainMs = 3000 - held;
                char msg[20];
                snprintf(msg, sizeof(msg), "Mati dalam %us...", (unsigned)(remainMs / 1000 + 1));
                uint8_t prog = (uint8_t)(100 - (remainMs * 100 / 2500));
                displayMgr.wakeup();
                displayMgr.showShutdown(msg, prog);
            }
        }
    }

    uint8_t btnPower = digitalRead(PIN_BTN_POWER);
    if (btnPower != btnPowerPrev && (now - btnPowerEdge) > 50) {
        btnPowerEdge = now;
        btnPowerPrev = btnPower;
    }

    if (displayMgr.isOn() &&
        (now - displayWakeTime) >= (uint32_t)DISPLAY_TIMEOUT_SEC * 1000UL) {
        displayMgr.off();
        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[DISP] Auto-off (idle timeout)");
    }
}

void setup() {
    esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
    bool fromDeepSleep = (wakeupCause == ESP_SLEEP_WAKEUP_TIMER ||
                          wakeupCause == ESP_SLEEP_WAKEUP_EXT0);

    if (DEBUG_ENABLED) {
        DEBUG_SERIAL.begin(DEBUG_BAUD);
        if (!fromDeepSleep) delay(2000);
        DEBUG_SERIAL.println("\n========================================");
        DEBUG_SERIAL.printf ("  Hauler GPS Tracker — Boot #%lu\n", bootCount + 1);
        if (fromDeepSleep)
            DEBUG_SERIAL.printf("[SYS] Wakeup from deep sleep (cause=%d)\n", (int)wakeupCause);
        DEBUG_SERIAL.println("========================================");
    }

    displayMgr.begin();
    displayMgr.showBoot("Tunggu ya, bos!", 0);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    // Cek GPIO 47 (VDD_SPI strap) — jika ditekan saat boot, tunggu dilepas
    // sebelum lanjut agar SPI peripheral tidak inisialisasi dalam state abnormal.
    pinMode(PIN_BTN_POWER, INPUT_PULLUP);
    if (digitalRead(PIN_BTN_POWER) == LOW) {
        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[SYS] PWR btn held at boot — waiting for release.");
        displayMgr.showError("BOOT WARNING", "Lepas tombol PWR");
        while (digitalRead(PIN_BTN_POWER) == LOW) delay(50);
        delay(100);
        displayMgr.showBoot("Tunggu ya, bos!", 0);
    }

    // Restore posisi terakhir dari RTC — agar distance calc valid setelah deep sleep
    lastSentLat = rtcLastSentLat;
    lastSentLon = rtcLastSentLon;

    displayMgr.showBoot("Nyalain GPS...", 10);
    gpsManager.begin();

    displayMgr.showBoot("Nyalain LoRa...", 30);
    while (!loraManager.begin()) {
        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[SYS] LoRa FAILED! Retry 10s.");
        displayMgr.showError("LoRa gagal!", "Coba lagi 10s...");
        delay(10000);
        displayMgr.showBoot("LoRa retry...", 30);
    }

    displayMgr.showBoot("Konek jaringan...", 60);
    while (!loraManager.join()) {
        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[SYS] Join FAILED! Retry 30s.");
        displayMgr.showError("Jaringan gagal!", "Coba lagi 30s...");
        delay(30000);
        displayMgr.showBoot("Konek jaringan...", 60);
    }
    if (DEBUG_ENABLED) DEBUG_SERIAL.println("[SYS] LoRaWAN joined!");

    // Warm start hanya valid jika sleep < 4 jam (ephemeris expire setelah ~2-4 jam)
    uint64_t sleepDurationMs = fromDeepSleep
        ? (esp_timer_get_time() / 1000ULL - rtcSleepEpochMs)
        : 0;
    bool gpsWarm = fromDeepSleep && (sleepDurationMs < 4ULL * 3600ULL * 1000ULL);
    uint32_t gpsTimeoutSec = gpsWarm ? GPS_WARMUP_SEC : GPS_TIMEOUT_SEC;
    if (DEBUG_ENABLED && fromDeepSleep)
        DEBUG_SERIAL.printf("[GPS] Sleep %llus — %s start\n",
                            sleepDurationMs / 1000ULL, gpsWarm ? "warm" : "cold");
    {
        uint32_t gpsStart   = millis();
        uint32_t lastUpdate = 0;

        while (!gpsManager.hasFix()) {
            gpsManager.update();

            uint32_t elapsedSec = (millis() - gpsStart) / 1000;

            if (millis() - lastUpdate >= 500) {
                lastUpdate = millis();
                uint8_t prog = (uint8_t)min(99UL, elapsedSec * 100UL / gpsTimeoutSec);
                char msg[28];
                snprintf(msg, sizeof(msg), gpsWarm ? "Cari sinyal %lus" : "Cari satelit %lus",
                         elapsedSec);
                displayMgr.showBoot(msg, prog);
                if (DEBUG_ENABLED)
                    DEBUG_SERIAL.printf("[GPS] Waiting fix... %lus\n", elapsedSec);
            }

            if (elapsedSec >= gpsTimeoutSec) {
                if (DEBUG_ENABLED) DEBUG_SERIAL.println("[GPS] Timeout — continuing without fix.");
                break;
            }
            delay(50);
        }

        if (gpsManager.hasFix()) {
            GpsData d = gpsManager.getCurrent();
            if (lastSentLat == 0.0 && lastSentLon == 0.0) {
                // Seed awal saat fresh boot agar distance tidak mulai dari (0,0)
                lastSentLat = d.latitude;
                lastSentLon = d.longitude;
            }
            if (DEBUG_ENABLED)
                DEBUG_SERIAL.printf("[GPS] Fix OK! Lat: %.6f, Lon: %.6f, Sats: %d, HDOP: %.1f\n",
                                    d.latitude, d.longitude, d.satellites, d.hdop);
        }
    }

    pinMode(PIN_BTN_DISPLAY, INPUT_PULLUP);
    pinMode(PIN_BTN_POWER,   INPUT_PULLUP);

    displayMgr.showBoot("Siap, bos!", 100);
    delay(fromDeepSleep ? 300 : 800);
    strncpy(txMsg, "Konek!", sizeof(txMsg));
    txStatus = TX_NONE;

    lastSuccessTx   = millis();
    displayWakeTime = millis();

    if (DEBUG_ENABLED) DEBUG_SERIAL.println("[SYS] Ready. Starting location tracking.");
}

void loop() {
    handleButtons();

    // Fix #2: watchdog unconditional di atas loop — menyala terlepas dari GPS fix
    // atau shouldSend. Jika tidak ada TX sukses selama MAX_NO_TX_MS → restart.
    if (lastSuccessTx > 0 && (millis() - lastSuccessTx) >= MAX_NO_TX_MS) {
        if (DEBUG_ENABLED)
            DEBUG_SERIAL.printf("[SYS] No successful TX for %lu min — restarting.\n",
                                MAX_NO_TX_MS / 60000UL);
        delay(200);
        ESP.restart();
    }

    gpsManager.update();

    // Fix #3: noFixStart di scope loop() agar bisa di-reset saat GPS kembali fix.
    // Bug sebelumnya: static di dalam blok if (!hasFix()) tidak pernah di-reset,
    // sehingga kali kedua GPS hilang, waitedSec menampilkan nilai yang salah (berjam-jam).
    static uint32_t noFixStart = 0;

    if (!gpsManager.hasFix()) {
        if (noFixStart == 0) noFixStart = millis();
        uint32_t waitedSec = (millis() - noFixStart) / 1000;
        if (displayMgr.isOn() && !btnDisplayHolding) {
            GpsData partial = gpsManager.getCurrent();
            displayMgr.showNoFix(waitedSec, partial.satellites, txStatus, txMsg);
        }
        delay(200);
        return;
    }

    noFixStart = 0;  // GPS kembali valid — reset timer untuk event no-fix berikutnya

    GpsData  current = gpsManager.getCurrent();
    uint32_t now     = millis();
    uint32_t elapsed = now - lastSendTime;

    double distMoved = 0.0;
    if (lastSendTime > 0) {
        distMoved = haversineMeters(lastSentLat, lastSentLon,
                                    current.latitude, current.longitude);
    }

    bool movingUI           = distMoved >= 5.0;
    bool movedSignificantly = distMoved >= MOVE_THRESHOLD_METERS;
    bool movingKeepalive    = movingUI  && elapsed >= INTERVAL_MOVING_SEC * 1000UL;
    bool idleHeartbeat      = elapsed   >= INTERVAL_IDLE_SEC   * 1000UL;
    bool firstSend          = lastSendTime == 0;

    bool shouldSend = firstSend || movedSignificantly || movingKeepalive || idleHeartbeat;

    uint32_t interval    = movingUI ? INTERVAL_MOVING_SEC * 1000UL : INTERVAL_IDLE_SEC * 1000UL;
    uint32_t nextSendSec = (elapsed < interval) ? (interval - elapsed) / 1000 : 0;

    if (shouldSend) {
        char reasonStr[24];
        if      (firstSend)          snprintf(reasonStr, sizeof(reasonStr), "First send");
        else if (movedSignificantly) snprintf(reasonStr, sizeof(reasonStr), "Moved %.0fm", distMoved);
        else if (movingKeepalive)    snprintf(reasonStr, sizeof(reasonStr), "Moving KA");
        else                         snprintf(reasonStr, sizeof(reasonStr), "Idle HB");

        if (DEBUG_ENABLED)
            DEBUG_SERIAL.printf("[SYS] Send trigger: %s | dist: %.1fm | elapsed: %lus\n",
                                reasonStr, distMoved, elapsed / 1000);

        txStatus = TX_SENDING;
        if (displayMgr.isOn() && !btnDisplayHolding) displayMgr.showTracking(current, 0, movingUI, TX_SENDING, reasonStr, 0);
        digitalWrite(PIN_LED, HIGH);

        bool sent = loraManager.sendGPS(current);

        digitalWrite(PIN_LED, LOW);
        lastSendTime = millis();

        lastSentLat = current.latitude;
        lastSentLon = current.longitude;
        // Simpan ke RTC agar distance calc tetap valid setelah deep sleep
        rtcLastSentLat = lastSentLat;
        rtcLastSentLon = lastSentLon;

        if (sent) {
            txStatus      = TX_OK;
            lastSuccessTx = millis();
            if (loraManager.hasSignal()) {
                snprintf(txMsg, sizeof(txMsg), "OK %ddBm S:%.1f",
                         (int)loraManager.getLastRSSI(), loraManager.getLastSNR());
            } else {
                strncpy(txMsg, "OK (no DL)", sizeof(txMsg));
            }
            if (DEBUG_ENABLED)
                DEBUG_SERIAL.printf("[SYS] Sent OK! Sats: %d, HDOP: %.1f\n",
                                    current.satellites, current.hdop);
        } else {
            txStatus = TX_FAIL;
            snprintf(txMsg, sizeof(txMsg), "TX FAIL #%d", loraManager.getConsecutiveFails());
            if (DEBUG_ENABLED) DEBUG_SERIAL.println("[SYS] Send FAILED.");

            if (loraManager.getConsecutiveFails() >= TX_FAIL_REJOIN) {
                if (DEBUG_ENABLED)
                    DEBUG_SERIAL.printf("[SYS] %d consecutive TX fails — starting recovery.\n",
                                        TX_FAIL_REJOIN);
                displayMgr.showError("Kirim gagal!", "Nyambung ulang...");
                loraManager.recover();
                // Fix #6: update lastSuccessTx setelah rejoin berhasil agar watchdog
                // tidak langsung fire jika idle failures melampaui MAX_NO_TX_MS
                lastSuccessTx = millis();
                strncpy(txMsg, "Nyambung lagi!", sizeof(txMsg));
                txStatus        = TX_NONE;
                displayWakeTime = millis();
                displayMgr.wakeup();
            }
        }

        nextSendSec = movingUI ? INTERVAL_MOVING_SEC : INTERVAL_IDLE_SEC;

        // Deep sleep dinonaktifkan — device selalu aktif agar tracking langsung
        // responsif saat hauler mulai bergerak.
    }

    if (displayMgr.isOn() && !btnDisplayHolding) {
        uint32_t timeOnMs    = now - displayWakeTime;
        uint32_t timeoutMs   = (uint32_t)DISPLAY_TIMEOUT_SEC * 1000UL;
        uint32_t offInSec    = (timeOnMs < timeoutMs) ? (timeoutMs - timeOnMs) / 1000 : 0;
        displayMgr.showTracking(current, nextSendSec, movingUI, txStatus, txMsg, offInSec);
    }
    delay(200);
}
