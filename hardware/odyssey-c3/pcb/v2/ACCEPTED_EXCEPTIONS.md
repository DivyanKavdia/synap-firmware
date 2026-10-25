# User-accepted design exceptions and unverified checks

## Accepted **unchanged** by explicit user direction on 2026-10-25
**A. Unregulated LiPo into SuperMini nominal 5 V pad.** Existing circuit is LiPo BAT+ → SPDT → SS14 → U1 `5V`. A 1S LiPo is approximately 3.0–4.2 V and loses additional voltage through SS14. A stable 3.3 V rail is **not guaranteed**. Possible SD write failures, brownouts, boot/recovery issues. This exception is documented, **not fixed**, **not electrically qualified**.

**B. BLE antenna keepout.** The existing SuperMini antenna sits toward the board interior, with copper beneath the indicative region on both carrier layers. BLE range and antenna matching are **not guaranteed**. This exception is documented, **not fixed**, **not RF qualified**.

## Remaining unverified requirements (not converted to PASS)
1. **Physical microSD overhang:** breakout offset 7.5 mm from relevant edge; fully inserted card assumed to extend 4 mm from module. Actual card insertion depth and projection are unmeasured. Verify no PCB-edge protrusion in actual assembly.
2. **USB charger and cell safety:** charger module identification, cell protection, charger load sharing, backfeed and USB programming under battery power untested.
3. **Exact module fit and hand-solderability:** mirrored back-side footprints, header tolerance, charger USB reach, SPDT actuation, front microphone acoustic side, NeoPixel and touch surface orientations unmeasured.
4. **Native Eagle DRC:** not performed. Frozen Gerbers passed custom checks. Avoid re-export until native and third-party CAM are reconciled.
5. **Manufacturer CAM approval:** required; not provided by this package.
6. **Device firmware/hardware qualification:** not powered, flashed or stress-tested. Battery ADC on this PCB uses 470k/470k (gain 2), which differs from earlier firmware divider ratio ≈3.13.

**Order decision:** File package is released for manufacturing a bare board at the requesting user's direction, **not as a certified production-ready assembled device**. The risk of failed boards or unreliable operation remains materially elevated. The first batch should be limited to 5 boards or fewer until built and validated.
