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
| GPIO4 | INMP441 | SCK / BCLK | I2S bit clock |
| GPIO5 | INMP441 | WS / LRCLK | I2S word-select clock |
| GPIO6 | INMP441 | SD | I2S microphone data input |
| GND | INMP441 | L/R | Select left I2S channel |
| 3V3 | INMP441 | VDD | Microphone power |
| GND | INMP441 | GND | Common ground |
| GPIO7 | TTP223 | OUT | Digital touch input, active HIGH |
| 3V3 | TTP223 | VCC | Touch sensor power |
| GND | TTP223 | GND | Common ground |
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
TTP223 OUT  ------| GPIO7               |
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

## INMP441 configuration

- VDD: 3.3 V
- SCK/BCLK: GPIO4
- WS/LRCLK: GPIO5
- SD: GPIO6
- L/R: GND
- GND: common ground

The L/R pin is intentionally tied to GND so the INMP441 transmits in the left I2S slot expected by the current Synap mono capture configuration.

## TTP223 configuration

The firmware treats the TTP223 as a digital input, not as an ESP32 native capacitive-touch input.

Expected module configuration:

- idle: LOW
- touched: HIGH
- momentary/non-latching mode
- OUT: GPIO7
- VCC: 3.3 V
- GND: common ground

Current interaction model:

- short press while idle: start listening
- double short press while recording: stop listening
- long press while recording (approximately 1.2 seconds): Remember This
- Remember This does not interrupt audio capture
- long press is ignored during OTA

## RGB status LED

GPIO48 drives the onboard addressable RGB LED. It is reserved by Synap and should not be used for another peripheral.

Current status model includes:

- red: BLE disconnected
- blue: connected / idle
- green: recording
- cyan acknowledgement: Remember This
- amber: OTA
- purple: error

## Battery

Connect a single-cell LiPo/Li-ion battery to the rear battery pads:

```text
Battery + -> B+
Battery - -> B-
```

Do not connect the raw LiPo cell to the ESP32 3V3 pin.

The current firmware does not measure battery voltage and does not allocate an ADC pin for battery level. Battery telemetry can be added later after the exact board battery/power circuit is audited and an appropriate measurement point is selected.

## Reserved / locked pins

For the current Synap hardware design, treat GPIO4, GPIO5, GPIO6, GPIO7 and GPIO48 as reserved. Any future PCB additions should be assigned to other audited GPIOs so microphone capture, physical interaction and status indication remain compatible with deployed firmware.
