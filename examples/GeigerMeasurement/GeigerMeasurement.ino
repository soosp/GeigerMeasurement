/**
 * GeigerMeasurement.ino
 *
 * Basic example for GeigerMeasurement.h on ESP32 / ESP8266.
 *
 * Output (Serial, 115200 baud):
 *   One line per second with CPM, µSv/h, EMA values, confidence, and window.
 */

#include <GeigerMeasurement.h>
#include <ExponentialAverage.h>

// ─── Configuration ────────────────────────────────────────────────────────────
constexpr int   GEIGER_PIN   = D7;      // NodeMCU D7 = GPIO13
constexpr float DEAD_TIME_US = 190.0f;  // µs — measure with getMeasuredDeadTime(), or leave 0

// ─── EMA ─────────────────────────────────────────────────────────────────
ExponentialAverage emaFast(0.10f);  // ~10 samples lag
ExponentialAverage emaSlow(0.01f);  // ~100 samples lag

GeigerMeasurement geiger(
    TUBE_SBM20,
    SOURCE_BACKGROUND,
    AveragingMode::FIXED_60S,
    DEAD_TIME_US
);

// ─── ISR ──────────────────────────────────────────────────────────────────────
void IRAM_ATTR geigerISR() {
    geiger.onPulse();
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    // Use INPUT, not INPUT_PULLUP. The matching of the input signal depends
    // on the circuit design of the GM counter.
    pinMode(GEIGER_PIN, INPUT);
    // ISR detects the falling edge of pulse from the GM circuit.
    attachInterrupt(digitalPinToInterrupt(GEIGER_PIN), geigerISR, FALLING);
    // Setting fieldFactor for the SBM-20 tube. See the documentation for
    // details.
    geiger.setFieldFactor(1.611f);
    Serial.println("Geiger monitor starting...");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t lastPrint = 0;

    if (millis() - lastPrint < 1000) return;
    lastPrint = millis();

    GeigerReading r = geiger.getReading();

    if (!r.valid) {
        emaFast.addSample(r.cpm);
        emaSlow.addSample(r.cpm);

        Serial.printf("[%6.1fs] Waiting for pulses...\n", millis() / 1000.0f);
        return;
    }

    Serial.printf(
        "[%6.1fs] CPM: %6.1f | µSv/h: %.4f | "
        "EMA-fast: %6.1f | EMA-slow: %6.1f | "
        "±%.0f%% | window: %.1fs | N: %u\n",
        millis() / 1000.0f,
        r.cpm, r.uSvH,
        emaFast.value(), emaSlow.value(),
        r.confidenceHalf,
        r.windowSec,
        r.pulseCount
    );
}
