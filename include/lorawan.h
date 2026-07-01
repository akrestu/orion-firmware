#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include <Preferences.h>
#include "config.h"
#include "gps.h"

// ============================================================
// LoRaWAN Session — RTC RAM untuk deep sleep, NVS untuk hard reset
// RadioLib 6.6.0: session & nonces disimpan terpisah
//
// Fix #9: definisi ada di main.cpp (satu TU), header hanya extern.
// Jika lorawan.h di-include di lebih dari satu .cpp, tidak terjadi ODR violation.
// ============================================================
extern uint8_t  lwNonces[];
extern uint8_t  lwSession[];
extern bool     lwNoncesValid;
extern uint32_t bootCount;

// NVS keys
static constexpr char NVS_NS[]     = "lora";
static constexpr char NVS_NONCES[] = "nonces";
static constexpr char NVS_VALID[]  = "valid";

// ============================================================
// LoRaWAN Manager
// ============================================================
class LoRaWANManager {
public:
    LoRaWANManager()
        : _radio(new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY)),
          _node(&_radio, &AS923, 0) {}

    bool begin() {
        bootCount++;
        if (!_initRadio()) return false;
        _configNode();
        return true;
    }

    // Join ke TTN — restore session dari RTC atau NVS jika tersedia.
    bool join() {
        int state;

        // Coba restore dari RTC RAM (survive deep sleep, tidak perlu kirim Join Request)
        if (lwNoncesValid && bootCount > 1) {
            if (DEBUG_ENABLED) DEBUG_SERIAL.print("[LoRa] Restoring session from RTC... ");
            _node.setBufferNonces(lwNonces);
            _node.setBufferSession(lwSession);
            state = _node.activateOTAA();
            if (state == RADIOLIB_LORAWAN_SESSION_RESTORED) {
                if (DEBUG_ENABLED) DEBUG_SERIAL.println("OK");
                _node.setDatarate(LORAWAN_DATARATE);
                return true;
            }
            if (DEBUG_ENABLED) DEBUG_SERIAL.printf("failed (code %d)\n", state);
        }

        // Fallback: muat nonces dari NVS agar DevNonce tidak mundur ke 0 dan ditolak TTN
        Preferences prefs;
        prefs.begin(NVS_NS, true);
        bool nvsValid = prefs.getBool(NVS_VALID, false);
        if (nvsValid) {
            if (DEBUG_ENABLED) DEBUG_SERIAL.print("[LoRa] Loading nonces from NVS... ");
            prefs.getBytes(NVS_NONCES, lwNonces, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
            prefs.end();
            _node.setBufferNonces(lwNonces);
            if (DEBUG_ENABLED) DEBUG_SERIAL.println("OK");
        } else {
            prefs.end();
            if (DEBUG_ENABLED) DEBUG_SERIAL.println("[LoRa] No NVS nonces, fresh OTAA join...");
        }

        if (DEBUG_ENABLED) DEBUG_SERIAL.print("[LoRa] OTAA Joining... ");
        state = _node.activateOTAA();

        // Fix #1: simpan nonces ke NVS setelah setiap attempt (sukses MAUPUN gagal).
        // RadioLib increment DevNonce di setiap activateOTAA() — kalau tidak disimpan,
        // retry berikutnya mengirim DevNonce lama yang sama dan TTN menolak sebagai replay.
        memcpy(lwNonces, _node.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
        prefs.begin(NVS_NS, false);
        prefs.putBytes(NVS_NONCES, lwNonces, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
        prefs.putBool(NVS_VALID, true);
        prefs.end();

        if (state != RADIOLIB_LORAWAN_NEW_SESSION) {
            if (DEBUG_ENABLED) DEBUG_SERIAL.printf("FAILED! Code: %d\n", state);
            return false;
        }

        if (DEBUG_ENABLED) DEBUG_SERIAL.println("JOINED!");
        _node.setDatarate(LORAWAN_DATARATE);

        // Simpan session penuh ke RTC RAM setelah join sukses
        memcpy(lwSession, _node.getBufferSession(), RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
        lwNoncesValid = true;

        return true;
    }

    // Encode GPS data ke payload 13 bytes dan kirim via LoRaWAN.
    // Byte 0-3 : Latitude   (int32 x1e6, big endian)
    // Byte 4-7 : Longitude  (int32 x1e6, big endian)
    // Byte 8   : Speed km/h (uint8, 0-255)
    // Byte 9   : HDOP x10   (uint8, 0-255)
    // Byte 10  : Unit ID    (uint8, 0-254; 255=unassigned)
    // Byte 11  : Heading    (uint8, degrees/2 → 0-179 = 0-358°, resolusi 2°)
    // Byte 12  : Satellites (uint8)
    bool sendGPS(const GpsData &gps) {
        uint8_t payload[13];
        int32_t latInt = (int32_t)(gps.latitude  * 1e6);
        int32_t lonInt = (int32_t)(gps.longitude * 1e6);

        payload[0]  = (latInt >> 24) & 0xFF;
        payload[1]  = (latInt >> 16) & 0xFF;
        payload[2]  = (latInt >>  8) & 0xFF;
        payload[3]  = (latInt      ) & 0xFF;
        payload[4]  = (lonInt >> 24) & 0xFF;
        payload[5]  = (lonInt >> 16) & 0xFF;
        payload[6]  = (lonInt >>  8) & 0xFF;
        payload[7]  = (lonInt      ) & 0xFF;
        payload[8]  = (uint8_t)constrain((int)gps.speed_kmh, 0, 255);
        payload[9]  = (uint8_t)constrain((int)(gps.hdop * 10), 0, 255);
        payload[10] = (uint8_t)UNIT_ID;
        payload[11] = (uint8_t)(gps.course_deg / 2.0f);
        payload[12] = gps.satellites;

        if (DEBUG_ENABLED)
            DEBUG_SERIAL.printf("[LoRa] Sending payload (%d bytes) unit=%d hdg=%.0f° sats=%d... ",
                                sizeof(payload), UNIT_ID, gps.course_deg, gps.satellites);

        int state = _node.sendReceive(payload, sizeof(payload), LORAWAN_PORT);
        bool sendOk = (state == RADIOLIB_ERR_NONE || state == RADIOLIB_LORAWAN_NO_DOWNLINK);

        if (!sendOk) {
            if (DEBUG_ENABLED) DEBUG_SERIAL.printf("retry (err %d)... ", state);
            // Tunggu 5s agar RX1+RX2 window dari attempt pertama selesai sebelum retry
            delay(5000);
            state = _node.sendReceive(payload, sizeof(payload), LORAWAN_PORT);
            sendOk = (state == RADIOLIB_ERR_NONE || state == RADIOLIB_LORAWAN_NO_DOWNLINK);
        }

        if (sendOk) {
            _consecutiveFails = 0;

            // Fix #7: RSSI/SNR hanya valid saat ada downlink yang diterima.
            // Saat NO_DOWNLINK, register SX1262 menyimpan nilai stale dari paket sebelumnya.
            if (state == RADIOLIB_ERR_NONE) {
                _lastRSSI  = _radio.getRSSI();
                _lastSNR   = _radio.getSNR();
                _hasSignal = true;
                if (DEBUG_ENABLED)
                    DEBUG_SERIAL.printf("OK! RSSI: %.1f dBm, SNR: %.1f dB\n", _lastRSSI, _lastSNR);
            } else {
                _hasSignal = false;
                if (DEBUG_ENABLED) DEBUG_SERIAL.println("OK (no downlink)");
            }

            // Re-lock DR setelah TX sukses — ADR downlink mungkin sudah diproses sebelum diblokir
            _node.setDatarate(LORAWAN_DATARATE);

            // Fix #4: hanya update RTC RAM (survive deep sleep).
            // NVS tidak perlu ditulis setiap uplink — DevNonce hanya berubah saat join,
            // dan RTC sudah cukup untuk menghindari re-join di antara deep sleep cycles.
            // Menulis NVS setiap 30s menyebabkan flash wear ribuan kali per hari.
            memcpy(lwNonces,  _node.getBufferNonces(),  RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
            memcpy(lwSession, _node.getBufferSession(), RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
            return true;
        } else {
            _consecutiveFails++;
            if (DEBUG_ENABLED) {
                DEBUG_SERIAL.printf("FAILED! Code: %d", state);
                if      (state == -1101) DEBUG_SERIAL.print(" (ACK timeout)");
                else if (state == -1100) DEBUG_SERIAL.print(" (not joined)");
                else if (state == -1112) DEBUG_SERIAL.print(" (no downlink)");
                else if (state == -1113) DEBUG_SERIAL.print(" (nonces discarded)");
                else if (state == -1114) DEBUG_SERIAL.print(" (dwell time exceeded)");
                else if (state == -706)  DEBUG_SERIAL.print(" (channel busy)");
                DEBUG_SERIAL.printf(" [fail #%d]\n", _consecutiveFails);
            }
            return false;
        }
    }

    // Recovery: reset hardware radio, lalu rejoin TTN.
    // Jika semua attempt gagal → ESP.restart().
    void recover() {
        if (DEBUG_ENABLED)
            DEBUG_SERIAL.printf("[LoRa] Recovery: %d consecutive fails\n", _consecutiveFails);

        // Reset SX1262 untuk membersihkan HW state yang mungkin corrupt
        // (stuck BUSY, SPI desync setelah power glitch), lalu config ulang node keys.
        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[LoRa] Resetting radio hardware...");
        _initRadio();
        _configNode();

        // Invalidasi RTC session agar join() lakukan OTAA penuh
        lwNoncesValid = false;

        for (int attempt = 1; attempt <= TX_FAIL_RESTART; attempt++) {
            if (DEBUG_ENABLED)
                DEBUG_SERIAL.printf("[LoRa] Rejoin attempt %d/%d...\n", attempt, TX_FAIL_RESTART);
            if (join()) {
                _consecutiveFails = 0;
                if (DEBUG_ENABLED) DEBUG_SERIAL.println("[LoRa] Rejoin OK!");
                return;
            }
            // Fix #5: jangan delay setelah attempt terakhir — langsung restart
            if (attempt < TX_FAIL_RESTART) delay(10000);
        }

        if (DEBUG_ENABLED) DEBUG_SERIAL.println("[LoRa] All rejoin attempts failed — restarting.");
        delay(200);
        ESP.restart();
    }

    void sleep() { _radio.sleep(); }

    int   getConsecutiveFails() const { return _consecutiveFails; }
    void  resetFails()                { _consecutiveFails = 0; }
    float getLastRSSI()         const { return _lastRSSI; }
    float getLastSNR()          const { return _lastSNR;  }
    bool  hasSignal()           const { return _hasSignal; }

private:
    SX1262       _radio;
    LoRaWANNode  _node;
    float        _lastRSSI        = 0.0f;
    float        _lastSNR         = 0.0f;
    bool         _hasSignal       = false;
    int          _consecutiveFails = 0;

    // Hanya inisialisasi hardware SX1262 — dipanggil di begin() dan recover().
    bool _initRadio() {
        // Hard-reset SX1262 untuk clear stuck BUSY state akibat power glitch
        pinMode(PIN_LORA_RST, OUTPUT);
        digitalWrite(PIN_LORA_RST, LOW);
        delay(10);
        digitalWrite(PIN_LORA_RST, HIGH);
        delay(10);

        // Jika BUSY masih HIGH >1s setelah reset → hardware fault, abort
        pinMode(PIN_LORA_BUSY, INPUT);
        uint32_t busyStart = millis();
        while (digitalRead(PIN_LORA_BUSY) == HIGH) {
            if (millis() - busyStart > 1000) {
                if (DEBUG_ENABLED) DEBUG_SERIAL.println("[LoRa] BUSY stuck after reset — hardware fault");
                return false;
            }
            delay(10);
        }

        if (DEBUG_ENABLED) DEBUG_SERIAL.print("[LoRa] Initializing SX1262... ");
        int state = _radio.begin();
        if (state != RADIOLIB_ERR_NONE) {
            if (DEBUG_ENABLED) DEBUG_SERIAL.printf("FAILED! Code: %d\n", state);
            return false;
        }
        if (DEBUG_ENABLED) DEBUG_SERIAL.println("OK");
        return true;
    }

    // Set OTAA keys + LoRaWAN policy — dipanggil setelah _initRadio().
    // Dipisah agar recover() bisa reset HW dulu tanpa mengubah urutan key setup.
    void _configNode() {
        uint8_t appKey[] = APPKEY;
        _node.beginOTAA(JOINEUI, DEVEUI, appKey, appKey);

        // Duty cycle enforcement dilakukan di server (TTN fair use policy).
        // Disable internal check agar tidak blokir uplink (-1108).
        _node.setDutyCycle(false);
        _node.setDwellTime(false, 0);

        // Disable ADR — TTN kirim LinkADRReq yang bisa nurunin DR ke SF12,
        // menyebabkan airtime >400ms dan TX gagal -1114 (AS923 dwell time limit).
        _node.setADR(false);
    }
};
