"""
PlatformIO pre-script: patch RadioLib AS923 coding rate CR 4/7 → CR 4/5.

RadioLib 6.6.0 mendefinisikan semua data rate AS923 dengan CR 4/7, tapi
standar LoRaWAN dan ChirpStack mengharapkan CR 4/5. Tanpa patch ini,
ChirpStack menolak semua uplink dengan error "Unknown data-rate: CR 4/7".

Script ini dijalankan otomatis sebelum setiap build via extra_scripts di
platformio.ini. Idempoten — aman dijalankan berulang kali.
"""

import os
Import("env")  # noqa: F821 — PlatformIO injects this

BANDS_FILE = os.path.join(
    env.subst("$PROJECT_DIR"),  # noqa: F821
    ".pio", "libdeps", env.subst("$PIOENV"),  # noqa: F821
    "RadioLib", "src", "protocols", "LoRaWAN", "LoRaWANBands.cpp"
)

OLD = "RADIOLIB_LORAWAN_DATA_RATE_CR_4_7"
NEW = "RADIOLIB_LORAWAN_DATA_RATE_CR_4_5"


def patch():
    if not os.path.isfile(BANDS_FILE):
        print(f"[patch_radiolib] File not found (library not installed yet): {BANDS_FILE}")
        return

    with open(BANDS_FILE, "r") as f:
        content = f.read()

    if OLD not in content:
        print("[patch_radiolib] Already patched — skipping.")
        return

    patched = content.replace(OLD, NEW)
    with open(BANDS_FILE, "w") as f:
        f.write(patched)

    count = content.count(OLD)
    print(f"[patch_radiolib] Patched {count} occurrence(s): CR_4_7 → CR_4_5 in LoRaWANBands.cpp")


patch()
