# MAKCM Keyboard Edition

A keyboard-focused fork of the MAKCM firmware. It clones a physical USB
keyboard and re-presents it to a PC through the two ESP32-S3 MCUs on the
MAKCM board, optimised for the lowest possible latency and full
**N-Key Rollover (NKRO)**.

```
Physical keyboard ──USB──▶ [Right MCU / USB Host]
                                   │  Serial1 @ 5 Mbaud (binary protocol)
                                   ▼
                             [Left MCU / USB Device] ──USB──▶ PC
```

- **Right MCU** (`MAKCM_ESP32s3_HID_Keyboard_Right`) — USB **host**. Reads the
  physical keyboard, builds a key bitmap, and forwards it over the inter-MCU
  UART as a compact binary frame.
- **Left MCU** (`MAKCM_ESP32s3_Device_Keyboard_Left`) — USB **device**. Presents
  an NKRO keyboard + media-key device to the PC, cloning the physical
  keyboard's VID/PID/strings.

This is a sibling of the mouse firmware in the repository root; the two share
the same board, handshake, and latency techniques but the keyboard edition
strips the mouse data path entirely.

---

## Features

| Feature | Notes |
|---|---|
| **Full NKRO output** | The PC sees every key at once via a 160-bit key bitmap (usages `0x00`–`0x9F`). No 6-key boot-protocol ceiling. |
| **Anti-ghosting** | A bitmap report cannot ghost — any combination of keys is representable. |
| **Media / consumer keys** | Play/pause, next, prev, stop, mute, vol±, fast-fwd, rewind, eject, email, calculator, browser home/back/forward/search. |
| **Low-latency binary link** | 22-byte binary frame instead of text; ISR-driven serial tasks; no `Serial1.flush()` in the hot path. |
| **1000 Hz preserved** | The USB transfer is copied and resubmitted within the same 1 ms frame so the host keeps polling at full rate. |
| **Key injection** | Press/release/tap/type arbitrary keys from an external controller over Serial0 — injected keys are NKRO too (no 6-key limit). |
| **Composable state** | Physical passthrough and injected keys are OR-combined, so a held physical key and an injected key coexist. |
| **Identity cloning** | The device inherits the real keyboard's VID/PID, USB version and manufacturer/product/serial strings. |

---

## Inter-MCU binary protocol (Right ➜ Left, Serial1 @ 5 Mbaud)

ASCII command bytes are always `< 0x80`, so the high-bit frame markers never
collide with text.

| Marker | Frame | Length | Meaning |
|---|---|---|---|
| `0xAB` | `[0xAB][modifiers][bitmap × 20]` | 22 B | Full keyboard state. Bit *k* of the bitmap = HID usage code *k*. |
| `0xAC` | `[0xAC][cc_lo][cc_hi]` | 3 B | Consumer/media 16-bit bitmap. |

The Left MCU also still speaks the text handshake (`USB_HELLO`, descriptor
JSON, `USB_INIT`, …) used to clone the device identity before enumeration.

---

## Serial0 command reference (external controller ➜ Left MCU)

All commands are newline-terminated. Keycodes are USB HID usage codes
(e.g. `4` = `a`, `0x28`/`40` = Enter, `0xE1`/`225` = Left Shift).

| Command | Action |
|---|---|
| `kb.press(code)` | Hold a key (or modifier `0xE0`–`0xE7`). |
| `kb.release(code)` | Release a key. |
| `kb.releaseall` | Release all injected keys. |
| `kb.tap(code)` | Press then release a key (reliable — reports are delivered in order). |
| `kb.type(text)` | Type an ASCII string (US layout), e.g. `kb.type(Hello World!)`. |
| `kb.isdown(code)` | Query key state (physical OR injected); replies `kb.state(code,0/1)`. |
| `kb.mtap(bit)` | Tap a media key by bit (see table below). |
| `kb.mset(bits)` | Set the injected media bitmap (hold/release; `kb.mset(0)` clears). |

### Media (consumer) bit assignments

| Bit | Key | Bit | Key |
|---|---|---|---|
| 0 | Play/Pause | 8 | Rewind |
| 1 | Next track | 9 | Eject |
| 2 | Previous track | 10 | Email |
| 3 | Stop | 11 | Calculator |
| 4 | Mute | 12 | Browser Home |
| 5 | Volume Up | 13 | Browser Back |
| 6 | Volume Down | 14 | Browser Forward |
| 7 | Fast Forward | 15 | Browser Search |

Example: `kb.mtap(5)` bumps the volume up once; `kb.mset(32)` holds Volume Up
(`bit 5 = 32`), `kb.mset(0)` releases it.

---

## Build & flash

Each MCU is a separate PlatformIO project (same as the mouse edition):

```bash
# Right MCU (USB host)
pio run -d keyboard/MAKCM_ESP32s3_HID_Keyboard_Right -t upload

# Left MCU (USB device)
pio run -d keyboard/MAKCM_ESP32s3_Device_Keyboard_Left -t upload
```

Firmware version string: `KB_V1_0_nkro`.

---

## Latency notes

- **Per-report path**: physical report → 22-byte UART frame (~44 µs at
  5 Mbaud) → single NKRO USB report to the PC. No per-key text formatting,
  no `flush()` stall.
- **NKRO = no split cost**: because the whole key state travels as one bitmap,
  there is never a multi-report split (unlike the 8-bit mouse-axis case), so
  even a full-hand chord is one report with zero added latency.
- `USBHID::SendReport` blocks until the host has polled the report, which
  guarantees ordered delivery for `kb.tap`/`kb.type` without artificial delays.

---

## Known limitations (v1)

- **Physical media keys**: media keys on the *physical* keyboard are usually on
  a separate consumer-control interface which this version does not yet forward.
  Media keys are fully available via injection (`kb.mtap` / `kb.mset`). Physical
  passthrough of that interface is a planned enhancement.
- **Physical NKRO input**: the host-side read follows the standard boot/report
  keycode-array layout (any number of array slots is supported). Keyboards that
  emit a *bitmap*-style NKRO report on their default interface would need report-
  descriptor parsing to decode — a planned enhancement. Output to the PC is
  always full NKRO regardless.
- **Boot protocol**: the device advertises report IDs (NKRO + consumer), so it
  is not a BIOS/UEFI boot keyboard. In-OS use is unaffected.
- Not yet validated on hardware — see the test plan in the PR.
