# MAKCM Right MCU — hub-enabled USB host (native ESP-IDF skeleton)

A minimal, native **ESP-IDF** firmware for the **Right MCU** that reads a mouse
**and** a keyboard through an external USB hub and forwards both to the Left MCU
over UART — the capability that is impossible on the current arduino-esp32 2.0.x
(ESP-IDF 4.4) firmware.

This is the **Phase 1–3 skeleton** described in [`../MIGRATION_PLAN.md`](../MIGRATION_PLAN.md).
It is a **starting point to build/flash/iterate on hardware**, not a finished
firmware. Read the plan first.

> ⚠️ This has **not** been compiled or hardware-tested (built against the verified
> ESP-IDF 5.5 `usb_host_hid` API, but you are the first to run it). Expect iteration,
> especially at Phase 2 (hub + two devices).

## What it does

- Installs the USB Host Library with **external hub support enabled**
  (`CONFIG_USB_HOST_HUBS_SUPPORTED=y` — the one flag that unlocks this).
- Uses Espressif's official `espressif/usb_host_hid` driver; each HID interface
  behind the hub (mouse, keyboard, …) raises its own connect event.
- Forces **boot protocol** so it gets fixed report layouts, then forwards:
  - **mouse** → 7-byte binary frame `[0xAA][buttons][X_lo][X_hi][Y_lo][Y_hi][wheel]`
  - **keyboard** → text line `kb.report(mods,k0,k1,k2,k3,k4,k5)\n`
- These are the **exact bytes the current Left MCU already parses**, so the Left
  MCU (and the PC-facing device) need no changes.

## What it does NOT do yet (marked `TODO` in `main/hub_host_main.c`)

- **Identity cloning** (Phase 4): reading the device VID/PID/strings and running the
  `USB_HELLO` / descriptor-JSON / `USB_INIT` handshake so the PC sees the real
  hardware's identity. Until then the Left MCU uses whatever default identity it has.
- **Non-boot / high-DPI mice** (16-bit axes): boot protocol caps deltas at ±127.
  Port the 8/12/16-bit axis parsing from `MAKCM_ESP32s3_HID_Mouse_Right`.
- **Physical media/consumer keys**, hotplug robustness, LED/status.

## Prerequisites

- **ESP-IDF v5.5.x** installed and exported (`. $IDF_PATH/export.sh`).
  v5.5 is the first stable line where low-speed devices work behind a full-speed
  hub on the ESP32-S3. (v5.4.4 or v6.0.x also work; 5.5 is the sweet spot.)
- An **external USB hub with its own 5 V power** (VBUS) for the downstream devices.

## Build & flash

### Option A — PlatformIO (recommended; verified building)

This project ships a `platformio.ini` using the **pioarduino** platform, which
provides **ESP-IDF 5.5.4** and, crucially, builds `framework = espidf` **from source**
so the hub flag in `sdkconfig.defaults` takes effect. It reuses an existing PlatformIO
install (its own Python) — no native ESP-IDF setup needed.

```bash
# from this folder:
pio run                              # first run downloads IDF 5.5.4 toolchain (~1 GB, one-time)
pio run -t upload                    # flash (add --upload-port COMx if not auto-detected)
pio device monitor -b 115200         # serial log
```

Verified: compiles cleanly, and the generated `sdkconfig.right_hub` contains
`CONFIG_USB_HOST_HUBS_SUPPORTED=y`.

### Option B — native ESP-IDF

Requires **ESP-IDF v5.5.x** installed and exported. The `main/` + top-level
`CMakeLists.txt` layout is also a standard native project:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Either way, the first build downloads the `espressif/usb_host_hid` managed component
automatically (from `main/idf_component.yml`).

## Wiring / hardware notes

- **Inter-MCU UART** (to the Left MCU): defaults to `UART1`, **TX = GPIO1, RX = GPIO2,
  5 000 000 baud, 8N1** — matching the base Right firmware. Change the `LINK_UART_*`
  defines in `main/hub_host_main.c` if your board differs.
- **USB host** uses the ESP32-S3 internal PHY (D+ = GPIO20, D- = GPIO19).
- The console/logs go out the default port (USB-Serial-JTAG or UART0) — a **separate**
  peripheral from the USB-OTG host, so they don't conflict.
- The **hub must supply its own VBUS**; the S3 host port does not power downstream
  devices.

## Bring-up sequence (do these in order, stop at the first failure)

1. **Phase 1 — one device, no hub.** Plug a mouse **directly**. `idf.py monitor`
   should log `HID CONNECTED ... proto=2` and the PC (via the Left MCU) should see
   mouse movement.
2. **Phase 2 — the milestone.** Plug a **powered hub** with a mouse **and** a
   keyboard. You should see **two** `HID CONNECTED` lines and both devices working.
   *This is the step that was impossible before.* If only the hub enumerates and no
   HID devices appear, re-check `CONFIG_USB_HOST_HUBS_SUPPORTED=y` took effect
   (`idf.py menuconfig` → Component config → USB-OTG) and that the hub is powered.
3. **Phase 3 — both at once on the PC.** Move the mouse and type — both should reach
   the PC simultaneously.

Then continue with Phases 4–5 from the migration plan (identity cloning, robustness).

## Files

| File | Purpose |
|---|---|
| `CMakeLists.txt` | ESP-IDF project entry |
| `sdkconfig.defaults` | **hub enable flag** + target + timing |
| `main/idf_component.yml` | pulls `espressif/usb_host_hid ^1.2.0` |
| `main/CMakeLists.txt` | component registration |
| `main/hub_host_main.c` | USB host + HID host + UART forwarding |
