# Bare PCB order specifications — Odyssey C3 Rev L v3

| Field | Selection |
|---|---|
| PCB outline | **40 mm × 45 mm**, rectangular/profile exactly as GKO |
| Layer count | 2 copper layers |
| Base material | FR-4 |
| Thickness | **1.6 mm** |
| Copper weight | **1 oz outer layers** |
| Solder mask | Green, both sides |
| Silkscreen | White, both sides |
| Surface finish | **Lead-free HASL** is an economical hand-soldering option; ENIG also acceptable if board house prefers |
| Drill | Plated through-holes per included Excellon `_PTH.TXT`; **61 total plated holes** including **23 vias** |
| Quantity | First article **5 pcs** preferred |
| Assembly | Bare PCB only, no SMT assembly required from factory |
| Impedance control | Not specified |
| Panelization | Board-house standard, preserve true outline and edge clearance |
| Fiducials | Not required for manual assembly |

Files in manufacturing ZIP: GTL (top copper), GBL (bottom copper), GTS/GBS (top/bottom mask), GTO/GBO (top/bottom legend), GTP (top paste; **do not order paste stencil unless desired**), GKO (routed edge), `_PTH.TXT` (Excellon plated drill).

## Independently checked DFM snapshot (custom parser, **not native Eagle DRC**)
- 37 machine checks PASS; 0 machine checks FAIL; 8 functional/mechanical/process holds.
- 61 PTH holes match source; all 23 vias included.
- Minimum copper spacing: top 0.325 mm, bottom 0.2783 mm.
- Minimum trace width: 0.30 mm.
- Copper-to-edge minimum: top 0.575 mm, bottom 0.450 mm.
- Minimum modeled green solder-mask web: 0.190 mm.
- No detected shorts, opens, or silkscreen collisions in frozen CAM.
- Manufacturer must verify import polarity, layer mapping, drill sizes/registration, minimum drill-to-copper/slot rules, and confirm CAM acceptance.

**Do not repour Gerbers from editable Eagle board for this order.** Submit the supplied exact Gerber/drill ZIP. Supplier should report any DFM issues before manufacture.
