/*
 * MAKCM Right MCU — hub-enabled USB host (native ESP-IDF v5.5.x, ESP32-S3)
 * ---------------------------------------------------------------------------
 * Reads a mouse AND a keyboard connected through an external USB hub and
 * forwards both to the Left MCU over UART, using the SAME wire protocol the
 * existing Left MCU already parses (so the Left MCU needs no changes):
 *
 *   - Mouse:    7-byte binary frame  [0xAA][buttons][X_lo][X_hi][Y_lo][Y_hi][wheel]
 *   - Keyboard: text line            "kb.report(mods,k0,k1,k2,k3,k4,k5)\n"
 *
 * This is the Phase 1-3 skeleton from ../MIGRATION_PLAN.md. It intentionally:
 *   - forces BOOT protocol on boot-capable devices (fixed, universal report
 *     layouts) — simplest correct path to prove the hub capability;
 *   - does NOT yet do identity cloning (Phase 4) or non-boot / 16-bit-axis
 *     report-descriptor parsing (port that from MAKCM_ESP32s3_HID_Mouse_Right);
 *   - does NOT yet forward physical media/consumer keys.
 * Every such gap is marked with TODO below.
 *
 * The USB host + HID host control flow follows Espressif's official
 * usb_host_hid example (esp-idf/examples/peripherals/usb/host/hid). The one
 * thing that makes two devices behind a hub work is CONFIG_USB_HOST_HUBS_SUPPORTED=y
 * in sdkconfig.defaults — not this code.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/uart.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid.h"

static const char *TAG = "makcm_hub_host";

/* ------------------------------------------------------------------------- *
 * Inter-MCU UART link to the Left MCU (the Arduino "Serial1" equivalent).
 * Base Right firmware used Serial1.begin(5000000, SERIAL_8N1, rxPin=2, txPin=1),
 * so RX = GPIO2, TX = GPIO1, 5 Mbaud 8N1. Adjust if your board differs.
 * ------------------------------------------------------------------------- */
#define LINK_UART_NUM      UART_NUM_1
#define LINK_UART_TX_GPIO  1
#define LINK_UART_RX_GPIO  2
#define LINK_UART_BAUD     5000000

static void link_uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = LINK_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART_NUM, 1024, 4096, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART_NUM, LINK_UART_TX_GPIO, LINK_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static inline void link_write(const uint8_t *buf, size_t len)
{
    uart_write_bytes(LINK_UART_NUM, (const char *)buf, len);
}

/* ------------------------------------------------------------------------- *
 * Forwarding — keep the byte layout IDENTICAL to what the Left MCU expects.
 * ------------------------------------------------------------------------- */

/* Boot mouse report = [buttons][int8 x][int8 y]([int8 wheel] optional). */
static void forward_mouse_boot(const uint8_t *data, size_t len)
{
    if (len < 3) return;
    uint8_t buttons = data[0];
    int16_t x = (int8_t)data[1];       /* boot protocol is 8-bit; sign-extend */
    int16_t y = (int8_t)data[2];
    int8_t  wheel = (len >= 4) ? (int8_t)data[3] : 0;

    uint8_t pkt[7] = {
        0xAA,
        buttons,
        (uint8_t)(x & 0xFF), (uint8_t)((x >> 8) & 0xFF),
        (uint8_t)(y & 0xFF), (uint8_t)((y >> 8) & 0xFF),
        (uint8_t)wheel,
    };
    link_write(pkt, sizeof(pkt));
    /* TODO Phase 4+: for non-boot / high-DPI mice, parse the report descriptor
     * and emit true 16-bit deltas (port the 8/12/16-bit axis logic from
     * MAKCM_ESP32s3_HID_Mouse_Right/src/esp_usb_host.cpp). Boot protocol caps
     * deltas at +/-127 per report. */
}

/* Boot keyboard report = [mods][reserved][k0..k5] (8 bytes). */
static void forward_keyboard_boot(const uint8_t *data, size_t len)
{
    if (len < 8) return;
    char line[64];
    int n = snprintf(line, sizeof(line),
                     "kb.report(%d,%d,%d,%d,%d,%d,%d)\n",
                     data[0], data[2], data[3], data[4], data[5], data[6], data[7]);
    if (n > 0 && n < (int)sizeof(line)) {
        link_write((const uint8_t *)line, (size_t)n);
    }
    /* NOTE: the Left MCU already parses kb.report(...). If you later switch the
     * Left MCU to the NKRO keyboard edition, emit the [0xAB][mods][bitmap*20]
     * binary frame here instead. */
}

/* ------------------------------------------------------------------------- *
 * HID host — per-interface event callback (raw input reports arrive here).
 * ------------------------------------------------------------------------- */
static void hid_iface_cb(hid_host_device_handle_t h,
                         const hid_host_interface_event_t event,
                         void *arg)
{
    uint8_t data[64];
    size_t len = 0;
    hid_host_dev_params_t p;
    ESP_ERROR_CHECK(hid_host_device_get_params(h, &p));

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        if (hid_host_device_get_raw_input_report_data(h, data, sizeof(data), &len) != ESP_OK) {
            return;
        }
        /* proto (MOUSE/KEYBOARD) is only meaningful for a boot interface that
         * we've put into BOOT protocol (done in hid_connect_handle). */
        if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
            if (p.proto == HID_PROTOCOL_MOUSE) {
                forward_mouse_boot(data, len);
            } else if (p.proto == HID_PROTOCOL_KEYBOARD) {
                forward_keyboard_boot(data, len);
            }
        } else {
            /* TODO: non-boot interface (many gaming mice, consumer/media
             * interfaces). Parse hid_host_get_report_descriptor() to classify
             * and lay out the report. Ignored for now. */
        }
        break;

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HID iface DISCONNECTED (proto=%d)", p.proto);
        ESP_ERROR_CHECK(hid_host_device_close(h));
        /* TODO Phase 4/5: tell the Left MCU (USB_GOODBYE) if this was the
         * cloned device, and handle re-enumeration cleanly. */
        break;

    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "HID iface TRANSFER_ERROR (proto=%d)", p.proto);
        break;

    default:
        ESP_LOGW(TAG, "HID iface unhandled event %d (proto=%d)", event, p.proto);
        break;
    }
}

/* Handle a newly connected HID interface: open, force boot protocol, start. */
static void hid_connect_handle(hid_host_device_handle_t h)
{
    hid_host_dev_params_t p;
    ESP_ERROR_CHECK(hid_host_device_get_params(h, &p));
    ESP_LOGI(TAG, "HID CONNECTED addr=%d iface=%d sub_class=%d proto=%d",
             p.addr, p.iface_num, p.sub_class, p.proto);

    const hid_host_device_config_t dev_cfg = {
        .callback     = hid_iface_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_device_open(h, &dev_cfg));

    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        /* Force fixed-layout boot reports (3-byte mouse / 8-byte keyboard). */
        ESP_ERROR_CHECK(hid_class_request_set_protocol(h, HID_REPORT_PROTOCOL_BOOT));
        if (p.proto == HID_PROTOCOL_KEYBOARD) {
            /* idle=0 → keyboard only reports on change (no key repeat spam). */
            ESP_ERROR_CHECK(hid_class_request_set_idle(h, 0, 0));
        }
    }

    /* MANDATORY: without start(), no input reports are delivered. */
    ESP_ERROR_CHECK(hid_host_device_start(h));

    /* TODO Phase 4: on the first identified device, read its device/string
     * descriptors and run the existing handshake (USB_HELLO + send* JSON +
     * USB_INIT) so the Left MCU clones this device's VID/PID/strings. */
}

/* ------------------------------------------------------------------------- *
 * HID host driver-level callback → hand the CONNECTED event to app_main's
 * task via a queue (do minimal work in the callback context).
 * ------------------------------------------------------------------------- */
typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t  event;
    void                    *arg;
} app_event_t;

static QueueHandle_t s_app_queue = NULL;

static void hid_driver_cb(hid_host_device_handle_t h,
                          const hid_host_driver_event_t event,
                          void *arg)
{
    const app_event_t e = { .handle = h, .event = event, .arg = arg };
    if (s_app_queue) {
        xQueueSend(s_app_queue, &e, 0);
    }
}

/* ------------------------------------------------------------------------- *
 * USB Host Library task — install the host, then pump its events forever.
 * Kept alive across device disconnects so hotplug through the hub works.
 * ------------------------------------------------------------------------- */
static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,               /* use the S3 internal USB PHY */
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive(arg);                        /* tell app_main install is done */

    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            /* No clients registered — free any orphaned devices, keep running. */
            usb_host_device_free_all();
        }
        /* Intentionally never break/uninstall: this is a permanent host. */
    }
}

void app_main(void)
{
    link_uart_init();
    ESP_LOGI(TAG, "MAKCM Right MCU hub host starting (native ESP-IDF, hub-enabled)");

    /* Optional: announce ourselves on the link, like the base firmware's
     * Serial1.println("MAKCK ...") banner. */
    static const char banner[] = "MAKCK HUB_HOST_IDF\r\n";
    link_write((const uint8_t *)banner, sizeof(banner) - 1);

    BaseType_t ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
                                            xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    assert(ok == pdTRUE);
    ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000));   /* wait for usb_host_install */

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size    = 4096,
        .core_id       = 0,
        .callback      = hid_driver_cb,
        .callback_arg  = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_cfg));

    s_app_queue = xQueueCreate(10, sizeof(app_event_t));
    assert(s_app_queue != NULL);

    ESP_LOGI(TAG, "Ready. Plug a hub with a mouse and a keyboard.");

    app_event_t e;
    while (true) {
        if (xQueueReceive(s_app_queue, &e, portMAX_DELAY)) {
            if (e.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
                hid_connect_handle(e.handle);
            }
        }
    }
}
