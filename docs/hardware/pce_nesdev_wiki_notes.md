# Notes from the PCEdev Wiki (pce.nesdev.org)

Source: https://pce.nesdev.org — CC0 content. **Summary made with a web reader on 2026-10-17, not a
literal copy.** Confirm critical values in the Geargrafx code.

## Virtual × physical addresses ("CPU addresses" page)

- The CPU sees a 16-bit **virtual** address ($0000–$FFFF), split into 8 pages of 8 KiB.
- The MMU maps each page to one of 256 physical 8 KiB **banks**, through the MPR0–MPR7 registers.
- **Physical** address = 21 bits = bank (8 bits) + offset (13 bits). E.g.: bank $81, offset $1FFF → $103FFF.

## Fixed locations in the virtual space ("CPU memory map" page)

| Virtual | Use |
|---|---|
| $2000–$20FF | Zero page |
| $2100–$21FF | Stack |
| $FFF6 | IRQ2 / BRK vector |
| $FFF8 | IRQ1 vector (VDC — e.g. VBlank) |
| $FFFA | Timer vector |
| $FFFC | NMI vector |
| $FFFE | Reset vector |

## Physical banks (basic PC Engine)

| Bank | Contents |
|---|---|
| $00–$7F | HuCard (ROM) |
| $F8 | 8 KiB of RAM |
| $F9–$FB | RAM mirrors |
| $FF | Hardware registers |
| others | Unmapped (may be used by expansions/CD; see the original table) |

## Registers in bank $FF (offsets)

| Offset | Device |
|---|---|
| $0000–$03FF | VDC (4 registers, mirrored) |
| $0400–$07FF | VCE (8 registers, mirrored) |
| $0800–$0BFF | PSG |
| $0C00–$0FFF | Timer (2 registers, mirrored) |
| $1000–$13FF | I/O port / joypad (1 register, mirrored) |
| $1400–$17FF | Interrupt controller (4 registers, mirrored) |
| $1800–$180F | CD-ROM interface (CD only; mirroring unknown) |

SuperGrafx: $0000–$0007 VDC1, $0008–$000F VPC, $0010–$0017 VDC2, $0020–$03FF mirrors.

Note: most HuCards do not decode a mapper and use the highest address bit as chip enable.

## NTSC clocks ("Timing" page)

| Signal | Value |
|---|---|
| Master clock | 6 × NTSC color burst = 315/88 MHz × 6 ≈ 21.47727 MHz |
| Fast CPU | master ÷ 3 ≈ 7.15909 MHz |
| Slow CPU | master ÷ 12 ≈ 1.78978 MHz |
| Timer | master ÷ 3072 ≈ 6.991 kHz |
| VCE pixel clock "10 MHz" / "7 MHz" / "5 MHz" | master ÷ 2 / ÷ 3 / ÷ 4 |
