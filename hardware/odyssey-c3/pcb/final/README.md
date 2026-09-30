# Synap Odyssey C3 Carrier — Rev K FINAL

This directory is the authoritative Odyssey C3 carrier PCB release.

## Manufacturing Gerbers
The exact Rev K Gerber/drill folder is stored losslessly as four parts of an xz-compressed tar archive under `gerber_archive_parts/`.

Rebuild it with:
```sh
./REBUILD_GERBERS.sh
```

Expected SHA-256 of the reconstructed archive:
`cf004abe2c5e4d5f530c0d971979131f8a9623b473943a529b0706343b552191`

The script verifies the SHA-256 and extracts the exact `gerbers/` directory. Zip that extracted directory if your PCB fab portal requires a ZIP.

The original local fabrication ZIP used during validation had SHA-256:
`7b506e75cbba401511d516741fec63c4cabca963fbed6af9bf872915458835b6`

## Editable source
`Synap_Odyssey_C3_Carrier_RevK_FINAL.brd.xz` is the losslessly compressed Autodesk Fusion/EAGLE board source. Decompress with:
```sh
xz -d Synap_Odyssey_C3_Carrier_RevK_FINAL.brd.xz
```

## Rev K
Rev K adds top-silkscreen pin labels. Copper, footprints, drill geometry, RF setback, component placement, and electrical routing are unchanged from the audited Rev J electrical baseline.

## Validation
- 0 different-net copper-clearance violations
- 0 disconnected functional nets
- 0 RF-slot setback violations
- 0 drill-spacing violations
- RF copper setback: 0.50 mm
- closest remaining drilled-hole edge gap: 0.700 mm
- bottom GND coverage: ~80.1%
- top silk-to-mask violations: 0
- minimum measured silk-to-mask clearance: 0.22 mm
- no microphone NPTH by design: MIC1 acoustic port faces TOP/outward

## MIC1
Nominal centres are 2.54 mm horizontal pitch and 7.62 mm row spacing. The exact breakout-manufacturer tolerance drawing was not located, so Rev K uses tolerance-friendly 1.40 mm plated holes / 2.10 mm pads and includes a 1:1 fit template.

Print `MIC1_1to1_FIT_CHECK.svg` at 100% and physically test the actual module/header before a volume order.
