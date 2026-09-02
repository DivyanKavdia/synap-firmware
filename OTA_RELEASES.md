# One-click releases — baseline 1.0.0

Every push to `main` runs `.github/workflows/firmware.yml`. Pull requests compile and validate but cannot publish. Only a successful build publishes a GitHub Release and updates `ota-releases/latest.json` atomically with its application binary. The PWA checks that feed while open and connected; it never installs without approval.

Target: ESP32-S3FH4R2 / SuperMini, 4 MB QIO flash, 2 MB QSPI PSRAM, GPIO48 RGB, Arduino-ESP32 3.3.5, **default 4 MB partition scheme** (two 0x140000-byte application slots). No bootloader, partition table or merged image is transferred by OTA. Do not change partition layout remotely. The source still defaults to generated test audio; real I2S microphone mode is unchanged.

Public baseline version remains **1.0.0**. Release builds use `1000 + github.run_number` (16-bit, maximum 65535); source-only manual builds default to 504. Releases include a standalone `.ino` with the release's exact build number. Never reset the workflow counter or rename/recreate its identity without choosing a higher build-number base. The PWA compares build numbers, not just the public version. To change the public version, update both the source identity and `tools/release.cjs`, and the PWA version when appropriate.

Feed: https://raw.githubusercontent.com/DivyanKavdia/synap-firmware/ota-releases/latest.json

The separate distribution branch serves public binaries with cross-origin browser access, avoiding GitHub release-download redirects. The manifest contains exact target, build, application length, SHA-256 and source commit. Binary paths are content-addressed and never overwritten. Failed builds leave the previous feed intact. GitHub Actions must be enabled and the publish job must have `contents: write`; branch protection may require repository-owner setup. No cross-repository token or owner key is needed by CI. Protect main and review firmware/workflow changes: GitHub account/repository integrity is the publisher trust boundary, not firmware signing.

## First-time setup

Install OTA-enabled firmware over USB once if not already installed. In USB Serial Monitor (115200 baud), send `OTAKEY` followed by newline. Enter the per-device key in the PWA and explicitly choose to remember authorization on a trusted browser. It is stored as a non-extractable HMAC CryptoKey in IndexedDB, not plaintext/localStorage. Someone with access to that browser/origin can still authorize updates; use Forget authorization on shared devices. Clearing site data or a factory reset requires enrollment again. Existing baseline build 503 has no hardware identity characteristic: the PWA asks you to confirm this exact board before its first automated update.

After enrollment, keep the powered pendant connected over Bluetooth, stop/save recording, tap **Update available**, then approve. No USB or physical button is needed for authenticated protocol-2 updates. Keep the app foregrounded; this is not a background update while the PWA is closed. The PWA verifies the manifest hash/image identity, the pendant verifies HMAC and flashed SHA-256, and success is reported only after reconnecting and reading the expected build and identity. Interrupted transfers restart from zero; uncertain commits require reconnect verification, not an automatic second flash. Stock Arduino bootloaders may not enable rollback: do not assume recovery from firmware that cannot boot.

## Checks

`node --test tests/release.cjs` tests release-image validation. CI additionally compiles the actual sketch and rejects incompatible, oversized or misidentified images. Hardware power-loss/reboot behavior still needs a real pendant test before a broad release.
