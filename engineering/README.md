# Engineering / Reverse Engineering

This directory contains reference material used while analysing the MGE UPS serial protocol and implementing the controller.

## Structure

- `upstream/nut/` – original Network UPS Tools (NUT) driver sources used as protocol references.
- Future project-specific notes, decoded frames, report maps and measurements should live directly below `engineering/` or in dedicated subdirectories.

## Current protocol finding

The observed binary frames from the UPS match **SHUT (Serial HID UPS Transfer)** rather than a proprietary ad-hoc frame format.

Examples observed in this project:

```text
85 33 02 03 00 01
85 33 02 23 00 21
85 33 02 24 00 26
85 44 16 55 B2 05 F4
```

Relevant interpretation:

- HID report `0x02`: PresentStatus
- HID report `0x16`: RemainingCapacity / RunTimeToEmpty
- SHUT sync tokens in the old driver:
  - `0x16` complete notifications
  - `0x17` light notifications
  - `0x18` notifications off
- SHUT uses `0x06` ACK handling.
- Serial speed used by the old NUT SHUT driver: 2400 baud.

## Upstream sources

The files under `upstream/nut/` are copied unchanged from the Network UPS Tools project for engineering/reference purposes.

### SHUT driver

Source repository:
https://github.com/networkupstools/nut-archive

Pinned revision:
`605fd9416c0bbb9b2e651695df856dc0aab08162`

Files:

- `drivers/mge-shut.c`
- `drivers/mge-shut.h`

### U-Talk comparison driver

Source repository:
https://github.com/networkupstools/nut

Pinned revision:
`1a8369f8688443a500527059167d2f66ca27535f`

Files:

- `drivers/mge-utalk.c`
- `drivers/mge-utalk.h`

U-Talk is kept only as a historical comparison. The binary traffic observed from the UPS matches SHUT/HID.

## Licensing

The copied NUT sources retain their original copyright and GPL license notices. They are reference/upstream material and should not be mistaken for original code of this repository.
