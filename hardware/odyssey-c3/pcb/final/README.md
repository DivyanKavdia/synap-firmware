# Synap Odyssey C3 Carrier — Rev K FINAL

This directory is the authoritative Odyssey C3 carrier PCB release.

## Manufacturing
- Board: 42.0 x 60.0 mm, 2-layer
- Final fabrication archive: `Synap_Odyssey_C3_Carrier_RevK_FINAL_Gerbers.zip`
- Editable Autodesk Fusion/EAGLE source is stored losslessly as `Synap_Odyssey_C3_Carrier_RevK_FINAL.brd.xz`; decompress with `xz -d`.
- No microphone NPTH is present by design: MIC1 acoustic-port/label side faces TOP/outward and the enclosure opening sits above the module.
- TTP223 touch face, NeoPixel emitting face, and ESP32-C3 component side all face TOP/outward.

## Rev K
Rev K adds top-silkscreen pin labels. Copper, footprints, drill geometry, RF setback, component placement, and electrical routing are unchanged from the independently audited Rev J electrical baseline.

## Validation
- 0 different-net copper-clearance violations
- 0 disconnected functional nets
- 0 RF-slot setback violations
- 0 drill-spacing violations
- RF copper setback: 0.50 mm
- closest remaining drilled-hole edge gap: 0.700 mm
- bottom GND coverage: ~80.1%
- top silk-to-mask violations: 0; minimum measured silk-to-mask clearance: 0.22 mm

## MIC1
Nominal centres are 2.54 mm horizontal pitch and 7.62 mm row spacing. Because the exact breakout manufacturer's tolerance drawing was not located, Rev K uses tolerance-friendly 1.40 mm plated holes / 2.10 mm pads and includes a 1:1 physical fit template. Print the fit template at 100% before a volume order.

Order a small first-article batch and run `FIRST_ARTICLE_TEST_PLAN.txt` before volume fabrication.
