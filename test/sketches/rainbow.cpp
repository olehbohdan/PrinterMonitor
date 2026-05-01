// ============================================================================
// rainbow.cpp - WS2812B rainbow cycle on GPIO5.
//   - Rainbow only animates while SW3 (GPIO2) or SW4 (GPIO8) is HIGH.
//   - Both switches LOW -> LED off (idle).
// ============================================================================

#include <Arduino.h>
#include <FastLED.h>

#define PIN_LED  5
#define PIN_SW3  2
#define PIN_SW4  8
#define NUM_LEDS 1

CRGB leds[NUM_LEDS];

void setup() {
    Serial.begin(115200);
    pinMode(PIN_SW3, INPUT_PULLDOWN);
    pinMode(PIN_SW4, INPUT_PULLDOWN);

    FastLED.addLeds<WS2812B, PIN_LED, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(60);
    leds[0] = CRGB::Black;
    FastLED.show();
}

void loop() {
    static uint8_t hue = 0;
    bool on = digitalRead(PIN_SW3) == HIGH || digitalRead(PIN_SW4) == HIGH;

    if (on) {
        leds[0] = CHSV(hue++, 255, 255);
    } else {
        leds[0] = CRGB::Black;
    }
    FastLED.show();
    delay(20);
}
