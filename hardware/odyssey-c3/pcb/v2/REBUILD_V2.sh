#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
OUT="${1:-./reconstructed}"
mkdir -p "$OUT"
cat release_archive_b64_parts/part-*.b64 | base64 --decode > "$OUT/release_v2.tar.xz"
EXPECTED="977ab5b496b4d3516fac28ff9aabff564b22e8e0353db705f57c4b37c126fe01"
ACTUAL="$(sha256sum "$OUT/release_v2.tar.xz" | awk '{print $1}')"
if [ "$ACTUAL" != "$EXPECTED" ]; then echo "Archive hash mismatch" >&2; exit 1; fi
tar -xJf "$OUT/release_v2.tar.xz" -C "$OUT"
(cd "$OUT/gerbers" && zip -q -9 ../Synap_Odyssey_C3_Carrier_PCB_V2_GERBERS.zip ./*)
echo "Reconstructed Eagle source and Gerber/drill files in $OUT"
echo "Use the ZIP inside that folder for CAM review; check README.md release holds before fabrication."
