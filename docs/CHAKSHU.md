# Chakshu hardware check

Chakshu is the third Synap module: Seeed XIAO ESP32S3 Sense, onboard PDM microphone, OV3660 camera and microSD. The initial build assumes an 8 MB flash / 8 MB OPI PSRAM XIAO and an installed 2 GB card. Capacity and camera sensor ID are read from hardware, not hardcoded as successful.

## Initial behavior

- The module stays awake while powered and advertises as **synap-Chakshu**.
- Microphone, camera and SD are probed at boot. A failed camera/card does not disable BLE audio.
- No TTP223, external LED, switch or battery divider is configured. Standby commands leave it awake.
- The main PWA microphone control records through the existing Synap journal/transcription flow: 16 kHz mono PCM16 preferred over BLE, with existing ADPCM fallback at small MTUs. PDM capture does not apply gain, filtering or noise reduction.
- Device settings show module identity and per-feature readiness. Bluetooth pairing still needs a browser picker; automatic detection happens after connection.
- Photo, ten-second WAV, and ten-second silent MJPEG checks save to SD under unique names in /synap. MJPEG uses VGA JPEG frames at a target of 2 fps; it has no audio track or fixed-rate container.
- A running SD check finishes on the pendant even if the app disconnects. Its result remains readable after reconnect until another check or reboot. These bounded checks are independent of the main PWA recording.
- SD checks and BLE audio/OTA are mutually exclusive. Files are never automatically deleted or overwritten. Failed writes retain partial files and report failure.
- Flash/NVS stores settings and device state. Audio is not written to internal flash.

## First flash

Use the Chakshu build only; a generic Synap S3 binary has different pins and memory settings.

The CI **verified-firmware** artifact includes:
- xiao-esp32s3-sense-8m/factory.bin: merged first-flash image, flash at address 0x0.
- xiao-esp32s3-sense-8m/firmware.bin: application image for the configured OTA slots.
- xiao-esp32s3-sense-8m/synap_chakshu.ino: portable Arduino source with the matching build counter; use this for an Arduino/Arduinodroid first flash.
- source-sync/synap_chakshu.ino: unversioned prepared source for auditing.

For a source build, run:

~~~sh
node tools/assemble-source.cjs
node tools/materialize-target.cjs xiao-esp32s3-sense-8m synap_esp32s3/synap_esp32s3.ino prepared/synap_chakshu/synap_chakshu.ino
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc' prepared/synap_chakshu
~~~

Use Arduino ESP32 core **3.3.5**, 8 MB flash, **OPI PSRAM**, and **default_8MB dual OTA partitions** (two 0x330000-byte slots). CI uses the generic ESP32S3 board definition with all Sense pins explicitly assigned. In Arduino IDE/Arduinodroid, use XIAO ESP32S3 or equivalent settings; do not choose Huge APP/no OTA. Local builds identify as build 0; versioned artifact source includes its build counter. The current PWA updater expects a release-range build identity, so use the versioned artifact source if you want subsequent BLE updates without another USB flash.

## Test with the 2 GB card

1. Connect the Sense expansion board firmly, insert the card and power by USB.
2. Open Serial at 115200 if available. Do not wait for Serial before boot: headless power works.
3. Connect **synap-Chakshu** in the PWA. Open Settings → Device. Confirm Chakshu, OV3660, microphone and SD readiness.
4. Use the main microphone control to record 20–30 seconds. Stop and play the audio in Synap.
5. Stop live recording, then run **Photo to SD**, **10-second WAV to SD**, and **10-second clip to SD** separately.
6. Wait for each saved result. Note the filename, power down and read the card. A successful WAV has 320,000 PCM data bytes plus its 44-byte header. Open JPEG normally. Play/convert the .mjpeg stream with software that accepts raw MJPEG, specifying 2 fps if required.
7. Test a missing card: BLE audio should remain usable. Reinsert the card and press **Check hardware** to remount it.

Try the installed FAT card as-is. Firmware does not format it. If mounting fails, back up its files before manually preparing a compatible FAT volume. Seeed recommends FAT32. Report the Serial log and Device panel status before changing pin definitions.

Readiness means driver initialization succeeded, not that real-world audio/image quality has been certified. Physical checks are required.

## Scope

This first version provides basic hardware verification and Synap audio integration. Continuous SD recording, simultaneous SD and live audio, synchronized audio/video, media listing/download in the PWA, Wi-Fi video, and cloud image/video understanding are future work. Do not treat these short checks as a background-recording solution for long sessions.

## References

- [Seeed microphone pins and PDM setup](https://wiki.seeedstudio.com/xiao_esp32s3_sense_mic/)
- [Seeed microSD wiring and preparation](https://wiki.seeedstudio.com/xiao_esp32s3_sense_filesystem/)
- [Espressif XIAO camera pin map, core 3.3.5](https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/ESP32/examples/Camera/CameraWebServer/camera_pins.h)
- [Espressif 8 MB partition layout](https://github.com/espressif/arduino-esp32/blob/3.3.5/tools/partitions/default_8MB.csv)
