# TouchMT

A user-mode touch screen driver for **MicroTouch serial capacitive controllers** (EX / "Excalibur" family) on Windows XP and later — including **Windows XP Professional x64**, where 3M's MT 7 kernel driver (`TwTouch.sys`) bluescreens.

TouchMT reads the controller on a COM port and turns touches into mouse input with `SendInput()`. There is no kernel driver, so it can't crash the system.

It was built for a Reflex International 17" touch CRT (TSFX72-301C, October 2000) with an external MicroTouch serial controller:

| | |
|---|---|
| Board | MicroTouch Systems 5405790 Rev 2.1 (©1998) |
| ASIC | MicroTouch Excalibur 19-550 Rev 2.0 |
| MCU | Winbond W78C32 |
| Firmware | 5604290 Rev 6.0 |
| `OI` reply | `A30600` |
| `UV` reply | `QMV***00` (serial capacitive, self-test OK) |

## Features

- Works with the controller in **7-bit text mode** (Format Decimal) or **8-bit binary mode** (Format Tablet), switching it to binary when it can
- **16-point grid calibration** with a smooth bicubic fit, which corrects the edge compression of old capacitive overlays
- **Border trace** step: stretches the outer strip of the screen so the furthest point you can physically reach lands on the true screen edge (taskbar, Start button, window controls)
- Optional **controller range widening** using the controller's own `CX` 2-point calibration, for controllers whose factory calibration clips before the screen edge
- Dead zone so taps don't turn into tiny drags, optional hold-for-right-click, smoothing
- Plays the Windows *Default Beep* as each calibration point is accepted, like the original MicroTouch software
- Multi-monitor aware; settings in a plain `touchmt.ini`
- Static, dependency-free binaries (64-bit and 32-bit) that use only XP-era Windows APIs

## Usage

Copy `touchmt-x64.exe` to a folder you can write to (e.g. `C:\TouchMT\`). Make sure nothing else is using the COM port, such as a printer driver, terminal program, or leftover MT 7 / Serial Mouse device.

```
touchmt-x64.exe                     run (calibrates first if needed)
touchmt-x64.exe --calibrate         16-point grid + border trace
touchmt-x64.exe --edges             redo only the border trace
touchmt-x64.exe --reset-controller  factory-reset the controller (8N1, binary), then calibrate
touchmt-x64.exe --widen-controller  rescale the controller's output so edges don't clip, then calibrate
touchmt-x64.exe --debug             console + touchmt.log with raw bytes and decoded packets
touchmt-x64.exe --list              list monitors (for monitor= in touchmt.ini)
touchmt-x64.exe --stop              stop the running copy
```

To start it with Windows, put a shortcut to `touchmt-x64.exe` in your Startup folder.

### Recommended first-time setup for a controller that came configured by an OEM

1. `touchmt-x64.exe --reset-controller` puts the controller into 8-bit binary mode with lift-off reporting.
2. `touchmt-x64.exe --widen-controller` stops the controller's output from clipping before the screen edges.
3. Complete the 16 targets, then trace the border of the glass and press Enter.

## touchmt.ini

| key | default | meaning |
|---|---|---|
| `port` | `COM1` | serial port |
| `baud` | `9600` | baud rate |
| `monitor` | `0` | 0 = primary, else index from `--list` |
| `deadzone` | `8` | px a touch must move before it becomes a drag |
| `rightclick_ms` | `0` | hold still this long for a right click (0 = off) |
| `smoothing` | `1` | 0 = raw; higher = steadier but laggier |
| `edge_snap` | `6` | px from an edge that snaps onto it |
| `edge_band` | `60` | width of the stretched strip at each edge |
| `edge_l/r/t/b` | measured | how far in the glass stops (set by the border trace) |
| `liftoff_timeout_ms` | `0` | release if packets stop arriving for this long |
| `swap_touch_bit` | auto | inverts the touch bit (auto-detected) |

## Protocol notes

- Commands: `<SOH>cmd<CR>`, where SOH is `0x01` (Ctrl+A in a terminal). Replies: `<SOH>reply<CR>`, with `0` = OK and `1` = failed.
- **Format Tablet**: 5-byte packets. Byte 0 has bit 7 set and bit 6 = touching. Then X low 7 bits, X high 7 bits, Y low, Y high (14-bit, origin at the bottom left). A single packet with bit 6 clear marks lift-off.
- **Format Decimal**: `<SOH>xxx,yyy<CR>` text with no lift-off report. TouchMT detects lift-off from a gap in the stream.
- This controller shipped from the OEM in 7-bit mode: on an 8N1 port every byte has bit 7 set, and it refuses `FT` until `PN81` or `RD` is sent.

## Building

On Linux with MinGW-w64:

```sh
./build.sh
```

This produces `touchmt-x64.exe` and `touchmt-x86.exe`, linked statically with subsystem version 5.02 / 5.01 so they load on XP.
