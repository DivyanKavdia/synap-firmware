# Synap firmware release process

Public product version remains **1.0.0**; monotonically increasing 16-bit build numbers identify firmware revisions.

## Channels

The release pipeline intentionally separates engineering builds from customer firmware:

- `ota-test`: automatically published by successful `main` pushes. Used for engineering/hardware qualification only.
- `ota-releases`: production feed consumed by `synap-pwa`. Published only through a manual GitHub Actions dispatch with `channel=production`.

Production feed:

`https://raw.githubusercontent.com/DivyanKavdia/synap-firmware/ota-releases/latest.json`

Test feed:

`https://raw.githubusercontent.com/DivyanKavdia/synap-firmware/ota-test/latest.json`

A normal source push can therefore never make a firmware build visible to customers by itself.

## Production signing

Production schema-2 manifests are signed with ECDSA P-256 / SHA-256 (`ES256`) and key ID `prod-2026-01`.

The public verification key is committed at `tools/release-public-key.pem` and embedded in the PWA. The private key is not stored in source control. Configure it only in the protected GitHub production environment/repository secret:

`SYNAP_RELEASE_PRIVATE_KEY_PEM`

Use a PKCS#8 PEM private key corresponding to the committed public key. Restrict access to repository administrators/release operators and retain an offline backup under your normal key-management process.

If the secret is absent or the generated signature is malformed, the production publish job fails closed.

## Migration from build 1008

The deployed production feed through build **1008** is unsigned. The PWA explicitly allows unsigned schema-1 production manifests only through build 1008. Any build above 1008 must be a valid signed schema-2 production manifest.

The first signed release still uses OTA protocol 3, so build-1008 devices can install it without USB migration.

## Production promotion

1. Merge reviewed firmware changes to `main`.
2. Let the automatic `ota-test` build complete.
3. Install/qualify that test binary on representative hardware.
4. Validate BLE connect/reconnect, real microphone capture if applicable, record/stop, long stream, OTA interruption/resume, reboot and diagnostics.
5. In GitHub Actions run **Build and publish pendant firmware** manually with `channel=production`.
6. The production environment signs and publishes the exact `main` commit only if it is still current.
7. The workflow verifies the public manifest, binary digest, signature policy and browser CORS response.

The production publisher refuses to advertise a source commit that has already been superseded by a newer `main` commit.

## Artifact contract

The manifest records exact target, channel, build, image length, SHA-256, source commit, hardware identity and immutable content-addressed binary URL. The updater transfers only the application image; bootloader and partition table are never changed by BLE OTA.

Target is ESP32-S3FH4R2 / SuperMini with 4 MB flash, 2 MB QSPI PSRAM and the default dual-OTA partition layout. Images larger than the inactive application slot or missing the exact target/build marker are rejected by CI and again by the updater.

## OTA recovery

A live device keeps transfer hash/offset state in RAM for up to two minutes after BLE interruption. RESUME requires matching transfer ID, image size/hash and permanent device ID. Power loss or restart loses that transient state and requires a fresh transfer; this is intentional because no local storage is used on the pendant.

After VERIFY the SHA-256 and image structure must pass before COMMIT can select the inactive boot partition. Where the provisioned bootloader has rollback enabled, the new application delays mark-valid until its early runtime health window passes.

## Tests

Host release/session tests:

```bash
node --test tests/*.cjs
```

The GitHub workflow additionally compiles the actual Arduino sketch with the pinned ESP32-S3 board/toolchain configuration. Host/compile checks do not replace the physical production-promotion smoke test above.
