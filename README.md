# Synap Firmware

Synap pendant firmware for supported ESP32 targets.

## ESP32-S3 SuperMini production audio

The production ESP32-S3 build uses the physical I2S microphone path.

Pin mapping:

- GPIO 4 -> I2S BCLK / SCK
- GPIO 5 -> I2S WS / LRCLK
- GPIO 6 <- I2S microphone SD / DOUT
- 3.3V -> microphone VDD
- GND -> microphone GND
- microphone L/R -> GND for the left slot used by firmware
- GPIO 48 -> onboard RGB status LED

The GitHub production workflow compiles the ESP32-S3 target with `USE_REAL_I2S_MIC=1`. The 440 Hz synthetic source remains available only as a development/test fallback when that flag is disabled.

## Firmware source

- `synap_esp32s3/synap_esp32s3.ino` — ESP32-S3 source
- `synap_esp32c3/synap_esp32c3.ino` — ESP32-C3 target source

## OTA

Production firmware is built and published by `.github/workflows/firmware.yml`.