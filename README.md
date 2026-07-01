# ORION — Fleet Intelligence

GPS tracker untuk unit hauler tambang berbasis LoRaWAN, dibangun di atas **Heltec Wireless
Tracker** (ESP32-S3 + SX1262 + GPS onboard). Setiap perangkat lapangan disebut **Orion
Beacon (OBC)** dan diberi nama `OBC_000N` (mis. `OBC_0001`).

Firmware membaca posisi GPS, mengirimkannya via LoRaWAN (OTAA, AS923) ke gateway, dan
melaporkan lokasi/kecepatan/heading tiap unit secara berkala ke network server
(ChirpStack/TTN) untuk ditampilkan di dashboard peta real-time.

---

## Daftar Isi

- [Arsitektur](#arsitektur)
- [Hardware](#hardware)
- [Struktur Repository](#struktur-repository)
- [Payload Format](#payload-format)
- [Setup Development](#setup-development)
- [Konfigurasi](#konfigurasi)
- [Flashing Massal (Multi-Unit)](#flashing-massal-multi-unit)
- [Payload Decoder](#payload-decoder-chirpstacttn)
- [Troubleshooting](#troubleshooting)
- [Keamanan Data](#keamanan-data)
- [Roadmap](#roadmap)

---

## Arsitektur

```
Heltec Wireless Tracker (unit hauler)
  → baca GPS (lat, lon, speed, heading, hdop, satellites)
  → deteksi perubahan posisi (kirim jika bergerak > 20m, atau heartbeat berkala)
  → encode ke payload 13 bytes
  → kirim via LoRa AS923 (RadioLib, OTAA)
    → Gateway LoRaWAN (SenseCAP M2, SX1302)
      → forward ke Network Server (ChirpStack / TTN)
        → Payload decoder (JavaScript)
          → Webhook / integrasi ke dashboard
            → Peta real-time per unit
```

Interval pengiriman:
| Kondisi | Interval |
|---|---|
| Bergerak | 30 detik |
| Diam | 120 detik (heartbeat, deteksi breakdown) |

ESP32 tidak deep-sleep — device selalu aktif untuk responsivitas real-time. Sesi LoRaWAN
(hasil join) disimpan di RTC RAM sehingga tidak perlu re-join tiap reboot ringan.

---

## Hardware

| Komponen | Keterangan |
|---|---|
| **Heltec Wireless Tracker** | MCU utama — ESP32-S3, radio LoRa SX1262, GPS UC6580 onboard |
| **Gateway LoRaWAN** | SenseCAP M2 (SX1302), frekuensi AS923 (Indonesia) |
| **Antena LoRa eksternal** | via pigtail uFL |
| **Antena GPS aktif** | via pigtail uFL, port GPS |
| **Baterai** | CITYORK, konektor JST 1.25mm |
| **Display** | LCD ST7735 onboard, menampilkan nama unit + status join/GPS |

### Pin Mapping (`include/config.h`)

```
LoRa   NSS:8   DIO1:14  RST:12  BUSY:13
GPS    RX:33   TX:34    RST:35  PWR:3
Lain   VEXT:36 LED:18   BTN_DISPLAY:0  BTN_POWER:47  LCD_BL:21
```

> **Catatan:** GPIO 47 adalah strapping pin ESP32-S3 (mempengaruhi VDD_SPI saat boot).
> Firmware mendeteksi jika tombol ini tertekan saat boot dan meminta dilepas dulu.

---

## Struktur Repository

```
hauler-gps-tracker/
├── CLAUDE.md              # Project memory (konteks, status, keputusan teknis)
├── platformio.ini         # Environment dev/release, board, library deps
├── units.csv              # Data per unit: unit_id, unit_name, JoinEUI, DevEUI, AppKey
├── include/
│   ├── config.h           # Pin, interval, LoRaWAN params, timeout GPS
│   ├── unit_config.h       # (generated, gitignored) UNIT_ID/UNIT_NAME/keys per unit
│   ├── gps.h               # GpsManager — parsing NMEA via TinyGPSPlus
│   ├── lorawan.h           # LoRaWANManager — OTAA join, uplink via RadioLib
│   ├── display.h           # Render status ke LCD ST7735
│   ├── unit_config.h       # UNIT_NAME dsb. per unit (hasil flash_unit.py)
│   └── HT_st7735*.h        # Driver LCD Heltec
├── src/
│   ├── main.cpp            # Logika utama: setup, loop, state machine
│   └── HT_st7735*.cpp
├── scripts/
│   ├── flash_unit.py       # Flash satu/banyak unit dari units.csv
│   └── patch_radiolib.py   # Pre-build patch untuk RadioLib
└── docs/
    └── FLASHING_GUIDE.md   # Panduan lengkap flashing massal 40-60 unit
```

---

## Payload Format

13 bytes, dikirim di **FPort 1**:

| Byte | Isi | Format |
|---|---|---|
| 0–3 | Latitude | int32 signed, ×1e6, big-endian |
| 4–7 | Longitude | int32 signed, ×1e6, big-endian |
| 8 | Speed (km/h) | uint8 |
| 9 | HDOP ×10 | uint8 |
| 10 | Unit ID | uint8 (0–254; 255 = unassigned) |
| 11 | Heading | uint8, derajat/2 → decode: value×2 = 0–358°, resolusi 2° |
| 12 | Jumlah satelit | uint8 |

Payload lama 11-byte (tanpa heading/satellites) tetap kompatibel — decoder men-default-kan
kedua field tersebut ke 0 jika tidak ada.

---

## Setup Development

### Prasyarat
- [PlatformIO Core](https://platformio.org/install/cli) (`pio --version` untuk cek)
- Python 3.8+ (dipakai oleh `scripts/flash_unit.py`)
- Driver USB-to-serial ESP32-S3 (CP210x/CH340 sesuai board)

### Build & Upload (mode development, debug output ON)

```bash
pio run -e dev -t upload
pio device monitor
```

### Build & Upload (mode release, dipakai untuk produksi)

```bash
pio run -e release -t upload
```

Beda environment `dev` vs `release`: `dev` mengaktifkan `DEBUG_ENABLED=1` (log serial detail),
`release` mematikannya — build environment default yang dipakai `flash_unit.py`.

---

## Konfigurasi

Semua parameter tuning ada di `include/config.h`:

- **LoRaWAN**: data rate (DR5/SF7BW125), TX power, port
- **GPS**: baud rate, timeout cold-start (180s), minimal satelit untuk fix valid (4)
- **Interval kirim**: threshold jarak (20m), interval bergerak/diam
- **Recovery**: jumlah gagal TX sebelum rejoin, sebelum restart ESP, timeout maksimum tanpa TX sukses

Identitas per unit (`UNIT_ID`, `UNIT_NAME`, `JOINEUI`, `DEVEUI`, `APPKEY`) di-generate
otomatis ke `include/unit_config.h` (gitignored) oleh `scripts/flash_unit.py` — jangan
diisi manual kecuali untuk testing satu device.

---

## Flashing Massal (Multi-Unit)

Untuk deploy ke banyak unit hauler sekaligus, gunakan `units.csv` + `scripts/flash_unit.py`.
Panduan lengkap (step-by-step, termasuk alur fisik untuk 40–60 unit) ada di
[`docs/FLASHING_GUIDE.md`](docs/FLASHING_GUIDE.md). Ringkas:

```bash
# Cek status semua unit di units.csv
python scripts/flash_unit.py --list

# Flash satu unit
python scripts/flash_unit.py <UNIT_ID>

# Build saja tanpa upload (cek compile)
python scripts/flash_unit.py --build <UNIT_ID>

# Flash semua unit secara berurutan (interaktif, minta konfirmasi tiap device)
python scripts/flash_unit.py --all
```

Setiap unit butuh **DevEUI** dan **AppKey** unik (register dulu di ChirpStack/TTN),
sementara **JoinEUI** biasanya sama untuk semua unit dalam satu Application.

---

## Payload Decoder (ChirpStack/TTN)

Pasang sebagai custom JavaScript codec di Console → Application → Codec:

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

---

## Troubleshooting

| Gejala | Kemungkinan Penyebab | Solusi |
|---|---|---|
| GPS tidak fix | Indoor / cold start | Uji di area terbuka, tunggu 2–3 menit; pastikan antena di port GPS (bukan LoRa) |
| OTAA join gagal | Gateway belum connect, DevEUI/AppKey salah | Cek gateway "Connected" di console; DevEUI harus LSB, AppKey MSB sesuai portal |
| Layar stuck "LoRa gagal!" | SX1262 BUSY timeout / antena belum tersambung | Cek pigtail antena LoRa, restart device |
| Layar stuck "Cari satelit..." | GPS belum fix, cold start lama | Pindah ke area terbuka, tunggu hingga timeout 180s |
| Upload gagal / port tidak ketemu | Lebih dari 1 device USB tersambung | Cabut device lain, cek `pio device list` |

Lihat juga bagian Troubleshooting di [`docs/FLASHING_GUIDE.md`](docs/FLASHING_GUIDE.md)
untuk isu spesifik proses flashing massal.

---

## Keamanan Data

- `include/unit_config.h` (berisi key plaintext hasil generate) **sudah di-gitignore** —
  jangan force-add ke git.
- `units.csv` berisi DevEUI + AppKey **seluruh armada** — perlakukan sebagai credential
  store, jangan upload ke chat/repo publik tanpa enkripsi.
- Backup `units.csv` di lokasi aman (password manager / encrypted drive) untuk referensi
  re-flash di masa depan.

---

## Roadmap

- [x] Integrasi gateway ↔ network server
- [x] Register end device & payload decoder
- [x] Fix radio init & strapping pin boot issue
- [x] Tuning interval kirim (bergerak/diam)
- [x] Extend payload: heading + jumlah satelit (13 bytes)
- [x] Flashing massal via `scripts/flash_unit.py`
- [ ] Test jangkauan LoRa di area tambang
- [ ] Desain & cetak enclosure IP54+
- [ ] Deploy ke hauler pertama, monitoring 1–2 shift
- [ ] Web app: peta real-time, riwayat rute, analytics fleet

---

## Kontak

Owner project: Restu ([fangthewhite@gmail.com](mailto:fangthewhite@gmail.com))
