# MAKCM Hub Passthrough — ultra-compatible mouse + keyboard edition

A self-contained firmware **pair** for running a **mouse *and* a keyboard through a
single external USB hub** on the MAKCM, and passing both through to the PC as one
composite HID device.

This is the "ultra-compatible" edition: it targets the **widest device support**
(16-bit mouse axes for high-DPI gaming mice + a standard boot-compatible keyboard
that works everywhere, including BIOS/KVMs).

```
[Mouse]  ┐
         ├─ HUB ─→ [Right MCU: USB host, ESP-IDF] ─UART→ [Left MCU: USB device, Arduino] ─→ PC
[Keyboard]┘
```

> **Why this needs a new Right MCU firmware:** the mouse-only/keyboard-only editions
> use arduino-esp32 2.0.x (**ESP-IDF 4.4**), whose USB host stack has **no external-hub
> support** — two devices behind a hub never enumerate. Reading a mouse **and** a
> keyboard through a hub requires **ESP-IDF ≥ 5.5** with the hub driver enabled, which
> can't be done from Arduino/PlatformIO. So the Right MCU here is a **native ESP-IDF**
> project. The Left MCU is unchanged (a composite device works fine on 4.4).
> Full background: [`MIGRATION_PLAN.md`](MIGRATION_PLAN.md).

## Contents

| Folder | MCU | Framework | Role | Status |
|---|---|---|---|---|
| [`right-mcu-idf/`](right-mcu-idf/) | Right (host) | **native ESP-IDF 5.5** | Reads mouse+keyboard behind a hub, forwards over UART | **New / experimental** — builds against verified API, needs on-hardware bring-up |
| [`left-mcu/`](left-mcu/) | Left (device) | PlatformIO / Arduino 2.0.x | Presents a composite mouse+keyboard to the PC, clones identity | **Proven** — the existing composite Left, unchanged |

## The two halves pair over UART — keep the protocol identical

The Left MCU already parses these; the Right MCU emits exactly these bytes:

- **Mouse:** `[0xAA][buttons][X_lo][X_hi][Y_lo][Y_hi][wheel]` (X/Y `int16_t` LE, 16-bit)
- **Keyboard:** `kb.report(mods,k0,k1,k2,k3,k4,k5)\n`
- **Handshake / identity:** `USB_HELLO`, descriptor JSON, `USB_INIT`, `READY`, `USB_GOODBYE`

This is the contract. As long as the Right MCU produces these, the Left MCU (and the
PC-facing device) needs no changes.

## Compatibility choice: standard keyboard, not NKRO

The Left here uses the **standard boot-compatible keyboard** (`USBHIDKeyboard`, 6-key
rollover) plus the **16-bit mouse** (`USBHIDMouse16`). Rationale for "ultra
**compatible**":

- The 16-bit mouse covers high-DPI gaming mice without truncation.
- A boot-compatible keyboard works with **everything** — including BIOS/UEFI and KVM
  switches that sometimes choke on NKRO. NKRO is more *capable* (all keys at once) but
  occasionally less *compatible*.

**Want NKRO instead?** The `keyboard/` edition already has a ready `USBHIDKeyboardNKRO`
class. Swapping it in here is a small, well-defined change (compose it with
`USBHIDMouse16`, set the consumer report ID to `0x03` to avoid colliding with the
mouse's `0x02`, and have the Right MCU emit the `[0xAB][mods][bitmap×20]` frame instead
of `kb.report`). Ask and I'll wire it up.

## Build

**Right MCU** (native ESP-IDF — see [`right-mcu-idf/README.md`](right-mcu-idf/README.md)):
```bash
cd right-mcu-idf
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```
Requires **ESP-IDF v5.5.x** installed (this is Phase 0 of the plan).

**Left MCU** (PlatformIO / Arduino — same as the other editions):
```bash
pio run -d left-mcu -t upload
```

## Status & honest caveats

- The **Left MCU is proven** (it's the existing composite firmware).
- The **Right MCU is a starting point**, built against the verified ESP-IDF 5.5
  `usb_host_hid` API but **not yet compiled/flashed by us**. Expect hardware iteration,
  especially the "hub + two devices enumerate" milestone (Phase 2 in the plan).
- Right-MCU `TODO`s: identity cloning, non-boot/16-bit mouse report parsing, physical
  media keys, hotplug robustness. See the plan and the code comments.
- The external hub must be **self-powered** (the S3 host port doesn't power downstream
  devices).

Start with the plan: [`MIGRATION_PLAN.md`](MIGRATION_PLAN.md).
