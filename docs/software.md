# Firmware setup and validation

## Source and dependencies

Open `firmware/NaaniCrush/NaaniCrush.ino` in Arduino IDE. Install the Espressif ESP32 board package and LovyanGFX. Wire, Preferences, Arduino, ESP timer, and FreeRTOS APIs are supplied by the ESP32 platform rather than separate application packages.

Original toolchain versions and a confirmed Arduino board profile were not supplied. The sketch requires classic ESP32 DAC support and uses `skip_unhandled_events` in its timer configuration; do not assume every ESP32-family target or core version is compatible. Record a successful build configuration before pinning versions.

Serial output uses 115200 baud. Saved preferences use namespace `naani` and keys `cards`, `level`, and `sound`.

## Public-source changes

Ten embedded personal JPEG arrays and their populated lookup table were removed. `PHOTO_COUNT` is zero, and a single null sentinel keeps the table declaration valid. The original `drawCardFace()` condition routes all cards through its existing text-only fallback. The rest of the sketch, including gameplay and card messages, is unchanged. The original upload remains outside the repository.

## Validation status

Source inspection and comparison with the uploaded sketch completed. No Arduino/ESP32 compiler or connected target board is available in this workspace, so compilation, flashing, audio, display, and touch behavior have not been validated.

## Hardware smoke-test checklist

- Verify and upload using the confirmed board profile; record core and library versions.
- Check landscape display layout and touch mapping at each edge.
- Confirm an invalid swap reverses and does not consume a move.
- Confirm successful swaps clear matches, collapse/refill the board, and resolve cascades.
- Exercise win, loss, level selection, and collection navigation.
- Confirm every public memory card renders its text fallback and flips correctly.
- Toggle audio and check effects during drawing and match resolution.
- Power-cycle after unlocking a level/card and changing sound preference; verify restoration.

Record outcomes and test hardware. This checklist describes work to run, not checks already passed.
