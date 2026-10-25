# Synap Odyssey C3 — PCB v2 (Rev L v3 compact carrier)

**Version name:** PCB v2  
**Physical revision:** Rev L v3  
**Dimensions:** 40 × 45 mm, 2-layer FR4, 1.6 mm, 1 oz copper  
**Assembly:** two-sided carrier PCB, hand soldered; all existing ESP32-C3 SuperMini, SD, INMP441, TTP223, NeoPixel, switch and charger breakout modules retained.  
**Source:** Eagle `.brd` in the reconstructed archive.  
**Fabrication outputs:** Gerber/drill files in the reconstructed archive.

## Retrieve manufacturing files

The binary package is stored as **lossless base64 chunks**, because the GitHub connector imposes per-blob upload limits. The source and all nine Gerber/drill outputs are preserved without altering their bytes.

```bash
bash REBUILD_V2.sh
```

This extracts `reconstructed/Synap_Odyssey_C3_Carrier_RevL_v3_40x45.brd`, the Gerber/drill files and supporting production docs. It also produces `reconstructed/Synap_Odyssey_C3_Carrier_PCB_V2_GERBERS.zip` for uploading to a PCB manufacturer. The regenerated ZIP is **content-equivalent, not byte-identical**, to the original ZIP because ZIP compression/metadata differ. File checksums in `RELEASE_MANIFEST.json` permit verification of the original Gerber/drill file bytes.

## Validation and limits

The user chose to release this board while accepting two known unresolved engineering risks:

1. **Power**: LiPo battery through SS14 into the C3 SuperMini's `5V` input may fail at low voltage or peak SD current.
2. **BLE antenna**: the antenna region overlaps copper routing, and RF performance has not been qualified.

Other outstanding qualification steps: actual SD-card inset and all breakout footprints/port positions, native Eagle DRC, manufacturer CAM verification and assembled first-article functional testing. DFM geometric checks passed on the supplied Gerber set; that does **not** certify a production-qualified electrical/mechanical design.

## Version history

- PCB v2 = Rev L v3, 40 × 45 mm (current user-selected Gerber release).
- Rev K = 42 × 60 mm earlier carrier; retained intact under `../final/` for historical traceability.

Do not change Gerbers without rerunning copper connectivity, DFM and drill audits.
