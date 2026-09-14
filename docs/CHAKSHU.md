# Chakshu capture and media

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

## Photo/video library extension

Capability descriptor byte 14 advertises media extension version 1 when its worker is available. C3/S3 keep byte 14 zero. The PWA associates the permanent Chakshu device ID with the signed-in account; this preference never gives access to another account's files.

- Online photo/video reads fresh JPEG frames through the existing BLE service while ordinary audio notifications continue. The PWA stores silent frame sequences and its audio journal separately. Throughput determines frame rate; this is not a full-frame-rate MP4 stream or Wi-Fi preview.
- Offline capture writes a matching `.mjpeg`, `.wav`, and `.json` under `/synap/`. WAV is mono 16 kHz PCM16; JSON records frame positions on the captured audio sample timeline. Takes stop at 60 seconds of PCM, 65 seconds of wall time, a stop request, or a storage/capture error. Disconnection does not stop an accepted SD take.
- SD offline capture excludes BLE audio, OTA and hardware checks. Photo/file transfers share the app's serialized native queue. The two media workers claim the shared camera/SD busy flag atomically.
- File reads are limited to generated `/synap/xxxxxxxx-xxxxxxxx` names and JPEG/WAV/MJPEG/JSON extensions. Listing exposes up to 100 photo/video entries; larger card archives can be imported with a card reader. Downloads never delete the originals. The PWA rejects files larger than 32 MiB.
- Existing ten-second hardware checks remain independent. Earlier silent video has no audio timing sidecar; it can be imported for playback/manual description, but cannot drive timestamped voice explanations.

### Media extension wire format

UUID suffix is `-0000-1000-8000-00805f9b34fb`.

| Characteristic | Layout |
| --- | --- |
| `4fa12354`, write with response | `CA`, operation byte, request ID uint32 LE, offset uint32 LE, optional UTF-8 path (up to 63 bytes) |
| `4fa12355`, read | `CB`, version 1, state (1 success, 2 error), error code, request ID uint32 LE, total bytes uint32 LE, offset uint32 LE, up to 480 payload bytes |

| Operation | Action |
| --- | --- |
| 1 / 2 | Capture fresh JPEG / read captured bytes at offset |
| 3 / 4 | Select SD file by path / read selected file or listing bytes |
| 5 / 6 | Start bounded SD audio/video take / request stop |
| 7 / 8 | Build JSON catalogue / read catalogue size |
| 9 | Read offline take status as JSON: active, state, error, progress, path |

The client sends one request at a time, polls for its matching response ID, validates every byte offset/total, and does not automatically repeat a capture. BLE callbacks only copy requests/results; camera and SD work execute in the worker. Stop/status remain available during an offline take. Changing the BLE connection invalidates a selected file or frame.

### Physical validation

CI compiles all three board targets. Simulated browser tests exercise account isolation, native queue transfers, separate audio/video storage and frame-limited inference. They do not establish camera/audio synchronization or throughput on a phone. For device validation, record an audible clap in view, verify the matching WAV/JSON/MJPEG timeline, test a disconnected SD take, and check card-full/removed-card recovery. Cloud descriptions require the matching PWA/backend deployment.

## Local voice commands: Hi Chakshu

The Chakshu-only source adds Espressif MultiNet5 Q8 English speech recognition. The activation phrase is **“Hi Chakshu”**. After a pause, say **“take photo”** / **“click photo”**, **“start video”**, **“stop video”**, **“audio on”**, or **“audio off”**. Each command requires a fresh activation within eight seconds, confidence at least 0.90 and a 1.2-second action cooldown. This uses a continuous phoneme recognizer plus an activation gate; no separately trained custom WakeNet model is included. Tune pronunciations/thresholds only after recording false-activation and missed-command measurements on the real pendant.

### Model installation

1. Use the firmware release's `chakshu-voice-model.zip`, or build it with `python3 tools/prepare-voice-model.py /tmp/chakshu-voice-model`.
2. With the pendant powered off, copy the ZIP's `synap` directory onto the SD card, preserving all existing recordings. The final path is `/synap/models/srmodels.bin`.
3. Reinsert the card, power on, connect the PWA and check Settings → Device → Local voice controls. Enable the listener if it was previously disabled.

The pack pins Espressif source revision `27da4f945f779bab2d238889924622f7988b1b1c`, verifies the three source blob identities and includes Espressif's license. Firmware verifies **2,177,224 bytes** and SHA-256 **9bb7348b31891a89eb494f5995970a7fc52b765759e4992d471ab2901bf9c47c** before passing weights to the model parser. A missing, mismatched or unavailable model reports a status; camera and ordinary audio remain usable. Weights load into PSRAM from SD. The existing dual OTA partitions are unchanged. No recognition audio is uploaded for command detection.

### Capture ownership and behavior

- BLE capture and the SD audio worker feed copies of their unmodified PCM into a bounded recognition queue. An idle reader uses the existing recursive microphone lock and rechecks ownership under that lock. It yields to either recording consumer. The AFE processes only its copy. Queue discontinuities reset recognition; stale commands are discarded.
- Enabled recognition holds the active CPU profile. Audio off stops a take while the listener remains available for audio on. Disable Local voice controls to stop command listening; the setting persists in NVS.
- A visible connected PWA claims a six-second, connection-generation-bound lease. Commands go to the PWA while that lease is valid, so existing account ownership, storage and mode-switch rules apply. No lease means local SD actions. Offline mode switches save the previous take, then create separate files for the next mode. Photo during an SD take is handled by the owning worker.
- Offline video creates matching silent MJPEG, mono PCM16 WAV and sample-timeline JSON files. Audio-only creates its own standalone WAV. Both are limited to 60 seconds of PCM or 65 seconds elapsed. Importing standalone WAVs puts them in the audio library; paired video audio is imported once. Stop cancels queued local starts while leaving PWA transfer requests intact.
- CI includes native activation, stale-command, routing and microphone-ownership checks, prepares the pinned model ZIP, and compiles all three targets. Real ESP32 linking, flash fit and runtime recognition still need a successful build/device check for each release candidate. Bench-test all commands while idle, during BLE audio/video and disconnected SD capture; measure PSRAM, recognizer drops, audio continuity and false activations. Stop and preserve partial files for SD-full/removed-card cases.

### Voice extension v1

Descriptor byte 15 is 1 for this firmware; C3/S3 remain zero. UUIDs share the suffix above.

| Characteristic | Layout |
| --- | --- |
| `4fa12356`, read/write | Read 20-byte status. Write `CC 01 op`: 0 disable, 1 enable, 2 renew PWA lease, 3 release lease. |
| `4fa12357`, notify | 20 bytes: `CD 01 status enabled`, sequence uint32 LE at 4, action at 8, result at 9, pendant milliseconds uint32 LE at 10, discontinuities uint32 LE at 14, offline-active at 18, reserved at 19. |

Status: 0 starting, 1 listening, 2 model missing, 3 insufficient memory, 4 invalid model/setup, 5 disabled. Action: 1 photo, 2 video start, 3 video stop, 4 audio on, 5 audio off. Result: 0 local request accepted/no-op, 1 busy/rejected, 2 delegated to the leased page. Accepted is not a claim that the later SD write succeeded; inspect media status and saved files. Media extension operations 10 and 11 start standalone SD audio and save a JPEG respectively.

## References

- [Seeed microphone pins and PDM setup](https://wiki.seeedstudio.com/xiao_esp32s3_sense_mic/)
- [Seeed microSD wiring and preparation](https://wiki.seeedstudio.com/xiao_esp32s3_sense_filesystem/)
- [Espressif XIAO camera pin map, core 3.3.5](https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/ESP32/examples/Camera/CameraWebServer/camera_pins.h)
- [Espressif 8 MB partition layout](https://github.com/espressif/arduino-esp32/blob/3.3.5/tools/partitions/default_8MB.csv)
- [Espressif MultiNet speech commands and phoneme format](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/speech_command_recognition/README.html)
