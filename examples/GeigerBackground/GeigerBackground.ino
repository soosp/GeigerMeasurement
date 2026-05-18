/**
 * GeigerBackground.ino
 *
 * Background radiation monitor with automatic mode transition.
 *
 * Strategy:
 *   - On startup, uses ADAPTIVE_FAST mode to provide readings as quickly as
 *     possible (from the first ~19 pulses onward, typically within seconds
 *     at background levels).
 *   - Once 60 seconds of data has accumulated, automatically switches to
 *     FIXED_60S mode for stable, low-noise background measurements.
 *
 * This gives the best of both worlds:
 *   - Immediate feedback during startup (adaptive fast)
 *   - Stable long-term readings once settled (fixed 60s sliding window)
 *
 * Hardware:
 *   GM tube output → GPIO D7 (NodeMCU) with external pull-up resistor to 3.3V.
 *   Note: INPUT_PULLUP is unreliable on NodeMCU D7 — use external resistor.
 *
 * Output (Serial, 115200 baud):
 *   One line per second with CPM, µSv/h, confidence, EMA values, and mode.
 *   Every minute: 30-minute and 1-hour averages from RollingStats.
 */

#include <GeigerMeasurement.h>
#include <ExponentialAverage.h>
#include <RollingStats.h>

// ─── Pin configuration ────────────────────────────────────────────────────
constexpr int GEIGER_PIN = 13;

// ─── GeigerMeasurement ───────────────────────────────────────────────────
// Start in ADAPTIVE_FAST — provides readings from the first ~19 pulses.
// Dead time left at 0 (disabled) until measureDeadTime() has enough data.
GeigerMeasurement geiger(
    TUBE_SBM20,
    SOURCE_BACKGROUND,
    AveragingMode::ADAPTIVE_FAST,
    0.0f
);

// ─── RollingStats ────────────────────────────────────────────────────────
// 128 bins × 60s = 7680s ≈ 2 hours maximum.
// Queryable windows: 60, 120, 180, ... 7680 seconds.
RollingStats<128, 60> stats;

// ─── EMA ─────────────────────────────────────────────────────────────────
ExponentialAverage emaFast(0.10f);  // ~10 samples lag
ExponentialAverage emaSlow(0.01f);  // ~100 samples lag

// ─── ISR ─────────────────────────────────────────────────────────────────
void IRAM_ATTR geigerISR() {
    geiger.onPulse();
}

// ─── State ───────────────────────────────────────────────────────────────
bool     inAdaptiveMode    = true;   // true until we switch to FIXED_60S
bool     tubeWasAlive      = true;   // tracks previous tubeAlive state for fault detection
uint32_t lastSecondMs      = 0;
uint32_t lastMinuteMs      = 0;

// ─── Setup ───────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(GEIGER_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(GEIGER_PIN), geigerISR, FALLING);

    // Setting fieldFactor for the SBM-20 tube. See the documentation for
    // details.
    geiger.setFieldFactor(1.611f);

    Serial.println("=== Background radiation monitor ===");
    Serial.println("Mode: ADAPTIVE_FAST (waiting for 60s of data...)");
    Serial.println("CPM        µSv/h    ±CI%  EMA-fast EMA-slow  Mode");
}

// ─── Loop ────────────────────────────────────────────────────────────────
void loop() {

    // ── Every second: read and display ────────────────────────────────────
    if (millis() - lastSecondMs >= 1000) {
        lastSecondMs = millis();

        GeigerReading r = geiger.getReading();
        if (r.valid) { emaFast.addSample(r.cpm); emaSlow.addSample(r.cpm); }

        // ── Fault detection ───────────────────────────────────────────────
        // tubeAlive: report only on state *change* to avoid flooding the log.
        // A false value during the first seconds after startup is normal —
        // the sensitivity-dependent timeout may not have elapsed yet.
        if (!r.tubeAlive && tubeWasAlive) {
            Serial.println("WARNING: no pulse — check tube, wiring, and HV supply");
        } else if (r.tubeAlive && !tubeWasAlive) {
            Serial.println("INFO: tube signal restored");
        }
        tubeWasAlive = r.tubeAlive;

        if (r.saturated) {
            Serial.println("WARNING: dead-time saturation — reading unreliable");
        }
        if (r.counterSaturated) {
            Serial.println("WARNING: pulse counter saturated — calling reset()");
            geiger.reset();
        }

        // ── Mode transition: ADAPTIVE_FAST → FIXED_60S ────────────────────
        // Switch once 60 seconds have elapsed since startup.
        // millis() > 60000 is a simple proxy — we could also check
        // geiger.lifetimePulses() or a flag set after the first valid reading.
        if (inAdaptiveMode && millis() >= 60000UL) {
            geiger.setMode(AveragingMode::FIXED_60S);
            inAdaptiveMode = false;
            Serial.println("--- Switched to FIXED_60S mode ---");
        }

        // ── Dead-time estimation (continuous, lifetime minimum) ──────────
        // getMeasuredDeadTime() returns the minimum inter-pulse interval
        // observed across all pulses since the last tube change — a
        // continuously improving upper bound on τ. No explicit trigger needed.
        // The value only improves over time, so applying it on every iteration
        // is safe and gradually increases compensation accuracy.
        float dt = geiger.getMeasuredDeadTime();
        if (dt > 0.0f) geiger.setDeadTime(dt);

        // ── Display current reading ───────────────────────────────────────
        if (!r.valid) {
            // Adaptive mode, not enough pulses yet
            Serial.printf("[%5.0fs] Waiting for pulses... (lifetime: %u)\n",
                          millis() / 1000.0f, (unsigned int)geiger.lifetimePulses());
        } else {
            Serial.printf(
                "[%5.0f s] %6.1f CPM  %6.4f µSv/h  ±%4.0f%%  "
                "fast:%5.1f  slow:%5.1f  τ:%7.0f µs  %s\n",
                millis() / 1000.0f,
                r.cpm,
                r.uSvH,
                r.confidenceHalf,
                emaFast.value(),
                emaSlow.value(),
                dt,
                inAdaptiveMode ? "ADAPTIVE" : "FIXED_60S"
            );

            // Feed RollingStats — use timestampMs from the reading, not millis(),
            // to avoid timing skew from processing time after getReading().
            stats.addSample(r.cpm, r.timestampMs);
        }
    }

    // ── Every minute: long-term statistics ───────────────────────────────
    if (millis() - lastMinuteMs >= 60000UL && lastMinuteMs > 0) {
        lastMinuteMs = millis();

        Serial.println("─── Long-term statistics ────────────────────────");

        // 30-minute window (1800s = 30 × 60s bins)
        // hasValidWindow(1800, 0.8f): require at least 80% of bins to have
        // actual data — avoids misleadingly low averages during startup when
        // most bins are NaN (e.g. only 2 minutes of data in a 30-min window).
        if (stats.hasValidWindow(1800, 0.8f)) {
            Serial.printf(
                "  30 min — avg: %.1f CPM  min: %.1f  max: %.1f  σ: %.1f\n",
                stats.average(1800),
                stats.minimum(1800),
                stats.maximum(1800),
                stats.stdDev(1800)
            );
        } else {
            // Show how many valid minutes are available so far
            Serial.printf("  30 min — collecting data (%us / 1800s valid)\n",
                          stats.validSeconds(1800));
        }

        // 1-hour window (3600s = 60 × 60s bins)
        if (stats.hasValidWindow(3600, 0.8f)) {
            Serial.printf(
                "   1 hr  — avg: %.1f CPM  min: %.1f  max: %.1f  σ: %.1f\n",
                stats.average(3600),
                stats.minimum(3600),
                stats.maximum(3600),
                stats.stdDev(3600)
            );
        } else {
            Serial.printf("   1 hr  — collecting data (%us / 3600s valid)\n",
                          stats.validSeconds(3600));
        }

        Serial.println("─────────────────────────────────────────────────");
    }

    // Initialise lastMinuteMs on first iteration to avoid immediate printout
    if (lastMinuteMs == 0) lastMinuteMs = millis();
}
