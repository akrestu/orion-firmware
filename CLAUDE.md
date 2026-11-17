# Hauler GPS Tracker — Project Memory for Claude Code

## Konteks Proyek
GPS tracker untuk unit hauler di tambang menggunakan LoRaWAN.
Owner: Restu (fangthewhite@gmail.com)

## Branding
- **Software/platform**: ORION — Fleet Intelligence
- **Hardware device**: Orion Beacon, disingkat **OBC**
- **Konvensi unit name**: `OBC_000N` (4 digit, zero-padded sesuai `unit_id`), contoh: `OBC_0001`, `OBC_0002`
- Nama unit ini tampil di header layar device (`UNIT_NAME` di `include/unit_config.h`, ditulis ke LCD via `include/display.h`) dan didaftarkan di `units.csv` (dibaca oleh `scripts/flash_unit.py` saat generate config per unit).

---

## Hardware yang Digunakan

| Komponen | Keterangan |
|---|---|
| **Heltec Wireless Tracker** | MCU utama (ESP32-S3 + SX1262 LoRa + UC6580 GPS onboard) |
| **SenseCAP M2 LoRaWAN Gateway** | SX1302, frekuensi AS923, untuk Indonesia |
| **LoRa External Antenna** | Disambung via kabel pigtail uFL ke board |
| **GPS Active Antenna** | Disambung via kabel pigtail uFL ke port GPS |
| **CITYORK Battery** | Dihubungkan ke konektor JST 1.25mm di board |

---

## Arsitektur Sistem

```
Heltec Wireless Tracker
  → baca GPS (lat, lon, speed, hdop)
  → encode ke payload 13 bytes (FPort 1) / status 3 bytes (FPort 2)
  → kirim via LoRa AS923 (RadioLib, OTAA)
    → SenseCAP M2 Gateway (Packet Forwarder, port 1700)
      → ChirpStack (network server, codec JS di bawah)
        → Dashboard peta real-time
```

---

## Status Konfigurasi Saat Ini

### Gateway
- **SenseCAP M2**, EUI `2CF7F11375000072`
- LuCI: **LoRa → LoRaWAN Network Settings** → Packet Forwarder, port 1700, Server Address = server ChirpStack
- Produksi 40-60 unit WAJIB ChirpStack (TTN fair use = 30 uplink/hari)
- **Belum diputuskan:** frequency plan AS923-1 (923-925 MHz, dipakai firmware) vs AS923-2
  (920-923 MHz, plan Indonesia di LoRaWAN Regional Parameters), dan TX power 22 dBm vs
  batas EIRP. Firmware, ChirpStack, dan gateway harus diubah bersamaan.

### End Device (ChirpStack)
- Unit terdaftar di `units.csv` (tidak di-commit). Flash per unit: `python scripts/flash_unit.py <unit_id>`
  → generate `include/unit_config.h`, build release, upload, lalu hapus `unit_config.h`
- Di Windows jalankan dengan `PYTHONIOENCODING=utf-8` jika output di-redirect ke file
  (PlatformIO crash `UnicodeEncodeError` di console cp1252)

---

## Struktur Project

```
orion-beacon/
├── CLAUDE.md              ← file ini (project memory)
├── platformio.ini         ← env dev (debug serial) & release; RadioLib di-pin 6.6.0
├── units.csv              ← key OTAA seluruh armada (gitignored)
├── include/
│   ├── config.h           ← pin, interval, threshold, port; include unit_config.h
│   ├── unit_config.h      ← key OTAA per unit (generated, gitignored)
│   ├── gps.h              ← GpsManager (TinyGPSPlus, filter sats & HDOP)
│   ├── lorawan.h          ← LoRaWANManager (join, sendGPS, sendStatus, recover)
│   ├── display.h          ← DisplayManager (layar ST7735)
│   └── HT_st7735*.h, icons.h
├── scripts/
│   ├── flash_unit.py      ← generate config + build + flash per unit
│   └── patch_radiolib.py  ← pre-build: patch AS923 CR 4/7 → 4/5 untuk ChirpStack
└── src/
    ├── main.cpp           ← logika utama (tracking, tombol, shutdown)
    └── HT_st7735*.cpp     ← driver layar
```

---

## Detail Teknis Penting

### Payload Format (13 bytes, FPort 1)
```
Byte 0-3  : Latitude   (int32, signed, x1e6, big endian)
Byte 4-7  : Longitude  (int32, signed, x1e6, big endian)
Byte 8    : Speed km/h (uint8)
Byte 9    : HDOP x10   (uint8)
Byte 10   : Unit ID    (uint8, 0-254; 255=unassigned)
Byte 11   : Heading    (uint8, degrees/2 → decode: value×2 = 0-358°, resolusi 2°)
Byte 12   : Satellites (uint8)
```

### ChirpStack Payload Decoder (pasang di Console → Application → Codec → Custom JavaScript)
```javascript
function decodeUplink(input) {
  var b = input.bytes;
  if (input.fPort === 2) {
    // Status: [unit_id, satellites, status_code]  0=no fix, 1=shutdown
    var codes = ["no_fix", "shutdown"];
    return { data: { unit_id: b[0], satellites: b[1],
                     status: codes[b[2]] || ("unknown_" + b[2]) } };
  }
  var lat = ((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
  if (lat > 0x7FFFFFFF) lat -= 0x100000000;
  lat = lat / 1e6;
  var lon = ((b[4] << 24) | (b[5] << 16) | (b[6] << 8) | b[7]);
  if (lon > 0x7FFFFFFF) lon -= 0x100000000;
  lon = lon / 1e6;
  return {
    data: {
      latitude: lat, longitude: lon,
      speed_kmh: b[8], hdop: b[9] / 10.0,
      unit_id: b[10],
      heading_deg: b.length > 11 ? b[11] * 2 : 0,
      satellites:  b.length > 12 ? b[12] : 0,
      location: { latitude: lat, longitude: lon }
    }
  };
}
```
Decoder backward-compatible: payload 11-byte lama tetap decode dengan benar (heading & satellites default 0).

### Status Uplink (3 bytes, FPort 2)
`[unit_id, satellites, status_code]` — `0` = hidup tapi GPS tidak fix (dikirim tiap 120 s selama no-fix,
HDOP > 5 juga dianggap no-fix), `1` = dimatikan operator. Watchdog 15 menit hanya mengukur radio.

### Power Management & Tombol
- Saat beroperasi tidak ada deep sleep — device selalu aktif untuk responsivitas real-time
- Session LoRaWAN disimpan di RTC RAM setelah SETIAP uplink (sukses/gagal) agar FCnt tidak mundur
- Interval: bergerak = 30 detik, diam = 120 detik (2 menit)
- **PRG (GPIO 0):** tekan = layar ON; **tahan 7 detik = matikan unit** (countdown mulai di detik ke-2,
  kirim status shutdown, lalu deep sleep dengan GPIO 3 & backlight di-hold LOW); tekan saat mati = hidupkan
- **PWR (GPIO 47):** tekan = layar ON saat unit hidup. Bukan RTC GPIO → tidak bisa membangunkan unit
- RST: restart / hidupkan unit

### Pin Heltec Wireless Tracker
```
LoRa NSS:8  DIO1:14  RST:12  BUSY:13
GPS RX:33   TX:34    Vext(GPS+TFT):3
LCD BL:21   LED:18   BTN PRG:0  BTN PWR:47
```

---

## Library (platformio.ini)
- `jgromes/RadioLib` — LoRaWAN OTAA, AS923
- `mikalhart/TinyGPSPlus` — parse NMEA GPS
- `heltec-dev/Heltec ESP32 Dev-Boards` — board support

---

## TODO / Next Steps

- [x] Sambungkan gateway ke ChirpStack
- [x] Register end device di ChirpStack
- [x] Fix radio init hang (BUSY pin timeout)
- [x] Fix GPIO 47 strapping pin — boot warning di display
- [x] Tuning idle interval 300→120 detik
- [x] Extend payload: tambah heading + satellites (13 bytes)
- [x] Pasang payload decoder baru di ChirpStack Console → Application → Codec
- [x] Upload firmware via PlatformIO (`pio run -e release -t upload`)
- [x] Verify decoded payload di ChirpStack live frame log — OBC_0001 join OK, payload terdecode benar (lat/lon/sats/hdop)
- [ ] Test range di area tambang
- [ ] Desain & cetak enclosure IP54+ (ST PLA, Bambulab A1 Mini)
- [ ] Deploy ke hauler pertama, monitor 1-2 shift
- [ ] **Fase berikutnya:** Build web app (real-time map, route history, analytics)

---

## Catatan Troubleshooting

### GPS tidak fix
- Test di area terbuka, cold start bisa 2-3 menit
- Pastikan antena GPS di port uFL yang benar (bukan port LoRa)

### OTAA join gagal
- Pastikan gateway online di ChirpStack dulu
- DevEUI, JoinEUI, AppKey semuanya MSB (RadioLib 6.6.0) — salin langsung dari portal
- Error `-1116` (NO_DOWNLINK) saat join = join accept tidak diterima: cek gateway online,
  JoinRequest muncul di LoRaWAN frames, antena LoRa terpasang, jarak ke gateway

### Fitur lanjutan yang bisa dikembangkan
- Geofencing: alert jika hauler keluar area tambang
- Downlink command: ubah interval dari server
- Multi-unit: flash firmware sama ke Heltec lain, register sebagai end device baru
