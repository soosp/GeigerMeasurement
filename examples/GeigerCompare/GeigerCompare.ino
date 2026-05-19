/**
 * GeigerCompare.ino
 *
 * Parallel tube sensitivity comparison tool.
 *
 * Purpose:
 *   Run identical firmware on multiple ESP8266/ESP32 devices — each with a
 *   different GM tube — to compare their relative sensitivities under the
 *   same background radiation field.  The output is CSV-friendly so readings
 *   from several devices can easily be pasted side-by-side in a spreadsheet.
 *
 * Usage:
 *   1. Set TUBE_TYPE below to match the tube installed in each device:
 *        TUBE_J305   — J305
 *        TUBE_M4011  — M4011 or J321
 *        TUBE_SBM20  — SBM-20
 *   2. Flash all devices with their respective TUBE_TYPE.
 *   3. Power them on at the same time (or note the start-time offset).
 *   4. Collect Serial output (115200 baud) from all devices.
 *   5. Compare the "avg_cpm" column across devices — the ratio should match
 *      the Rad Lab sensitivity ratios:
 *        J305  / SBM-20 ≈ 1.274   (135.2 / 106.1)
 *        M4011 / SBM-20 ≈ 1.021   (108.3 / 106.1)
 *        J305  / M4011  ≈ 1.249   (135.2 / 108.3)
 *
 * Output format:
 *   Header line on startup, then one CSV line per minute:
 *     elapsed_s, avg_cpm, min_cpm, max_cpm, sigma_cpm, avg_usvh, n_samples
 *   Plus a human-readable summary block every 30 minutes.
 *
 * Measurement strategy:
 *   Starts in ADAPTIVE_FAST for immediate feedback, then switches to
 *   FIXED_60S after 60 s.  RollingStats accumulates 1-minute CPM averages.
 *   Only the long-term CPM average is relevant for the comparison —
 *   short-term fluctuations are expected (Poisson statistics).
 *
 * Hardware:
 *   GM tube output → GPIO D7 (NodeMCU) with external pull-up resistor to 3.3V.
 *   Note: INPUT_PULLUP is unreliable on NodeMCU D7 — use an external resistor.
 *
 * Required libraries:
 *   GeigerMeasurement  (this library)
 *   RunningStatistics  (companion library — provides RollingStats and CumulativeStats)
 */

#include <GeigerMeasurement.h>
#include <RollingStats.h>
#include <CumulativeStats.h>

// ─── Configuration ────────────────────────────────────────────────────────────
//
// Set TUBE_TYPE to match the tube in this device before flashing.
//
#define TUBE_TYPE   TUBE_SBM20      // ← change per device: TUBE_J305 / TUBE_M4011 / TUBE_SBM20

// GPIO pin connected to the GM tube output (active-low pulse).
constexpr int GEIGER_PIN = 13;

// Minimum number of valid 1-minute bins required before printing the
// summary block.  At 1 bin/minute this equals a 10-minute warm-up.
constexpr uint32_t SUMMARY_MIN_BINS = 10;

// Summary interval in milliseconds (default: every 30 minutes).
constexpr uint32_t SUMMARY_INTERVAL_MS = 30UL * 60UL * 1000UL;

// ─── Tube label ───────────────────────────────────────────────────────────────
// tubeLabel() is provided by GeigerTubes.h — returns "Custom" for TUBE_CUSTOM.
static const char* TUBE_LABEL = tubeLabel(TUBE_TYPE);

// ─── GeigerMeasurement ────────────────────────────────────────────────────────
// ADAPTIVE_FAST for the first 60 s, then switched to FIXED_60S in loop().
// Dead time disabled (0) — not needed for a relative comparison.
GeigerMeasurement geiger(
    TUBE_TYPE,
    SOURCE_BACKGROUND,
    AveragingMode::ADAPTIVE_FAST,
    0.0f
);

// ─── RollingStats ─────────────────────────────────────────────────────────────
// 128 bins × 60 s = 7680 s ≈ 2.1 hours of history.
// Queried at 30-minute (1800 s) and 1-hour (3600 s) windows.
RollingStats<128, 60> stats;

// ─── CumulativeStats ──────────────────────────────────────────────────────────
// Lifetime average and dose — never discards data, ideal for tube comparison.
CumulativeStats lifetime;

// ─── ISR ──────────────────────────────────────────────────────────────────────
void IRAM_ATTR geigerISR() {
    geiger.onPulse();
}

// ─── State ────────────────────────────────────────────────────────────────────
bool     inAdaptiveMode = true;
bool     tubeWasAlive   = true;
uint32_t lastSecondMs   = 0;
uint32_t lastMinuteMs   = 0;
uint32_t lastSummaryMs  = 0;
uint32_t minuteCount    = 0;    // number of complete 1-minute bins added to stats

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Print the CSV header.  Called once from setup().
static void printHeader() {
    Serial.println();
    Serial.printf("=== GeigerCompare — tube: %s ===\n", TUBE_LABEL);
    Serial.printf("Rad Lab sensitivity (Cs-137 / background): %.3f CPM/(uSv/h)\n",
                  tubeSourceSensitivity(TUBE_TYPE, SOURCE_BACKGROUND));
    Serial.println();
    // CSV columns:
    //   elapsed_s       — seconds since boot
    //   win_avg_cpm     — sliding window average CPM (all data so far)
    //   win_sigma_cpm   — sliding window standard deviation
    //   life_avg_cpm    — lifetime average CPM (never discards data)
    //   life_sigma_cpm  — lifetime standard deviation
    //   life_usvh       — lifetime average µSv/h
    //   life_dose_usv   — accumulated dose since boot [µSv]
    //   n_samples       — total valid samples added
    Serial.println("elapsed_s,win_avg_cpm,win_sigma_cpm,life_avg_cpm,life_sigma_cpm,life_usvh,life_dose_usv,n_samples");
}

// Print a human-readable summary for the given rolling window (seconds).
static void printSummaryWindow(uint32_t windowSec, const char* label) {
    if (!stats.hasValidWindow(windowSec, 0.8f)) {
        Serial.printf("  %s — collecting data (%us / %us valid)\n",
                      label, stats.validSeconds(windowSec), windowSec);
        return;
    }
    float avg   = stats.average(windowSec);
    float sens  = tubeSourceSensitivity(TUBE_TYPE, SOURCE_BACKGROUND);
    Serial.printf(
        "  %s — avg: %.2f CPM  min: %.1f  max: %.1f  σ: %.2f  → %.4f µSv/h\n",
        label, avg,
        stats.minimum(windowSec),
        stats.maximum(windowSec),
        stats.stdDev(windowSec),
        avg / sens
    );
}

// Print the lifetime summary block.
static void printLifetimeSummary() {
    float sens = tubeSourceSensitivity(TUBE_TYPE, SOURCE_BACKGROUND);
    Serial.println("# ─── Lifetime ────────────────────────────────────");
    if (!lifetime.hasData(10)) {
        Serial.println("#   (not enough data yet)");
        return;
    }
    Serial.printf("#   avg:   %.2f CPM  σ: %.2f  → %.4f µSv/h\n",
                  lifetime.averageCpm(),
                  lifetime.sigmaCpm(),
                  lifetime.averageCpm() / sens);
    Serial.printf("#   min:   %.1f CPM   max: %.1f CPM\n",
                  lifetime.minCpm(), lifetime.maxCpm());
    Serial.printf("#   dose:  %.4f µSv  (%.6f mSv)\n",
                  lifetime.totalDoseUSv(), lifetime.totalDoseMSv());
    Serial.printf("#   n:     %u samples over %us\n",
                  (unsigned int)lifetime.sampleCount(),
                  (unsigned int)lifetime.elapsedSeconds());
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(GEIGER_PIN, INPUT);  // External pull-up — INPUT_PULLUP unreliable on D7
    attachInterrupt(digitalPinToInterrupt(GEIGER_PIN), geigerISR, FALLING);

    printHeader();
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {

    uint32_t now = millis();

    // ── Every second: check tube status and mode transition ───────────────
    if (now - lastSecondMs >= 1000) {
        lastSecondMs = now;

        GeigerReading r = geiger.getReading();

        // Tube fault detection — report only on state change
        if (!r.tubeAlive && tubeWasAlive) {
            Serial.printf("# WARNING [%us]: no pulse — check tube, wiring, HV supply\n",
                          now / 1000);
        } else if (r.tubeAlive && !tubeWasAlive) {
            Serial.printf("# INFO [%us]: tube signal restored\n", now / 1000);
        }
        tubeWasAlive = r.tubeAlive;

        if (r.saturated) {
            Serial.printf("# WARNING [%us]: dead-time saturation\n", now / 1000);
        }
        if (r.counterSaturated) {
            Serial.printf("# WARNING [%us]: counter saturated — resetting\n", now / 1000);
            geiger.reset();
        }

        // Mode transition: ADAPTIVE_FAST → FIXED_60S after 60 s
        if (inAdaptiveMode && now >= 60000UL) {
            geiger.setMode(AveragingMode::FIXED_60S);
            inAdaptiveMode = false;
            Serial.printf("# INFO [%us]: switched to FIXED_60S\n", now / 1000);
        }
    }

    // ── Every minute: emit one CSV data line ──────────────────────────────
    if (lastMinuteMs > 0 && now - lastMinuteMs >= 60000UL) {
        lastMinuteMs = now;

        GeigerReading r = geiger.getReading();

        if (!r.valid) {
            // Still in warm-up — print a placeholder so timestamps stay aligned
            Serial.printf("# [%us] waiting for data (lifetime pulses: %u)\n",
                          now / 1000, (unsigned int)geiger.lifetimePulses());
        } else {
            // Feed both stats objects and emit CSV line
            stats.addSample(r.cpm, r.timestampMs);
            lifetime.addSample(r.cpm, r.uSvH, r.timestampMs);
            minuteCount++;

            float sens    = tubeSourceSensitivity(TUBE_TYPE, SOURCE_BACKGROUND);
            uint32_t wsec = (minuteCount >= 2) ? (uint32_t)(minuteCount * 60) : 60;
            if (wsec > 7680) wsec = 7680;

            float winAvg   = stats.hasValidWindow(wsec, 0.0f) ? stats.average(wsec) : r.cpm;
            float winSigma = stats.hasValidWindow(wsec, 0.0f) ? stats.stdDev(wsec)  : 0.0f;

            Serial.printf("%u,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%u\n",
                now / 1000,
                winAvg,
                winSigma,
                lifetime.averageCpm(),
                lifetime.sigmaCpm(),
                lifetime.averageCpm() / sens,
                lifetime.totalDoseUSv(),
                (unsigned int)lifetime.sampleCount()
            );
        }
    }

    // Initialise lastMinuteMs on first iteration
    if (lastMinuteMs == 0) lastMinuteMs = now;

    // ── Periodic summary block ─────────────────────────────────────────────
    if (minuteCount >= SUMMARY_MIN_BINS &&
        now - lastSummaryMs >= SUMMARY_INTERVAL_MS)
    {
        lastSummaryMs = now;

        Serial.println("# ─── Summary ─────────────────────────────────────");
        Serial.printf( "# tube: %s  uptime: %us  lifetime pulses: %u\n",
                       TUBE_LABEL, now / 1000,
                       (unsigned int)geiger.lifetimePulses());
        printSummaryWindow(1800, " 30 min");
        printSummaryWindow(3600, "  1 hr ");
        printSummaryWindow(7200, "  2 hr ");
        printLifetimeSummary();
        Serial.println("# ─────────────────────────────────────────────────");
    }
}
