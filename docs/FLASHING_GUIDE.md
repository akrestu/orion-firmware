# Panduan Flashing Massal — Hauler GPS Tracker

Panduan ini untuk proses flash firmware ke banyak unit Heltec Wireless Tracker
sekaligus, menggunakan `scripts/flash_unit.py` + `units.csv`.

---

## 1. Prasyarat

### 1.1 Software
- [PlatformIO Core](https://platformio.org/install/cli) terinstal dan bisa dipanggil sebagai `pio` dari terminal.
  Cek: `pio --version`
- Python 3.8+ (untuk menjalankan `scripts/flash_unit.py`).
- Driver USB-to-Serial untuk ESP32-S3 (CP210x / CH340, tergantung board) sudah terinstal.

### 1.2 Data per unit (dari ChirpStack)
Setiap unit butuh **DevEUI** dan **AppKey** unik, plus **JoinEUI** (sama untuk semua unit
dalam satu Application). Register setiap device di ChirpStack Console terlebih dahulu:

1. ChirpStack Console → Applications → `Genesis-Eagle-Eye` → Devices → **Add device**
2. Isi nama device (samakan dengan `unit_name`, mis. `HAUL-02`), pilih Device Profile yang sesuai (AS923, OTAA).
3. ChirpStack akan generate **DevEUI** — catat.
4. Di tab **Keys (OTAA)**, generate atau isi **Application Key (AppKey)** — catat.
5. **JoinEUI** ambil dari Application settings (biasanya sama untuk semua device dalam Application).
6. Ulangi untuk setiap unit yang akan di-deploy.

> Tip: ChirpStack punya fitur **Import devices** dari CSV/JSON jika mau register banyak
> device sekaligus — cek halaman Devices → Import di Console, supaya tidak generate manual satu-satu.

### 1.3 Hardware
- Satu unit Heltec Wireless Tracker per sesi flash (kabel USB-C).
- **Hanya boleh ada SATU device yang terhubung ke USB** saat flashing — kalau ada beberapa,
  PlatformIO bisa salah pilih port dan flash ke device yang salah.

---

## 2. Isi `units.csv`

File `units.csv` di root project adalah sumber data untuk semua unit:

```csv
unit_id,unit_name,joineui,deveui,appkey
1,HAUL-01,0000000000000000,70B3D57ED0078319,BB40568BFCBC06C44C0A322EBD98D12E
2,HAUL-02,0000000000000000,<DEVEUI_DARI_CHIRPSTACK>,<APPKEY_DARI_CHIRPSTACK>
3,HAUL-03,...
```

Aturan format:
| Kolom | Format | Catatan |
|---|---|---|
| `unit_id` | integer | Harus unik, dipakai sebagai `UNIT_ID` di payload (byte 10) |
| `unit_name` | string pendek | Tampil di display LCD unit, mis. `HAUL-02` |
| `joineui` | 16 hex char | Boleh diisi nol jika ChirpStack Application memang pakai all-zero JoinEUI — **konfirmasi dulu di ChirpStack** |
| `deveui` | 16 hex char | Dari ChirpStack, case-insensitive (script auto-uppercase) |
| `appkey` | 32 hex char | Dari ChirpStack, case-insensitive |

`units.csv` **tidak perlu di-gitignore** (tidak berisi key — hanya referensi; key sensitif
hanya pernah ditulis ke `include/unit_config.h` yang sudah ada di `.gitignore`).
Namun karena berisi DevEUI/AppKey asli per unit, **perlakukan sebagai data sensitif** —
jangan commit ke repo publik. Cek dulu apakah `.gitignore` perlu menambahkan `units.csv`.

Validasi cepat sebelum mulai flash:

```bash
python scripts/flash_unit.py --list
```

Output akan menampilkan status tiap unit — `READY` atau `INCOMPLETE (...)` jika DevEUI/AppKey
masih kosong (nol).

---

## 3. Proses Flash — Satu Unit

Untuk flash satu device (mis. testing unit baru atau re-flash):

```bash
python scripts/flash_unit.py <UNIT_ID>
```

Contoh:
```bash
python scripts/flash_unit.py 2
```

Script ini otomatis akan:
1. Validasi data unit (DevEUI/AppKey tidak boleh nol).
2. Generate `include/unit_config.h` untuk unit tersebut.
3. Build firmware environment `release` (`pio run -e release`) — debug output OFF.
4. **Berhenti dan minta konfirmasi** — pastikan hanya unit yang benar tersambung ke USB.
5. Upload (`pio run -e release -t upload`).

Mode tambahan:
```bash
# Build saja tanpa upload — cek dulu apakah compile sukses
python scripts/flash_unit.py --build 2

# Lihat semua unit dan statusnya
python scripts/flash_unit.py --list
```

---

## 4. Proses Flash — Banyak Unit Sekaligus

```bash
python scripts/flash_unit.py --all
```

Alur:
1. Menampilkan daftar semua unit di `units.csv` beserta statusnya.
2. Minta konfirmasi `yes` sebelum mulai.
3. Loop tiap unit secara berurutan (sorted by `unit_id`):
   - Unit dengan data `INCOMPLETE` otomatis **di-skip** (tidak menghentikan proses).
   - Setiap unit minta konfirmasi manual sebelum upload — **ganti device fisik yang
     terhubung ke USB di sini**, baca nama unit di prompt dengan teliti.
4. Di akhir, ringkasan: berapa sukses, berapa gagal, dan unit ID mana yang gagal.

### Alur kerja fisik yang disarankan (untuk 40-60 unit)

1. Siapkan label / sticker fisik untuk tiap unit (`HAUL-01`, `HAUL-02`, dst) — tempel
   ke board **sebelum** flashing supaya tidak tertukar.
2. Sambungkan unit pertama via USB.
3. Jalankan `python scripts/flash_unit.py --all`.
4. Saat prompt menunjukkan nama unit yang akan di-flash, **cocokkan dengan label fisik**
   di tangan sebelum tekan Enter.
5. Setelah upload sukses, lepas unit, sambungkan unit berikutnya, lanjut ke prompt berikutnya.
6. Setelah semua selesai, cek ringkasan akhir — re-run untuk unit yang gagal saja
   (`python scripts/flash_unit.py <UNIT_ID>`).

---

## 5. Verifikasi Setelah Flash

Untuk **setiap** unit setelah di-flash, lakukan minimal:

1. **Boot check** — nyalakan unit (atau biarkan otomatis nyala setelah upload), lihat
   layar LCD: harus muncul `UNIT_NAME` yang benar di header, lalu progress GPS/LoRa join.
2. **Join check** — tunggu sampai layar masuk mode tracking (bukan stuck di "Konek jaringan...").
   Jika gagal terus, cek DevEUI/AppKey di ChirpStack Console cocok dengan yang di-flash.
3. **ChirpStack live frame check** — buka Console → Application → Device → **LIVE LORAWAN FRAMES**,
   pastikan uplink masuk dan ter-decode (lat/lon masuk akal, bukan 0,0 atau NaN).
4. **Unit ID check** — pastikan payload byte 10 (`unit_id`) sesuai dengan device fisik
   (penting supaya data tidak tertukar antar hauler di dashboard nanti).

Opsional tapi disarankan untuk verifikasi massal: simpan checklist sederhana
(spreadsheet) dengan kolom `unit_id | flashed | joined | gps_fix_ok | dipasang_ke_hauler`.

---

## 6. Troubleshooting

| Gejala | Kemungkinan Penyebab | Solusi |
|---|---|---|
| `[ERROR] Unit ID tidak ada di units.csv` | Salah ketik unit_id, atau belum ditambah ke CSV | Cek `--list`, perbaiki CSV |
| `[SKIP] Unit tidak lengkap` | DevEUI/AppKey masih nol | Register device di ChirpStack, isi CSV |
| Upload gagal / port tidak ketemu | Lebih dari satu device USB tersambung, atau driver belum terinstal | Cabut device lain, cek Device Manager / `pio device list` |
| Device flash sukses tapi tidak join | DevEUI/AppKey salah ketik di CSV, atau device belum di-Activate di ChirpStack | Cocokkan ulang dengan ChirpStack Console |
| Layar stuck "LoRa gagal!" | Hardware fault (SX1262 BUSY stuck) atau antena LoRa belum tersambung | Cek pigtail antena, restart device |
| Layar stuck "Cari satelit..." lama | GPS belum fix (cold start bisa 2-3 menit outdoor) | Pindah ke area terbuka, tunggu sampai timeout 180s |

---

## 7. Catatan Keamanan Data

- `include/unit_config.h` (hasil generate, berisi key plaintext) **sudah di-gitignore** —
  jangan paksa commit dengan `git add -f`.
- `units.csv` berisi DevEUI + AppKey **seluruh armada** dalam satu file — perlakukan
  seperti credential store. Jangan upload ke chat publik, repo publik, atau cloud storage
  tanpa enkripsi.
- Setelah selesai flash semua unit, pertimbangkan backup `units.csv` di lokasi aman
  (mis. password manager / encrypted drive) untuk referensi re-flash di masa depan
  (board rusak, ganti device, dll).
