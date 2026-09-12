# Hardware and board behavior

S3 refers to the deployed ESP32-S3FH4R2 SuperMini variant: 4 MB flash, 2 MB PSRAM, onboard GPIO48 RGB LED and rear battery pads. C3 SuperMini has 4 MB flash, no PSRAM and an onboard active-low blue LED. C3 does not use an external NeoPixel.

## Wiring

| Signal | S3 | C3 |
| --- | --- | --- |
| INMP441 SCK / BCLK | GPIO4 | GPIO4 |
| INMP441 WS / LRCLK | GPIO5 | GPIO5 |
| INMP441 SD | GPIO6 | GPIO6 |
| INMP441 L/R | GND, left slot | GND, left slot |
| INMP441 VDD / TTP223 VCC | 3V3 | 3V3 |
| Peripheral ground | Common GND | Common GND |
| TTP223 OUT | GPIO13 | GPIO3 |
| Battery divider junction | GPIO8 | GPIO1 |
| Onboard status LED | GPIO48, RGB | GPIO8, blue |

TTP223 must be active-HIGH and momentary: LOW idle, HIGH touched. Firmware reads its digital output rather than using native capacitive touch. Reserve all assigned GPIOs for these functions.

C3 cannot reuse S3's touch pin: GPIO13 belongs to the flash interface, and C3 deep-sleep wake requires GPIO0–5. C3 GPIO8 has no ADC, so its battery input is GPIO1. See [C3 GPIO restrictions](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-reference/peripherals/gpio.html).

## Gestures

| State / action | S3 | C3 |
| --- | --- | --- |
| Connected idle: start | Double tap | Double tap |
| Recording: stop, then standby | Double tap | Double tap |
| BLE standby: start | Double tap | Double tap |
| Awake: deep sleep | Triple tap | Hold 4 seconds, then release |
| Deep sleep: wake | Triple tap | Hold through 4-second boot validation, then release |

S3 waits briefly after a double tap for a possible third tap. C3 acts on the second valid tap. A single C3 tap does nothing. Short or incomplete deep-sleep gestures return to sleep before BLE initializes. C3 wake validation begins after firmware starts, so allow boot overhead. Wake alone does not start recording.

Active recording stops before intentional deep sleep. Release is required to prevent immediate level-triggered wake. OTA interrupts/discards touch gestures. Retained and durable sleep markers prevent unexpected resets from bypassing wake validation.

## Status LEDs

| State | S3 RGB | C3 blue |
| --- | --- | --- |
| Disconnected | Brief red pulse | 100 ms flash every 6 seconds |
| Connected idle | Brief blue pulse | Two 80 ms flashes every 3 seconds, 160 ms off between |
| Connected standby | Off | Same connected double flash |
| Recording | Brief green pulse | 100 ms blink every second |
| OTA | Amber double pulse | Two 55 ms flashes every 1.4 seconds |
| Error | Purple pulse | 70 ms flash every 1.2 seconds |
| Deep sleep | Off | Off; GPIO held HIGH |

S3 low-battery pulses can override normal state colors, except standby and OTA. C3 uses connection/recording patterns irrespective of battery level. Both indicators use the existing non-blocking control loop. The C3 blue LED is LOW-on/HIGH-off; do not attach NeoPixel DIN to its output. GPIO8 is also a strapping pin, so preserve its reset requirements.

## Battery divider and calibration

Both dividers connect battery+ through an upper resistor to the ADC junction, then a lower resistor from that junction to common GND. The 100 nF capacitor (104) is in parallel with the lower resistor. The junction connects directly to the ADC pad.

| Setting | S3 | C3 |
| --- | --- | --- |
| Upper resistor | 1 MΩ | Nominally 1 MΩ |
| Lower resistor | 470 kΩ | Nominally 1 MΩ |
| Capacitor to GND | 100 nF | 100 nF |
| ADC attenuation | 6 dB | 11 dB |
| Cell mV conversion | ADC mV × 4130 / 1320, rounded | ADC mV × 3990 / 1360, rounded |
| Full-charge percentage anchor | 4.13 V | 4.15 V |
| Automatic critical-battery protection | Enabled with corroborating samples | Disabled pending validation |

S3 calibration reference is 4.13 V cell / 1.32 V ADC / raw 1544. The specified S3 board uses its rear battery pads; do not connect a raw LiPo cell to 3V3 or assume that the S3 charging arrangement applies to C3.

**C3 calibration is provisional and charging behavior is unresolved.** The selected meter reference is 3.99 V cell / 1.36 V junction, which differs from a nominal equal-resistor ratio. While charging, diagnostics have reported approximately 2.08 V ADC; the configured ratio reconstructs approximately 6.10 V and correctly reports percentage unavailable. Do not treat this as a full/empty battery or auto-switch calibration based on the reading.

Confirm battery+ and junction voltages against the same C3 GND, charging and unplugged, before changing this ratio. The resistor junction, capacitor signal terminal and GPIO1 must be the same electrical point. Diagnostics preserve ADC millivolts, raw counts and reconstructed cell voltage. The PWA displays valid percentages and “—” for unavailable readings.
