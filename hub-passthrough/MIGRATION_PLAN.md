# MAKCM Hub Passthrough — Right MCU migration plan

Goal: make the **Right MCU** (USB host) read a **mouse *and* a keyboard connected
through an external USB hub** at the same time, and forward both to the Left MCU
over the existing UART link — so the PC sees a composite mouse+keyboard.

This is the one capability that is **impossible on the current firmware** and why.

---

## 1. Why it was blocked, and what changed

The current firmware builds against **arduino-esp32 2.0.16 → ESP-IDF 4.4.7**. The
ESP-IDF 4.4 USB Host Library supports **only a single directly-attached device** —
there is no external-hub driver and no config option to add one. So a hub with a
mouse + keyboard never enumerates its downstream devices; the Right MCU literally
never receives their reports. No amount of firmware logic can fix that on 4.4.

What changed upstream:

| ESP-IDF | External hub driver | Low-Speed device behind a Full-Speed hub |
|---|---|---|
| 4.4.x | none | — |
| 5.3 | scaffolding, "under development" | no |
| 5.4.0 | opt-in, `USB_HOST_HUBS_SUPPORTED` | **not supported** (explicit limitation) |
| **5.4.4 / 5.5+ / 6.0.x** | opt-in | **supported** (limitation removed, release note #15683) |

The ESP32-S3 is a **Full-Speed-only** USB host. The one remaining hub limitation in
5.5/6.0 ("No Transaction Translator") applies **only to High-Speed hosts (ESP32-P4)**
and is structurally irrelevant to the S3: a USB-2.0 hub attached to an FS host runs
in FS mode and passes FS/LS devices through natively.

**Net:** on the ESP32-S3, from **ESP-IDF ≥ 5.5** (or 5.4.4), with the hub driver
enabled, a mouse and keyboard behind a full-speed hub both enumerate and work.

---

## 2. Key constraint that shapes the whole plan

`CONFIG_USB_HOST_HUBS_SUPPORTED` defaults to **off**, and **cannot be enabled from a
stock Arduino/PlatformIO project** because arduino-esp32 ships *precompiled* ESP-IDF
libraries (the flag is baked in at their build time, off). Enabling it requires a
build that compiles ESP-IDF from source with the flag on. That means one of:

- **(chosen) Native ESP-IDF** for the Right MCU — full control of `menuconfig`, and
  the cleanest path to the USB host + hub + HID components.
- **Arduino-as-ESP-IDF-component** — keep Arduino APIs but build under ESP-IDF so
  `menuconfig` is available. Viable fallback, finickier build; see §8.

We choose **native ESP-IDF** for the Right MCU. The reason it's not as large as it
sounds: the existing Right-MCU firmware **already calls the raw ESP-IDF USB Host API
directly** (`usb_host_install`, `usb_host_device_open`, `usb_host_interface_claim`,
`usb_host_transfer_submit`, …). Only the *thin* Arduino conveniences need porting
(Serial → UART driver, `millis()` → `esp_timer`, etc.). And we replace the
hand-rolled HID parsing with Espressif's official **USB HID Host component**, which
natively supports multiple HID devices.

---

## 3. Scope — only the Right MCU moves

| Board half | Role | Migration |
|---|---|---|
| **Right MCU** | USB **host** (reads mouse+keyboard) | **Yes** — native ESP-IDF 5.5+, hub enabled |
| **Left MCU** | USB **device** (composite mouse+keyboard to PC) | **No change** — TinyUSB device + composite HID already works fine on IDF 4.4 |

The two firmwares are independent programs that only talk over UART. Keeping the
**UART protocol identical** means the Left MCU (and the PC-facing behaviour) does not
change at all. This is the single most important design rule of the migration.

### UART protocol to preserve (Right → Left, Serial1 @ 5 000 000 baud, 8N1)

The Left MCU already parses these. The new Right firmware must emit the **same bytes**:

- **Mouse**, 7-byte binary frame:
  `[0xAA][buttons][X_lo][X_hi][Y_lo][Y_hi][wheel]`
  (X/Y are `int16_t` little-endian; wheel is `int8_t`.)
- **Keyboard**, text line (current base firmware):
  `kb.report(mods,k0,k1,k2,k3,k4,k5)\n`
  *Optional upgrade:* switch to the keyboard-edition binary/NKRO frame
  `[0xAB][mods][bitmap×20]` — but only if the Left MCU is also moved to the NKRO
  device. For a first cut, **keep `kb.report(...)` text** so the unchanged Left MCU
  works as-is.
- **Handshake / identity** lines (`USB_HELLO`, the `send*` descriptor JSON, `USB_INIT`,
  `USB_GOODBYE`, `READY`, …) must be reproduced so the Left MCU's cloning handshake
  still completes. For an MVP you may stub identity with a fixed VID/PID and add the
  full clone back in Phase 4.

---

## 4. Target toolchain

- **ESP-IDF v5.5.x** (newest stable line with the hub fix; v5.5.5 at time of writing).
  v6.0.2 also works but has no stable Arduino core and buys nothing here — prefer 5.5.
- **Target:** `esp32s3`.
- **USB HID Host component:** `espressif/usb_host_hid` (managed component, pulled via
  `idf_component.yml`). *Exact version pinned in the skeleton's `idf_component.yml`.*
- Build/flash with `idf.py` (not PlatformIO's Arduino flow).

---

## 5. Critical configuration (`sdkconfig.defaults`)

The decisive flags (full file in the skeleton):

```ini
CONFIG_IDF_TARGET="esp32s3"

# --- External USB hub support ---
CONFIG_USB_HOST_HUBS_SUPPORTED=y      # THE required flag (devices behind a hub)
CONFIG_USB_HOST_HUB_MULTI_LEVEL=y     # only for hub-behind-hub; default-on, harmless
```

Only `CONFIG_USB_HOST_HUBS_SUPPORTED=y` is strictly required — the official HID-host
example's entire `sdkconfig.defaults` is that one line. `CONFIG_USB_HOST_HUB_MULTI_LEVEL`
concerns **cascaded** hubs (a hub plugged into a hub), not one hub carrying two
devices; it defaults to `y` anyway.

There is **no USB-host channel-count config** in ESP-IDF 5.5 — the S3's USB-DWC core
has **8 host channels in hardware**, and hub + mouse + keyboard uses only a few, so
there is nothing to raise. The only loosely related tunable is
`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` (default 256); bump to 512 only if a
device's config descriptor exceeds 256 bytes (rare). USB-OTG host mode needs no
switch — calling `usb_host_install()` puts the OTG controller in host mode.

---

## 6. Phased bring-up (each phase is independently verifiable on hardware)

Because I cannot flash or test here, the plan is built as small, checkable steps.
**Do them in order and stop at the first that fails** — that isolates problems.

- **Phase 0 — Toolchain.** Install ESP-IDF 5.5.x, `idf.py set-target esp32s3`, build &
  flash a blink/hello. Confirms toolchain + board flashing before any USB work.

- **Phase 1 — Single HID device, no hub.** Bring up `usb_host` + `usb_host_hid`
  component. Plug a mouse **directly**. Log its raw input reports over the console.
  Confirms the USB host + HID component + your board's USB-OTG wiring work.
  *(This already worked on 4.4, so a regression here means a toolchain/wiring issue.)*

- **Phase 2 — Hub + two devices (the milestone).** Enable the hub flags (§5). Plug a
  **hub** with a mouse **and** a keyboard. Confirm **two** HID connect events fire and
  both stream raw reports. **This is the capability that was impossible before** —
  verify it in isolation before wiring any forwarding.

- **Phase 3 — Forward to Left MCU.** Emit the mouse `0xAA` binary frame and the
  `kb.report(...)` keyboard line over Serial1 to the (unchanged) Left MCU. Confirm the
  PC sees mouse movement **and** keystrokes simultaneously. End-to-end passthrough.

- **Phase 4 — Identity cloning.** Re-add reading the device VID/PID/strings and the
  descriptor handshake so the PC-facing device clones the real hardware (mirror the
  keyboard-priority logic from the current firmware).

- **Phase 5 — Robustness.** Hotplug (unplug/replug through the hub), error handling,
  disconnect cleanup, LED/status, remove debug logging.

---

## 7. Risks & honest caveats

- **Untested by me.** I can write correct-by-construction code against the official
  API, but I cannot compile or flash it. Expect hardware iteration, especially Phase 2.
- **Hub driver is not hardened.** ESP-IDF's external-hub driver (5.5/6.0) documents
  gaps: no remote wakeup, limited overcurrent/error handling, no interface selection,
  no downstream-port debounce. The happy path (hub + a couple of HID devices) works;
  messy hotplug/error cases may not.
- **Channel budget.** The S3 OTG has a limited channel count; hub + mouse + keyboard
  is fine, but this caps how many devices you can add.
- **Device speed.** FS and LS devices behind an FS hub are supported (that's the 5.5
  fix). Exotic/High-Speed-only peripherals are still out (S3 is FS-only silicon).
- **Two toolchains in the repo.** Right MCU on ESP-IDF, Left MCU on PlatformIO/Arduino.
  That's fine (separate binaries), but document it so builds don't get confused.

---

## 8. Fallback approach — Arduino as an ESP-IDF component

If the native port of the Right MCU's non-USB code (UART, JSON, ring buffers) proves
too costly, an alternative keeps the Arduino APIs:

- Create an ESP-IDF project, add `espressif/arduino-esp32` **3.3.x** (ESP-IDF 5.5) as a
  managed component. You get `menuconfig` (so you can set `CONFIG_USB_HOST_HUBS_SUPPORTED=y`)
  **and** `#include <Arduino.h>` (Serial, etc.).
- Port the existing Right-MCU sketch from arduino-esp32 **2.0.x → 3.3.x** (breaking
  changes, mostly around the USB/host and some peripheral APIs).
- Trade-off: less code rewriting, but a more delicate build, and you inherit arduino's
  precompiled-vs-source nuances. Use this only if §2's native path stalls.

---

## 9. What the skeleton in this folder provides

`right-mcu-idf/` is a **minimal, native ESP-IDF project** that targets Phases 1–3:

- `sdkconfig.defaults` with the hub flags and USB host budget.
- `main/` that installs the USB host + HID host component, handles connect events for
  **multiple** HID devices, reads raw input reports, and forwards them to the Left MCU
  over UART using the **existing binary/text protocol**.
- It is a **starting point**, not a finished firmware: identity cloning (Phase 4) and
  robustness (Phase 5) are stubbed with clearly marked `TODO`s and pointers to the
  corresponding code in the current `MAKCM_ESP32s3_HID_Mouse_Right`.

Build/flash instructions are in `right-mcu-idf/README.md`.
