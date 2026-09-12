# Synap Hardware Pinout

This is the hardware mapping for both production firmware targets. Shared signals use the same GPIO wherever compatible with the deployed S3 wiring.

## Controller

MakerBazaar ESP32-S3 SuperMini variant used by Synap:

- ESP32-S3FH4R2
- 4 MB flash
- 2 MB PSRAM
- USB-C
- onboard addressable RGB LED on GPIO48
- rear B+ / B- pads for the board's 1-cell battery interface

The ESP32-C3 SuperMini target uses 4 MB flash and no PSRAM. Its RGB status output requires a separate external NeoPixel; the board's ordinary blue LED is not an addressable RGB LED.

## Pin mapping

| Device / signal | ESP32-S3 SuperMini | ESP32-C3 SuperMini | Firmware purpose |
| --- | --- | --- | --- |
| INMP441 SCK / BCLK | GPIO4 | GPIO4 | I2S bit clock |
| INMP441 WS / LRCLK | GPIO5 | GPIO5 | I2S word-select clock |
| INMP441 SD | GPIO6 | GPIO6 | I2S microphone data input |
| INMP441 L/R | GND | GND | Select left I2S channel |
| INMP441 VDD | 3V3 | 3V3 | Microphone power |
| INMP441 GND | GND | GND | Common ground |
| TTP223 OUT | GPIO13 | GPIO3 | Active-HIGH touch input and deep-sleep wake |
| TTP223 VCC | 3V3 | 3V3 | Touch sensor power |
| TTP223 GND | GND | GND | Common ground |
| Battery divider midpoint | GPIO8, 1 MΩ / 470 kΩ | GPIO1, 1 MΩ / 1 MΩ | Battery ADC sense |
| NeoPixel DATA / DIN | GPIO48, onboard | GPIO8, external NeoPixel | Synap RGB status |

All peripheral grounds are common.

## Why three GPIO assignments differ

The existing mapping already shares every compatible same-purpose GPIO while keeping S3 wiring intact:

- Touch: C3 GPIO13 belongs to the flash interface and cannot wake from deep sleep. C3 deep-sleep wake requires GPIO0–5, so TTP223 stays on GPIO3.
- Battery: C3 GPIO8 has no ADC. GPIO1 reads the C3's equal-resistor battery divider.
- RGB: C3 has GPIO0–21, so S3's onboard LED pin GPIO48 cannot be copied. The external C3 NeoPixel uses GPIO8.

These restrictions follow the [Espressif C3 GPIO reference](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-reference/peripherals/gpio.html). Matching more pins would require changing S3 hardware connections.

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
- OUT: GPIO13 on S3; GPIO3 on C3
- VCC: 3.3 V
- GND: common ground

### ESP32-S3 SuperMini interaction

The S3 behavior is intentionally unchanged:

- while connected and idle: double tap to start recording
- while recording: double tap to stop recording and enter BLE standby
- in BLE standby: double tap to wake and start recording
- in any non-OTA state: triple tap to enter deep sleep; active recording stops first
- from deep sleep: triple tap to wake; one or two taps return to deep sleep without initializing BLE
- double-tap actions wait briefly for a possible third tap, keeping the sleep gesture separate from recording

### ESP32-C3 SuperMini interaction

The C3 uses a target-specific gesture model designed for reliable GPIO3 level wake:

- while connected and idle: double tap to start recording
- while recording: double tap to stop recording and enter BLE standby
- in BLE standby: double tap to wake and start recording
- in any awake non-OTA state: hold the TTP223 for at least four seconds, then release, to enter deep sleep; active recording stops first
- from deep sleep: hold the TTP223 continuously for four seconds after firmware starts, then release, to confirm wake and continue normal boot; allow brief boot overhead
- a short deep-sleep touch wakes the silicon electrically but is rejected by firmware and returns to deep sleep before BLE starts
- a single tap while awake has no action
- C3 does not use triple tap
- the second valid tap acts immediately; unlike S3 there is no wait for a possible third tap
- before entering deep sleep, firmware requires the TTP223 line to be released so the GPIO3 HIGH-level wake source cannot immediately wake the C3 again

For both targets, touch actions are ignored during OTA. Deep-sleep state is guarded by retained and durable markers so a reset during shutdown or wake validation does not bypass the intended power gesture.

## RGB status LED

S3 uses its onboard addressable RGB LED on GPIO48; no external data connection is needed. C3 uses a separate external NeoPixel with DIN connected to GPIO8 and ground connected to the board's ground. Choose the LED supply and any data-level conversion for the specific NeoPixel module.

The [C3 SuperMini's blue LED](https://makerbazar.in/products/esp32-c3-supermini-iot-development-board) also uses GPIO8. It can respond to the data signal but does not provide the firmware's RGB status. GPIO8 is also a boot-strapping pin; the external circuit must preserve its required reset level. Reserve each board's RGB pin for this use.

The production power-saving status model uses short dim pulses rather than leaving the LED continuously illuminated:

- red pulse: BLE disconnected
- blue pulse: connected / idle
- green pulse: recording
- amber pulse pattern: OTA
- purple pulse pattern: error

Standby is dark even when battery is low; OTA keeps its amber indication. Turning the RGB output off does not remove the LED's supply current.

## Battery

On the specified S3 board, connect a single-cell LiPo/Li-ion battery to the rear B+ / B- pads. S3 battery telemetry uses an external high-value divider:

```text
Battery + ---- 1 MOhm ----+---- GPIO8
                           |
                         470 kOhm
                           |
Battery - / GND -----------+---- GND

GPIO8 ---- 100 nF ---------- GND
```

Do not connect the raw LiPo cell to the ESP32 3V3 pin. The current S3 calibration uses the measured full-charge point of 4.13 V cell / 1.32 V ADC (raw 1544).

C3 battery telemetry uses the installed divider:

| Connection | Component |
| --- | --- |
| Battery positive to GPIO1 | 1 MΩ |
| GPIO1 to common ground | 1 MΩ |
| GPIO1 to common ground | 104 capacitor (100 nF) |

The equal divider halves the cell voltage. The selected reference is 4.15 V at the cell and 2.075 V at GPIO1, giving calibrated ADC millivolts × 2 and a 4.15 V full-charge anchor. C3 uses 11 dB attenuation for this input. This replaces the earlier provisional ratio; actual percentage accuracy still needs hardware validation. The existing 16-sample averaging and 15-second interval are unchanged. Automatic battery-triggered sleep and OTA lockout remain inactive; touch and timeout sleep still work. The S3 battery-pad and charging arrangement does not apply to C3.

## Reserved / locked pins

| Board | Reserved GPIOs |
| --- | --- |
| S3 | 4, 5, 6, 8, 13, 48 |
| C3 | 1 (battery sense), 3, 4, 5, 6, 8 |

Assign future peripherals to other audited GPIOs so microphone capture, battery telemetry, touch interaction and status indication remain compatible with deployed firmware.
