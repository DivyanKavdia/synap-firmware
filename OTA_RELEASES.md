# One-click releases — baseline 1.0.0

Every push to `main` runs `.github/workflows/firmware.yml`. Pull requests compile and validate but cannot publish. Only a successful build publishes a GitHub Release and updates `ota-releases/latest.json` atomically with its application binary. The PWA checks that feed while open and connected; it never installs without approval.

Target: ESP32-S3FH4R2 / SuperMini, 4 MB QIO flash, 2 MB QSPI PSRAM, GPIO48 RGB, Arduino-ESP32 3.3.5, **default 4 MB partition scheme** (two 0x140000-byte application slots). No bootloader, partition table or merged image is transferred by OTA. Do not change partition layout remotely. The source still defaults to generated test audio; real I2S microphone mode is unchanged.

Public baseline version remains **1.0.0**. Release builds use `1000 + github.run_number` (16-bit, maximum 65535); source-only manual builds default to 506. Releases include a standalone `.ino` with the release's exact build number. Never reset the workflow counter or rename/recreate its identity without choosing a higher build-number base. The PWA compares build numbers, not just the public version. To change the public version, update both the source identity and `tools/release.cjs`, and the PWA version when appropriate.

Feed: https://raw.githubusercontent.com/DivyanKavdia/synap-firmware/ota-releases/latest.json

The separate distribution branch serves public binaries with cross-origin browser access, avoiding GitHub release-download redirects. The manifest contains exact target, build, application length, SHA-256 and source commit. Binary paths are content-addressed and never overwritten. Failed builds leave the previous feed intact. GitHub Actions must be enabled and the publish job must have `contents: write`; branch protection may require repository-owner setup. No cross-repository token or owner key is needed by CI. Protect main and review firmware/workflow changes: GitHub account/repository integrity is the publisher trust boundary, not firmware signing.

## Device-ID provisioning and updates

Ship new devices with protocol-3 device-ID firmware. An existing protocol-1/2 prototype needs one developer USB flash of the new sketch; the PWA cannot remove the lock in already-installed firmware. Customers do not retrieve keys, configure signing or press device buttons.

Connect, stop/save recording, then approve the GitHub update. The PWA targets the retained permanent device ID. BEGIN carries that ID and the device rejects a mismatch before flash. It verifies flashed SHA-256, image headers and compatibility markers. Success is reported only after reconnecting and reading the same device ID plus expected build identity. Pending results are keyed by permanent ID, not a browser's temporary Bluetooth handle.

This is unauthenticated OTA. Public device IDs and SHA-256 are not owner or publisher authentication. A nearby BLE client can initiate an update, including malicious code with matching headers/markers/hash. No signing, secret, Secure Boot or exclusive ownership protection is added. This tradeoff does not constitute production security qualification.

An interrupted uncommitted transfer remains resumable in device RAM for two minutes and is rebound only when session, image hash, size and device ID all match. A reboot, power loss or expired window starts a new transfer. A lost commit ACK requires reconnect verification. Stock Arduino bootloaders may not enable rollback: do not assume recovery from a valid image that cannot boot.

## Checks

`node --test tests/*.cjs` tests release-image validation and the actual native transfer engine. CI additionally compiles the actual sketch and rejects incompatible, oversized or misidentified images. Hardware power-loss/reboot behavior still needs a real pendant test before a broad release.
