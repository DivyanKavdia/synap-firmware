# C3+SD native timeout ownership hardening (candidate)

Date: 2026-10-18. Target: `esp32c3-supermini-4m-sd-ota`, BLE-only.

## Verified in source

- The current SDSPI backend serializes mount, recording, transfer and maintenance with the existing `OdysseySdGuard` mutex. Pin contract remains GPIO0 CS, GPIO10 SCK, GPIO21 MOSI, GPIO20 MISO.
- Current 2 MHz data / standard 400 kHz card initialization and aligned 2 KiB CMD25 recording writes are unchanged; an actual 5 s CMD25 program/data timeout has *not* been eliminated.
- **Fixed software flaw:** previously both sleep and OTA reset paths called `odysseySdReleaseLocked` *before* checking whether native CMD13 succeeded. They could unmount the card or free SPI2 while it was still busy. Now they only release after an idle response and successful teardown, and they preserve mount/host ownership on failure.
- **Fixed blind spot:** CMD17/18 timeouts during the FAT mount phase were ignored when the mount API had not yet returned a non-null card pointer. The native hook now quarantines these memory-transfer timeouts before mounting finishes; expected CMD5/52 probe timeouts are still excluded from first-fault diagnostics.
- **Fixed warm-reset bypass:** a read-side native timeout (including BLE sync) now retains an internal-flash NVS `railBad` marker, so an OTA/warm CPU reset cannot reinitialize the continuously-powered SD. Only a successful, genuine cold power-on and verified FAT mount clears it.

## Deliberate non-changes

No retry of uncertain CMD24/25, no forced CMD12, no increased 5000 ms timeout, no optimistic success, no format, no auto-delete of non-empty recordings, no change to SD frequency without an A/B test, no new Wi-Fi, no OTA partition changes.

## Root-cause work still required on hardware

A CMD25 that returns `ESP_ERR_TIMEOUT=263` at ~5 s is lower than FatFs; the ESP cannot determine whether the card accepted some/all blocks. Firmware cannot guarantee audio capture across such a stall on a C3 with limited available RAM. Distinguish card-internal busy/controller failure from SPI physical integrity by testing **two genuine FAT32 card models** under USB and battery and scoping SD socket VCC/GND, CS/SCK/MOSI/MISO at the failure. Inspect installed 3.3 V module, regulator and socket and confirm 10 kOhm pull-ups on required SD lines; avoid relying on battery ADC to infer SD VCC.

## Hardware qualification

1. Preserve existing recordings. On a *disposable* FAT32 card and with USB unplugged, slider OFF until the SD rail discharges, ON; check SD ready state and clock field.
2. Record/STOP 10 short captures, 10 min capture, BLE sync, then immediate additional capture without reset.
3. Record 3 × 30 min; verify saved WAV size, playback, checksums and absence of stage 66/CMD25 or CMD17/18 errors.
4. Repeat on another brand/model card and both power conditions. If a new fault occurs capture live op34 and retained op27 and a scope trace before cold power-cycle.
5. If CMD25 still returns 263, classify as **unresolved root cause**; no source-level test or CI result proves the rail, contacts or card internal controller.

No production fitness claim until real hardware passes.
