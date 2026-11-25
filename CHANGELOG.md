# Changelog

## v1.0r8 — 2026-11-25 — first public release

TouchMT is a user-mode driver for MicroTouch serial capacitive touch controllers on Windows XP Professional x64. It replaces 3M's MT 7 kernel driver, which bluescreens on XP x64.

### Why it exists

On XP x64, 3M MicroTouch MT 7 (both the late-2009 build and 7.12.6) crashed with `STOP 0x0000007E` (access violation) in **`TwTouch.sys`** when driving a Reflex International 17" touch CRT (TSFX72-301C, October 2000). XP x64 runs on the Windows Server 2003 kernel, and 3M's 64-bit drivers appear to have been built for Vista and 7.

### How the hardware was identified

- Controller: MicroTouch board **5405790 Rev 2.1** (©1998), **Excalibur 19-550** ASIC, Winbond W78C32 MCU, firmware **5604290 Rev 6.0**.
- The `OI` command returned `A30600` (firmware 6.00).
- The `UV` command returned `QMV***00`: an EX-family serial capacitive controller with no self-test errors.
- The controller had been configured by the OEM for **7-bit serial**, so every byte arrives with bit 7 set on an 8N1 port, and it rejects the binary `FT` (Format Tablet) command.
- Gotcha along the way: COM1 was claimed by an old ImageWriter II printer driver, which made terminal programs fail with "unable to open COM1". This may also have contributed to the original bluescreens.

### Features

- **No kernel driver.** Touches become mouse input through `SendInput()`, so the driver can't bluescreen the system.
- **Static XP-compatible binaries** (x64 and x86), built with MinGW-w64, subsystem 5.02/5.01, importing only kernel32, user32, gdi32 and msvcrt.
- **Both controller data formats:**
  - Format Tablet (8-bit binary, 5-byte packets, explicit lift-off)
  - Format Decimal (7-bit text `xxx,yyy`), with lift-off detected from a gap in the stream
- **Automatic controller setup:** it detects 7-bit mode, sends `PN81` to switch to 8-bit, then `FT` and `MS`, and falls back to Format Decimal if the controller refuses.
- **`--reset-controller`:** sends `RD` (factory defaults: 9600 8N1, Format Tablet, controller calibration cleared).
- **`--widen-controller`:** runs the controller's own `CX` 2-point calibration with targets placed 5% in instead of the assumed 12.5%, so the controller's output no longer clips at 0 before the screen edge.
- **16-point grid calibration** (4×4, outer targets 3.5% from the edges) with a least-squares affine pre-fit and a **bicubic Lagrange surface** inverted by Newton's method. This corrects the edge compression typical of old capacitive overlays on curved CRT glass.
- **Border trace step:** you slide your finger around the bezel, and TouchMT measures where the glass physically stops on each side. It then stretches the outer `edge_band` pixels so the furthest reachable point lands on the true screen edge, making the taskbar, Start button and window controls reachable.
- **Touch handling:** tap dead zone, optional hold-for-right-click, smoothing, and edge snap.
- **Touch-bit polarity auto-detection:** if the touch bit is reversed, TouchMT flips it and saves the setting.
- **Windows "Default Beep"** as each calibration point is accepted, like the original MicroTouch and SMART Board calibration tools.
- **Diagnostics:** `--debug` opens a console and writes `touchmt.log` with raw serial bytes, decoded packets and controller replies. The calibration screen shows live byte and packet counters.
- **Quality of life:** single instance, `--stop`, `--list` for multi-monitor setups, and plain-text `touchmt.ini` settings.

### Development history

| rev | change |
|---|---|
| r1 | First version: Format Tablet parser, 4-point affine calibration, `SendInput` injection |
| r2 | Fixed the debug console (direct `WriteConsoleA` plus `touchmt.log`); live data on the calibration screen; touch-bit auto-detection |
| r3 | Found that the controller was in 7-bit mode and rejected `FT`; added `PN81` switching and a Format Decimal fallback with gap-based lift-off. **First successful calibration.** |
| r4 | Corner reach: targets moved closer to the edges, `edge_snap`, and `--reset-controller` |
| r5 | Replaced the 4-point calibration with a 16-point grid and smooth non-linear mapping |
| r6 | Debug log showed the controller clipping at raw 0,0 short of the edge; added `--widen-controller` (CX rescale) |
| r7 | Debug log showed the glass physically ends about 30 px short of the image under the bezel; added the border-trace edge stretch. **Taskbar, Start button and window controls reachable.** |
| r8 | Default Beep on each accepted calibration point; README, build script and MIT license for public release |

### Recommended setup

1. Uninstall MT 7 completely and make sure nothing else is using the COM port.
2. Run `touchmt-x64.exe --reset-controller`.
3. Run `touchmt-x64.exe --widen-controller`.
4. Complete the 16 targets, trace the border and press Enter.
5. Put a shortcut to `touchmt-x64.exe` in your Startup folder.
