// ---------------------------------------------------------------------------
// SX1262 low-level probe. Answers one question: is the radio physically there
// and wired to the pins we think it is?
//
// Runs no RadioLib code, so it fails in ways we can interpret.
//
// Test 1 (float test) tells module-absent from module-present without any SPI:
//   an unconnected GPIO follows whichever internal pull we enable; a pin driven
//   by a real chip does not.
// Test 3 reads the SX126x version-string register at 0x0320, which holds ASCII
//   "SX1261"/"SX1262". This works straight after reset, on the internal RC
//   oscillator, so a TCXO misconfiguration cannot mask a present chip.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <SPI.h>
#include "board_pins.h"

static SPIClass probeSpi(FSPI);

static bool waitBusyLow(uint32_t timeoutMs) {
    uint32_t t0 = millis();
    while (digitalRead(LORA_BUSY) == HIGH) {
        if (millis() - t0 > timeoutMs) return false;
        delay(1);
    }
    return true;
}

// Returns: 2 = driven, 1 = floating (follows internal pull), 0 = stuck
static int floatTest(uint8_t pin, const char *name) {
    pinMode(pin, INPUT_PULLUP);
    delay(5);
    int withPullup = digitalRead(pin);
    pinMode(pin, INPUT_PULLDOWN);
    delay(5);
    int withPulldown = digitalRead(pin);
    pinMode(pin, INPUT);

    const char *verdict;
    int result;
    if (withPullup == 1 && withPulldown == 0) {
        verdict = "FLOATING  <- nothing is driving this pin";
        result = 1;
    } else if (withPullup == withPulldown) {
        verdict = (withPullup == 0) ? "driven LOW  (something is here)"
                                    : "driven HIGH (something is here)";
        result = 2;
    } else {
        verdict = "inconsistent";
        result = 0;
    }
    Serial.printf("  %-6s (GPIO%2u): pullup=%d pulldown=%d  %s\n",
                  name, pin, withPullup, withPulldown, verdict);
    return result;
}

static void radioReset() {
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delay(2);
    digitalWrite(LORA_RST, HIGH);
    delay(20);
}

static void readVersionString(char *out, size_t n) {
    const uint16_t REG_VERSION_STRING = 0x0320;
    digitalWrite(LORA_CS, LOW);
    probeSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    probeSpi.transfer(0x1D);                            // ReadRegister opcode
    probeSpi.transfer((REG_VERSION_STRING >> 8) & 0xFF);
    probeSpi.transfer(REG_VERSION_STRING & 0xFF);
    probeSpi.transfer(0x00);                            // status byte
    for (size_t i = 0; i < n; i++) out[i] = probeSpi.transfer(0x00);
    probeSpi.endTransaction();
    digitalWrite(LORA_CS, HIGH);
}


// ---------------------------------------------------------------------------
// Full header scan. Rather than trust a pin map, find the chip's outputs by
// observation: hold the radio in reset and scan every XIAO pin, then release
// reset and scan again. Pins that go from floating to driven are outputs of a
// live SX1262 -- BUSY in particular goes low once the chip is ready.
// ---------------------------------------------------------------------------
struct XiaoPin { uint8_t gpio; const char *name; };
static const XiaoPin XIAO_PINS[] = {
    {1, "D0"}, {2, "D1"}, {3, "D2"}, {4, "D3"}, {5, "D4"}, {6, "D5"},
    {43, "D6"}, {44, "D7"}, {7, "D8"}, {8, "D9"}, {9, "D10"},
};
static const size_t XIAO_PIN_COUNT = sizeof(XIAO_PINS) / sizeof(XIAO_PINS[0]);

// 'F' = floating, 'H' = driven high, 'L' = driven low, '?' = inconsistent
static char pinState(uint8_t pin) {
    pinMode(pin, INPUT_PULLUP);
    delay(3);
    int up = digitalRead(pin);
    pinMode(pin, INPUT_PULLDOWN);
    delay(3);
    int down = digitalRead(pin);
    pinMode(pin, INPUT);
    if (up == 1 && down == 0) return 'F';
    if (up == 1 && down == 1) return 'H';
    if (up == 0 && down == 0) return 'L';
    return '?';
}

static void scanAllPins() {
    char inReset[XIAO_PIN_COUNT];
    char running[XIAO_PIN_COUNT];

    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delay(20);
    for (size_t i = 0; i < XIAO_PIN_COUNT; i++) {
        if (XIAO_PINS[i].gpio == LORA_RST) { inReset[i] = '-'; continue; }
        inReset[i] = pinState(XIAO_PINS[i].gpio);
    }

    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, HIGH);
    delay(50);
    for (size_t i = 0; i < XIAO_PIN_COUNT; i++) {
        if (XIAO_PINS[i].gpio == LORA_RST) { running[i] = '-'; continue; }
        running[i] = pinState(XIAO_PINS[i].gpio);
    }

    Serial.println("\n[4] Header scan (F=floating, H=driven high, L=driven low, -=our reset pin):");
    Serial.println("     pin  gpio  in-reset  running   expected role");
    for (size_t i = 0; i < XIAO_PIN_COUNT; i++) {
        const char *role = "-";
        uint8_t g = XIAO_PINS[i].gpio;
        if (g == LORA_CS)        role = "CS";
        else if (g == LORA_DIO1) role = "DIO1";
        else if (g == LORA_BUSY) role = "BUSY";
        else if (g == LORA_RST)  role = "RESET";
        else if (g == LORA_RXEN) role = "RXEN";
        else if (g == LORA_SCK)  role = "SCK";
        else if (g == LORA_MISO) role = "MISO";
        else if (g == LORA_MOSI) role = "MOSI";
        Serial.printf("     %-4s %4u     %c         %c       %s\n",
                      XIAO_PINS[i].name, g, inReset[i], running[i], role);
    }

    Serial.println("\n  Any pin that is 'F' in both columns has no electrical connection");
    Serial.println("  to the module -- an unsoldered or cold joint, or a pin the module");
    Serial.println("  genuinely does not use. Chip outputs should read H or L when running.");
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);

    Serial.println("\n=== SX1262 probe :: " NODE_NAME " node ===");
    Serial.printf("Pins: CS=%d DIO1=%d BUSY=%d RST=%d RXEN=%d | SCK=%d MISO=%d MOSI=%d\n",
                  LORA_CS, LORA_DIO1, LORA_BUSY, LORA_RST, LORA_RXEN,
                  LORA_SCK, LORA_MISO, LORA_MOSI);

    Serial.println("\n[1] Float test, radio held in reset (all pins should read FLOATING\n"
                   "    only if no module is attached):");
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);   // hold in reset so the chip drives nothing
    delay(10);
    int busyFloat = floatTest(LORA_BUSY, "BUSY");
    floatTest(LORA_DIO1, "DIO1");
    floatTest(LORA_MISO, "MISO");

    Serial.println("\n[2] Releasing reset. A present SX1262 pulls BUSY low when ready:");
    pinMode(LORA_BUSY, INPUT);
    radioReset();
    bool busyOk = waitBusyLow(1000);
    Serial.printf("  BUSY went low within 1 s: %s\n", busyOk ? "YES" : "NO");

    Serial.println("\n[3] SPI read of the chip version register (expect \"SX1262\"):");
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
    probeSpi.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    radioReset();
    waitBusyLow(1000);

    char ver[7] = {0};
    readVersionString(ver, 6);
    Serial.print("  raw bytes:");
    for (int i = 0; i < 6; i++) Serial.printf(" %02X", (uint8_t)ver[i]);
    for (int i = 0; i < 6; i++) if (!isprint((uint8_t)ver[i])) ver[i] = '.';
    Serial.printf("\n  as text : \"%s\"\n", ver);

    scanAllPins();

    Serial.println("\n--- verdict ---");
    if (strstr(ver, "SX126") != nullptr) {
        Serial.println("  Radio FOUND and talking. Pin map is correct.");
    } else if (busyFloat == 1 && !busyOk) {
        Serial.println("  No module detected: BUSY floats and never goes low.");
        Serial.println("  -> The LoRa board is not seated, or not connected at all.");
    } else if (busyOk) {
        Serial.println("  Something is driving BUSY, but SPI returns nothing useful.");
        Serial.println("  -> Module is present; suspect SPI pins (CS/SCK/MISO/MOSI).");
    } else {
        Serial.println("  Inconclusive - see the raw values above.");
    }
    Serial.println("--- end ---");
}

void loop() { delay(1000); }
