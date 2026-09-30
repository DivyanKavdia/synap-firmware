# Synap Odyssey C3 Carrier — FINAL

Canonical hardware revision: **Rev K FINAL**

This directory identifies the production-intent Odyssey C3 carrier design finalized on 2026-09-30.

## Firmware pin contract
Design pin contract was validated against firmware commit `21872010fc7c8c414284a0ca5090dabf1022a0e6`.

- GPIO0 — microSD CS
- GPIO10 — microSD SCK
- GPIO21 — microSD MOSI
- GPIO20 — microSD MISO
- GPIO4 — INMP441 SCK/BCLK
- GPIO5 — INMP441 WS
- GPIO6 — INMP441 SD
- GPIO3 — TTP223 OUT
- GPIO1 — battery ADC divider
- GPIO8 — NeoPixel DIN via 330R

## Rev K status
Rev K supersedes Rev J and adds top-silkscreen pin labels. Electrical routing, footprints, drills, RF setback and component placement are unchanged from validated Rev J.

Key final checks:
- 0 different-net copper-clearance violations
- 0 disconnected functional nets
- 0 RF-slot setback violations
- 0 drill-spacing violations at the checked 0.254 mm edge-to-edge rule
- 0 top-silkscreen / solder-mask-opening violations
- RF routed-edge copper setback: 0.50 mm
- MIC1: nominal 2.54 mm horizontal pitch / 7.62 mm row spacing, 1.40 mm PTH, 2.10 mm pad
- microphone acoustic port faces TOP/outward; no carrier NPTH microphone hole
- TTP223 touch face TOP/outward
- ESP32-C3 component side TOP/outward
- NeoPixel TOP/outward

## Manufacturing artifact
Canonical local artifact name: `Synap_Odyssey_C3_Carrier_RevK_FINAL_Gerbers.zip`.

The repository connector used for this commit supports UTF-8 repository writes but not direct local binary-file upload. Therefore do **not** infer that the Gerber ZIP is present from this manifest alone; the binary manufacturing archive must be committed separately and byte-verified before fabrication is sourced from GitHub.
