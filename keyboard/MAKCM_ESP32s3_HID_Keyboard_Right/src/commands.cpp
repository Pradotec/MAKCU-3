#include "EspUsbHost.h"
#include "esp_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <sstream>


bool enableYield = false;

void EspUsbHost::handleIncomingCommands(const String &command)
{
    ESP_LOGI("EspUsbHost", "Received command: %s", command.c_str());

    if (command == "DEBUG_ON")
    {
        debugModeActive = true;
        serial1Send("Debug mode activated.\n");
        serial1Send("USB_ISDEBUG\n");
        ESP_LOGI("EspUsbHost", "Debug mode activated.");
    }
    else if (command == "DEBUG_OFF")
    {
        if (USB_IS_DEBUG)
        {
            return;
        }
        else
        {
            debugModeActive = false;
            ESP_LOGI("EspUsbHost", "Debug mode deactivated. System will restart.");
            Serial1.print("USB_GOODBYE\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
    else if (command == "READY")
    {
        if (debugModeActive)
        {
            serial1Send("USB_ISDEBUG\n");
            ESP_LOGI("EspUsbHost", "Debug mode is active. Sent USB_ISDEBUG.");
        }
        else
        {
            if (EspUsbHost::deviceConnected)
            {
                serial1Send("USB_HELLO\n");
                ESP_LOGI("EspUsbHost", "Device is connected.");
            }
            else if (openDeviceCount > 0 && firstDeviceOpenTime > 0 &&
                     (millis() - firstDeviceOpenTime) > 3000)
            {
                EspUsbHost::deviceConnected = true;
                serial1Send("USB_HELLO\n");
                ESP_LOGW("EspUsbHost", "HID identification timeout, proceeding with available identity.");
            }
            else
            {
                serial1Send("USB_ISNULL\n");
                if (openDeviceCount > 0) {
                    ESP_LOGI("EspUsbHost", "Waiting for HID device identification...");
                } else {
                    ESP_LOGW("EspUsbHost", "No device is connected.");
                }
            }
        }
    }
    else if (command == "USB_INIT")
    {
        deviceMouseReady = true;
        deviceKeyboardReady = true;
        serial1Send("USB Initialized. Keyboard ready.\n");
        ESP_LOGI("EspUsbHost", "USB initialized. Keyboard ready.");
    }
    else if (command == "sendDeviceInfo")
    {
        sendDeviceInfo();
        ESP_LOGI("EspUsbHost", "Sending device information.");
    }
    else if (command == "sendDescriptorDevice")
    {
        sendDescriptorDevice();
        ESP_LOGI("EspUsbHost", "Sending device descriptor.");
    }
    else if (command == "sendEndpointDescriptors")
    {
        sendEndpointDescriptors();
        ESP_LOGI("EspUsbHost", "Sending endpoint descriptors.");
    }
    else if (command == "sendInterfaceDescriptors")
    {
        sendInterfaceDescriptors();
        ESP_LOGI("EspUsbHost", "Sending interface descriptors.");
    }
    else if (command == "sendHidDescriptors")
    {
        sendHidDescriptors();
        ESP_LOGI("EspUsbHost", "Sending HID descriptors.");
    }
    else if (command == "sendIADescriptors")
    {
        sendIADescriptors();
        ESP_LOGI("EspUsbHost", "Sending Interface Association Descriptors.");
    }
    else if (command == "sendEndpointData")
    {
        sendEndpointData();
        ESP_LOGI("EspUsbHost", "Sending endpoint data.");
    }
    else if (command == "sendUnknownDescriptors")
    {
        sendUnknownDescriptors();
        ESP_LOGI("EspUsbHost", "Sending unknown descriptors.");
    }
    else if (command == "sendDescriptorconfig")
    {
        sendDescriptorconfig();
        ESP_LOGI("EspUsbHost", "Sending configuration descriptor.");
    }
    else if (command == "YIELD")
    {
        enableYield = true;
        serial1Send("Yield enabled.\n");
        ESP_LOGI("EspUsbHost", "Yield enabled.");
    }
    else if (command == "YIELD_END")
    {
        enableYield = false;
        serial1Send("Yield disabled.\n");
        ESP_LOGI("EspUsbHost", "Yield disabled.");
    }
    else
    {
        serial1Send("Unknown command received: %s\n", command.c_str());
        ESP_LOGW("EspUsbHost", "Unknown command received: %s", command.c_str());
    }
}
