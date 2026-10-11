# Odyssey C3+SD assembled resistor confirmation — 2026-10-11

The actual sealed Odyssey C3+SD unit has been inspected and its fitted battery divider is **R1=1 MΩ (BAT+ to GPIO1 ADC)** and **R2=470 kΩ (GPIO1 ADC to GND)**. This has been physically confirmed by the hardware owner.

The archived Rev K fabrication BOM lists R1/R2 as 470k/470k and must remain an accurate historical description of that archived design. It is **not** authoritative for the assembled field C3+SD variant. For the assembled device, use the voltage divider coefficient `(1000000 + 470000)/470000 = 147/47`, corresponding to existing firmware `1470/470`. C3 boot SD mount/read/write eligibility must never depend on battery ADC readings. Voltage thresholds used for low-battery safety still require trustworthy sampling.

No PCB trace change, C3 pin reassignment, SD formatting or replacement of existing recordings is implied by this field variant documentation.
