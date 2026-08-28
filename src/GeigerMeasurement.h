/**
 * @file GeigerMeasurement.h
 * @brief Geiger-Müller counter measurement library for ESP32 and ESP8266.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Péter Soós — https://github.com/soosp
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *
 * This library implements the core measurement algorithms of the RadPro project
 * by Gissio, adapted for the Arduino/ESP platform. It provides:
 *
 *   - Pulse buffering using a circular (ring) buffer of microsecond timestamps
 *   - Five averaging modes: adaptive fast, adaptive precision, 10s, 30s, 60s
 *   - Dead-time compensation using the non-paralyzable (Type I) model
 *   - 95% confidence interval estimation (Poisson statistics, RadPro formula)
 *   - Tube/source sensitivity from GeigerTubes.h (RadPro Rad Lab data and own measurement)
 *   - In-situ dead-time measurement (upper bound estimation)
 *   - Live calibration against a known dose-rate source
 *   - Fault detection: tube alive, saturation, counter overflow
 *
 * -----------------------------------------------------------------------------
 * DESIGN PHILOSOPHY
 * -----------------------------------------------------------------------------
 * This library handles only the physics and signal processing. Application-level
 * decisions (alarm thresholds, confidence requirements, display formatting) are
 * left to the caller. This keeps the library reusable across different projects.
 *
 * Hardware interaction is also kept minimal: the ISR must be registered by the
 * application (see usage example). This avoids singleton constraints and allows
 * multiple instances on different pins.
 *
 * CUMULATIVE DOSE:
 *   This library does not implement cumulative dose calculation (µSv total).
 *   The compensatedPulseCount field provides the building block for this, but
 *   the accumulation logic is left to the application. Cumulative dose support
 *   is planned for a future version.
 *
 * -----------------------------------------------------------------------------
 * ACKNOWLEDGEMENTS
 * -----------------------------------------------------------------------------
 * Core algorithms adapted from:
 *   Gissio/RadPro v3.1.1 — https://github.com/Gissio/radpro
 *   Licensed under the MIT License.
 *   Special thanks to Gissio for the excellent open-source work.
 *
 * Algorithms taken or adapted from RadPro:
 *   - Instantaneous rate: (N-1) / T formula (pulses.c)
 *   - High-rate correction: pulseCount > ticks case (pulses.c)
 *   - Dead-time compensation: non-paralyzable model, 10x cap (pulses.c)
 *   - Dead-time carry: integer accumulator for long averaging (pulses.c)
 *   - Dead-time measurement: running minimum of inter-pulse intervals (stm32_tube.c)
 *     RadPro uses a hardware timer; this library uses micros() (~1-2 µs jitter)
 *   - Confidence interval: 95% Poisson CI with first-order correction (cmath.c)
 *   - Adaptive averaging: pulse count + time combined condition (instantaneous.c)
 *   - Loss-of-count timeout: sensitivity-dependent formula (tube.c)
 *   - Tube/source sensitivity data: from RadPro tube.c, originally computed
 *     by Rad Lab (https://github.com/Gissio/radlab), a separate simulation
 *     tool by the same author
 *
 * Extensions beyond RadPro:
 *   - Live calibration against a known dose-rate source
 *   - Counter saturation detection (uint32_t clamping)
 *   - Configurable PULSE_BUFFER_SIZE via preprocessor define
 *   - ESP32/ESP8266 platform abstraction for critical sections
 *   - RunningStatistics integration via timestampMs field
 *
 * -----------------------------------------------------------------------------
 * USAGE EXAMPLE
 * -----------------------------------------------------------------------------
 * @code
 *   #include <GeigerMeasurement.h>
 *
 *   GeigerMeasurement geiger(TUBE_SBM20, SOURCE_BACKGROUND);
 *
 *   void IRAM_ATTR geigerISR() { geiger.onPulse(); }
 *
 *   void setup() {
 *       Serial.begin(115200);
 *       pinMode(4, INPUT);
 *       attachInterrupt(digitalPinToInterrupt(4), geigerISR, FALLING);
 *   }
 *
 *   void loop() {
 *       GeigerReading r = geiger.getReading();
 *       if (r.valid) {
 *           Serial.printf("CPM: %.1f  µSv/h: %.4f  ±%.0f%%\n",
 *                         r.cpm, r.uSvH, r.confidenceHalf);
 *       }
 *       delay(1000);
 *   }
 * @endcode
 */

#pragma once
#include <Arduino.h>
#include "GeigerTubes.h"

// =============================================================================
// PLATFORM ABSTRACTION — CRITICAL SECTIONS
// =============================================================================
/**
 * Critical sections protect shared data between the ISR and the main loop.
 *
 * WHY ARE CRITICAL SECTIONS NEEDED?
 *   The ISR writes to _timestamps, _head, _count, and the pulse counters
 *   asynchronously. If getReading() reads these while the ISR is mid-write,
 *   an inconsistent snapshot results (e.g. _head updated but _timestamps not
 *   yet written). Critical sections prevent this by blocking concurrent access
 *   for the short duration of the snapshot copy.
 *
 * ESP32 vs ESP8266:
 *   ESP32 has two cores running FreeRTOS. noInterrupts() only protects the
 *   current core — the other core can still access shared data. A FreeRTOS
 *   spinlock (portMUX) must be used to exclude both cores simultaneously.
 *
 *   ESP8266 is single-core, so noInterrupts() / interrupts() is sufficient.
 *
 * TWO MACRO PAIRS:
 *   GEIGER_ENTER_CRITICAL() / GEIGER_EXIT_CRITICAL()
 *     For use in normal (task/loop) context. On ESP32 this is
 *     taskENTER_CRITICAL(), which may not be called from an ISR.
 *
 *   GEIGER_ENTER_CRITICAL_ISR() / GEIGER_EXIT_CRITICAL_ISR()
 *     For use inside the ISR (onPulse()). On ESP32 this is
 *     taskENTER_CRITICAL_ISR(), which uses the same portMUX and therefore
 *     mutually excludes with the task-context macros. Without this, a
 *     task-context critical section would NOT block the ISR on ESP32,
 *     allowing the ISR to corrupt shared state mid-snapshot.
 *
 *   On ESP8266 both pairs expand to nothing in the ISR: the single-core
 *   architecture guarantees that an executing ISR cannot be preempted by
 *   the main loop, so no additional protection is needed inside onPulse().
 *
 * Macros are used (not inline functions) because the two platforms require
 * different API signatures and the choice must be made at compile time.
 */
#ifdef ESP32
    #include <freertos/FreeRTOS.h>
    static portMUX_TYPE _geigerMux = portMUX_INITIALIZER_UNLOCKED;
    #define GEIGER_ENTER_CRITICAL()      taskENTER_CRITICAL(&_geigerMux)
    #define GEIGER_EXIT_CRITICAL()       taskEXIT_CRITICAL(&_geigerMux)
    #define GEIGER_ENTER_CRITICAL_ISR()  taskENTER_CRITICAL_ISR(&_geigerMux)
    #define GEIGER_EXIT_CRITICAL_ISR()   taskEXIT_CRITICAL_ISR(&_geigerMux)
#else
    #define GEIGER_ENTER_CRITICAL()      noInterrupts()
    #define GEIGER_EXIT_CRITICAL()       interrupts()
    #define GEIGER_ENTER_CRITICAL_ISR()  // ESP8266: ISR already excludes the main loop
    #define GEIGER_EXIT_CRITICAL_ISR()
#endif

// =============================================================================
// AVERAGING MODES
// =============================================================================

/**
 * @brief Controls how the instantaneous rate window is determined.
 *
 * ADAPTIVE modes (from RadPro "instantaneous" measurement):
 *   The window length is determined by the pulse arrival times, not by a fixed
 *   clock. This gives faster response at high radiation levels (many pulses
 *   arrive quickly) while automatically extending the window at low levels.
 *
 *   ADAPTIVE_FAST:      window = time spanned by last 19 pulses, capped at 5s.
 *                       Only the pulse count condition applies (no minimum time).
 *                       Best for detecting rapid changes.
 *
 *   ADAPTIVE_PRECISION: window = time spanned by last 19 pulses, but at least 5s.
 *                       Both conditions must be met: ≥19 pulses AND ≥5 seconds.
 *                       Better statistical accuracy at the cost of slower response.
 *
 * FIXED modes (sliding window):
 *   A fixed-duration window slides over the pulse timestamp buffer. The rate is
 *   computed from all pulses that fall within the window. With fewer than 2
 *   pulses in the window the rate is reported as 0 (not "invalid") — this is
 *   the RadPro behaviour for fixed modes.
 *
 *   FIXED_10S:  10-second sliding window — responsive but noisy.
 *   FIXED_30S:  30-second sliding window — balanced.
 *   FIXED_60S:  60-second sliding window — recommended for background monitoring.
 */
enum class AveragingMode {
    ADAPTIVE_FAST,       ///< Last 19 pulses, max 5s window (RadPro "Adaptive fast")
    ADAPTIVE_PRECISION,  ///< Last 19 pulses, min 5s window (RadPro "Adaptive precision")
    FIXED_10S,           ///< Fixed 10-second sliding window
    FIXED_30S,           ///< Fixed 30-second sliding window
    FIXED_60S,           ///< Fixed 60-second sliding window (recommended for background)
};

// =============================================================================
// COMPILE-TIME CONSTANTS
// =============================================================================

/**
 * @brief Compile-time constants for the pulse buffer.
 *
 * PULSE_BUFFER_SIZE MUST be a power of 2. This is enforced by static_assert.
 *
 * WHY POWER OF 2?
 *   The circular buffer uses bitwise AND for index wrapping:
 *     index = (index + 1) & PULSE_BUFFER_MASK
 *   This is equivalent to modulo (% PULSE_BUFFER_SIZE) but executes as a
 *   single CPU instruction instead of a division — critical in an ISR.
 *
 * DEFAULT SIZE (256):
 *   For an SBM-20 at background levels (~20 CPM) the buffer holds ~13 minutes of history.
 *   At 1000 CPM it holds ~15 seconds. Each entry costs 4 bytes (uint32_t).
 *
 * CUSTOMISING THE BUFFER SIZE:
 *   Define GEIGER_PULSE_BUFFER_SIZE before including this header to override
 *   the default without editing the library source:
 *
 *   // In your sketch or build flags:
 *   #define GEIGER_PULSE_BUFFER_SIZE 512
 *   #include <GeigerMeasurement.h>
 *
 *   Or in platformio.ini:
 *   build_flags = -DGEIGER_PULSE_BUFFER_SIZE=512
 *
 *   The value must be a power of 2 — the static_assert below will catch
 *   invalid values at compile time with a clear error message.
 */
#ifndef GEIGER_PULSE_BUFFER_SIZE
    #define GEIGER_PULSE_BUFFER_SIZE 256
#endif

namespace GeigerConfig {
    // Bridge from preprocessor to typed constexpr — preserves type safety
    // and allows static_assert to operate on a typed value rather than a macro.
    constexpr uint32_t PULSE_BUFFER_SIZE = GEIGER_PULSE_BUFFER_SIZE;
    constexpr uint32_t PULSE_BUFFER_MASK = PULSE_BUFFER_SIZE - 1;
    constexpr uint32_t MIN_PULSES_FOR_RATE = 2;

    // Adaptive averaging defaults (RadPro: INSTANTANEOUS_RATE_PULSE_COUNT_MIN = 19)
    // 19 pulses gives a 95% CI of ~50%, which RadPro uses as the threshold for
    // "good enough" adaptive averaging.
    constexpr float DEFAULT_ADAPTIVE_PULSES     = 19.0f;
    constexpr float DEFAULT_ADAPTIVE_MAX_WINDOW =  5.0f;  // ADAPTIVE_FAST cap [s]
    constexpr float DEFAULT_ADAPTIVE_MIN_WINDOW =  5.0f;  // ADAPTIVE_PRECISION floor [s]

    // Dead-time compensation defaults (RadPro: denominator capped at 0.1 → factor ≤ 10)
    constexpr float DEFAULT_DEAD_TIME_MAX_FACTOR = 10.0f;
    constexpr float DEFAULT_DEAD_TIME_WARN_RATIO =  0.8f;  // warn at 80% of the cap


    // Default maximum confidence for calibrate() [%]
    constexpr float DEFAULT_CALIBRATE_MAX_CONFIDENCE = 20.0f;

    // Compile-time validation
    static_assert(PULSE_BUFFER_SIZE > 0,
                  "GEIGER_PULSE_BUFFER_SIZE must be greater than 0");
    static_assert((PULSE_BUFFER_SIZE & (PULSE_BUFFER_SIZE - 1)) == 0,
                  "GEIGER_PULSE_BUFFER_SIZE must be a power of 2 "
                  "(e.g. 64, 128, 256, 512, 1024)");
}

// =============================================================================
// MEASUREMENT RESULT STRUCTURE
// =============================================================================

/**
 * @brief Complete measurement result returned by GeigerMeasurement::getReading().
 *
 * Always check the `valid` flag before using numerical fields.
 *
 * Use timestampMs (not millis()) when passing data to RollingStats::addSample()
 * to avoid timing skew introduced by subsequent processing in the loop.
 */
struct GeigerReading {
    // --- Dose rate ---
    float    cpm;       ///< Dead-time compensated rate [counts per minute]
                        ///<   Compensation uses float multiplication — accurate for
                        ///<   instantaneous rate display.
    float    uSvH;      ///< Dose rate [µSv/h] = cpm / sensitivity

    // --- Statistical quality ---
    float    confidenceHalf; ///< 95% confidence interval as a percentage (±%)
                             ///<   Based on Poisson statistics with first-order
                             ///<   correction (RadPro formula). Smaller is better.
                             ///<   Example: 20.0 means the true rate is within
                             ///<   ±20% of the reported value with 95% probability.
                             ///<   Value of 269.0 indicates insufficient data (N≤1).

    // --- Pulse counts ---
    uint32_t pulseCount;            ///< Raw pulse count in the averaging window
    uint32_t compensatedPulseCount; ///< Dead-time compensated pulse count using integer
                                    ///<   carry accumulation (RadPro method). More accurate
                                    ///<   than cpm * windowSec / 60 for long-term averaging
                                    ///<   because fractional remainders are carried forward.

    // --- Timing ---
    float    windowSec;   ///< Actual averaging window duration [s]
    uint32_t timestampMs; ///< Time of measurement [ms since boot, from millis()]
                          ///<   Use this (not millis()) when calling RollingStats::addSample()
                          ///<   to avoid skew from processing time after getReading().
                          ///<   Set on every return path, including when valid is false.

    // --- Status flags ---
    bool valid;            ///< true if numerical fields contain a valid measurement.
                           ///<   false during startup (not enough pulses yet) or in
                           ///<   adaptive mode with insufficient data.
    bool saturated;        ///< true if the dead-time compensation factor exceeds
                           ///<   deadTimeWarnRatio × deadTimeMaxFactor (default: 80%
                           ///<   of the 10× cap). Indicates the tube may be counting
                           ///<   inaccurately due to very high radiation.
    bool tubeAlive;        ///< false if no pulse has arrived within the timeout period.
                           ///<   Set on every return path, including when valid is
                           ///<   false: whether the tube is alive matters most when
                           ///<   no rate can be computed yet.
                           ///<   The timeout is sensitivity-dependent (RadPro formula):
                           ///<   time for 10 pulses at 0.05 µSv/h. A false value may
                           ///<   indicate a broken tube, open circuit, or missing HV supply.
    bool counterSaturated; ///< true if totalPulses has reached UINT32_MAX (clamped).
                           ///<   At background levels this takes ~408 years. At 60 000 CPM
                           ///<   it takes ~49 days. Call reset() to clear and restart.
};

// =============================================================================
// MAIN CLASS
// =============================================================================

class GeigerMeasurement {
public:

    // =========================================================================
    // CONSTRUCTOR
    // =========================================================================

    /**
     * @brief Construct a GeigerMeasurement instance.
     *
     * Sensitivity is computed automatically from the tube/source combination
     * using data from GeigerTubes.h (RadPro Rad Lab simulations).
     *
     * All tunable parameters use sensible defaults and can be adjusted later
     * via setter methods without reconstructing the object.
     *
     * @param tube       GM tube type (default: TUBE_SBM20)
     * @param source     Radiation source preset (default: SOURCE_BACKGROUND)
     * @param mode       Averaging mode (default: FIXED_60S)
     * @param deadTimeUs Tube dead time in microseconds (default: 0 = disabled)
     *                   Typical values: 100–200 µs for most GM tubes.
     *                   Use measureDeadTime() to estimate this automatically.
     */
    explicit GeigerMeasurement(
        GeigerTube    tube       = TUBE_SBM20,
        GeigerSource  source     = SOURCE_BACKGROUND,
        AveragingMode mode       = AveragingMode::FIXED_60S,
        float         deadTimeUs = 0.0f
    )
        : _tube(tube)
        , _source(source)
        , _radlabSensitivity(tubeSourceSensitivity(tube, source))
        , _fieldFactor(1.0f)
        // For TUBE_CUSTOM, tubeSourceSensitivity() returns NaN — store 0 as
        // sentinel so the uSvH calculation returns NaN (not inf or garbage).
        // Call setSensitivity() after construction to enable dose conversion.
        , _sensitivity(isnan(_radlabSensitivity) ? 0.0f : _radlabSensitivity)
        , _mode(mode)
        , _deadTime_s(deadTimeUs * 1e-6f)
        , _adaptivePulses(GeigerConfig::DEFAULT_ADAPTIVE_PULSES)
        , _adaptiveMaxWindow(GeigerConfig::DEFAULT_ADAPTIVE_MAX_WINDOW)
        , _adaptiveMinWindow(GeigerConfig::DEFAULT_ADAPTIVE_MIN_WINDOW)
        , _deadTimeMaxFactor(GeigerConfig::DEFAULT_DEAD_TIME_MAX_FACTOR)
        , _deadTimeWarnRatio(GeigerConfig::DEFAULT_DEAD_TIME_WARN_RATIO)
        // Timeout for the tube fault detection
        // 0 = use sensitivity-dependent timeout (RadPro: getLossOfCountTime)
        , _tubeTimeoutMs(0)
        , _head(0)
        , _count(0)
        , _totalPulses(0)
        , _lifetimePulses(0)
        , _lastPulseMs(0)
        , _minIntervalUs(UINT32_MAX)
        , _dtCarry(0.0f)
    {}

    // =========================================================================
    // ISR — must be called from the hardware interrupt handler
    // =========================================================================

    /**
     * @brief Record a pulse. Call this from the GPIO interrupt handler.
     *
     * This function stores the current microsecond timestamp in a circular
     * ring buffer and increments the pulse counters.
     *
     * IMPORTANT: The interrupt handler on the application side must be declared
     * with IRAM_ATTR to ensure it is placed in fast RAM (required on ESP8266/ESP32
     * so it runs correctly even when the flash cache is busy):
     *
     * @code
     *   void IRAM_ATTR geigerISR() { geiger.onPulse(); }
     *   attachInterrupt(digitalPinToInterrupt(PIN), geigerISR, FALLING);
     * @endcode
     *
     * RING BUFFER MECHANICS:
     *   Timestamps are stored at _timestamps[_head], then _head advances using
     *   bitwise AND: _head = (_head + 1) & MASK. When the buffer is full, the
     *   oldest entry is silently overwritten (the buffer always holds the most
     *   recent PULSE_BUFFER_SIZE timestamps).
     *
     * OVERFLOW PROTECTION:
     *   _totalPulses and _lifetimePulses use _addClamped() to prevent wrapping
     *   to zero — they saturate at UINT32_MAX and stay there.
     */
    void IRAM_ATTR onPulse() {
        // On ESP32, taskENTER_CRITICAL_ISR() acquires the same portMUX as
        // taskENTER_CRITICAL() used in task context, so the ISR and the main
        // loop mutually exclude each other. Without this, a task-context
        // critical section would not block the ISR on ESP32, allowing it to
        // corrupt shared state mid-snapshot.
        // On ESP8266 these macros expand to nothing — the single-core ISR
        // already excludes the main loop by design.
        GEIGER_ENTER_CRITICAL_ISR();
        uint32_t _now = micros();
        _timestamps[_head] = _now;
        _lastPulseMs = millis();
        if (_count > 1) {
            uint32_t _prev = _timestamps[(_head - 1) & GeigerConfig::PULSE_BUFFER_MASK];
            uint32_t interval = _now - _prev;
            if (interval < _minIntervalUs) _minIntervalUs = interval;
        }
        _head = (_head + 1) & GeigerConfig::PULSE_BUFFER_MASK;
        if (_count < GeigerConfig::PULSE_BUFFER_SIZE) _count++;
        _totalPulses    = _addClamped(_totalPulses, 1);
        _lifetimePulses = _addClamped(_lifetimePulses, 1);
        GEIGER_EXIT_CRITICAL_ISR();
    }

    // =========================================================================
    // PRIMARY MEASUREMENT
    // =========================================================================

    /**
     * @brief Compute and return the current measurement result.
     *
     * Call this from loop() — typically once per second. It is safe to call
     * more frequently; results will update as new pulses arrive.
     *
     * THREAD SAFETY:
     *   A brief critical section copies the volatile ISR data (timestamps, head,
     *   count) into local variables before any computation. This prevents the ISR
     *   from modifying the buffer mid-calculation. The critical section is as short
     *   as possible (just the copy loop) to minimise interrupt latency.
     *
     *   On ESP8266: uses noInterrupts() / interrupts().
     *   On ESP32:   uses FreeRTOS taskENTER_CRITICAL() / taskEXIT_CRITICAL()
     *               which protects against both cores.
     *
     * RATE CALCULATION (RadPro formula):
     *   rate [CPS] = (N - 1) × 1000 / ticks
     *   where ticks = window duration in milliseconds, N = pulse count in window.
     *   Using (N-1) instead of N avoids the boundary bias: we measure the time
     *   between the first and last pulse, which spans exactly (N-1) intervals.
     *
     * HIGH-RATE CORRECTION (RadPro):
     *   When pulse count exceeds the millisecond tick count (>1000 CPS = 60 000 CPM),
     *   both values are incremented by 1 before the division. This reduces the
     *   boundary bias that accumulates at very high count rates.
     *
     * @return GeigerReading struct. Always check the `valid` field first.
     */
    GeigerReading getReading() {
        GeigerReading r{};
        r.pulseCount = 0;
        r.valid = false;

        // --- Consistent snapshot of ISR data (under critical section) ---
        // The entire timestamp buffer is copied while the critical section is
        // held, so the ISR cannot modify it mid-copy. The result is a consistent
        // point-in-time view of all ISR-shared variables.
        // memcpy() is not used: the ESP8266 GCC toolchain rejects the implicit
        // volatile→const* conversion. A manual loop reads each element
        // individually, which is correct for volatile memory.
        GEIGER_ENTER_CRITICAL();
        uint32_t head  = _head;
        uint32_t count = _count;
        uint32_t now = micros();
        AveragingMode mode = _mode;
        float sensitivity = _sensitivity;
        float deadTimeMaxFactor = _deadTimeMaxFactor;
        float deadTimeWarnRatio = _deadTimeWarnRatio;
        uint32_t lastPulseMs = _lastPulseMs;
        uint32_t totalPulses = _totalPulses;
        uint32_t buf[GeigerConfig::PULSE_BUFFER_SIZE];
        for (uint32_t i = 0; i < GeigerConfig::PULSE_BUFFER_SIZE; i++)
            buf[i] = _timestamps[i];
        GEIGER_EXIT_CRITICAL();

        // --- Determine fixed vs adaptive mode ---
        // Fixed modes accept rate = 0 when fewer than 2 pulses are in the window.
        // Adaptive modes require at least 2 pulses (physical minimum for an interval).
        // This matches RadPro behaviour: minPulseCount = 0 for fixed modes.
        bool fixedMode = (mode == AveragingMode::FIXED_10S  ||
                          mode == AveragingMode::FIXED_30S  ||
                          mode == AveragingMode::FIXED_60S);

        if (!fixedMode && count < GeigerConfig::MIN_PULSES_FOR_RATE) {
            // Adaptive mode, not enough data yet
            r.tubeAlive  = _tubeAliveCheck(lastPulseMs);
            return r;
        }

        if (fixedMode && count == 0) {
            // Fixed mode, no pulses at all yet — report zero rate as valid
            r.cpm            = 0.0f;
            r.uSvH           = 0.0f;
            r.confidenceHalf = 269.0f;  // maximum uncertainty (N≤1)
            r.tubeAlive      = _tubeAliveCheck(lastPulseMs);
            r.valid          = true;
            return r;
        }

        // --- Determine averaging window duration ---
        float windowDurationUs = _windowDurationUs(head, count, buf, now);
        if (windowDurationUs <= 0.0f) {
            // Invalid reading, but tubeAlive and timestampMs are meaningful
            // regardless of valid and must be set on every return path.
            r.tubeAlive   = _tubeAliveCheck(lastPulseMs);
            r.timestampMs = millis();
            return r;
        }

        // --- Count pulses within the window ---
        uint32_t windowPulses = _countInWindow(head, count, buf, now, windowDurationUs);

        // Handle the case of fewer than 2 pulses within the window
        if (windowPulses < GeigerConfig::MIN_PULSES_FOR_RATE) {
            if (fixedMode) {
                // Fixed mode: report zero rate, still valid
                r.cpm            = 0.0f;
                r.uSvH           = 0.0f;
                r.confidenceHalf = 269.0f;
                r.windowSec      = windowDurationUs * 1e-6f;
                r.timestampMs    = millis();
                r.tubeAlive      = _tubeAliveCheck(lastPulseMs);
                r.valid          = true;
                return r;
            }
            // Adaptive mode — not enough data in the window. Routine at
            // background rates, where fewer than two pulses often fall inside
            // a short adaptive window. valid stays false, but tubeAlive and
            // timestampMs are still filled in: a caller needs to know whether
            // the tube is alive precisely when the rate is not yet measurable,
            // and a zero timestampMs underflows RollingStats::addSample().
            r.tubeAlive   = _tubeAliveCheck(lastPulseMs);
            r.timestampMs = millis();
            return r;
        }

        // --- Instantaneous rate (RadPro formula) ---
        // Convert window duration to millisecond ticks (RadPro uses ms internally).
        // rate = (N - 1) × 1000 / ticks  [CPS]
        //
        // HIGH-RATE CORRECTION (RadPro: pulseCount > ticks):
        // At >1000 CPS (60 000 CPM), more pulses arrive than milliseconds elapse.
        // Incrementing both values by 1 reduces the boundary bias at extreme rates.
        // At background levels this condition never triggers.
        uint32_t ticks = (uint32_t)(windowDurationUs / 1000.0f);
        float rawCPS;
        if (ticks > 0 && windowPulses > ticks) {
            // High-rate path: apply RadPro correction
            uint32_t adjPulses = _addClamped(windowPulses, 1);
            uint32_t adjTicks  = _addClamped(ticks, 1);
            rawCPS = (float)(adjPulses - 1) * 1000.0f / adjTicks;
        } else {
            // Normal path
            rawCPS = (ticks > 0) ? (float)(windowPulses - 1) * 1000.0f / ticks
                                 : 0.0f;
        }
        float windowSec = windowDurationUs * 1e-6f;
        float rawCPM    = rawCPS * 60.0f;

        // --- Dead-time compensation (non-paralyzable / Type I model) ---
        float dtFactor;
        float cpm = _applyDeadTime(rawCPS, rawCPM, dtFactor);

        // --- 95% Confidence interval (RadPro formula) ---
        // Uses Poisson statistics with a first-order correction term for small N.
        // The result is the relative half-width of the 95% CI expressed as a %.
        // For large N: approaches 1.96/√(N-1) × 100%.
        // For N≤1:    returns 269% (effectively "no information").
        float confidence = (windowPulses >= 2)
            ? _confidenceInterval(windowPulses - 1) * 100.0f
            : 269.0f;

        // --- Tube alive check ---
        // Delegated to _tubeAliveCheck() for overflow-safe millis() arithmetic.
        bool tubeAlive = _tubeAliveCheck(lastPulseMs);

        // --- Assemble result ---
        r.cpm                   = cpm;
        // _sensitivity == 0 means TUBE_CUSTOM with no setSensitivity() call yet.
        // Return NaN for uSvH so the caller knows the conversion is not possible,
        // while CPM (the raw count rate) remains valid and useful.
        r.uSvH                  = (sensitivity > 0.0f) ? (cpm / sensitivity) : NAN;
        r.confidenceHalf        = confidence;
        r.pulseCount            = windowPulses;
        r.compensatedPulseCount = _applyDeadTimeCarry(windowPulses, dtFactor);
        r.windowSec             = windowSec;
        r.timestampMs           = millis();
        r.valid                 = true;
        r.saturated             = dtFactor >= (deadTimeMaxFactor * deadTimeWarnRatio);
        r.tubeAlive             = tubeAlive;
        r.counterSaturated      = (totalPulses == 0xFFFFFFFFUL);
        return r;
    }

    // =========================================================================
    // PULSE COUNTERS
    // =========================================================================

    /// Total pulses since last reset() — saturates at UINT32_MAX (no wrap).
    /// Read under a critical section for consistency with getReading() on ESP32:
    /// bare volatile reads are not sequentially consistent across cores without
    /// a memory barrier, which the portMUX acquire/release provides.
    uint32_t totalPulses() const {
        GEIGER_ENTER_CRITICAL();
        uint32_t v = _totalPulses;
        GEIGER_EXIT_CRITICAL();
        return v;
    }

    /// Total pulses since power-on — never reset, saturates at UINT32_MAX.
    uint32_t lifetimePulses() const {
        GEIGER_ENTER_CRITICAL();
        uint32_t v = _lifetimePulses;
        GEIGER_EXIT_CRITICAL();
        return v;
    }

    // =========================================================================
    // CONTROL
    // =========================================================================

    /**
     * @brief Reset the pulse buffer and pulse counter.
     *
     * Clears the ring buffer (_head, _count → 0) and resets _totalPulses.
     * _lifetimePulses is NOT reset — it accumulates across the entire device
     * lifetime.
     *
     * Use this after counterSaturated becomes true, or after changing the
     * averaging mode to start fresh.
     */
    void reset() {
        GEIGER_ENTER_CRITICAL();
        _head = _count = _totalPulses = 0;
        _lastPulseMs = 0;
        _dtCarry = 0.0f;
        GEIGER_EXIT_CRITICAL();
    }

    // =========================================================================
    // SETTERS — tube and source
    // =========================================================================

    /**
     * @brief Change the tube type at runtime. Recomputes sensitivity automatically.
     * Does not require reset() — sensitivity is just a conversion factor.
     */
    void setTube(GeigerTube tube) {
        GEIGER_ENTER_CRITICAL();
        float radlab = tubeSourceSensitivity(tube, _source);
        float sens = isnan(radlab) ? 0.0f : radlab;
        _tube = tube;
        _fieldFactor = 1.0f;          // field factor is tube- and source-specific — reset on tube/source change
        _radlabSensitivity = radlab;
        _sensitivity = sens;
        _minIntervalUs = UINT32_MAX;  // dead time is a physical property of the tube — reset on tube change
        GEIGER_EXIT_CRITICAL();
    }

    /**
     * @brief Change the radiation source preset at runtime.
     * Recomputes sensitivity automatically. Does not require reset().
     *
     * Use this when you move the detector from background monitoring to a
     * known source (e.g. a Cs-137 check source):
     * @code
     *   geiger.setSource(SOURCE_CS137);
     * @endcode
     */
    void setSource(GeigerSource source) {
        GEIGER_ENTER_CRITICAL();
        float radlab = tubeSourceSensitivity(_tube, source);
        float sens = isnan(radlab) ? 0.0f : radlab;
        _source = source;
        _fieldFactor = 1.0f;          // field factor is source-specific — reset on source change
        _radlabSensitivity = radlab;
        _sensitivity = sens;
        GEIGER_EXIT_CRITICAL();
    }

    /**
     * @brief Change both tube and source simultaneously. Recomputes sensitivity.
     */
    void setTubeAndSource(GeigerTube tube, GeigerSource source) {
        float radlab = tubeSourceSensitivity(tube, source);
        float sens = isnan(radlab) ? 0.0f : radlab;
        GEIGER_ENTER_CRITICAL();
        _tube = tube;
        _source = source;
        _fieldFactor = 1.0f;          // field factor is tube- and source-specific — reset on tube/source change
        _radlabSensitivity = radlab;
        _sensitivity = sens;
        _minIntervalUs = UINT32_MAX;  // dead time is a physical property of the tube — reset on tube change
        GEIGER_EXIT_CRITICAL();
    }

    /**
     * @brief Override sensitivity directly with a measured or known value.
     *
     * Sets both the effective sensitivity and the field factor so they stay
     * consistent: fieldFactor = s / radlabSensitivity.
     *
     * Use this when the sensitivity is known from direct measurement or a
     * manufacturer's datasheet, rather than derived from Rad Lab simulation.
     * This is the calibration method for TUBE_CUSTOM, and is also useful when
     * working with tubes not covered by the built-in presets.
     *
     * @param s  Sensitivity in CPM / (µSv/h) — must be > 0
     */
    void setSensitivity(float s) {
        if (s <= 0.0f) return;
        GEIGER_ENTER_CRITICAL();
        _sensitivity = s;
        _fieldFactor = isnan(_radlabSensitivity) ? 1.0f : s / _radlabSensitivity;
        GEIGER_EXIT_CRITICAL();
    }

    // =========================================================================
    // GETTERS — tube and source
    // =========================================================================

    /// Return the current tube type.
    GeigerTube   getTube()   const { return _tube; }
    /// Return the current radiation source preset.
    GeigerSource getSource() const { return _source; }

    // =========================================================================
    // SETTERS — averaging mode
    // =========================================================================

    /**
     * @brief Change the averaging mode.
     *
     * Clears the pulse buffer. _totalPulses and _lifetimePulses are preserved.
     */
    void setMode(AveragingMode m) {
        GEIGER_ENTER_CRITICAL();
        _mode = m;
        _head = _count = 0;
        GEIGER_EXIT_CRITICAL();
    }

    // =========================================================================
    // GETTERS — averaging mode
    // =========================================================================

    /// Return the current averaging mode.
    AveragingMode getMode() const { return _mode; }

    // =========================================================================
    // SETTERS — dead time
    // =========================================================================

    /**
     * @brief Set the tube dead time for compensation.
     *
     * The dead time τ is the minimum time after a detected pulse during which
     * the tube cannot detect another pulse. Most GM tubes have τ ≈ 100–200 µs.
     *
     * Setting τ = 0 disables compensation (default). Use measureDeadTime() to
     * estimate τ from the pulse buffer.
     *
     * @param us  Dead time in microseconds (0 = disabled)
     */
    void setDeadTime(float us) {
        if (us < 0.0f || isnan(us)) return;
        _deadTime_s = us * 1e-6f;
    }

    /// Maximum dead-time compensation factor (default: 10×, matching RadPro).
    /// At this cap the tube is saturated — the reading is unreliable.
    void setDeadTimeMaxFactor(float f) { _deadTimeMaxFactor = f; }

    /// Fraction of the max factor at which `saturated` becomes true (default: 0.8).
    /// At 0.8 × 10 = 8×, the measurement is warned as potentially unreliable.
    void setDeadTimeWarnRatio(float r) { _deadTimeWarnRatio = r; }

    // =========================================================================
    // GETTERS — dead time
    // =========================================================================

    /// Return the current dead time [µs]. 0 = disabled.
    float getDeadTime()           const { return _deadTime_s * 1e6f; }
    /// Return the dead-time compensation cap (default: 10.0).
    float getDeadTimeMaxFactor()  const { return _deadTimeMaxFactor; }
    /// Return the saturation warning threshold as a fraction of the cap (default: 0.8).
    float getDeadTimeWarnRatio()  const { return _deadTimeWarnRatio; }

    // =========================================================================
    // SETTERS — adaptive window parameters
    // =========================================================================

    /**
     * @brief Set the target pulse count for adaptive averaging (default: 19).
     *
     * RadPro uses 19 because 19 pulses gives a 95% CI of ~50%, which is the
     * threshold for "good enough" adaptive averaging.
     * Clears the buffer (changing this changes which pulses are considered).
     */
    void setAdaptivePulses(float n) {
        GEIGER_ENTER_CRITICAL();
        _adaptivePulses = n;
        _head = _count = 0;
        GEIGER_EXIT_CRITICAL();
    }

    /// Maximum window for ADAPTIVE_FAST (default: 5s). Clears buffer.
    void setAdaptiveMaxWindow(float s) {
        GEIGER_ENTER_CRITICAL();
        _adaptiveMaxWindow = s;
        _head = _count = 0;
        GEIGER_EXIT_CRITICAL();
    }

    /// Minimum window for ADAPTIVE_PRECISION (default: 5s). Clears buffer.
    void setAdaptiveMinWindow(float s) {
        GEIGER_ENTER_CRITICAL();
        _adaptiveMinWindow = s;
        _head = _count = 0;
        GEIGER_EXIT_CRITICAL();
    }

    // =========================================================================
    // GETTERS — adaptive window parameters
    // =========================================================================

    /// Return the target pulse count for adaptive averaging (default: 19).
    float getAdaptivePulses()     const { return _adaptivePulses; }
    /// Return the ADAPTIVE_FAST window cap [s] (default: 5).
    float getAdaptiveMaxWindow()  const { return _adaptiveMaxWindow; }
    /// Return the ADAPTIVE_PRECISION window floor [s] (default: 5).
    float getAdaptiveMinWindow()  const { return _adaptiveMinWindow; }


    // =========================================================================
    // SETTERS — tube alive timeout
    // =========================================================================

    /**
     * @brief Override the tube-alive timeout (default: 0 = auto-computed).
     *
     * When set to 0 (default), the timeout is computed from the sensitivity
     * using the RadPro formula: time for 10 pulses at 0.05 µSv/h.
     * This is tube-specific — a less sensitive tube needs a longer timeout.
     *
     * Set a positive value to override with a fixed timeout in milliseconds.
     *
     * @param ms  Timeout in ms. 0 = use sensitivity-dependent formula.
     */
    void setTubeTimeoutMs(uint32_t ms) { _tubeTimeoutMs = ms; }

    // =========================================================================
    // GETTERS — tube alive timeout
    // =========================================================================

    /// Return the current tube-alive timeout override [ms]. 0 = auto (RadPro formula).
    uint32_t getTubeTimeoutMs() const { return _tubeTimeoutMs; }

    // =========================================================================
    // DEAD-TIME MEASUREMENT
    // =========================================================================

    /**
     * @brief Return the lifetime minimum inter-pulse interval as a dead-time estimate.
     *
     * Unlike measureDeadTime() which scans the current ring buffer, this method
     * returns the minimum interval observed across ALL pulses since the last tube
     * change — a continuously improving upper bound on the true dead time.
     *
     * The dead time is a physical property of the tube, not of the session, so
     * accumulating the minimum across all sessions gives the best estimate over time.
     * The value is reset by setTube() and setTubeAndSource() when the tube changes.
     *
     * @return  Estimated dead time in microseconds (upper bound), or 0 if fewer than
     *          2 pulses have been observed since the last tube change.
     */
    float getMeasuredDeadTime() const {
        GEIGER_ENTER_CRITICAL();
        uint32_t v = _minIntervalUs;
        GEIGER_EXIT_CRITICAL();
        return (v == UINT32_MAX) ? 0.0f : v;
    }

    /**
     * @brief Estimate the tube dead time by scanning a recent window of the pulse buffer.
     *
     * The dead time τ is the minimum inter-pulse interval the tube can detect.
     * This function scans the stored timestamps and returns the shortest observed
     * interval — which is an upper bound on τ (the true τ may be shorter, but
     * no interval shorter than τ can ever be observed).
     *
     * This mirrors the approach used in RadPro's hardware ISR (stm32_tube.c),
     * which maintains a running minimum of inter-pulse intervals. RadPro uses a
     * high-resolution hardware timer; this library uses micros() with ~1–2 µs
     * jitter, which is acceptable for dead-time estimation.
     *
     * RELATIONSHIP TO getMeasuredDeadTime():
     *   getMeasuredDeadTime() returns the minimum across ALL pulses since the
     *   last tube change — a continuously improving lifetime estimate. Prefer
     *   it for normal use, as it never forgets a previously observed minimum.
     *
     *   measureDeadTime() is useful when a time-limited window is needed:
     *   - Diagnosing whether tube behaviour has changed recently (ageing, HV drift)
     *   - Measuring dead time only during a high-rate event
     *   - Limiting the scan to a specific number of recent pulses via sampleCount
     *
     * ACCURACY NOTES:
     *   - The estimate improves with more pulses. For an SBM-20 at background
     *     levels (~20 CPM), collecting 500 pulses takes ~25 minutes.
     *   - A radioactive check source dramatically speeds up the process.
     *   - Returns 0 if fewer than 2 pulses are available in the scanned window.
     *
     * @param sampleCount  How many recent pulses to scan (default: full buffer)
     * @return             Estimated dead time in microseconds (upper bound), or 0.
     */
    float measureDeadTime(uint32_t sampleCount = GeigerConfig::PULSE_BUFFER_SIZE) {
        GEIGER_ENTER_CRITICAL();
        uint32_t head  = _head;
        uint32_t count = _count;
        uint32_t buf[GeigerConfig::PULSE_BUFFER_SIZE];
        for (uint32_t i = 0; i < GeigerConfig::PULSE_BUFFER_SIZE; i++)
            buf[i] = _timestamps[i];
        GEIGER_EXIT_CRITICAL();

        uint32_t limit = min(count, sampleCount);
        if (limit < 2) return 0.0f;

        // Scan backwards through the buffer, computing consecutive intervals.
        // The minimum interval observed is the upper bound on τ.
        uint32_t minInterval = UINT32_MAX;
        for (uint32_t i = 0; i < limit - 1; i++) {
            uint32_t idx     = (head - 1 - i) & GeigerConfig::PULSE_BUFFER_MASK;
            uint32_t idxPrev = (head - 2 - i) & GeigerConfig::PULSE_BUFFER_MASK;
            // Subtraction is overflow-safe for uint32_t (micros() wraps at ~71 min)
            uint32_t interval = buf[idx] - buf[idxPrev];
            if (interval < minInterval) minInterval = interval;
        }

        if (minInterval == UINT32_MAX) return 0.0f;
        return minInterval;  // µs
    }

    // =========================================================================
    // CALIBRATION
    // =========================================================================

    /**
     * @brief Calibrate sensitivity against a known dose-rate source.
     *
     * Computes the field correction factor from the current measured CPM and
     * the known dose rate of the reference source:
     *
     *   fieldFactor = (measured_CPM / known_dose_rate) / radlabSensitivity
     *   sensitivity = radlabSensitivity x fieldFactor
     *
     * WHEN IS CALIBRATION USEFUL?
     *   Use calibration when you have access to a reference source with a known
     *   dose rate -- for example, a certified Cs-137 check source, or a location
     *   with a well-characterised background level from official measurements.
     *   Calibration accounts for tube-to-tube manufacturing variations and
     *   environmental factors that the Rad Lab simulation data cannot capture.
     *   In the tests conducted, some GM tubes measured ~1.3–1.6× more than
     *   Rad Lab predicts for background radiation; calibration corrects for this.
     *
     *   If you only have a GM tube and no reference source, the Rad Lab presets
     *   from GeigerTubes.h are a good starting point.
     *
     * FIELD FACTOR LIFETIME:
     *   The field factor is tube- and source-specific. It is reset to 1.0
     *   by setTube(), setSource(), and setTubeAndSource() -- matching the
     *   standard practice of calibrating separately for each source type.
     *   Save and restore it with the help of getFieldFactor() / setFieldFactor() if needed.
     *
     * @code
     *   // Calibrate against known background (e.g. official station data)
     *   while (!geiger.calibrate(0.087f, 15.0f)) { delay(1000); }
     *   float ff = geiger.getFieldFactor();  // save, e.g. to EEPROM
     *
     *   // Later, restore without re-calibrating:
     *   geiger.setFieldFactor(ff);
     *
     *   // Switching tube resets the field factor:
     *   geiger.setTube(TUBE_J305);       // fieldFactor = 1.0
     * @endcode
     *
     * The calibration only proceeds if:
     *   1. The current reading is valid (enough pulses for rate calculation)
     *   2. The 95% confidence interval is within maxConfidencePct
     *
     * @param knownUsvH       Known dose rate of the reference source [µSv/h]
     * @param maxConfidencePct Maximum acceptable 95% CI [%] (default: 20%)
     * @return true if calibration was performed, false if conditions not met.
     */
    bool calibrate(float knownUsvH, float maxConfidencePct = GeigerConfig::DEFAULT_CALIBRATE_MAX_CONFIDENCE) {
        GeigerReading r = getReading();
        if (!r.valid)                            return false;
        if (knownUsvH <= 0.0f)                   return false;
        if (r.confidenceHalf > maxConfidencePct) return false;
        // In theory, the value of _radlabSensitivity could change between the
        // return of getReading() and the subsequent critical section (e.g., a
        // call to setTube() from another task), meaning there is a potential
        // race condition; however, this should not occur during a calibration
        // session provided the software is used correctly on the user’s end.
        GEIGER_ENTER_CRITICAL();
        float radlabSensitivity = _radlabSensitivity;
        if (!isnan(radlabSensitivity)) {
            float newSens = r.cpm / knownUsvH;
            _sensitivity = newSens;
            _fieldFactor = newSens / radlabSensitivity;
        }
        GEIGER_EXIT_CRITICAL();
        return (!isnan(radlabSensitivity));
    }

    // =========================================================================
    // SETTERS — calibration
    // =========================================================================

    /**
     * @brief Set the field calibration factor directly.
     *
     * The field factor corrects for the difference between the Rad Lab
     * simulation sensitivity and the measured real-world sensitivity of a
     * specific tube in a specific radiation field (typically background).
     *
     * The effective sensitivity becomes:
     *   sensitivity = radlabSensitivity × fieldFactor
     *
     * A value of 1.0 means no correction (Rad Lab default).
     * Values > 1.0 mean the tube is more sensitive than the simulation predicts.
     * In the tests conducted, values for some tubes ranged from ~1.3 to ~1.6
     * when measuring background radiation.
     *
     * The field factor is tube- and source-specific. It is automatically
     * reset to 1.0 by setTube(), setSource(), and setTubeAndSource() --
     * matching the standard practice of calibrating separately per source.
     * It can also be reset explicitly with resetFieldFactor().
     *
     * @note Has no meaningful effect for TUBE_CUSTOM since there is no
     *   Rad Lab baseline (_radlabSensitivity is NaN). For custom tubes,
     *   use setSensitivity() directly instead.
     *
     * @param factor  Correction factor (must be > 0).
     */
    void setFieldFactor(float factor) {
        if (factor <= 0.0f) return;
        GEIGER_ENTER_CRITICAL();
        if (!isnan(_radlabSensitivity)) { // TUBE_CUSTOM: use setSensitivity()
            float newSens = _radlabSensitivity * factor;
            _fieldFactor = factor;
            _sensitivity = newSens;
        }
        GEIGER_EXIT_CRITICAL();
    }

    /// Reset the field factor to 1.0 (Rad Lab default, no correction).
    /// The effective sensitivity reverts to the Rad Lab simulation value.
    void resetFieldFactor() {
        GEIGER_ENTER_CRITICAL();
        _fieldFactor = 1.0f;
        _sensitivity = isnan(_radlabSensitivity) ? 0.0f : _radlabSensitivity;
        GEIGER_EXIT_CRITICAL();
    }

    // =========================================================================
    // GETTERS — calibration
    // =========================================================================

    /// Return the current field calibration factor.
    /// 1.0 = Rad Lab default, > 1.0 = empirically more sensitive.
    float getFieldFactor() const { return _fieldFactor; }

    /// Return the Rad Lab baseline sensitivity [CPM/(µSv/h)] for the current
    /// tube/source combination, before field factor correction.
    float getRadlabSensitivity() const { return _radlabSensitivity; }

    /// Return the current effective sensitivity [CPM/(µSv/h)].
    /// This is radlabSensitivity × fieldFactor, or a directly set value.
    float getSensitivity() const { return _sensitivity; }

private:

    // =========================================================================
    // MEMBER VARIABLES
    // =========================================================================

    // --- Tube and sensitivity ---
    GeigerTube    _tube;               ///< Current tube type
    GeigerSource  _source;             ///< Current source preset
    float         _radlabSensitivity;  ///< Rad Lab baseline [CPM/(µSv/h)] — tube+source, no field correction
    float         _fieldFactor;        ///< Field calibration factor (1.0 = Rad Lab default)
    float         _sensitivity;        ///< Effective sensitivity = _radlabSensitivity × _fieldFactor
    AveragingMode _mode;               ///< Current averaging mode
    float         _deadTime_s;         ///< Dead time in seconds (0 = disabled)

    // --- Tunable parameters (application-level, set via setters) ---
    float    _adaptivePulses;    ///< Target pulse count for adaptive modes (default: 19)
    float    _adaptiveMaxWindow; ///< ADAPTIVE_FAST: window cap [s] (default: 5)
    float    _adaptiveMinWindow; ///< ADAPTIVE_PRECISION: window floor [s] (default: 5)
    float    _deadTimeMaxFactor; ///< Maximum compensation factor (default: 10)
    float    _deadTimeWarnRatio; ///< saturated flag threshold, fraction of max (default: 0.8)
    uint32_t _tubeTimeoutMs;     ///< Tube-alive timeout [ms]. 0 = auto (RadPro formula)

    // --- ISR-shared data (volatile: may change at any time from the ISR) ---
    volatile uint32_t _timestamps[GeigerConfig::PULSE_BUFFER_SIZE];
                                 ///< Circular buffer of pulse timestamps [µs]
    volatile uint32_t _head;     ///< Next write position in the ring buffer
    volatile uint32_t _count;    ///< Number of valid entries (capped at PULSE_BUFFER_SIZE)
    volatile uint32_t _totalPulses;    ///< Pulses since last reset() (clamped at UINT32_MAX)
    volatile uint32_t _lifetimePulses; ///< Pulses since power-on (never reset, clamped)
    volatile uint32_t _lastPulseMs;    ///< millis() at the most recent pulse (0 = no pulse yet)
    volatile uint32_t _minIntervalUs;  ///< Lifetime minimum inter-pulse interval [µs]
                                       ///<   Reset only by setTube() and setTubeAndSource().
                                       ///<   Used by getMeasuredDeadTime() for a continuously
                                       ///<   improving dead-time estimate across all sessions.

    // --- State (not ISR-shared) ---
    float _dtCarry;  ///< Dead-time compensation fractional carry (RadPro: deadTimeCompensationRemainder).
                     ///< Accumulates the fractional part of the compensated pulse count
                     ///< across calls, so long-term averages do not lose sub-integer counts.

    // =========================================================================
    // PRIVATE HELPERS
    // =========================================================================

    // -------------------------------------------------------------------------
    // Window duration determination
    // -------------------------------------------------------------------------

    /**
     * @brief Compute the averaging window duration in microseconds.
     *
     * For fixed modes, returns a constant. For adaptive modes, uses the
     * timestamps of the most recent _adaptivePulses pulses.
     *
     * ADAPTIVE_FAST:
     *   Window = time spanned by last N pulses, capped at _adaptiveMaxWindow.
     *   Only pulse count matters — no minimum time requirement.
     *   (RadPro: minTime = 0, minPulseCount = INSTANTANEOUS_RATE_PULSE_COUNT_MIN)
     *
     * ADAPTIVE_PRECISION:
     *   Window = time spanned by last N pulses, floored at _adaptiveMinWindow.
     *   Both conditions must be met: enough pulses AND enough time elapsed.
     *   (RadPro: minTime = 5s, minPulseCount = INSTANTANEOUS_RATE_PULSE_COUNT_MIN)
     *   Before enough pulses accumulate, returns the full elapsed time so far.
     * 
     * Called from getReading() after the main critical section has been released.
     * Acquires its own brief critical section to snapshot adaptive parameters.
     */
    float _windowDurationUs(uint32_t head, uint32_t count,
                            const uint32_t* buf, uint32_t now) const {
        GEIGER_ENTER_CRITICAL();
        AveragingMode mode = _mode;
        float adaptivePulses    = _adaptivePulses;
        float adaptiveMaxWindow = _adaptiveMaxWindow;
        float adaptiveMinWindow = _adaptiveMinWindow;
        GEIGER_EXIT_CRITICAL();
        switch (mode) {
            case AveragingMode::FIXED_10S: return 10e6f;
            case AveragingMode::FIXED_30S: return 30e6f;
            case AveragingMode::FIXED_60S: return 60e6f;

            case AveragingMode::ADAPTIVE_FAST: {
                uint32_t n      = min((uint32_t)adaptivePulses, count);
                uint32_t oldest = buf[(head - n) & GeigerConfig::PULSE_BUFFER_MASK];
                float span      = (float)(now - oldest);
                return min(span, adaptiveMaxWindow * 1e6f);
            }

            case AveragingMode::ADAPTIVE_PRECISION: {
                uint32_t n      = min((uint32_t)adaptivePulses, count);
                uint32_t oldest = buf[(head - n) & GeigerConfig::PULSE_BUFFER_MASK];
                float span      = (float)(now - oldest);
                // Not yet enough pulses: use all available time
                if (count < (uint32_t)adaptivePulses)
                    return (float)(now - buf[(head - count) & GeigerConfig::PULSE_BUFFER_MASK]);
                // Enough pulses: enforce minimum window
                return max(span, adaptiveMinWindow * 1e6f);
            }
        }
        return 60e6f;  // fallback (should never be reached)
    }

    // -------------------------------------------------------------------------
    // Pulse counting within window
    // -------------------------------------------------------------------------

    /**
     * @brief Count pulses that fall within the averaging window.
     *
     * Scans backwards from the most recent pulse. Since pulses are stored in
     * chronological order, the first timestamp older than the cutoff means all
     * remaining entries are also outside the window — we break early.
     *
     * OVERFLOW-SAFE COMPARISON:
     *   (int32_t)(buf[idx] - cutoff) >= 0
     *   Both values are uint32_t. Subtraction wraps correctly even when micros()
     *   has overflowed (~71 min period). Casting to int32_t then allows us to
     *   interpret the result as signed: positive → inside window, negative → outside.
     */
    uint32_t _countInWindow(uint32_t head, uint32_t count,
                             const uint32_t* buf, uint32_t now,
                             float windowUs) const {
        uint32_t cutoff = now - (uint32_t)windowUs;
        uint32_t n      = 0;
        uint32_t limit  = min(count, (uint32_t)GeigerConfig::PULSE_BUFFER_SIZE);
        for (uint32_t i = 0; i < limit; i++) {
            uint32_t idx = (head - 1 - i) & GeigerConfig::PULSE_BUFFER_MASK;
            if ((int32_t)(buf[idx] - cutoff) >= 0) n++;
            else break;  // all older entries are also outside the window
        }
        return n;
    }

    // -------------------------------------------------------------------------
    // Dead-time compensation
    // -------------------------------------------------------------------------

    /**
     * @brief Apply dead-time compensation to the instantaneous rate.
     *
     * Uses the non-paralyzable (Type I) model:
     *   compensated_rate = measured_rate / (1 - measured_rate × τ)
     *
     * WHY NON-PARALYZABLE?
     *   In the Type I model, pulses arriving during the dead time are simply
     *   lost — they do not restart the dead time. At high rates the tube under-
     *   counts but never completely paralyzes. Most GM tubes approximate this
     *   behaviour more closely than the paralyzable (Type II) model.
     *
     * The compensation factor is capped at _deadTimeMaxFactor (default: 10×)
     * to prevent division-by-zero and nonsensical results near saturation.
     * (RadPro: denominator capped at 0.1, equivalent to factor ≤ 10.)
     *
     * @param rawCPS   Measured rate in counts per second
     * @param rawCPM   Measured rate in counts per minute
     * @param outFactor Output: the compensation factor applied (≥1.0)
     * @return         Compensated rate in CPM
     */
    float _applyDeadTime(float rawCPS, float rawCPM, float& outFactor) const {
        GEIGER_ENTER_CRITICAL();
        float deadTime_s = _deadTime_s;
        float deadTimeMaxFactor = _deadTimeMaxFactor;
        GEIGER_EXIT_CRITICAL();
        if (deadTime_s <= 0.0f || isnan(deadTime_s)) {
            outFactor = 1.0f;
            return rawCPM;
        }
        float factor = 1.0f / (1.0f - rawCPS * deadTime_s);
        factor    = min(factor, deadTimeMaxFactor);
        outFactor = factor;
        return rawCPM * factor;
    }

    /**
     * @brief Apply dead-time compensation to a pulse count using integer carry.
     *
     * For instantaneous rate display, float multiplication (_applyDeadTime) is
     * sufficient. For long-term pulse count accumulation, rounding errors
     * accumulate over many calls. This function uses the RadPro carry technique:
     * the fractional remainder of the compensated count is carried forward to
     * the next call, so no sub-integer counts are permanently lost.
     *
     * Example (factor = 1.18, pulseCount = 10):
     *   Exact compensated = 11.8
     *   Returned          = 11  (integer part)
     *   _dtCarry          = 0.8 (carried to next call)
     *   Next call: 0.8 + 11.8 = 12.6 → returns 12, carry = 0.6
     *
     * @param pulseCount  Raw pulse count in the window
     * @param factor      Dead-time compensation factor from _applyDeadTime
     * @return            Integer compensated pulse count
     */
    uint32_t _applyDeadTimeCarry(uint32_t pulseCount, float factor) {
        // _dtCarry is also written by reset() under a critical section,
        // so we must hold the lock here too to exclude concurrent reset() calls
        // on ESP32 (two cores).
        GEIGER_ENTER_CRITICAL();
        _dtCarry += factor * (float)pulseCount;
        uint32_t compensated = (uint32_t)_dtCarry;
        _dtCarry -= (float)compensated;
        GEIGER_EXIT_CRITICAL();
        return compensated;
    }

    // -------------------------------------------------------------------------
    // Tube-alive check
    // -------------------------------------------------------------------------

    /**
     * @brief Check whether the tube is alive based on elapsed time since the last pulse.
     *
     * Uses lastPulseMs (set by onPulse()) for all cases, including startup.
     * This approach is overflow-safe for millis() because unsigned subtraction
     * wraps correctly at the 32-bit boundary (~49-day period):
     *
     *   elapsed = millis() - lastPulseMs   always gives the correct interval
     *
     * At startup (lastPulseMs == 0, no pulse yet), elapsed equals millis()
     * itself — which is the time since boot, a reasonable proxy for "time since
     * the tube was powered on and expected to start counting."
     *
     * @param lastPulseMs Timestamp of last detected pulse in ms.
     */
    bool _tubeAliveCheck(uint32_t lastPulseMs) const {
        uint32_t timeoutMs = _tubeTimeoutMsCalc();
        // Overflow-safe: unsigned subtraction wraps correctly at 2^32
        return (millis() - lastPulseMs) < timeoutMs;
    }

    // -------------------------------------------------------------------------
    // Tube-alive timeout
    // -------------------------------------------------------------------------

    /**
     * @brief Compute the tube-alive timeout in milliseconds.
     *
     * When _tubeTimeoutMs is 0 (default), uses the RadPro formula
     * (getLossOfCountTime in tube.c):
     *
     *   timeout = (60 × 10 / 0.05) / sensitivity × 1000 ms + 1000 ms
     *           = 12 000 000 / sensitivity  ms  +  1000 ms
     *
     * Interpretation: how long until we expect 10 pulses at 0.05 µSv/h?
     * A more sensitive tube (higher sensitivity value) gets a shorter timeout.
     * The +1000 ms is a safety margin (from RadPro's +1 in integer arithmetic).
     *
     * Example timeouts at background levels:
     *   SBM-20  (106 CPM/(µSv/h)):  ~114 s  ≈ 2 min
     *   SI-3BG  (3.3 CPM/(µSv/h)):  ~3637 s ≈ 60 min
     */
    uint32_t _tubeTimeoutMsCalc() const {
        GEIGER_ENTER_CRITICAL();
        uint32_t tubeTimeoutMs = _tubeTimeoutMs;
        float sensitivity = _sensitivity;
        GEIGER_EXIT_CRITICAL();
        if (tubeTimeoutMs > 0) return tubeTimeoutMs;
        // Guard against division by zero when _sensitivity == 0 (TUBE_CUSTOM
        // before setSensitivity() has been called). Fall back to 2 minutes —
        // long enough to avoid a spurious tubeAlive=false at startup, short
        // enough to still detect a genuinely dead tube.
        if (sensitivity <= 0.0f) return 120000UL;
        return (uint32_t)((60.0f * 10.0f / 0.05f) / sensitivity * 1000.0f) + 1000UL;
    }

    // -------------------------------------------------------------------------
    // Overflow-safe addition
    // -------------------------------------------------------------------------

    /**
     * @brief Add two uint32_t values, clamping at UINT32_MAX instead of wrapping.
     *
     * Standard uint32_t addition wraps to zero on overflow, silently corrupting
     * counters. This function saturates at the maximum value instead.
     *
     * WHY NOT uint64_t?
     *   ESP32/ESP8266 are 32-bit cores. A 64-bit addition requires two 32-bit
     *   operations and is NOT atomic. A volatile uint64_t read between two ISR
     *   writes could return a half-updated value (race condition). uint32_t
     *   operations are atomic on 32-bit ARM/Xtensa cores.
     *
     * CHECKING FOR SATURATION:
     *   if (geiger.totalPulses() == 0xFFFFFFFF) { // saturated
     *   }
     *   Or check r.counterSaturated in GeigerReading.
     */
    static uint32_t _addClamped(uint32_t a, uint32_t b) {
        return (a > (0xFFFFFFFFUL - b)) ? 0xFFFFFFFFUL : a + b;
    }

    // -------------------------------------------------------------------------
    // Confidence interval
    // -------------------------------------------------------------------------

    /**
     * @brief Compute the 95% Poisson confidence interval for n counts.
     *
     * This is a direct port of getConfidenceInterval() from RadPro's cmath.c.
     *
     * The simple approximation 1/√n gives the ±1σ (68%) interval. For a 95%
     * interval we need the z=1.96 factor: 1.96/√n. At small n, the Poisson
     * distribution is asymmetric and the normal approximation is poor, so a
     * first-order correction term is added.
     *
     * The returned value is the relative half-width of the CI:
     *   0.20 means the true rate is within ±20% of the measured value
     *   with 95% probability.
     *
     * Special cases from RadPro (pre-computed exact values):
     *   n ≤ 1: 2.689 (essentially no information)
     *   n = 2: 1.786
     *
     * For n ≥ 3:
     *   CI = 1.959964/√n + 0.92095147/(0.34074598 + n)
     *
     * CONSTANT ORIGINS:
     *   1.959964   — z_{0.975}, the 97.5th percentile of the standard normal
     *                distribution (exact: qnorm(0.975) ≈ 1.959964). Standard.
     *
     *   2.6888794, 1.7858216 — hardcoded exact-Poisson 95% CI half-widths for
     *                n = 1 and n = 2 respectively. The asymmetric Poisson
     *                distribution makes the normal approximation poor at small n;
     *                these are pre-computed from the exact Poisson formula and
     *                taken directly from RadPro's cmath.c.
     *
     *   0.92095147, 0.34074598 — empirical Padé-style fit constants for the
     *                small-n correction term. Sourced from RadPro's cmath.c;
     *                the derivation is not documented there. They are a curve fit
     *                to exact Poisson CI values for n ≥ 3 — validated numerically
     *                against scipy.stats.poisson for n = 3..100.
     *
     * @param n  Number of observed counts (use windowPulses - 1)
     * @return   Relative 95% CI half-width (dimensionless, not percent)
     */
    static float _confidenceInterval(uint32_t n) {
        if (n <= 1) return 2.6888794f;
        if (n == 2) return 1.7858216f;
        float normalApprox = 1.959964f / sqrtf((float)n);
        return normalApprox + 0.92095147f / (0.34074598f + (float)n);
    }
};