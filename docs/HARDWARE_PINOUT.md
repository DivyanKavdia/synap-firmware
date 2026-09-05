# Synap Hardware Pinout

This is the locked hardware mapping for the current Synap pendant firmware and prototype PCB.

## Controller

MakerBazaar ESP32-S3 SuperMini variant used by Synap:

- ESP32-S3FH4R2
- 4 MB flash
- 2 MB PSRAM
- USB-C
- onboard addressable RGB LED on GPIO48
- rear B+ / B- pads for the board's 1-cell battery interface

## Pin mapping

| ESP32-S3 SuperMini | Device | Device pin | Firmware purpose |
| --- | --- | --- | --- |
| GPIO4 | INMP441 / INMP44x | SCK / BCLK | I2S bit clock |
| GPIO5 | INMP441 / INMP44x | WS / LRCLK | I2S word-select clock |
| GPIO6 | INMP441 / INMP44x | SD | I2S microphone data input |
| GND | INMP441 / INMP44x | L/R | Select left I2S channel |
| 3V3 | INMP441 / INMP44x | VDD | Microphone power |
| GND | INMP441 / INMP44x | GND | Common ground |
| GPIO13 | TTP223 | OUT | Digital touch input, active HIGH |
| 3V3 | TTP223 | VCC | Touch sensor power |
| GND | TTP223 | GND | Common ground |
| GPIO8 | 1 MOhm / 470 kOhm divider | midpoint | Battery ADC sense |
| GPIO48 | onboard RGB | DATA | Synap status indication; no external connection |
| B+ | 1S LiPo/Li-ion | + | Battery positive |
| B- | 1S LiPo/Li-ion | - | Battery negative |

## Wiring

```text
                    ESP32-S3 SuperMini
                  +---------------------+
INMP441 SCK ------| GPIO4               |
INMP441 WS  ------| GPIO5               |
INMP441 SD  ------| GPIO6               |
TTP223 OUT  ------| GPIO13              |
Battery sense ----| GPIO8               |
                  |                     |
                  | GPIO48 -> onboard RGB
                  |                     |
INMP441 VDD --+---| 3V3                 |
TTP223 VCC ---+   |                     |
                  |                     |
INMP441 GND --+---| GND                 |
TTP223 GND ---+   +---------------------+
INMP441 L/R ---+

1S LiPo + ---------------- B+
1S LiPo - ---------------- B-
```

All peripheral grounds are common.

## INMP441 / INMP44x configuration

- VDD: 3.3 V
- SCK/BCLK: GPIO4
- WS/LRCLK: GPIO5
- SD: GPIO6
- L/R: GND
- GND: common ground

The L/R pin is intentionally tied to GND so the microphone transmits in the left I2S slot expected by the current Synap mono capture configuration.

## TTP223 configuration

The firmware treats the TTP223 as a digital input, not as an ESP32 native capacitive-touch input.

Expected module configuration:

- idle: LOW
- touched: HIGH
- momentary/non-latching mode
- OUT: GPIO13
- VCC: 3.3 V
- GND: common ground

Current interaction model is intentionally asymmetric to prevent accidental recording starts/stops from clothing or handling:

- while connected and idle: press and release for approximately 0.55–1.4 seconds to start listening
- while recording: a deliberate 0.30–0.95 second tap stops listening
- while recording: long press for approximately 1.2 seconds triggers Remember This
- while idle: hold for at least 3 seconds to enter deep sleep
- after connect or a recording-state transition, touch is ignored for approximately 1.5 seconds
- Remember This does not interrupt audio capture
- touch actions are ignored during OTA
- deep sleep is not entered while TTP223 OUT is still HIGH, preventing an immediate wake loop

## RGB status LED

GPIO48 drives the onboard addressable RGB LED. It is reserved by Synap and should not be used for another peripheral.

The production power-saving status model uses short dim pulses rather than leaving the LED continuously illuminated:

- red pulse: BLE disconnected
- blue pulse: connected / idle
- green pulse: recording
- cyan acknowledgement: Remember This
- amber pulse pattern: OTA
- purple pulse pattern: error

## Battery

Connect a single-cell LiPo/Li-ion battery to the rear battery pads. Battery telemetry uses an external high-value divider:

```text
Battery + ---- 1 MOhm ----+---- GPIO8
                           |
                         470 kOhm
                           |
Battery - / GND -----------+---- GND

GPIO8 ---- 100 nF ---------- GND
```

Do not connect the raw LiPo cell to the ESP32 3V3 pin. The current S3 calibration uses the measured full-charge point of 4.13 V cell / 1.32 V ADC (raw 1544). ESP32-C3 battery monitoring remains disabled until its physical battery-sense path is separately audited.

## Reserved / locked pins

For the current S3 hardware design, treat GPIO4, GPIO5, GPIO6, GPIO8, GPIO13 and GPIO48 as reserved. Any future PCB additions should be assigned to other audited GPIOs so microphone capture, battery telemetry, physical interaction and status indication remain compatible with deployed firmware.
