/**
 * GeigerMonitor.ino
 *
 * Background radiation monitor — GeigerMeasurement + RollingStats.
 *
 * Wiring:
 *   GM tube output → GEIGER_PIN with external pull-up resistor to 3.3 V.
 *   Most GM modules produce an active-low pulse on each detected particle.
 *
 * Output (Serial, 115200 baud):
 *   Every second: CPM, µSv/h, confidence interval, EMA values.
 *   Every minute: 30-minute and 1-hour rolling averages, min, max, σ.
 *
 * Required libraries:
 *   GeigerMeasurement  (this library)
 *   RunningStats       (companion library — provides RollingStats)
 */

#include "GeigerMeasurement.h"
#include <RollingStats.h>

// ─── Configuration ────────────────────────────────────────────────────────────
constexpr int   GEIGER_PIN   = 4;
constexpr float DEAD_TIME_US = 150.0f;  // µs — measure with getMeasuredDeadTime(), or leave 0

// ─── GeigerMeasurement ────────────────────────────────────────────────────────
// SBM-20 tube, background radiation, 60-second sliding window
GeigerMeasurement geiger(
    TUBE_SBM20,
    SOURCE_BACKGROUND,
    AveragingMode::FIXED_60S,
    DEAD_TIME_US
);

// ─── RollingStats ─────────────────────────────────────────────────────────────
// 128 bins × 60 s = 7680 s ≈ 2 hours maximum history.
// Queryable windows: any multiple of 60 s up to 7680 s.
RollingStats<128, 60> stats;

// ─── ISR ──────────────────────────────────────────────────────────────────────
void IRAM_ATTR geigerISR() {
    geiger.onPulse();
}

// ─── State ────────────────────────────────────────────────────────────────────
bool     tubeWasAlive = true;
uint32_t lastSecond   = 0;
uint32_t lastMinute   = 0;

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(GEIGER_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(GEIGER_PIN), geigerISR, FALLING);
    Serial.println("=== Geiger monitor starting ===");
    Serial.printf("Max window: %u s (~%.1f min)\n",
                  stats.maxWindowSeconds(),
                  stats.maxWindowSeconds() / 60.0f);
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {

    // ── Every second: instantaneous reading ───────────────────────────────
    if (millis() - lastSecond >= 1000) {
        lastSecond = millis();

        GeigerReading r = geiger.getReading();

        // Tube fault detection — report only on state change to avoid flooding
        if (!r.tubeAlive && tubeWasAlive)
            Serial.println("WARNING: no pulse — check tube, wiring, and HV supply");
        else if (r.tubeAlive && !tubeWasAlive)
            Serial.println("INFO: tube signal restored");
        tubeWasAlive = r.tubeAlive;

        if (r.saturated)
            Serial.println("WARNING: dead-time saturation — reading unreliable");

        if (!r.valid) {
            Serial.printf("[%5.0fs] Waiting...\n", millis() / 1000.0f);
            return;
        }

        Serial.printf(
            "[%5.0fs] CPM: %6.1f | µSv/h: %.4f | ±%.0f%% | "
            "EMA-fast: %5.1f | EMA-slow: %5.1f\n",
            millis() / 1000.0f,
            r.cpm, r.uSvH, r.confidenceHalf,
            r.cpmEmaFast, r.cpmEmaSlow
        );

        // Feed RollingStats — use timestampMs from the reading, not millis(),
        // to avoid timing skew from processing time after getReading().
        // compensatedPulseCount uses integer carry accumulation (RadPro method),
        // which is more accurate than r.cpm for long-term rolling averages.
        float compensatedCpm = r.compensatedPulseCount * 60.0f / r.windowSec;
        stats.addSample(compensatedCpm, r.timestampMs);

        // Apply the lifetime dead-time estimate whenever it improves.
        // getMeasuredDeadTime() accumulates the minimum inter-pulse interval
        // across all pulses since the last tube change — no explicit trigger needed.
        float dt = geiger.getMeasuredDeadTime();
        if (dt > 0.0f) geiger.setDeadTime(dt);
    }

    // ── Every minute: rolling statistics ──────────────────────────────────
    if (lastMinute > 0 && millis() - lastMinute >= 60000) {
        lastMinute = millis();

        Serial.println("─── Rolling statistics ─────────────────────────");

        // 30-minute window — require at least 80% of bins to have valid data
        if (stats.hasValidWindow(1800, 0.8f)) {
            Serial.printf(
                "  30 min — avg: %.1f CPM | min: %.1f | max: %.1f | σ: %.1f\n",
                stats.average(1800),
                stats.minimum(1800),
                stats.maximum(1800),
                stats.stdDev(1800)
            );
        } else {
            Serial.printf("  30 min — collecting data (%u s / 1800 s valid)\n",
                          stats.validSeconds(1800));
        }

        // 1-hour window
        if (stats.hasValidWindow(3600, 0.8f)) {
            Serial.printf(
                "   1 hr  — avg: %.1f CPM | min: %.1f | max: %.1f | σ: %.1f\n",
                stats.average(3600),
                stats.minimum(3600),
                stats.maximum(3600),
                stats.stdDev(3600)
            );
        } else {
            Serial.printf("   1 hr  — collecting data (%u s / 3600 s valid)\n",
                          stats.validSeconds(3600));
        }

        Serial.println("────────────────────────────────────────────────");
    }

    if (lastMinute == 0) lastMinute = millis();
}
