#pragma once

// ============================================================
// UNIT-SPECIFIC CONFIG — Di-generate otomatis oleh scripts/flash_unit.py
// JANGAN edit section ini secara manual untuk deployment massal.
// Untuk dev/test satu unit, edit include/unit_config.h langsung.
// ============================================================
#if __has_include("unit_config.h")
  #include "unit_config.h"
#endif

// Fallback jika unit_config.h tidak ada (fresh clone / dev manual)
#ifndef UNIT_ID
  #define UNIT_ID     0xFF        // 0xFF = unassigned
#endif
#ifndef UNIT_NAME
  #define UNIT_NAME   "UNSET"
#endif
#ifndef JOINEUI
  #define JOINEUI     0x0000000000000000ULL   // Isi dari portal ChirpStack/TTN
#endif
#ifndef DEVEUI
  #define DEVEUI      0x0000000000000000ULL   // Unik per unit
#endif
#ifndef APPKEY
  #define APPKEY      { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, \
                        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }
#endif

// ============================================================
// KONFIGURASI NETWORK SERVER
// ============================================================
// Firmware tidak perlu tahu alamat server — dikonfigurasi di gateway
// (SenseCAP M2 LuCI → LoRa → LoRaWAN Network Settings → Server Address).
//
// Produksi 40-60 unit: WAJIB ChirpStack (TTN fair use = 30 uplink/hari).
// TTN       : au1.cloud.thethings.network  port 1700
// ChirpStack: <ip-server>                  port 1700
//
// Format RadioLib 6.6.0:
//   JOINEUI — MSB, sama untuk semua unit dalam satu Application
//   DEVEUI  — MSB, unik per unit (dari label fisik board / portal)
//   APPKEY  — MSB, unik per unit (generate di portal)

// ============================================================
// PIN DEFINITION — Heltec Wireless Tracker (ESP32-S3)
// ============================================================
#define PIN_LORA_NSS    8
#define PIN_LORA_DIO1   14
#define PIN_LORA_RST    12
#define PIN_LORA_BUSY   13

#define PIN_GPS_RX      33
#define PIN_GPS_TX      34
#define PIN_GPS_RST     35
#define PIN_GPS_PWR     3
#define PIN_VEXT        36
#define PIN_LED         18

// ============================================================
// KONFIGURASI LORAWAN
// ============================================================
// DR5 = SF7BW125, ToA ~46ms — jauh di bawah AS923 dwell time 400ms
#define LORAWAN_DATARATE    5
#define LORAWAN_TX_POWER    22
#define LORAWAN_PORT        1

// ============================================================
// KONFIGURASI GPS
// ============================================================
#define GPS_BAUD_RATE           115200
#define GPS_TIMEOUT_SEC         180     // Cold start outdoor bisa 2-3 menit
#define GPS_WARMUP_SEC          30      // Warm start setelah deep sleep (UC6580 almanac tersimpan)
#define GPS_MIN_SATELLITES      4       // Minimal 4 sats untuk fix 3D yang valid

// ============================================================
// KONFIGURASI PENGIRIMAN — Location-change detection
// ============================================================
#define MOVE_THRESHOLD_METERS   20.0    // Kirim jika berpindah > 20 meter dari posisi terakhir
#define INTERVAL_MOVING_SEC     30      // Keepalive saat bergerak (meski < threshold)
#define INTERVAL_IDLE_SEC       120     // Heartbeat saat diam (2 menit — lebih aman untuk deteksi breakdown)

// ============================================================
// KONFIGURASI RECOVERY — Auto-recovery saat LoRa gagal
// ============================================================
#define TX_FAIL_REJOIN          5       // Gagal TX ke-5 → coba rejoin
#define TX_FAIL_RESTART         3       // Gagal rejoin ke-3 → restart ESP
#define MAX_NO_TX_MS            900000UL  // 15 menit tanpa TX sukses → restart

// ============================================================
// KONFIGURASI TOMBOL & DISPLAY
// ============================================================
#define PIN_LCD_BL          21   // Backlight LED ST7735 (HIGH=ON, LOW=OFF)
#define PIN_BTN_DISPLAY     0    // Tombol PRG onboard (active LOW, pull-up internal)
// NOTE: GPIO 47 pada ESP32-S3 mempengaruhi VDD_SPI voltage saat boot (HIGH=3.3V, LOW=1.8V).
// Jika ditekan saat power-on/reset bisa menyebabkan SPI peripheral berperilaku abnormal.
// Firmware mendeteksi kondisi ini di setup() dan meminta tombol dilepas sebelum lanjut.
#define PIN_BTN_POWER       47
#define DISPLAY_TIMEOUT_SEC 30   // Layar auto-mati setelah idle (detik)

// ============================================================
// KONFIGURASI DEBUG
// Set via build_flags: -D DEBUG_ENABLED=1  (development)
// Default false untuk production build.
// ============================================================
#define DEBUG_SERIAL    Serial
#define DEBUG_BAUD      115200
#ifndef DEBUG_ENABLED
  #define DEBUG_ENABLED false
#endif
