#!/bin/bash
# Pull this phone's firmware off it over adb into fw-wifi/ at the top of the repository.
#
# The vendor's, so it stays out of git. Wi-Fi here is a WCN7850 "Kiwi" (PCIe + MHI): its firmware
# and its configuration are what a driver for it needs; see devices/corvette/README.md.
set -e
cd "$(dirname "$0")/../.."
OUT=fw-wifi
mkdir -p "$OUT"
adb pull /vendor/firmware_mnt/image/kiwi/ "$OUT/kiwi/"
adb pull /vendor/firmware/wlan/qca_cld/kiwi_v2/WCNSS_qcom_cfg.ini "$OUT/"
ls -l "$OUT"
