/*
 * MAKCM Right MCU — hub-enabled USB host (native ESP-IDF v5.5.x, ESP32-S3)
 * ============================================================================
 * COMPLETE version: reads a mouse AND a keyboard through an external USB hub,
 * performs the full descriptor handshake + identity clone with the Left MCU,
 * and forwards HID reports — all with heavy debug so problems are obvious.
 *
 * Debug is emitted TWO ways so you can always see it:
 *   1. ESP_LOGx to the Right's own console (USB-Serial-JTAG / UART0).
 *   2. Mirrored over the UART link as "ESPLOG_<msg>" lines — the Left prints
 *      those to its Serial0 (the CH343 / COMx you already watch). So even
 *      though the Right has no direct USB, you see its logs on the Left's port.
 *
 * Handshake (Left drives, Right reacts):
 *   Left: READY                 -> Right: USB_HELLO (device ready) / USB_ISNULL
 *   Left: sendDeviceInfo        -> Right: USB_sendDeviceInfo:{...strings...}
 *   Left: sendDescriptorDevice  -> Right: USB_sendDescriptorDevice:{...VID/PID...}
 *   Left: sendEndpoint/Interface/Hid/EndpointData/Unknown -> "[]"
 *   Left: sendIADescriptors     -> {...zeros...}
 *   Left: sendDescriptorconfig  -> {...minimal...}
 *   Left: USB_INIT              -> Right enables forwarding
 *
 * Forwarding (after USB_INIT):
 *   mouse    -> [0xAA][buttons][X_lo][X_hi][Y_lo][Y_hi][wheel]
 *   keyboard -> "kb.report(mods,k0,k1,k2,k3,k4,k5)"
 *
 * The one flag that makes devices behind a hub enumerate is
 * CONFIG_USB_HOST_HUBS_SUPPORTED=y (sdkconfig.defaults), not this code.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/uart.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid.h"

static const char *TAG = "makcm_hub";

/* ---- Inter-MCU UART link to the Left MCU (Arduino "Serial1" equivalent) ---- */
/* Base Right used Serial1.begin(5000000, SERIAL_8N1, rxPin=2, txPin=1). */
#define LINK_UART_NUM      UART_NUM_1
#define LINK_UART_TX_GPIO  1
#define LINK_UART_RX_GPIO  2
#define LINK_UART_BAUD     5000000
#define LINK_RX_BUF        2048

/* Binary frame marker (mouse). Keyboard uses text kb.report(...). */
#define MOUSE_MARKER       0xAA

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */
typedef struct {
    volatile bool valid;
    uint16_t idVendor, idProduct, bcdUSB, bcdDevice;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
    uint8_t  speed, dev_addr, bConfigurationValue;
    char     manufacturer[64], product[64], serial[64];
} clone_identity_t;

static clone_identity_t s_id;                       /* cloned device identity  */
static volatile bool    s_device_connected = false; /* a HID device is present */
static volatile bool    s_forwarding_ready = false; /* USB_INIT received       */
static volatile bool    s_is_debug         = false; /* DEBUG_ON toggled        */

static SemaphoreHandle_t        s_uart_mutex;
static usb_host_client_handle_t s_id_client;        /* client used to read descriptors */

static void uart_line(const char *s);

/* ------------------------------------------------------------------------- */
/* Debug helpers                                                             */
/*   LOGC = console only (verbose, safe).                                    */
/*   LOGL = console + mirror over the link as ESPLOG_ (visible on Left port).*/
/* ------------------------------------------------------------------------- */
#define LOGC(fmt, ...)  ESP_LOGI(TAG, fmt, ##__VA_ARGS__)

static void LOGL(const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", msg);
    char line[224];
    snprintf(line, sizeof(line), "ESPLOG_%s", msg);
    uart_line(line);
}

/* ------------------------------------------------------------------------- */
/* UART link                                                                 */
/* ------------------------------------------------------------------------- */
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
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART_NUM, LINK_RX_BUF, 4096, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART_NUM, LINK_UART_TX_GPIO, LINK_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

/* Write a newline-terminated text line atomically. */
static void uart_line(const char *s)
{
    size_t n = strlen(s);
    if (s_uart_mutex) xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(100));
    uart_write_bytes(LINK_UART_NUM, s, n);
    uart_write_bytes(LINK_UART_NUM, "\n", 1);
    if (s_uart_mutex) xSemaphoreGive(s_uart_mutex);
}

/* Write raw bytes (a binary frame) atomically. */
static void uart_raw(const uint8_t *buf, size_t len)
{
    if (s_uart_mutex) xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(10));
    uart_write_bytes(LINK_UART_NUM, (const char *)buf, len);
    if (s_uart_mutex) xSemaphoreGive(s_uart_mutex);
}

/* ------------------------------------------------------------------------- */
/* JSON helpers                                                              */
/* ------------------------------------------------------------------------- */
static void json_sanitize(const char *in, char *out, size_t out_sz)
{
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') out[j++] = ' ';
        else if (c >= 0x20 && c < 0x7F) out[j++] = (char)c;
        else out[j++] = '?';
    }
    out[j] = 0;
}

/* Convert a USB UTF-16LE string descriptor to ASCII. */
static void str16_to_ascii(const usb_str_desc_t *s, char *out, size_t out_sz)
{
    out[0] = 0;
    if (!s || s->bLength < 2) return;
    int n = (s->bLength - 2) / 2;
    int j = 0;
    for (int i = 0; i < n && j < (int)out_sz - 1; i++) {
        uint16_t c = s->wData[i];
        out[j++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[j] = 0;
}

/* ------------------------------------------------------------------------- */
/* Identity capture (via our own USB host client)                           */
/* ------------------------------------------------------------------------- */
static void capture_identity(uint8_t dev_addr)
{
    usb_device_handle_t dh;
    esp_err_t err = usb_host_device_open(s_id_client, dev_addr, &dh);
    if (err != ESP_OK) {
        LOGL("id: open addr %d failed (0x%x)", dev_addr, err);
        return;
    }

    const usb_device_desc_t *dd = NULL;
    if (usb_host_get_device_descriptor(dh, &dd) == ESP_OK && dd) {
        if (dd->bDeviceClass == USB_CLASS_HUB) {
            LOGL("id: addr %d is a HUB (class 09), skipping identity", dev_addr);
            usb_host_device_close(s_id_client, dh);
            return;
        }
        s_id.idVendor           = dd->idVendor;
        s_id.idProduct          = dd->idProduct;
        s_id.bcdUSB             = dd->bcdUSB;
        s_id.bcdDevice          = dd->bcdDevice;
        s_id.bDeviceClass       = dd->bDeviceClass;
        s_id.bDeviceSubClass    = dd->bDeviceSubClass;
        s_id.bDeviceProtocol    = dd->bDeviceProtocol;
        s_id.bMaxPacketSize0    = dd->bMaxPacketSize0;
        s_id.iManufacturer      = dd->iManufacturer;
        s_id.iProduct           = dd->iProduct;
        s_id.iSerialNumber      = dd->iSerialNumber;
        s_id.bNumConfigurations = dd->bNumConfigurations;
    } else {
        LOGL("id: get_device_descriptor failed for addr %d", dev_addr);
    }

    usb_device_info_t info;
    if (usb_host_device_info(dh, &info) == ESP_OK) {
        s_id.speed               = info.speed;
        s_id.dev_addr            = info.dev_addr;
        s_id.bConfigurationValue = info.bConfigurationValue;
        if (info.bMaxPacketSize0) s_id.bMaxPacketSize0 = info.bMaxPacketSize0;
        str16_to_ascii(info.str_desc_manufacturer, s_id.manufacturer, sizeof(s_id.manufacturer));
        str16_to_ascii(info.str_desc_product,      s_id.product,      sizeof(s_id.product));
        str16_to_ascii(info.str_desc_serial_num,   s_id.serial,       sizeof(s_id.serial));
    } else {
        LOGL("id: usb_host_device_info failed for addr %d", dev_addr);
    }

    s_id.valid = true;
    s_device_connected = true;
    LOGL("id: cloned VID=%04X PID=%04X '%s' / '%s'", s_id.idVendor, s_id.idProduct,
         s_id.manufacturer, s_id.product);

    usb_host_device_close(s_id_client, dh);
}

static void id_client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        LOGL("USB NEW_DEV at address %d", msg->new_dev.address);
        capture_identity(msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        LOGL("USB DEV_GONE");
        break;
    default:
        break;
    }
}

static void id_client_task(void *arg)
{
    const usb_host_client_config_t ccfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = id_client_cb, .callback_arg = NULL },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&ccfg, &s_id_client));
    LOGC("identity client registered");
    while (1) {
        usb_host_client_handle_events(s_id_client, portMAX_DELAY);
    }
}

/* ------------------------------------------------------------------------- */
/* Handshake responders (build JSON with real identity where it matters)     */
/* ------------------------------------------------------------------------- */
static void send_device_info(void)
{
    char mfr[80], prod[80], ser[80], buf[320];
    json_sanitize(s_id.manufacturer, mfr, sizeof(mfr));
    json_sanitize(s_id.product,      prod, sizeof(prod));
    json_sanitize(s_id.serial,       ser, sizeof(ser));
    snprintf(buf, sizeof(buf),
             "USB_sendDeviceInfo:{\"speed\":%u,\"dev_addr\":%u,\"vMaxPacketSize0\":%u,"
             "\"bConfigurationValue\":%u,\"str_desc_manufacturer\":\"%s\","
             "\"str_desc_product\":\"%s\",\"str_desc_serial_num\":\"%s\"}",
             s_id.speed, s_id.dev_addr, s_id.bMaxPacketSize0, s_id.bConfigurationValue,
             mfr, prod, ser);
    uart_line(buf);
    LOGC("HS-> DeviceInfo");
}

static void send_descriptor_device(void)
{
    char buf[320];
    snprintf(buf, sizeof(buf),
             "USB_sendDescriptorDevice:{\"bLength\":18,\"bDescriptorType\":1,\"bcdUSB\":%u,"
             "\"bDeviceClass\":%u,\"bDeviceSubClass\":%u,\"bDeviceProtocol\":%u,\"bMaxPacketSize0\":%u,"
             "\"idVendor\":%u,\"idProduct\":%u,\"bcdDevice\":%u,\"iManufacturer\":%u,\"iProduct\":%u,"
             "\"iSerialNumber\":%u,\"bNumConfigurations\":%u}",
             s_id.bcdUSB, s_id.bDeviceClass, s_id.bDeviceSubClass, s_id.bDeviceProtocol,
             s_id.bMaxPacketSize0, s_id.idVendor, s_id.idProduct, s_id.bcdDevice,
             s_id.iManufacturer, s_id.iProduct, s_id.iSerialNumber,
             s_id.bNumConfigurations ? s_id.bNumConfigurations : 1);
    uart_line(buf);
    LOGC("HS-> DescriptorDevice VID=%04X PID=%04X", s_id.idVendor, s_id.idProduct);
}

static void handle_left_line(const char *line)
{
    LOGC("RX<- '%s'", line);

    if (strcmp(line, "READY") == 0) {
        if (s_is_debug)              uart_line("USB_ISDEBUG");
        else if (s_device_connected) { uart_line("USB_HELLO"); LOGL("READY -> USB_HELLO (device ready)"); }
        else                         uart_line("USB_ISNULL");
        return;
    }
    if (strcmp(line, "sendDeviceInfo") == 0)            { send_device_info(); return; }
    if (strcmp(line, "sendDescriptorDevice") == 0)      { send_descriptor_device(); return; }
    if (strcmp(line, "sendEndpointDescriptors") == 0)   { uart_line("USB_sendEndpointDescriptors:[]"); return; }
    if (strcmp(line, "sendInterfaceDescriptors") == 0)  { uart_line("USB_sendInterfaceDescriptors:[]"); return; }
    if (strcmp(line, "sendHidDescriptors") == 0)        { uart_line("USB_sendHidDescriptors:[]"); return; }
    if (strcmp(line, "sendIADescriptors") == 0) {
        uart_line("USB_sendIADescriptors:{\"bLength\":0,\"bDescriptorType\":0,\"bFirstInterface\":0,"
                  "\"bInterfaceCount\":0,\"bFunctionClass\":0,\"bFunctionSubClass\":0,"
                  "\"bFunctionProtocol\":0,\"iFunction\":0}");
        return;
    }
    if (strcmp(line, "sendEndpointData") == 0)          { uart_line("USB_sendEndpointData:[]"); return; }
    if (strcmp(line, "sendUnknownDescriptors") == 0)    { uart_line("USB_sendUnknownDescriptors:[]"); return; }
    if (strcmp(line, "sendDescriptorconfig") == 0) {
        uart_line("USB_sendDescriptorconfig:{\"bLength\":9,\"bDescriptorType\":2,\"wTotalLength\":0,"
                  "\"bNumInterfaces\":0,\"bConfigurationValue\":1,\"iConfiguration\":0,"
                  "\"bmAttributes\":160,\"bMaxPower\":50}");
        LOGL("HS-> DescriptorConfig (last handshake reply sent)");
        return;
    }
    if (strcmp(line, "USB_INIT") == 0) {
        s_forwarding_ready = true;
        LOGL("USB_INIT received -> forwarding ENABLED. Move the mouse / press keys.");
        return;
    }
    if (strcmp(line, "DEBUG_ON") == 0)  { s_is_debug = true;  LOGC("DEBUG_ON");  return; }
    if (strcmp(line, "DEBUG_OFF") == 0) { s_is_debug = false; LOGC("DEBUG_OFF"); return; }
    LOGC("RX<- (unhandled) '%s'", line);
}

static void uart_rx_task(void *arg)
{
    static char line[600];
    size_t pos = 0;
    uint8_t byte;
    LOGC("UART RX task started (waiting for Left's READY...)");
    while (1) {
        int n = uart_read_bytes(LINK_UART_NUM, &byte, 1, portMAX_DELAY);
        if (n != 1) continue;
        if (byte == '\r') continue;
        if (byte == '\n') {
            line[pos] = 0;
            if (pos > 0) handle_left_line(line);
            pos = 0;
        } else if (pos < sizeof(line) - 1) {
            line[pos++] = (char)byte;
        } else {
            pos = 0;
            LOGC("RX line overflow, resync");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Forwarding (HID reports -> Left), gated on USB_INIT                       */
/* ------------------------------------------------------------------------- */
static void forward_mouse_boot(const uint8_t *data, size_t len)
{
    if (len < 3) return;
    uint8_t buttons = data[0];
    int16_t x = (int8_t)data[1];
    int16_t y = (int8_t)data[2];
    int8_t  wheel = (len >= 4) ? (int8_t)data[3] : 0;
    uint8_t pkt[7] = {
        MOUSE_MARKER, buttons,
        (uint8_t)(x & 0xFF), (uint8_t)((x >> 8) & 0xFF),
        (uint8_t)(y & 0xFF), (uint8_t)((y >> 8) & 0xFF),
        (uint8_t)wheel,
    };
    uart_raw(pkt, sizeof(pkt));
}

static void forward_keyboard_boot(const uint8_t *data, size_t len)
{
    if (len < 8) return;
    char l[64];
    snprintf(l, sizeof(l), "kb.report(%d,%d,%d,%d,%d,%d,%d)",
             data[0], data[2], data[3], data[4], data[5], data[6], data[7]);
    uart_line(l);
}

static void hid_iface_cb(hid_host_device_handle_t h,
                         const hid_host_interface_event_t event, void *arg)
{
    static uint32_t mouse_cnt = 0, kbd_cnt = 0;
    uint8_t data[64];
    size_t len = 0;
    hid_host_dev_params_t p;
    if (hid_host_device_get_params(h, &p) != ESP_OK) return;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        if (hid_host_device_get_raw_input_report_data(h, data, sizeof(data), &len) != ESP_OK) return;
        if (!s_forwarding_ready) {
            LOGC("report before USB_INIT (dropped) proto=%d len=%d", p.proto, (int)len);
            return;
        }
        if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
            if (p.proto == HID_PROTOCOL_MOUSE) {
                forward_mouse_boot(data, len);
                if (++mouse_cnt <= 3 || (mouse_cnt % 500) == 0)
                    LOGL("mouse report #%lu (len %d)", (unsigned long)mouse_cnt, (int)len);
            } else if (p.proto == HID_PROTOCOL_KEYBOARD) {
                forward_keyboard_boot(data, len);
                if (++kbd_cnt <= 5 || (kbd_cnt % 100) == 0)
                    LOGL("keyboard report #%lu (len %d)", (unsigned long)kbd_cnt, (int)len);
            }
        } else {
            LOGC("non-boot HID report (proto=%d) - not forwarded", p.proto);
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        LOGL("HID iface DISCONNECTED (proto=%d)", p.proto);
        hid_host_device_close(h);
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        LOGL("HID iface TRANSFER_ERROR (proto=%d)", p.proto);
        break;
    default:
        break;
    }
}

static void hid_connect_handle(hid_host_device_handle_t h)
{
    hid_host_dev_params_t p;
    ESP_ERROR_CHECK(hid_host_device_get_params(h, &p));
    LOGL("HID CONNECTED addr=%d iface=%d sub=%d proto=%d (%s)",
         p.addr, p.iface_num, p.sub_class, p.proto,
         p.proto == HID_PROTOCOL_MOUSE ? "MOUSE" :
         p.proto == HID_PROTOCOL_KEYBOARD ? "KEYBOARD" : "other");

    const hid_host_device_config_t dev_cfg = { .callback = hid_iface_cb, .callback_arg = NULL };
    ESP_ERROR_CHECK(hid_host_device_open(h, &dev_cfg));
    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        ESP_ERROR_CHECK(hid_class_request_set_protocol(h, HID_REPORT_PROTOCOL_BOOT));
        if (p.proto == HID_PROTOCOL_KEYBOARD)
            ESP_ERROR_CHECK(hid_class_request_set_idle(h, 0, 0));
    }
    ESP_ERROR_CHECK(hid_host_device_start(h));
    s_device_connected = true;  /* in case the identity client lagged */
}

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t  event;
} hid_evt_t;

static QueueHandle_t s_hid_queue;

static void hid_driver_cb(hid_host_device_handle_t h,
                          const hid_host_driver_event_t event, void *arg)
{
    const hid_evt_t e = { .handle = h, .event = event };
    if (s_hid_queue) xQueueSend(s_hid_queue, &e, 0);
}

/* ------------------------------------------------------------------------- */
/* USB host lib task                                                         */
/* ------------------------------------------------------------------------- */
static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive(arg);
    LOGC("usb_host installed");
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

void app_main(void)
{
    link_uart_init();
    s_uart_mutex = xSemaphoreCreateMutex();
    memset(&s_id, 0, sizeof(s_id));

    LOGL("=== MAKCM Right hub host booting (ESP-IDF, hub-enabled) ===");

    BaseType_t ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
                                            xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    assert(ok == pdTRUE);
    ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(2000));

    xTaskCreatePinnedToCore(id_client_task, "id_client", 4096, NULL, 3, NULL, 0);

    s_hid_queue = xQueueCreate(8, sizeof(hid_evt_t));
    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size    = 4096,
        .core_id       = 0,
        .callback      = hid_driver_cb,
        .callback_arg  = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_cfg));
    LOGL("hid_host installed - plug a hub with a mouse and a keyboard");

    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx", 4096, NULL, 6, NULL, 0);

    hid_evt_t e;
    while (1) {
        if (xQueueReceive(s_hid_queue, &e, portMAX_DELAY)) {
            if (e.event == HID_HOST_DRIVER_EVENT_CONNECTED) hid_connect_handle(e.handle);
        }
    }
}
