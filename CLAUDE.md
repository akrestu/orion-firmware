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
  → encode ke payload 10 bytes
  → kirim via LoRa AS923 (RadioLib, OTAA)
    → SenseCAP M2 Gateway
      → forward ke TTN (au1.cloud.thethings.network)
        → Payload decoder (JavaScript di TTN)
          → Webhook ke Datacake
            → Dashboard peta real-time
```

---

## Status Konfigurasi Saat Ini

### TTN (The Things Network)
- **Cluster**: `au1.cloud.thethings.network` (Asia Pacific, AS923)
- **Gateway ID**: `genesis25`
- **Gateway Name**: Sensecap
- **Gateway EUI**: `2CF7F11375000072`
- **Frequency Plan**: Asia 923-925 MHz (AS923)
- **Fix yang perlu dilakukan**: Ubah Server Address di SenseCAP LuCI dari `eu1` ke `au1`

### SenseCAP M2 LuCI
- Menu: **LoRa → LoRaWAN Network Settings**
- Mode: Packet Forwarder
- Port: 1700 (sudah benar)
- Server Address: harus `au1.cloud.thethings.network`

### End Device di TTN (Heltec)
- **Belum dibuat** — perlu register end device di TTN Application
- Setelah register, isi ke `include/config.h`: DevEUI, JoinEUI, AppKey

---

## Struktur Project

```
hauler-gps-tracker/
├── CLAUDE.md              ← file ini (project memory)
├── platformio.ini         ← konfigurasi PlatformIO (board, library)
├── include/
│   ├── config.h           ← SEMUA setting: TTN keys, pin, interval kirim
│   ├── gps.h              ← GpsManager class
│   └── lorawan.h          ← LoRaWANManager class
└── src/
    └── main.cpp           ← logika utama
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

### Power Management
- ESP32 deep sleep dinonaktifkan — device selalu aktif untuk responsivitas real-time
- Session LoRaWAN disimpan di RTC RAM (tidak perlu re-join setiap wake up)
- Interval: bergerak = 30 detik, diam = 120 detik (2 menit)

### Pin Heltec Wireless Tracker
```
LoRa NSS:8  DIO1:14  RST:12  BUSY:13
GPS RX:33   TX:34    PWR:3
VEXT:36     LED:18
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
- Pastikan gateway Connected (hijau) di TTN dulu
- DevEUI harus LSB (byte dibalik dari tampilan TTN)
- AppKey harus MSB (langsung dari TTN)

### Fitur lanjutan yang bisa dikembangkan
- Geofencing: alert jika hauler keluar area tambang
- Downlink command: ubah interval dari server
- Multi-unit: flash firmware sama ke Heltec lain, register sebagai end device baru
