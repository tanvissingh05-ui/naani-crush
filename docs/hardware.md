# Hardware configuration

Source target: ESP32-2432S022C. This is the sketch's declared target, not a board identified or tested in this workspace.

## Source-configured connections

| Function | GPIO / setting |
| --- | --- |
| Display controller | ST7789, 240×320, rotation 1 |
| Display bus | Parallel 8-bit, configured write frequency 25 MHz |
| WR / RD / RS | 4 / 2 / 16 |
| D0 / D1 / D2 / D3 | 15 / 13 / 12 / 14 |
| D4 / D5 / D6 / D7 | 27 / 25 / 33 / 32 |
| Display CS | 17 |
| Display reset | -1: no reset GPIO configured |
| Backlight | 0, driven HIGH |
| Touch SDA / SCL | 21 / 22 |
| Touch address | 0x15, CST816 register layout |
| Audio DAC | 26 |

The audio comment references the board's speaker amplifier. Exact amplifier, speaker, touch variant, and board revision are unspecified. This table describes firmware configuration; it is not an independently verified wiring schematic.

## Still needed

- Board revision and confirmed component specifications.
- Verified wiring or board schematic and electrical limits.
- Battery, charger, protection, power switching, and enclosure details.
- Assembly photos and measured power consumption/runtime.

Do not infer battery connections from the firmware. Confirm the physical hardware before flashing or modifying wiring.
