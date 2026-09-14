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
| Awake: deep sleep | Hold 4 seconds, then release | Hold 4 seconds, then release |
| Deep sleep: wake | Hold through 4-second boot validation, then release | Hold through 4-second boot validation, then release |

Both boards act on the second valid tap immediately; a single tap does nothing.
A valid tap lasts 60–500 ms and the double-tap gap is at most 550 ms. The firmware
uses 35 ms debounce and a 250 ms lockout after connection/recording state changes.
Short wake holds return to sleep before BLE initializes. Wake validation begins
after firmware starts, so allow boot overhead before releasing. Wake alone does
not start recording. Older S3 firmware used triple tap for sleep/wake; update it
to use the shared four-second hold gesture. GPIO assignments are unchanged.

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
| Cell mV conversion | ADC mV × 4130 / 1320, rounded | ADC mV × 2 |
| Full-charge percentage anchor | 4.13 V | 4.15 V |
| Automatic critical-battery protection | Enabled with corroborating samples | Disabled pending validation |

S3 calibration reference is 4.13 V cell / 1.32 V ADC / raw 1544. The specified S3 board uses its rear battery pads; do not connect a raw LiPo cell to 3V3 or assume that the S3 charging arrangement applies to C3.

C3 uses the nominal equal-resistor ratio: cell voltage = GPIO1 ADC voltage × 2. The 4.15 V full-charge anchor corresponds to 2.075 V at GPIO1; valid readings at or above that anchor report 100%. For example, a charging sample of 2.080 V reports 4.160 V and 100%. The existing 2.80–4.35 V validity range still rejects implausible readings.

The resistor junction, capacitor signal terminal and GPIO1 must be the same electrical point, measured against C3 GND. Diagnostics preserve ADC millivolts, raw counts and reconstructed cell voltage. The PWA displays valid percentages and “—” for unavailable readings.

## Chakshu: XIAO ESP32S3 Sense

These connections are on the Sense board. No external wiring is required for bring-up.

| Function | GPIO |
| --- | --- |
| PDM microphone clock / data | 42 / 41 |
| SD SCK / MISO / MOSI / CS | 7 / 8 / 9 / 21 |
| Camera XCLK / SCCB SDA / SCCB SCL | 10 / 40 / 39 |
| Camera D0 / D1 / D2 / D3 | 15 / 17 / 18 / 16 |
| Camera D4 / D5 / D6 / D7 | 14 / 12 / 11 / 48 |
| Camera VSYNC / HREF / PCLK | 38 / 47 / 13 |
| Camera PWDN / RESET | Unconnected (-1) |
| External touch / battery divider / NeoPixel | Disabled |

GPIO8, GPIO13 and GPIO48 must not inherit the SuperMini battery, touch or LED behavior. See [Chakshu bring-up](CHAKSHU.md).
