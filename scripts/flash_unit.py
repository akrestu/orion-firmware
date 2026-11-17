#!/usr/bin/env python3
"""
flash_unit.py — Flash firmware ke unit hauler GPS tracker.

Usage:
  python scripts/flash_unit.py <UNIT_ID>          # Flash satu unit
  python scripts/flash_unit.py --list             # Tampilkan semua unit di units.csv
  python scripts/flash_unit.py --build <UNIT_ID>  # Build saja, tidak flash
  python scripts/flash_unit.py --all              # Flash semua unit satu per satu (interaktif)

Data unit dibaca dari units.csv di root project.
Script ini:
  1. Baca data unit dari units.csv
  2. Generate include/unit_config.h untuk unit tersebut
  3. Build firmware (pio run -e release)
  4. Upload ke device yang terhubung (pio run -e release -t upload)

Pastikan hanya satu device yang terhubung ke USB saat flash.
"""

import sys
import os
import csv
import shutil
import subprocess
import argparse
import re

HEX_RE  = re.compile(r"[0-9A-F]+")
NAME_RE = re.compile(r"[A-Za-z0-9_\-]{1,16}")

# Path relatif dari root project
SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
UNITS_CSV    = os.path.join(PROJECT_ROOT, "units.csv")
UNIT_CONFIG  = os.path.join(PROJECT_ROOT, "include", "unit_config.h")


def find_pio():
    """Cari executable pio: PATH dulu, fallback ke lokasi default instalasi PlatformIO."""
    found = shutil.which("pio") or shutil.which("platformio")
    if found:
        return found
    fallback = os.path.join(os.path.expanduser("~"), ".platformio", "penv", "Scripts", "platformio.exe")
    if os.path.exists(fallback):
        return fallback
    print("[ERROR] Executable 'pio' tidak ditemukan di PATH maupun di lokasi default PlatformIO.")
    print("        Install PlatformIO Core atau tambahkan ke PATH.")
    sys.exit(1)


PIO_EXE = find_pio()

UNIT_CONFIG_TEMPLATE = """\
// ============================================================
// UNIT CONFIG — Di-generate oleh scripts/flash_unit.py
// JANGAN commit file ini ke git (sudah ada di .gitignore).
// ============================================================

#define UNIT_ID     {unit_id}
#define UNIT_NAME   "{unit_name}"

// OTAA Keys dari portal ChirpStack / TTN
// Format: MSB (big-endian), sesuai RadioLib 6.6.0
#define JOINEUI     0x{joineui}ULL
#define DEVEUI      0x{deveui}ULL
#define APPKEY      {{{appkey_bytes}}}
"""


def load_units():
    units = {}
    if not os.path.exists(UNITS_CSV):
        print(f"[ERROR] File tidak ditemukan: {UNITS_CSV}")
        sys.exit(1)
    with open(UNITS_CSV, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            uid = int(row["unit_id"])
            units[uid] = {
                "unit_id":  uid,
                "unit_name": row["unit_name"].strip(),
                # Tanpa zfill: key yang kepotong harus ketahuan, bukan dipad nol diam-diam
                "joineui":  row["joineui"].strip().upper(),
                "deveui":   row["deveui"].strip().upper(),
                "appkey":   row["appkey"].strip().upper(),
            }
    return units


def _check_hex(name, value, length, secret=False):
    # AppKey rahasia — jangan pernah dicetak ke console/log
    shown = "" if secret else f" ('{value}')"
    if len(value) != length:
        return f"{name} harus {length} hex chars, got {len(value)}{shown}"
    if not HEX_RE.fullmatch(value):
        return f"{name} mengandung karakter non-hex{shown}"
    return None


def format_appkey_bytes(appkey_hex):
    """Ubah 32-char hex string ke C array bytes: 0xBB, 0x40, ..."""
    if len(appkey_hex) != 32:
        raise ValueError(f"AppKey harus 32 hex chars, got {len(appkey_hex)}")
    pairs = [f"0x{appkey_hex[i:i+2]}" for i in range(0, 32, 2)]
    # Dua baris, 8 byte per baris — perlu backslash continuation karena #define multi-baris
    line1 = ", ".join(pairs[:8])
    line2 = ", ".join(pairs[8:])
    return f"{line1}, \\\n                      {line2}"


def validate_unit(unit):
    """Validasi format & isi data unit sebelum generate config."""
    errors = []
    if not 0 <= unit["unit_id"] <= 254:
        errors.append(f"unit_id {unit['unit_id']} di luar rentang 0-254 (255 = unassigned)")
    if not NAME_RE.fullmatch(unit["unit_name"]):
        errors.append(f"unit_name '{unit['unit_name']}' tidak valid (huruf/angka/_/-, maks 16)")
    for name, length in (("joineui", 16), ("deveui", 16), ("appkey", 32)):
        err = _check_hex(name.upper(), unit[name], length, secret=(name == "appkey"))
        if err:
            errors.append(err)
    if errors:
        return errors
    if unit["deveui"] == "0" * 16:
        errors.append("DEVEUI masih 0 — isi dari portal ChirpStack/TTN")
    if unit["appkey"] == "0" * 32:
        errors.append("APPKEY masih 0 — isi dari portal ChirpStack/TTN")
    return errors


def generate_unit_config(unit):
    appkey_bytes = format_appkey_bytes(unit["appkey"])
    content = UNIT_CONFIG_TEMPLATE.format(
        unit_id=unit["unit_id"],
        unit_name=unit["unit_name"],
        joineui=unit["joineui"],
        deveui=unit["deveui"],
        appkey_bytes=appkey_bytes,
    )
    with open(UNIT_CONFIG, "w") as f:
        f.write(content)
    print(f"[OK] Generated {UNIT_CONFIG}")


def run_pio(args, cwd=None):
    cmd = [PIO_EXE] + args
    print(f"[RUN] {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=cwd or PROJECT_ROOT)
    return result.returncode == 0


def list_units(units):
    print(f"\n{'ID':>4}  {'Name':<10}  {'DevEUI':<16}  {'Status'}")
    print("-" * 64)
    for uid, u in sorted(units.items()):
        errors = validate_unit(u)
        status = "READY" if not errors else f"INCOMPLETE ({errors[0]})"
        print(f"{uid:>4}  {u['unit_name']:<10}  {u['deveui']:<16}  {status}")
    print()


def flash_unit(unit, build_only=False):
    print(f"\n{'='*60}")
    print(f"  Unit: {unit['unit_id']} — {unit['unit_name']}")
    print(f"  DevEUI : {unit['deveui']}")
    print(f"  JoinEUI: {unit['joineui']}")
    print(f"{'='*60}")

    errors = validate_unit(unit)
    if errors:
        for e in errors:
            print(f"[ERROR] {e}")
        print("[SKIP] Unit tidak lengkap, skip flash.")
        return False

    generate_unit_config(unit)
    try:
        return _build_and_flash(unit, build_only)
    finally:
        # Hapus agar key unit ini tidak ikut ter-flash ke device lain lewat
        # 'pio run -t upload' manual berikutnya (risiko DevEUI duplikat).
        if os.path.exists(UNIT_CONFIG):
            os.remove(UNIT_CONFIG)
            print(f"[CLEAN] {UNIT_CONFIG} dihapus.")


def _build_and_flash(unit, build_only):
    print("\n[BUILD] Compiling firmware...")
    if not run_pio(["run", "-e", "release"]):
        print("[FAIL] Build gagal!")
        return False

    if build_only:
        print("[OK] Build sukses (--build mode, tidak flash).")
        return True

    input(f"\nPastikan HANYA unit {unit['unit_id']} ({unit['unit_name']}) yang terhubung via USB.\nTekan Enter untuk mulai flash, atau Ctrl+C untuk batal...")

    print("\n[FLASH] Uploading firmware...")
    if not run_pio(["run", "-e", "release", "-t", "upload"]):
        print("[FAIL] Upload gagal!")
        return False

    print(f"\n[OK] Unit {unit['unit_id']} ({unit['unit_name']}) berhasil di-flash!")
    return True


def main():
    parser = argparse.ArgumentParser(description="Flash firmware GPS tracker ke unit hauler.")
    parser.add_argument("unit_id", nargs="?", type=int, help="Unit ID yang akan di-flash")
    parser.add_argument("--list",  action="store_true", help="Tampilkan semua unit")
    parser.add_argument("--all",   action="store_true", help="Flash semua unit secara berurutan")
    parser.add_argument("--build", action="store_true", help="Build saja, tidak upload ke device")
    args = parser.parse_args()

    units = load_units()

    if args.list or (args.unit_id is None and not args.all):
        list_units(units)
        if not args.list:
            parser.print_help()
        return

    if args.all:
        list_units(units)
        confirm = input("Flash SEMUA unit? (yes/no): ").strip().lower()
        if confirm != "yes":
            print("Dibatalkan.")
            return
        success = []
        failed  = []
        for uid, unit in sorted(units.items()):
            ok = flash_unit(unit, build_only=args.build)
            (success if ok else failed).append(uid)
        print(f"\n{'='*60}")
        print(f"Selesai: {len(success)} sukses, {len(failed)} gagal")
        if failed:
            print(f"Gagal: unit ID {failed}")
        return

    if args.unit_id not in units:
        print(f"[ERROR] Unit ID {args.unit_id} tidak ada di units.csv")
        list_units(units)
        sys.exit(1)

    flash_unit(units[args.unit_id], build_only=args.build)


if __name__ == "__main__":
    main()
