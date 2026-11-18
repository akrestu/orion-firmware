#include <Arduino.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <sys/time.h>
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
// Waktu (detik, jam RTC) saat masuk deep sleep. gettimeofday() di ESP32 dibackup
// oleh RTC timer sehingga tetap berjalan selama deep sleep — beda dengan
// esp_timer_get_time() yang reset ke 0 setiap boot.
RTC_DATA_ATTR int64_t  rtcSleepEpochSec = 0;

GpsManager     gpsManager;
LoRaWANManager loraManager;
HT_st7735      display;
DisplayManager displayMgr(display);

// Last transmitted position
static double   lastSentLat    = 0.0;
static double   lastSentLon    = 0.0;
static uint32_t lastSendTime   = 0;
static uint32_t lastSuccessTx  = 0;
static uint32_t lastStatusTime = 0;   // Heartbeat status saat no-fix

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

static int64_t rtcNowSec() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec;
}

static void wakeDisplay(uint32_t now) {
    displayWakeTime = now;
    if (!displayMgr.isOn()) {
        displayMgr.wakeup();
        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[BTN] Display ON");
    }
}

static void powerOff() {
    if (DEBUG_ENABLED) DEBUG_SERIAL.println("[SYS] Powering off...");
    displayMgr.wakeup();
    displayMgr.showBoot("Sampai jumpa, bos!", 100);

    // Beri tahu server bahwa unit dimatikan sengaja (bukan rusak/hilang sinyal).
    // Satu attempt saja tanpa retry — jangan tahan operator 5+ detik.
    // Lewati jika belum join / radio belum siap (mis. shutdown saat "LoRa gagal!"):
    // memanggil SX1262 yang belum di-init bisa hang menunggu pin BUSY.
    if (loraManager.isJoined()) {
        GpsData g = gpsManager.getCurrent();
        loraManager.sendStatus(g.satellites, STATUS_SHUTDOWN, false);
    }
    delay(500);

    rtcSleepEpochSec = rtcNowSec();
    if (loraManager.isRadioReady()) loraManager.sleep();
    digitalWrite(PIN_GPS_PWR, LOW);   // GPIO 3 = Vext GPS + TFT
    digitalWrite(PIN_LCD_BL, LOW);

    // Kunci level pin selama deep sleep — tanpa hold, pin output dilepas dan
    // bisa mengambang sehingga GPS/TFT tetap menyedot baterai.
    gpio_hold_en((gpio_num_t)PIN_GPS_PWR);
    gpio_hold_en((gpio_num_t)PIN_LCD_BL);
    gpio_deep_sleep_hold_en();

    // Tunggu tombol dilepas agar tidak langsung wake lagi
    while (digitalRead(PIN_BTN_DISPLAY) == LOW) delay(10);
    delay(50);

    // Deep sleep tanpa wakeup timer — hanya RST atau tombol PRG yang bisa bangunkan
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_DISPLAY);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_DISPLAY, 0);
    esp_deep_sleep_start();
}

// Tahan PRG → countdown → shutdown. Dipakai di loop() maupun saat menunggu di setup().
static void handleShutdownHold(uint32_t now) {
    uint8_t btnDisplay = digitalRead(PIN_BTN_DISPLAY);
    if (btnDisplay != btnDisplayPrev && (now - btnDisplayEdge) > 50) {
        btnDisplayEdge = now;
        if (btnDisplay == LOW) {
            btnDisplayHoldStart = now;
            btnDisplayHolding   = true;
            wakeDisplay(now);
        } else {
            if (btnDisplayHolding && (now - btnDisplayHoldStart) >= SHUTDOWN_WARN_MS) {
                displayMgr.resetShutdown();   // Batal — paksa redraw layar normal
            }
            btnDisplayHolding = false;
        }
        btnDisplayPrev = btnDisplay;
    }
    if (!btnDisplayHolding) return;

    uint32_t held = now - btnDisplayHoldStart;
    if (held >= SHUTDOWN_HOLD_MS) {
        btnDisplayHolding = false;
        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[BTN] PRG long press — power off");
        powerOff();
    } else if (held >= SHUTDOWN_WARN_MS) {
        // Countdown setiap 200ms agar tidak spam redraw
        static uint32_t lastCountdownDraw = 0;
        if (now - lastCountdownDraw >= 200) {
            lastCountdownDraw = now;
            uint32_t remainMs = SHUTDOWN_HOLD_MS - held;
            char msg[20];
            snprintf(msg, sizeof(msg), "Mati dalam %us...", (unsigned)((remainMs + 999) / 1000));
            uint8_t prog = (uint8_t)((held - SHUTDOWN_WARN_MS) * 100UL /
                                     (SHUTDOWN_HOLD_MS - SHUTDOWN_WARN_MS));
            displayMgr.wakeup();
            displayMgr.showShutdown(msg, prog);
        }
    }
}

static void handleButtons() {
    uint32_t now = millis();
    handleShutdownHold(now);

    // Tombol PWR (GPIO 47): tekan = nyalakan layar
    uint8_t btnPower = digitalRead(PIN_BTN_POWER);
    if (btnPower != btnPowerPrev && (now - btnPowerEdge) > 50) {
        btnPowerEdge = now;
        btnPowerPrev = btnPower;
        if (btnPower == LOW) wakeDisplay(now);
    }

    if (displayMgr.isOn() && !btnDisplayHolding &&
        (now - displayWakeTime) >= (uint32_t)DISPLAY_TIMEOUT_SEC * 1000UL) {
        displayMgr.off();
        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[DISP] Auto-off (idle timeout)");
    }
}

// delay() yang tetap merespons tahan-PRG untuk shutdown (dipakai saat retry di setup)
static void waitWithShutdown(uint32_t ms) {
    uint32_t start = millis();
    while (millis() - start < ms) {
        handleShutdownHold(millis());
        delay(20);
    }
}

void setup() {
    // Lepas hold pin dari deep sleep sebelumnya agar bisa dikendalikan lagi
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)PIN_GPS_PWR);
    gpio_hold_dis((gpio_num_t)PIN_LCD_BL);

    esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
    bool fromDeepSleep = (wakeupCause == ESP_SLEEP_WAKEUP_TIMER ||
                          wakeupCause == ESP_SLEEP_WAKEUP_EXT0);

    if (DEBUG_ENABLED) {
        DEBUG_SERIAL.begin(DEBUG_BAUD);
        if (!fromDeepSleep) delay(2000);
        DEBUG_SERIAL.println("\n========================================");
        DEBUG_SERIAL.printf ("  ORION Beacon %s — Boot #%u\n", UNIT_NAME, (unsigned)(bootCount + 1));
        if (fromDeepSleep)
            DEBUG_SERIAL.printf("[SYS] Wakeup from deep sleep (cause=%d)\n", (int)wakeupCause);
        DEBUG_SERIAL.println("========================================");
    }

    displayMgr.begin();
    displayMgr.showBoot("Tunggu ya, bos!", 0);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    pinMode(PIN_BTN_DISPLAY, INPUT_PULLUP);
    pinMode(PIN_BTN_POWER,   INPUT_PULLUP);

    // Cek GPIO 47 (VDD_SPI strap) — jika ditekan saat boot, tunggu dilepas
    // sebelum lanjut agar SPI peripheral tidak inisialisasi dalam state abnormal.
    if (digitalRead(PIN_BTN_POWER) == LOW) {
        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[SYS] PWR btn held at boot — waiting for release.");
        displayMgr.showError("BOOT WARNING", "Lepas tombol PWR");
        while (digitalRead(PIN_BTN_POWER) == LOW) delay(50);
        delay(100);
        displayMgr.showBoot("Tunggu ya, bos!", 0);
    }

    // PRG mungkin masih ditekan setelah dipakai untuk wake — jangan hitung sebagai hold baru
    btnDisplayPrev = digitalRead(PIN_BTN_DISPLAY);

    // Restore posisi terakhir dari RTC — agar distance calc valid setelah deep sleep
    lastSentLat = rtcLastSentLat;
    lastSentLon = rtcLastSentLon;

    displayMgr.showBoot("Nyalain GPS...", 10);
    gpsManager.begin();

    displayMgr.showBoot("Nyalain LoRa...", 30);
    while (!loraManager.begin()) {
        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[SYS] LoRa FAILED! Retry 10s.");
        displayMgr.showError("LoRa gagal!", "Coba lagi 10s...");
        waitWithShutdown(10000);
        displayMgr.showBoot("LoRa retry...", 30);
    }

    // Join dengan exponential backoff (30s → 60s → ... maks 300s) agar tidak
    // membanjiri airtime saat gateway/server down. Tahan PRG tetap bisa mematikan unit.
    displayMgr.showBoot("Konek jaringan...", 60);
    uint32_t joinWaitSec = JOIN_RETRY_MIN_SEC;
    while (!loraManager.join()) {
        if (DEBUG_ENABLED)
            DEBUG_SERIAL.printf("[SYS] Join FAILED! Retry %us.\n", (unsigned)joinWaitSec);
        char msg[24];
        snprintf(msg, sizeof(msg), "Coba lagi %us...", (unsigned)joinWaitSec);
        displayMgr.showError("Jaringan gagal!", msg);
        waitWithShutdown(joinWaitSec * 1000UL);
        joinWaitSec = min<uint32_t>(joinWaitSec * 2, JOIN_RETRY_MAX_SEC);
        displayMgr.showBoot("Konek jaringan...", 60);
    }
    if (DEBUG_ENABLED) DEBUG_SERIAL.println("[SYS] LoRaWAN joined!");

    // Warm start hanya valid jika sleep < 4 jam (ephemeris expire setelah ~2-4 jam)
    int64_t sleepDurationSec = (fromDeepSleep && rtcSleepEpochSec > 0)
        ? (rtcNowSec() - rtcSleepEpochSec)
        : -1;
    bool gpsWarm = sleepDurationSec >= 0 && sleepDurationSec < 4LL * 3600LL;
    uint32_t gpsTimeoutSec = gpsWarm ? GPS_WARMUP_SEC : GPS_TIMEOUT_SEC;
    if (DEBUG_ENABLED && fromDeepSleep)
        DEBUG_SERIAL.printf("[GPS] Sleep %llds — %s start\n",
                            (long long)sleepDurationSec, gpsWarm ? "warm" : "cold");
    {
        uint32_t gpsStart   = millis();
        uint32_t lastUpdate = 0;

        while (!gpsManager.hasFix()) {
            gpsManager.update();
            handleShutdownHold(millis());

            uint32_t elapsedSec = (millis() - gpsStart) / 1000;

            if (millis() - lastUpdate >= 500 && !btnDisplayHolding) {
                lastUpdate = millis();
                uint8_t prog = (uint8_t)min<uint32_t>(99, elapsedSec * 100UL / gpsTimeoutSec);
                char msg[28];
                snprintf(msg, sizeof(msg), gpsWarm ? "Cari sinyal %us" : "Cari satelit %us",
                         (unsigned)elapsedSec);
                displayMgr.showBoot(msg, prog);
                if (DEBUG_ENABLED)
                    DEBUG_SERIAL.printf("[GPS] Waiting fix... %us\n", (unsigned)elapsedSec);
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
                DEBUG_SERIAL.printf("[GPS] Fix OK! Lat: %.6f, Lon: %.6f, Sats: %u, HDOP: %.1f\n",
                                    d.latitude, d.longitude, (unsigned)d.satellites, d.hdop);
        }
    }

    displayMgr.showBoot("Siap, bos!", 100);
    delay(fromDeepSleep ? 300 : 800);
    strncpy(txMsg, "Konek!", sizeof(txMsg));
    txStatus = TX_NONE;

    lastSuccessTx   = millis();
    displayWakeTime = millis();

    if (DEBUG_ENABLED) DEBUG_SERIAL.println("[SYS] Ready. Starting location tracking.");
}

// Update tampilan + jalankan recovery setelah setiap TX (GPS maupun status)
static void afterTransmit(bool sent) {
    if (sent) {
        txStatus      = TX_OK;
        lastSuccessTx = millis();
        if (loraManager.hasSignal()) {
            snprintf(txMsg, sizeof(txMsg), "OK %ddBm S:%.1f",
                     (int)loraManager.getLastRSSI(), loraManager.getLastSNR());
        } else {
            strncpy(txMsg, "OK (no DL)", sizeof(txMsg));
        }
        return;
    }

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

void loop() {
    handleButtons();

    // Watchdog: tidak ada TX sukses selama MAX_NO_TX_MS → restart.
    // Heartbeat status tetap dikirim saat no-fix, jadi ini murni mengukur radio.
    if (lastSuccessTx > 0 && (millis() - lastSuccessTx) >= MAX_NO_TX_MS) {
        if (DEBUG_ENABLED)
            DEBUG_SERIAL.printf("[SYS] No successful TX for %u min — restarting.\n",
                                (unsigned)(MAX_NO_TX_MS / 60000UL));
        delay(200);
        ESP.restart();
    }

    gpsManager.update();

    static uint32_t noFixStart = 0;

    if (!gpsManager.hasFix()) {
        uint32_t now = millis();
        if (noFixStart == 0) noFixStart = now;
        uint32_t waitedSec = (now - noFixStart) / 1000;
        GpsData partial = gpsManager.getCurrent();

        // Heartbeat no-fix: server tahu unit hidup walau GPS hilang (pit/tebing).
        // Tunggu NO_FIX_GRACE_SEC dulu: setelah TX (~2-3 s blocking) data GPS basi sesaat
        // (age > 2 s) — tanpa grace ini setiap uplink GPS diikuti status no_fix palsu.
        bool lostLongEnough = waitedSec >= NO_FIX_GRACE_SEC;
        if (lostLongEnough &&
            (lastStatusTime == 0 || (now - lastStatusTime) >= INTERVAL_IDLE_SEC * 1000UL)) {
            lastStatusTime = now;
            txStatus = TX_SENDING;
            digitalWrite(PIN_LED, HIGH);
            bool sent = loraManager.sendStatus(partial.satellites, STATUS_NO_FIX);
            digitalWrite(PIN_LED, LOW);
            afterTransmit(sent);
        }

        if (displayMgr.isOn() && !btnDisplayHolding) {
            displayMgr.showNoFix(waitedSec, partial.satellites, txStatus, txMsg, partial.hdop);
        }
        delay(200);
        return;
    }

    // GPS kembali valid — reset timer tampilan no-fix. lastStatusTime sengaja TIDAK
    // di-reset: kalau fix naik-turun (HDOP di sekitar batas), heartbeat tetap maks 1x/120 s.
    noFixStart = 0;

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

    // Jeda minimum antar-uplink: tanpa ini hauler 40 km/h kirim tiap ~2.5 s (trigger 20 m)
    // → simulasi 50 unit: ~19-58% paket tabrakan. Dengan 10 s: ~5% (8 channel).
    bool minGapOk   = elapsed >= MIN_SEND_INTERVAL_SEC * 1000UL;
    bool shouldSend = firstSend ||
                      (minGapOk && (movedSignificantly || movingKeepalive || idleHeartbeat));

    uint32_t interval    = movingUI ? INTERVAL_MOVING_SEC * 1000UL : INTERVAL_IDLE_SEC * 1000UL;
    uint32_t nextSendSec = (elapsed < interval) ? (interval - elapsed) / 1000 : 0;

    if (shouldSend) {
        char reasonStr[24];
        if      (firstSend)          snprintf(reasonStr, sizeof(reasonStr), "First send");
        else if (movedSignificantly) snprintf(reasonStr, sizeof(reasonStr), "Moved %.0fm", distMoved);
        else if (movingKeepalive)    snprintf(reasonStr, sizeof(reasonStr), "Moving KA");
        else                         snprintf(reasonStr, sizeof(reasonStr), "Idle HB");

        if (DEBUG_ENABLED)
            DEBUG_SERIAL.printf("[SYS] Send trigger: %s | dist: %.1fm | elapsed: %us\n",
                                reasonStr, distMoved, (unsigned)(elapsed / 1000));

        txStatus = TX_SENDING;
        if (displayMgr.isOn() && !btnDisplayHolding) displayMgr.showTracking(current, 0, movingUI, TX_SENDING, reasonStr, 0);
        digitalWrite(PIN_LED, HIGH);

        bool sent = loraManager.sendGPS(current);

        digitalWrite(PIN_LED, LOW);
        lastSendTime = millis();
        if (lastSendTime == 0) lastSendTime = 1;   // 0 dipakai sebagai sentinel "belum pernah kirim"

        lastSentLat    = current.latitude;
        lastSentLon    = current.longitude;
        rtcLastSentLat = lastSentLat;
        rtcLastSentLon = lastSentLon;

        if (sent && DEBUG_ENABLED)
            DEBUG_SERIAL.printf("[SYS] Sent OK! Sats: %u, HDOP: %.1f\n",
                                (unsigned)current.satellites, current.hdop);
        afterTransmit(sent);

        nextSendSec = movingUI ? INTERVAL_MOVING_SEC : INTERVAL_IDLE_SEC;
    }

    if (displayMgr.isOn() && !btnDisplayHolding) {
        uint32_t timeOnMs    = now - displayWakeTime;
        uint32_t timeoutMs   = (uint32_t)DISPLAY_TIMEOUT_SEC * 1000UL;
        uint32_t offInSec    = (timeOnMs < timeoutMs) ? (timeoutMs - timeOnMs) / 1000 : 0;
        displayMgr.showTracking(current, nextSendSec, movingUI, txStatus, txMsg, offInSec);
    }
    delay(200);
}
