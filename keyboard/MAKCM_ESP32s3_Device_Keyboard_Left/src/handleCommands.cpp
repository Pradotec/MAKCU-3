#include "handleCommands.h"
#include "InitSettings.h"
#include "tasks.h"
#include <Arduino.h>
#include <USB.h>
#include "USBSetup.h"
#include <esp_intr_alloc.h>
#include <cstring>
#include <atomic>
#include <RingBuf.h>

// ---------------------------------------------------------------------------
// Binary protocol markers received from the Right (host) MCU over Serial1.
//   0xAB : full keyboard state  -> [0xAB][modifiers][bitmap * NKRO_BITMAP_BYTES]
//   0xAC : consumer/media state -> [0xAC][cc_lo][cc_hi]
// ASCII text commands (kb.*) never contain bytes >= 0x80, so the markers
// can never collide with a text command.
// ---------------------------------------------------------------------------
#define KB_MARKER       0xAB
#define KB_FRAME_LEN    (2 + NKRO_BITMAP_BYTES)   // marker + modifiers + bitmap
#define CC_MARKER       0xAC
#define CC_FRAME_LEN    3                         // marker + cc_lo + cc_hi

std::atomic<bool> serial0Locked(true);

volatile bool deviceConnected = false;
bool usbReady = false;
bool processingUsbCommands = false;

RingBuf<char, 1024> serial0RingBuffer;
RingBuf<char, 1024> serial1RingBuffer;
int currentCommandIndex = 0;

const unsigned long ledFlashTime = 25; // LED flash timer in ms

const char *commandQueue[] = {
    "sendDeviceInfo",
    "sendDescriptorDevice",
    "sendEndpointDescriptors",
    "sendInterfaceDescriptors",
    "sendHidDescriptors",
    "sendIADescriptors",
    "sendEndpointData",
    "sendUnknownDescriptors",
    "sendDescriptorconfig"
};

// Command tables
CommandEntry serial0CommandTable[] = {
    {"DEBUG_", handleDebug},
    {"SERIAL_", handleSerial0Speed}
};

CommandEntry debugCommandTable[] = {
    {"ESPLOG_", handleEspLog},
    {"PRINT_Parsed_Descriptors", printParsedDescriptors},
    {"HID_Descriptors", [](const char* arg) { Serial1.print(arg); }}
};

CommandEntry normalCommandTable[] = {
    {"kb.press(", handleKbPress},
    {"kb.release(", handleKbRelease},
    {"kb.releaseall", handleKbReleaseAll},
    {"kb.tap(", handleKbTap},
    {"kb.type(", handleKbType},
    {"kb.isdown(", handleKbIsDown},
    {"kb.mtap(", handleKbMediaTap},
    {"kb.mset(", handleKbMediaSet}
};

CommandEntry usbCommandTable[] = {
    {"USB_HELLO", handleUsbHello},
    {"USB_GOODBYE", handleUsbGoodbye},
    {"USB_ISNULL", handleNoDevice},
    {"USB_sendDeviceInfo:", receiveDeviceInfo},
    {"USB_sendDescriptorDevice:", receiveDescriptorDevice},
    {"USB_sendEndpointDescriptors:", receiveEndpointDescriptors},
    {"USB_sendInterfaceDescriptors:", receiveInterfaceDescriptors},
    {"USB_sendHidDescriptors:", receiveHidDescriptors},
    {"USB_sendIADescriptors:", receiveIADescriptors},
    {"USB_sendEndpointData:", receiveEndpointData},
    {"USB_sendUnknownDescriptors:", receiveUnknownDescriptors},
    {"USB_sendDescriptorconfig:", receivedescriptorConfiguration}
};

void processCommand(const char *command);

void trimCommand(char* command) {
    int len = strlen(command);
    while (len > 0 && (command[len - 1] == ' ' || command[len - 1] == '\n' || command[len - 1] == '\r')) {
        command[len - 1] = '\0';
        len--;
    }
}

void serial0RX() {
    while (Serial0.available() > 0) {
        char byte = Serial0.read();

        if (byte == '\r') {
            continue;
        }

        if (!serial0RingBuffer.isFull()) {
            serial0RingBuffer.push(byte);
        } else {
            Serial0.println("Serial0 ring buffer overflow detected.");
        }

        if (byte == '\n') {
            char commandBuffer[1024];
            int commandIndex = 0;

            while (!serial0RingBuffer.isEmpty() && commandIndex < sizeof(commandBuffer) - 1) {
                serial0RingBuffer.pop(commandBuffer[commandIndex++]);
            }

            commandBuffer[commandIndex] = '\0';
            trimCommand(commandBuffer);
            processCommand(commandBuffer);
        }
    }
}

// ---------------------------------------------------------------------------
// Binary keyboard-state handlers (passthrough from the physical keyboard).
// ---------------------------------------------------------------------------
static void handleBinaryKeyboard(const uint8_t *packet) {
    // packet[0] = marker, packet[1] = modifiers, packet[2..] = bitmap
    Keyboard.setPassthrough(packet[1], &packet[2]);
}

static void handleBinaryConsumer(const uint8_t *packet) {
    uint16_t bits = (uint16_t)packet[1] | ((uint16_t)packet[2] << 8);
    Keyboard.setPassthroughConsumer(bits);
}

void serial1RX() {
    while (Serial1.available() > 0) {
        uint8_t head = (uint8_t)Serial1.peek();

        if (head == KB_MARKER) {
            if (Serial1.available() >= KB_FRAME_LEN) {
                uint8_t packet[KB_FRAME_LEN];
                Serial1.readBytes(packet, KB_FRAME_LEN);
                handleBinaryKeyboard(packet);
            } else {
                break;
            }
            continue;
        }

        if (head == CC_MARKER) {
            if (Serial1.available() >= CC_FRAME_LEN) {
                uint8_t packet[CC_FRAME_LEN];
                Serial1.readBytes(packet, CC_FRAME_LEN);
                handleBinaryConsumer(packet);
            } else {
                break;
            }
            continue;
        }

        char byte = Serial1.read();

        if (byte == '\r') {
            continue;
        }

        if (!serial1RingBuffer.isFull()) {
            serial1RingBuffer.push(byte);
        } else {
            Serial0.println("Serial1 ring buffer overflow detected.");
        }

        if (byte == '\n') {
            char commandBuffer[1024];
            int commandIndex = 0;

            while (!serial1RingBuffer.isEmpty() && commandIndex < sizeof(commandBuffer) - 1) {
                serial1RingBuffer.pop(commandBuffer[commandIndex++]);
            }

            commandBuffer[commandIndex] = '\0';
            trimCommand(commandBuffer);

            if (strncmp(commandBuffer, "kb.", 3) != 0) {
                Serial0.printf("[S1 len=%d] %.50s\n", strlen(commandBuffer), commandBuffer);
            }

            processCommand(commandBuffer);
        }
    }
}

void ledFlashTask(void *parameter) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        digitalWrite(9, HIGH);
        vTaskDelay(ledFlashTime / portTICK_PERIOD_MS);
        digitalWrite(9, LOW);
        vTaskDelay(ledFlashTime / portTICK_PERIOD_MS);
    }
}

void notifyLedFlashTask() {
    xTaskNotifyGive(ledFlashTaskHandle);
}

void handleUsbHello(const char *command) {
    Serial0.println("[HS] USB_HELLO received, starting handshake");
    deviceConnected = true;
    usbReady = true;
    processingUsbCommands = true;
    currentCommandIndex = 0;
    sendNextCommand();
}

void handleUsbGoodbye(const char *command) {
    Serial0.println("USB Device disconnected. Restarting!");
    static const uint8_t zeroMap[NKRO_BITMAP_BYTES] = {0};
    Keyboard.releaseAllKeys();
    Keyboard.setPassthrough(0, zeroMap);
    Keyboard.setPassthroughConsumer(0);
    Keyboard.setConsumer(0);
    vTaskDelay(100);
    ESP.restart();
}

void sendNextCommand() {
    if (!processingUsbCommands || currentCommandIndex >= sizeof(commandQueue) / sizeof(commandQueue[0])) {
        return;
    }
    const char *command = commandQueue[currentCommandIndex];
    Serial0.printf("[HS] Sending cmd %d: %s\n", currentCommandIndex, command);
    Serial1.println(command);
    currentCommandIndex++;
    if (currentCommandIndex >= sizeof(commandQueue) / sizeof(commandQueue[0])) {
        Serial0.println("[HS] All commands done, calling InitUSB");
        usbReady = false;
        processingUsbCommands = false;
        InitUSB();
        vTaskDelay(700);
        serial0Locked = false;
        Serial1.println("USB_INIT");
    }
}

void processCommand(const char *command) {
    for (const auto &entry : debugCommandTable) {
        if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
            entry.handler(command);
            return;
        }
    }

    for (const auto &entry : serial0CommandTable) {
        if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
            entry.handler(command);
            return;
        }
    }

    for (const auto &entry : usbCommandTable) {
        if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
            entry.handler(command);
            return;
        }
    }

    if (!processingUsbCommands) {
        for (const auto &entry : normalCommandTable) {
            if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
                entry.handler(command);
                return;
            }
        }
    }

    handleDebugcommand(command);
}

void handleEspLog(const char *command) {
    const char *message = command + strlen("ESPLOG_");
    if (strlen(message) > 0) {
        Serial0.println(message);
    } else {
        Serial0.println("ESPLOG_ command received, but no message to log.");
    }
}

void handleSerial0Speed(const char *command) {
    int speed;
    if (sscanf(command + strlen("SERIAL_"), "%d", &speed) == 1) {
        if (speed >= 115200 && speed <= 5000000) {
            Serial0.print("Setting Serial0 speed to: ");
            Serial0.println(speed);
            Serial0.end();
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            Serial0.begin(speed);
            Serial0.onReceive(serial0ISR);
            Serial0.println("Serial0 speed change successful.");
        } else {
            Serial0.println("Speed was out of bounds. Min: 115200, Max: 5000000.");
        }
    } else {
        Serial0.println("Invalid SERIAL command. Expected format: SERIAL_<speed>");
    }
}

void handleDebug(const char *command) {
    int debugLevel;
    if (strcmp(command, "DEBUG_ON") == 0) {
        Serial1.println("DEBUG_ON");
    } else if (strcmp(command, "DEBUG_OFF") == 0) {
        Serial1.println("DEBUG_OFF");
    } else if (sscanf(command + strlen("DEBUG_"), "%d", &debugLevel) == 1) {
        Serial1.println(command);
    } else {
        Serial0.println("Invalid DEBUG command. Bytes received:");
        for (int i = 0; i < strlen(command); i++) {
            Serial0.print("Byte ");
            Serial0.print(i);
            Serial0.print(": '");
            Serial0.print(command[i]);
            Serial0.print("' (ASCII: ");
            Serial0.print((int)command[i]);
            Serial0.println(")");
        }
    }
}

void handleDebugcommand(const char *command) {
    Serial0.print("[DBG] ");
    Serial0.println(command);
}

void handleNoDevice(const char *command) {
    Serial0.print(".");
    deviceConnected = false;
}

// ---------------------------------------------------------------------------
// Keyboard injection (external controller over Serial0). All of these operate
// on the "simulated" domain, which is OR-combined with the physical keyboard
// passthrough before every report, so injected and physical keys coexist.
// ---------------------------------------------------------------------------
void handleKbPress(const char *command) {
    int keycode = 0;
    if (sscanf(command + 9, "%d", &keycode) == 1) {
        Keyboard.pressKey((uint8_t)keycode);
    }
}

void handleKbRelease(const char *command) {
    int keycode = 0;
    if (sscanf(command + 11, "%d", &keycode) == 1) {
        Keyboard.releaseKey((uint8_t)keycode);
    }
}

void handleKbReleaseAll(const char *command) {
    Keyboard.releaseAllKeys();
}

void handleKbTap(const char *command) {
    int keycode = 0;
    if (sscanf(command + 7, "%d", &keycode) == 1) {
        // SendReport blocks until the host has polled each report, so the
        // press is always delivered before the release: a reliable tap with
        // no artificial delay.
        Keyboard.pressKey((uint8_t)keycode);
        Keyboard.releaseKey((uint8_t)keycode);
    }
}

// US-layout ASCII -> HID usage code (+ shift). Returns false for unmapped chars.
static bool asciiToKey(char c, uint8_t &mod, uint8_t &key) {
    mod = 0;
    key = 0;
    if (c >= 'a' && c <= 'z') { key = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { key = 0x04 + (c - 'A'); mod = 0x02; return true; }
    if (c >= '1' && c <= '9') { key = 0x1E + (c - '1'); return true; }
    switch (c) {
        case '0':  key = 0x27; return true;
        case ' ':  key = 0x2C; return true;
        case '\n': key = 0x28; return true;
        case '\r': key = 0x28; return true;
        case '\t': key = 0x2B; return true;
        case '-':  key = 0x2D; return true;
        case '=':  key = 0x2E; return true;
        case '[':  key = 0x2F; return true;
        case ']':  key = 0x30; return true;
        case '\\': key = 0x31; return true;
        case ';':  key = 0x33; return true;
        case '\'': key = 0x34; return true;
        case '`':  key = 0x35; return true;
        case ',':  key = 0x36; return true;
        case '.':  key = 0x37; return true;
        case '/':  key = 0x38; return true;
        case '!':  key = 0x1E; mod = 0x02; return true;
        case '@':  key = 0x1F; mod = 0x02; return true;
        case '#':  key = 0x20; mod = 0x02; return true;
        case '$':  key = 0x21; mod = 0x02; return true;
        case '%':  key = 0x22; mod = 0x02; return true;
        case '^':  key = 0x23; mod = 0x02; return true;
        case '&':  key = 0x24; mod = 0x02; return true;
        case '*':  key = 0x25; mod = 0x02; return true;
        case '(':  key = 0x26; mod = 0x02; return true;
        case ')':  key = 0x27; mod = 0x02; return true;
        case '_':  key = 0x2D; mod = 0x02; return true;
        case '+':  key = 0x2E; mod = 0x02; return true;
        case '{':  key = 0x2F; mod = 0x02; return true;
        case '}':  key = 0x30; mod = 0x02; return true;
        case '|':  key = 0x31; mod = 0x02; return true;
        case ':':  key = 0x33; mod = 0x02; return true;
        case '"':  key = 0x34; mod = 0x02; return true;
        case '~':  key = 0x35; mod = 0x02; return true;
        case '<':  key = 0x36; mod = 0x02; return true;
        case '>':  key = 0x37; mod = 0x02; return true;
        case '?':  key = 0x38; mod = 0x02; return true;
    }
    return false;
}

static void typeChar(char c) {
    uint8_t mod = 0, key = 0;
    if (!asciiToKey(c, mod, key)) return;
    bool shift = (mod & 0x02) != 0;
    if (shift) Keyboard.pressKey(0xE1);   // Left Shift
    Keyboard.pressKey(key);
    Keyboard.releaseKey(key);
    if (shift) Keyboard.releaseKey(0xE1);
}

void handleKbType(const char *command) {
    const char *start = strchr(command, '(');
    if (!start) return;
    start++;
    const char *end = strrchr(start, ')');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    for (size_t i = 0; i < len; i++) {
        typeChar(start[i]);
    }
}

void handleKbIsDown(const char *command) {
    int keycode = 0;
    if (sscanf(command + 10, "%d", &keycode) == 1) {
        bool isDown = Keyboard.isKeyDown((uint8_t)keycode);
        Serial0.print("kb.state(");
        Serial0.print(keycode);
        Serial0.print(",");
        Serial0.print(isDown ? "1" : "0");
        Serial0.println(")");
    }
}

void handleKbMediaTap(const char *command) {
    int bit = 0;
    if (sscanf(command + 8, "%d", &bit) == 1) {
        if (bit >= 0 && bit <= 15) {
            Keyboard.pressConsumer((uint8_t)bit);
            Keyboard.releaseConsumer((uint8_t)bit);
        }
    }
}

void handleKbMediaSet(const char *command) {
    int bits = 0;
    if (sscanf(command + 8, "%d", &bits) == 1) {
        Keyboard.setConsumer((uint16_t)bits);
    }
}
