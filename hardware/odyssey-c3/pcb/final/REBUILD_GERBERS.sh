#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

ARCHIVE="Synap_Odyssey_C3_Carrier_RevK_FINAL_Gerbers.tar.xz"
EXPECTED="cf004abe2c5e4d5f530c0d971979131f8a9623b473943a529b0706343b552191"

cat   gerber_archive_parts/part-00   gerber_archive_parts/part-01   gerber_archive_parts/part-02   gerber_archive_parts/part-03   > "$ARCHIVE"

printf '%s  %s\n' "$EXPECTED" "$ARCHIVE" | sha256sum -c -

rm -rf gerbers
tar -xJf "$ARCHIVE"

echo "Rev K Gerber/drill files extracted to ./gerbers/"
echo "If your fab portal requires ZIP: zip -r Synap_Odyssey_C3_Carrier_RevK_FINAL_Gerbers.zip gerbers/"
