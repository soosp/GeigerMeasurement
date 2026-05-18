/**
 * GeigerPulseIndicator.ino
 *
 * Demonstrates per-pulse LED flash and passive buzzer click using a
 * volatile flag set by the ISR and consumed in loop().
 *
 * At low count rates every pulse triggers an indication. At high count
 * rates the indications are decimated — similar to the approach used in
 * RadPro (pulses.c: onPulseTick / indicationRemainder) — so the LED and
 * buzzer remain responsive without flooding the main loop.
 *
 * Wiring:
 *   GM tube output → GEIGER_PIN (active-low pulse, external pull-up)
 *   LED            → LED_PIN (anode via ~220 Ω resistor to 3.3 V)
 *   Passive buzzer → BUZZER_PIN (PWM-capable pin)
 *
 * Indication behaviour:
 *   - Below INDICATION_MAX_CPM: every pulse triggers LED + click.
 *   - Above INDICATION_MAX_CPM: one indication per
 *     (cpm / INDICATION_MAX_CPM) pulses, keeping the indication rate
 *     at or below INDICATION_MAX_CPM per minute.
 *
 * The LED and buzzer durations are kept short so they do not overlap at
 * background rates (~20 CPM → one pulse every ~3 seconds).
 */

#include "GeigerMeasurement.h"

// ─── Pin configuration ────────────────────────────────────────────────────────
constexpr int GEIGER_PIN = 4;
constexpr int LED_PIN    = 2;
constexpr int BUZZER_PIN = 5;

// ─── Indication parameters ────────────────────────────────────────────────────
constexpr uint32_t LED_ON_MS          = 10;     // LED flash duration [ms]
constexpr uint32_t BUZZER_FREQ_HZ     = 1000;   // click frequency [Hz]
constexpr uint32_t BUZZER_ON_MS       = 5;      // click duration [ms]
constexpr float    INDICATION_MAX_CPM = 200.0f; // max indication rate [CPM]
                                                // above this, indications
                                                // are decimated

// ─── GeigerMeasurement ────────────────────────────────────────────────────────
GeigerMeasurement geiger(TUBE_SBM20, SOURCE_BACKGROUND);

// ─── ISR shared state ─────────────────────────────────────────────────────────
// _pulseFlag is set by the ISR on every pulse and cleared in loop().
// If loop() is slower than the pulse rate, flags are merged — that is
// intentional: the decimation logic below handles high-rate smoothing.
volatile bool _pulseFlag = false;

void IRAM_ATTR geigerISR() {
    geiger.onPulse();
    _pulseFlag = true;
}

// ─── Indication state ─────────────────────────────────────────────────────────
uint32_t ledOffMs    = 0;  // millis() when LED should turn off (0 = off)
uint32_t buzzerOffMs = 0;  // millis() when buzzer should stop  (0 = off)

// Decimation accumulator (RadPro-style):
//   Every consumed pulse adds (1 / indicationPeriod) to the accumulator.
//   When it reaches 1.0, an indication fires and 1.0 is subtracted.
//   At low rates (cpm ≤ INDICATION_MAX_CPM): period = 1 → every pulse fires.
//   At high rates: period > 1 → indications are spread out.
float indicationAccumulator = 0.0f;

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(GEIGER_PIN, INPUT);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    attachInterrupt(digitalPinToInterrupt(GEIGER_PIN), geigerISR, FALLING);

    Serial.println("=== GeigerPulseIndicator ===");
    Serial.printf("LED:     pin %d, %u ms flash\n", LED_PIN, LED_ON_MS);
    Serial.printf("Buzzer:  pin %d, %u Hz, %u ms click\n",
                  BUZZER_PIN, BUZZER_FREQ_HZ, BUZZER_ON_MS);
    Serial.printf("Decimation threshold: %.0f CPM\n", INDICATION_MAX_CPM);
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    // ── Consume pulse flag ────────────────────────────────────────────────
    if (_pulseFlag) {
        _pulseFlag = false;

        // Compute the decimation period from the current CPM estimate.
        // Use getReading() if valid; fall back to 1 CPM during startup
        // so the first pulse always triggers an indication.
        GeigerReading r = geiger.getReading();
        float cpm = (r.valid && r.cpm > 0.0f) ? r.cpm : 1.0f;

        // period = how many pulses per indication.
        // Clamped to ≥ 1: at low rates every pulse fires.
        float period = cpm / INDICATION_MAX_CPM;
        if (period < 1.0f) period = 1.0f;

        indicationAccumulator += 1.0f / period;

        if (indicationAccumulator >= 1.0f) {
            indicationAccumulator -= 1.0f;

            // ── LED ───────────────────────────────────────────────────────
            // Skip if the previous flash is still active — avoids
            // extending it unintentionally at high rates.
            if (ledOffMs == 0) {
                digitalWrite(LED_PIN, HIGH);
                ledOffMs = now + LED_ON_MS;
            }

            // ── Buzzer ────────────────────────────────────────────────────
            if (buzzerOffMs == 0) {
                tone(BUZZER_PIN, BUZZER_FREQ_HZ);
                buzzerOffMs = now + BUZZER_ON_MS;
            }
        }
    }

    // ── Turn off LED when duration expires ────────────────────────────────
    // Cast to int32_t for overflow-safe comparison across millis() rollover.
    if (ledOffMs > 0 && (int32_t)(now - ledOffMs) >= 0) {
        digitalWrite(LED_PIN, LOW);
        ledOffMs = 0;
    }

    // ── Turn off buzzer when duration expires ─────────────────────────────
    if (buzzerOffMs > 0 && (int32_t)(now - buzzerOffMs) >= 0) {
        noTone(BUZZER_PIN);
        buzzerOffMs = 0;
    }
}
