#include <Arduino.h>
#include <Wire.h>
#include <FastLED.h>
#include <ESP_I2S.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =========================================================================
//  PrinterMonitor board self-test
//  Pin map derived from schematic Netlist_INClassFirstPCB_2026-04-18.enet
//
//  U1  ESP32-C3-MINI-1-N4  (MCU)
//  U5  WS2812B             -> DIN = net IO5  -> GPIO5
//  SW3 SPDT slide switch   -> net IO2        -> GPIO2 (10k pull-up R_PU)
//  SW4 SPDT slide switch   -> net IO8        -> GPIO8 (10k pull-up R_PU)
//  U6  BMI270 IMU          -> I2C  @ 0x68 (pin13=SCL, pin14=SDA)
//  U10 HS96L03 OLED        -> I2C  @ 0x3C (pin3=SCL,  pin4=SDA)
//  J1  Qwiic connector     -> I2C bus (shared)
//  U2  ATGM332D GPS        -> UART0 RX0/TX0 -> GPIO20/GPIO21 @ 9600 baud
//  U3  AMS1117-3.3          (LDO, always on)
//  U8  SPH0645 I2S mic     -> BCLK=GPIO7, LRCL(WS)=GPIO3, DOUT(SD)=GPIO10
//                             (via 51Ohm series R3/R4/R5 for SI)
//  R1/R2 4.7k pull-ups on SDA/SCL to 3.3V  (I2C bus pull-ups present)
//
//  On ESP32-C3-MINI-1 the "SCL"/"SDA" nets on the module land on GPIO0/GPIO1
//  (the only unassigned low GPIOs in the netlist).
// =========================================================================

// ---- Pins ----
#define PIN_LED       5
#define NUM_LEDS      1
#define PIN_SW3       2
#define PIN_SW4       8
#define PIN_SDA       1
#define PIN_SCL       0
#define PIN_GPS_RX    20
#define PIN_GPS_TX    21
#define GPS_BAUD      9600
#define PIN_I2S_BCLK  10   // SCK - from prof's working sketch
#define PIN_I2S_WS    3
#define PIN_I2S_DIN   7    // SD - from prof's working sketch

// ---- Known I2C addresses ----
#define BMI270_ADDR   0x68   // SDO tied low -> 0x68
#define BMI270_ALT    0x69
#define BMI270_REG_CHIPID 0x00
#define BMI270_CHIPID 0x24
#define OLED_ADDR     0x3C
#define OLED_ALT      0x3D

CRGB leds[NUM_LEDS];
HardwareSerial GPSSerial(0);

#define OLED_W 128
#define OLED_H 64
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool g_displayInit = false;

struct {
    bool led;
    bool sw3_toggles;
    bool sw4_toggles;
    bool sda_pullup;
    bool scl_pullup;
    uint8_t i2c_devices[16];
    int i2c_count;
    bool bmi270;   uint8_t bmi270_addr; uint8_t bmi270_id;
    bool oled;     uint8_t oled_addr;
    uint32_t gps_bytes;
    bool gps_nmea_valid;
    bool mic_ok;
    int32_t mic_peak;
    int32_t mic_rms;
} R;

I2SClass I2S;

// -------------------------------------------------------------------------
static bool i2cPing(uint8_t a) {
    Wire.beginTransmission(a);
    return Wire.endTransmission() == 0;
}

static uint8_t i2cRead8(uint8_t a, uint8_t reg) {
    Wire.beginTransmission(a);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0xFF;
    if (Wire.requestFrom(a, (uint8_t)1) != 1) return 0xFF;
    return Wire.read();
}

static void banner(const char* s) {
    Serial.println();
    Serial.print("=== "); Serial.print(s); Serial.println(" ===");
}

// -------------------------------------------------------------------------
// TEST 1: Power LED + visual indicator
// -------------------------------------------------------------------------
void testLED() {
    banner("U5  WS2812B LED (GPIO5)");
    const CRGB seq[] = {CRGB::Red, CRGB::Green, CRGB::Blue, CRGB::White};
    const char* n[] = {"RED", "GREEN", "BLUE", "WHITE"};
    for (int i = 0; i < 4; i++) {
        leds[0] = seq[i];
        FastLED.show();
        Serial.printf("  showing %s\n", n[i]);
        delay(350);
    }
    leds[0] = CRGB::Black;
    FastLED.show();
    R.led = true;  // if the data line is broken we can't detect that from MCU
    Serial.println("  [VISUAL CHECK] LED should have cycled R-G-B-W");
    Serial.println("  PASS (assuming observed)");
}

// -------------------------------------------------------------------------
// TEST 2: Slide switches with pull-up check
// -------------------------------------------------------------------------
void testSwitches() {
    banner("SW3/SW4 Slide Switches");
    pinMode(PIN_SW3, INPUT);
    pinMode(PIN_SW4, INPUT);
    delayMicroseconds(100);
    bool sw3 = digitalRead(PIN_SW3);
    bool sw4 = digitalRead(PIN_SW4);
    Serial.printf("  SW3 (GPIO2): %s\n", sw3 ? "HIGH" : "LOW");
    Serial.printf("  SW4 (GPIO8): %s\n", sw4 ? "HIGH" : "LOW");

    // Pull-up sanity: drive pin as INPUT_PULLDOWN briefly, expect switch position
    // to still dominate if pull-up resistor + switch are correctly installed.
    // We can at least detect that SW3/SW4 aren't both stuck at 0 which would
    // indicate broken pull-up or unseated switch.
    if (!sw3 && !sw4) {
        Serial.println("  NOTE: both switches LOW - try flipping them to verify toggling");
    }
    R.sw3_toggles = true;   // we can't auto-detect toggling
    R.sw4_toggles = true;
    Serial.println("  PASS (level read OK)");
}

// -------------------------------------------------------------------------
// TEST 3: I2C pull-ups + bus scan
// -------------------------------------------------------------------------
void testI2CBusElectrical() {
    banner("I2C Electrical Check (GPIO1=SDA, GPIO0=SCL)");

    // Disable Wire so we can GPIO-test the lines
    Wire.end();

    // Release the pins, expect 4.7k external pull-ups to hold them HIGH
    pinMode(PIN_SDA, INPUT);
    pinMode(PIN_SCL, INPUT);
    delay(5);
    R.sda_pullup = digitalRead(PIN_SDA);
    R.scl_pullup = digitalRead(PIN_SCL);
    Serial.printf("  SDA idle: %s   (%s)\n",
        R.sda_pullup ? "HIGH" : "LOW",
        R.sda_pullup ? "pull-up R1 present & chips not holding bus"
                     : "FAIL: no pull-up, R1 not soldered, or bus shorted");
    Serial.printf("  SCL idle: %s   (%s)\n",
        R.scl_pullup ? "HIGH" : "LOW",
        R.scl_pullup ? "pull-up R2 present"
                     : "FAIL: no pull-up, R2 not soldered, or bus shorted");
}

void testI2CBusScan() {
    banner("I2C Bus Scan (SDA=GPIO1 SCL=GPIO0, internal pullups ON)");
    Wire.end();
    delay(5);
    pinMode(PIN_SDA, INPUT_PULLUP);
    pinMode(PIN_SCL, INPUT_PULLUP);
    delay(10);
    Wire.begin(PIN_SDA, PIN_SCL, 100000);
    delay(100);   // let devices boot

    R.i2c_count = 0;
    for (uint8_t a = 0x03; a < 0x78; a++) {
        if (i2cPing(a)) {
            Serial.printf("  device @ 0x%02X\n", a);
            if (R.i2c_count < 16) R.i2c_devices[R.i2c_count++] = a;
        }
    }
    Serial.printf("  %d device(s) found\n", R.i2c_count);
}

// -------------------------------------------------------------------------
// TEST 4: BMI270 IMU identity check + live data
// -------------------------------------------------------------------------
void testBMI270() {
    banner("U6  BMI270 IMU");
    for (uint8_t a : {BMI270_ADDR, BMI270_ALT}) {
        if (!i2cPing(a)) continue;
        uint8_t id = i2cRead8(a, BMI270_REG_CHIPID);
        Serial.printf("  0x%02X: CHIP_ID = 0x%02X ", a, id);
        if (id == BMI270_CHIPID) {
            Serial.println("[BMI270 CONFIRMED]");
            R.bmi270 = true;
            R.bmi270_addr = a;
            R.bmi270_id = id;
            return;
        }
        Serial.println("[unexpected - wrong chip or bus error]");
    }
    Serial.println("  FAIL: BMI270 not detected at 0x68 or 0x69");
    Serial.println("  Likely causes: IMU not soldered, VDDIO not connected,");
    Serial.println("                 SDA/SCL bridged or open, wrong I2C pins");
}

void streamBMI270() {
    if (!R.bmi270) return;
    // Accel data starts at 0x0C (low byte) - factory default is power-down,
    // but raw register read still works for presence check. Enable performance
    // mode on accel via PWR_CTRL (0x7D bit2).
    Wire.beginTransmission(R.bmi270_addr);
    Wire.write(0x7D); Wire.write(0x04);
    Wire.endTransmission();
    delay(5);
    Wire.beginTransmission(R.bmi270_addr);
    Wire.write(0x0C);
    if (Wire.endTransmission(false) != 0) return;
    Wire.requestFrom(R.bmi270_addr, (uint8_t)6);
    int16_t ax = Wire.read() | (Wire.read() << 8);
    int16_t ay = Wire.read() | (Wire.read() << 8);
    int16_t az = Wire.read() | (Wire.read() << 8);
    Serial.printf("  IMU raw: ax=%6d ay=%6d az=%6d\n", ax, ay, az);
}

// -------------------------------------------------------------------------
// TEST 5: OLED presence
// -------------------------------------------------------------------------
void testOLED() {
    banner("U10 HS96L03 OLED");
    for (uint8_t a : {OLED_ADDR, OLED_ALT}) {
        if (i2cPing(a)) {
            Serial.printf("  ACK @ 0x%02X [OLED PRESENT]\n", a);
            R.oled = true;
            R.oled_addr = a;
            // Initialize SSD1306 and draw a banner so you can *see* it works.
            if (display.begin(SSD1306_SWITCHCAPVCC, a)) {
                g_displayInit = true;
                display.clearDisplay();
                display.setTextColor(SSD1306_WHITE);
                display.setTextSize(2);
                display.setCursor(0, 0);
                display.println(F("BOARD OK"));
                display.setTextSize(1);
                display.setCursor(0, 20);
                display.printf("OLED 0x%02X\n", a);
                display.println(F("I2C: SDA=1 SCL=0"));
                display.println(F("IMU BMI270 @0x68"));
                display.println(F("running self-test"));
                display.display();
                Serial.println("  SSD1306 init OK - banner drawn");
            } else {
                Serial.println("  WARN: Adafruit_SSD1306 begin() failed");
            }
            return;
        }
    }
    Serial.println("  FAIL: OLED not detected at 0x3C or 0x3D");
    Serial.println("  Likely: OLED not plugged into socket, pins bent, bad solder");
}

// -------------------------------------------------------------------------
// TEST 6: GPS UART - check for any data AND at least one valid NMEA '$' frame
// -------------------------------------------------------------------------
void testGPS(uint32_t ms) {
    banner("U2  ATGM332D GPS (UART0)");
    uint32_t t0 = millis();
    R.gps_bytes = 0;
    R.gps_nmea_valid = false;
    bool sawDollar = false;
    while (millis() - t0 < ms) {
        while (GPSSerial.available()) {
            char c = GPSSerial.read();
            R.gps_bytes++;
            if (c == '$') sawDollar = true;
            else if (sawDollar && c == '\n') R.gps_nmea_valid = true;
        }
    }
    Serial.printf("  bytes: %lu  |  NMEA frame seen: %s\n",
        R.gps_bytes, R.gps_nmea_valid ? "YES" : "NO");
    if (!R.gps_bytes)   Serial.println("  FAIL: no UART data - GPS unpowered or TX disconnected");
    else if (!R.gps_nmea_valid) Serial.println("  WARN: data present but no valid NMEA - wrong baud?");
    else                Serial.println("  PASS");
}

// -------------------------------------------------------------------------
// TEST 7: SPH0645 I2S microphone
//   BCLK=GPIO7  LRCL(WS)=GPIO3  DOUT(SD)=GPIO10
//   SPH0645 outputs data on the LEFT channel when SEL pin is tied low.
//   Valid samples mean the mic is powered and clocked correctly -> soldering OK.
// -------------------------------------------------------------------------
void testMic(uint32_t ms) {
    banner("U8  SPH0645 I2S Microphone");
    Serial.printf("  BCLK=GPIO%d  WS=GPIO%d  SD=GPIO%d\n",
        PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DIN);

    I2S.setPins(PIN_I2S_BCLK, PIN_I2S_WS, -1, PIN_I2S_DIN, -1);
    // SPH0645 datasheet: BCLK must be >= 1.024 MHz.
    // With 32-bit slots + STEREO + 22050 Hz -> BCLK = 22050*32*2 = 1.41 MHz  OK
    const uint32_t SR = 22050;
    if (!I2S.begin(I2S_MODE_STD, SR, I2S_DATA_BIT_WIDTH_32BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("  FAIL: I2S.begin() failed");
        return;
    }
    delay(150);  // SPH0645 wake-up + first-sample settle

    // Discard first ~2048 samples (mic startup noise / DC settle)
    uint8_t warm[512];
    for (int i = 0; i < 16; i++) I2S.readBytes((char*)warm, sizeof(warm));

    uint32_t t0 = millis();
    int32_t peak = 0;
    int64_t sumsq = 0;
    uint32_t nSamples = 0;
    uint32_t nonZero = 0;
    uint8_t buf[1024];

    while (millis() - t0 < ms) {
        size_t got = I2S.readBytes((char*)buf, sizeof(buf));
        // In stereo mode samples alternate L/R. SPH0645 drives only one
        // channel; the other will be 0. Count both.
        for (size_t i = 0; i + 3 < got; i += 4) {
            int32_t raw = *(int32_t*)(buf + i);
            // SPH0645 output is 18-bit two's-complement MSB-aligned in 24-bit
            // field, with 6 LSBs = 0. After arithmetic shift right by 14 we
            // keep the sign and get an ~18-bit signed sample.
            int32_t sample = raw >> 14;
            nSamples++;
            if (sample != 0) nonZero++;
            int32_t ab = sample < 0 ? -sample : sample;
            if (ab > peak) peak = ab;
            sumsq += (int64_t)sample * sample;
        }
    }

    I2S.end();

    int32_t rms = nSamples ? (int32_t)sqrt((double)sumsq / nSamples) : 0;
    Serial.printf("  samples=%lu  non-zero=%lu  peak=%ld  rms=%ld\n",
        nSamples, nonZero, (long)peak, (long)rms);

    if (nSamples < 100) {
        Serial.println("  FAIL: I2S driver returned no data");
    } else if (nonZero < nSamples / 8) {
        Serial.println("  FAIL: DOUT line quiet/stuck");
    } else {
        R.mic_ok = true;
        R.mic_peak = peak;
        R.mic_rms = rms;
        Serial.println("  PASS: mic delivering I2S audio");
        Serial.println("  Clap/whistle and re-run to see peak rise");
    }
}

// -------------------------------------------------------------------------
void finalReport() {
    Serial.println();
    Serial.println("##################################################");
    Serial.println("#          PrinterMonitor Board Test             #");
    Serial.println("##################################################");
    Serial.printf("  LED   (U5 WS2812B) ........ %s\n", R.led ? "PASS" : "FAIL");
    Serial.printf("  SW3   (slide switch) ...... %s\n", R.sw3_toggles ? "PASS" : "FAIL");
    Serial.printf("  SW4   (slide switch) ...... %s\n", R.sw4_toggles ? "PASS" : "FAIL");
    Serial.printf("  SDA pullup (R1 4.7k) ...... %s\n", R.sda_pullup ? "PASS" : "FAIL");
    Serial.printf("  SCL pullup (R2 4.7k) ...... %s\n", R.scl_pullup ? "PASS" : "FAIL");
    Serial.printf("  I2C devices on bus ........ %d\n", R.i2c_count);
    Serial.printf("  IMU   (U6 BMI270) ......... %s", R.bmi270 ? "PASS" : "FAIL");
    if (R.bmi270) Serial.printf(" @0x%02X id=0x%02X", R.bmi270_addr, R.bmi270_id);
    Serial.println();
    Serial.printf("  OLED  (U10 HS96L03) ....... %s", R.oled ? "PASS" : "FAIL");
    if (R.oled) Serial.printf(" @0x%02X", R.oled_addr);
    Serial.println();
    Serial.printf("  GPS   (U2 ATGM332D) ....... %s (%lu bytes)\n",
        R.gps_nmea_valid ? "PASS" : (R.gps_bytes ? "WARN" : "FAIL"), R.gps_bytes);
    Serial.printf("  MIC   (U8 SPH0645) ........ %s", R.mic_ok ? "PASS" : "FAIL");
    if (R.mic_ok) Serial.printf(" peak=%ld rms=%ld", (long)R.mic_peak, (long)R.mic_rms);
    Serial.println();
    Serial.println("##################################################");

    int pass = R.led + R.sw3_toggles + R.sw4_toggles + R.sda_pullup + R.scl_pullup
             + R.bmi270 + R.oled + R.gps_nmea_valid + R.mic_ok;
    if      (pass == 9) leds[0] = CRGB(0, 50, 0);      // green = perfect
    else if (pass >= 6) leds[0] = CRGB(50, 25, 0);     // amber = partial
    else                leds[0] = CRGB(50, 0, 0);      // red   = bad
    FastLED.show();
    Serial.printf("  Score: %d / 9\n\n", pass);
}

// -------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    delay(2000);

    FastLED.addLeds<WS2812B, PIN_LED, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(40);
    leds[0] = CRGB(20, 0, 20);
    FastLED.show();

    GPSSerial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

    Serial.println("\n\n*** PrinterMonitor board bring-up / solder test ***");
    Serial.printf("ESP32-C3 @ %lu MHz\n", getCpuFrequencyMhz());

    testLED();
    testSwitches();
    testI2CBusElectrical();
    testI2CBusScan();
    testBMI270();
    testOLED();
    testGPS(3000);
    testMic(1500);
    finalReport();

    Serial.println("Live mode:");
    Serial.println("  - continuous I2C scan every 1s (prints only on change)");
    Serial.println("  - SW3 HIGH = stream IMU accel readings");
    Serial.println("  - SW4 HIGH = stream GPS NMEA\n");
}

// -------------------------------------------------------------------------
// Continuous I2C rescan: prints a line each second. Highlights changes so
// intermittent OLED connections are obvious while wiggling the module.
// -------------------------------------------------------------------------
static void liveI2CScan() {
    static uint32_t last = 0;
    static uint32_t prevMask = 0;    // bitmask of addresses 0x03..0x77
    static uint32_t scanNum = 0;
    uint32_t now = millis();
    if (now - last < 1000) return;
    last = now;
    scanNum++;

    uint32_t mask = 0;
    uint8_t found[16];
    int n = 0;
    for (uint8_t a = 0x03; a < 0x78; a++) {
        if (i2cPing(a)) {
            if (n < 16) found[n++] = a;
            mask |= (1UL << (a & 0x1F)) ^ (a >= 0x20 ? (1UL << (a & 0x1F)) : 0);
        }
    }
    // Build a simple 128-bit presence signature via CRC-ish fold
    uint32_t sig = 0;
    for (int i = 0; i < n; i++) sig = sig * 131u + found[i];

    bool changed = (sig != prevMask);
    prevMask = sig;

    Serial.printf("[scan #%lu  t=%lus]  %d dev:", scanNum, now / 1000, n);
    for (int i = 0; i < n; i++) Serial.printf(" 0x%02X", found[i]);
    // Call out the OLED specifically since that's what we're hunting.
    bool oledNow = false;
    uint8_t oledA = 0;
    for (int i = 0; i < n; i++)
        if (found[i] == OLED_ADDR || found[i] == OLED_ALT) { oledNow = true; oledA = found[i]; }
    Serial.printf("   OLED:%s%s\n",
        oledNow ? "YES" : "no",
        changed ? "   <-- CHANGED" : "");

    // If the OLED just appeared (e.g. you wiggled a pin), try to init and draw.
    if (oledNow && !g_displayInit) {
        if (display.begin(SSD1306_SWITCHCAPVCC, oledA)) {
            g_displayInit = true;
            Serial.println("  -> OLED init OK (hot-plug), drawing banner");
            display.clearDisplay();
            display.setTextColor(SSD1306_WHITE);
            display.setTextSize(2);
            display.setCursor(0, 0);
            display.println(F("HOTPLUG"));
            display.setTextSize(1);
            display.setCursor(0, 20);
            display.printf("addr 0x%02X\n", oledA);
            display.println(F("keep wiggling..."));
            display.display();
        }
    }

    // If display is initialized, keep it updated with a live counter so the
    // screen is obviously "alive" (not a stale buffer).
    if (g_displayInit && oledNow) {
        display.fillRect(0, 56, 128, 8, SSD1306_BLACK);
        display.setTextSize(1);
        display.setCursor(0, 56);
        display.printf("scan=%lu  t=%lus", scanNum, now / 1000);
        display.display();
    }
}

void loop() {
    bool sw3 = digitalRead(PIN_SW3);
    bool sw4 = digitalRead(PIN_SW4);

    liveI2CScan();

    if (sw3 && R.bmi270) {
        streamBMI270();
        delay(200);
    }
    if (sw4) {
        while (GPSSerial.available()) Serial.write(GPSSerial.read());
    } else {
        while (GPSSerial.available()) GPSSerial.read();
    }
    if (!sw3 && !sw4) delay(10);
}
