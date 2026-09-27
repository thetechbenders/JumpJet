# JumpJet Controller Rev A.4.1 - multisheet draft

Open `JumpJet_Controller_RevA4.kicad_pro`, then open the schematic.

The design is split into four functional sheets:

1. Power, protection, and switched outputs
2. ESP32-S3 controller and service interface
3. Thermistor signal conditioning
4. Qwiic, expansion, and test points

Inter-sheet signals use global labels.

## Programmatic schematic repair - 2026-08-25

This copy was repaired from the supplied Rev A.4.1 multisheet draft without changing the intended product architecture or treating provisional choices as finalized.

Structural fixes include:

- Corrected stock-symbol rotations where the generated symbol orientation did not match the wire geometry.
- Corrected generated anchor offsets for the 24 V to 5 V module, service header, Qwiic connector, and expansion header.
- Re-terminated wires and labels at actual KiCad electrical pin endpoints rather than symbol body edges.
- Repaired the existing 74AHCT125 buffer topology so each used channel has its intended PWM input, driven output, and active-low OE tied low; the unused channel input/OE remain low and its output remains explicitly NC.
- Restored the intended heater, blower, optional AUX, thermistor, Qwiic, service-header, and test-point connectivity.
- Removed redundant ADC alias labels that could create ambiguous net naming.
- Preserved the custom ESP32-S3 SuperMini 2x9 symbol/footprint numbering and the existing provisional GPIO assignment.

`REPAIR_AUDIT.txt` records the programmatic structural/connectivity checks performed on this repaired copy.

## Still provisional / must be engineered before fabrication

This is **not fabrication-approved**. In particular, the following remain open or require independent verification:

- `MOD1` exact converter module, pinout, transient behavior, and `BUCK_EN` policy
- Main/heater/Qwiic fuse selection and protection coordination
- TVS selection and clamp behavior versus the PSU and downstream devices
- MOSFET exact part numbers, gate-drive suitability, switching loss, SOA, thermal design, and derating
- Blower polarity, flyback strategy/component ratings, and airflow adequacy (nominal current now confirmed at ~0.36 A @ 24 V)
- PTC heater cold-start current and operating current
- Connector footprints, contact current ratings, wire gauge, PCB copper/trace/plane sizing, and clearances
- Thermistor beta/Steinhart-Hart coefficients, ADC range, fault thresholds, and calibration
- Thermal fuse / resettable thermal switch physical placement and system-level cutoff behavior
- ESP32 GPIO assignments, strapping constraints, and expansion pinout as final design choices

The repaired files passed a custom structural/connectivity audit, but `kicad-cli` is not available in the repair environment. Open the project in KiCad and run ERC before treating the schematic as an engineering baseline.


## Presentation-only cleanup pass

This copy was generated from the electrically repaired Rev A.4.1 baseline. The pass changes only visible schematic presentation (Reference/Value property positions, label justification, and annotation placement). It intentionally does not move electrical anchors, symbol bodies, wire endpoints, junctions, no-connect markers, or net labels. The same structural/connectivity audit must pass after this pass.


## Depth-safe final schematic polish

This copy supersedes the earlier POLISHED archive. The earlier geometry pass accidentally relied on whitespace indentation to identify top-level KiCad objects; several repaired wires/labels had noncanonical indentation and were therefore left behind visually. This pass starts again from the connectivity-validated PRESENTATION baseline and identifies top-level objects from s-expression parenthesis depth, then applies the same rigid functional-block translations to every relevant object regardless of whitespace. No electrical design intent is changed.

### Final cleanup correction

The first POLISHED archive should be considered superseded. A whitespace-sensitive geometry pass had left several valid top-level wires/labels at their pre-polish coordinates because older repair scripts had given those objects noncanonical indentation. The corrected polish identifies top-level objects by KiCad s-expression depth, normalizes their indentation for auditing, and removes two stale local ADC alias labels (`OUT_ADC`, `CASE_ADC`) that conflicted with the canonical `TEMP_OUT_ADC` and `TEMP_CASE_ADC` nets. The complete structural/connectivity audit passes after these corrections.

## J2 heater terminal selection
J2 is assigned to Phoenix Contact MKDS 1,5/2 (1715022), 5.00 mm pitch, 17.5 A nominal, using KiCad footprint `TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-2_1x02_P5.00mm_Horizontal`. The nominal heater load is ~8.3 A at 24 V. The heater's short fabric/nylon-jacketed leads may be extended externally with a suitably rated Wago-style splice; provide strain relief and use conductor/ferrule sizes allowed by the terminal and splice manufacturers.


## J3 blower connector selection
J3 is assigned to the standard 2-pin JST-XH board header footprint `Connector_JST:JST_XH_B2B-XH-A_1x02_P2.50mm_Vertical`. The confirmed blower load is approximately 0.36 A at 24 V, comfortably within the JST-XH current class for this use. The intended mating housing is JST XHP-2 with suitable crimp contacts. Electrical connectivity and pin numbering are unchanged; verify final blower polarity before building the harness.
