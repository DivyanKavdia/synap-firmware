# Rev L v3 hand assembly and first-board test checklist

## Pre-solder inspection
- [ ] Print fit SVG at **100%** and verify the scale bar using a caliper/ruler.
- [ ] Check **the exact** ESP32-C3 SuperMini, Ø14 mm INMP441, TTP223, SD breakout, TP4056 charger (or actual charger board), 5050 NeoPixel, right-angle SPDT and battery fit.
- [ ] Insert microSD fully and verify no card protrudes beyond the PCB outline; check removal access.
- [ ] Verify U1/SD1/CHG1 rear-side footprints **from the back**, not viewed through the PCB from the front.
- [ ] Check USB-C programming/charge port access, slide travel, clear mic acoustic opening, and lanyard/enclosure clearance.

## Before inserting battery
- [ ] Inspect silkscreen labels / pin orientation and microscope-check bridges, cold joints and pin shorts.
- [ ] DMM continuity: BAT+, SW_BAT, VIN_C3, 3V3, GND, SD power, touch, mic, GPIO and NeoPixel nets, including absence of shorts.
- [ ] Confirm D2 SS14 orientation and SW1 center/outer mapping.
- [ ] Confirm charger board/cell protection and USB power interaction before connecting a battery.

## Powered verification
- [ ] Use current-limited bench supply first; measure 3V3 at idle and during worst-case BLE + SD write.
- [ ] Check power-up behavior and brownout at 4.2V, 3.7V and 3.4V battery-equivalent voltages; **known architecture may fail**.
- [ ] Confirm ADC calibration for **470k / 470k** divider.
- [ ] Verify boot and normal GPIO9 behavior; SD card detect/mount/read/write, 0-byte cleanup, offline recording >30min, BLE file sync/delete, OTA.
- [ ] Verify RGB LED, touch input and microphone audio quality.
- [ ] Verify BLE usability at intended pendant range within final enclosure; **RF layout unqualified**.
- [ ] Thermal check charger, diode and SuperMini under normal/charging/stress conditions.
- [ ] Document any failures and reroute before ordering production volume.
