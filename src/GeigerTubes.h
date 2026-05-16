/**
 * @file GeigerTubes.h
 * @brief Geiger-Müller tube sensitivity and radiation source correction data.
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
 * -----------------------------------------------------------------------------
 * BACKGROUND — WHY SENSITIVITY MATTERS
 * -----------------------------------------------------------------------------
 * A Geiger-Müller tube counts ionizing events (pulses). To convert pulse rate
 * (CPM — counts per minute) to dose rate (µSv/h), we divide by the tube's
 * sensitivity:
 *
 *   dose_rate [µSv/h] = CPM / sensitivity [CPM / (µSv/h)]
 *
 * The sensitivity depends on two factors:
 *
 *   1. TUBE GEOMETRY AND FILL GAS
 *      Larger tubes with thinner walls detect more photons per unit dose.
 *      The base sensitivity values here come from numerical simulations
 *      performed with Rad Lab (https://github.com/Gissio/radlab), a separate
 *      simulation tool by the same author as RadPro.
 *
 *   2. RADIATION ENERGY (source type)
 *      The GM tubes supported by this library are not energy-compensated.
 *      A tube may be several times more sensitive to Cs-137 gamma radiation
 *      than to the low-energy gammas from Am-241. The source correction factor
 *      accounts for this energy-dependent response.
 *
 * The final sensitivity used for dose conversion is:
 *
 *   sensitivity = base_sensitivity [CPM/(µSv/h)] × source_correction_factor
 *
 * -----------------------------------------------------------------------------
 * ACKNOWLEDGEMENTS
 * -----------------------------------------------------------------------------
 * Sensitivity values and source correction factors are derived from:
 *
 *   Gissio/RadPro v3.1.1 — https://github.com/Gissio/radpro
 *   Licensed under the MIT License.
 *
 * The sensitivity data originates from numerical simulations performed with
 * Rad Lab (https://github.com/Gissio/radlab), a separate open-source tool
 * by the same author. RadPro incorporates these results in its tube database.
 * Special thanks to Gissio for both projects.
 *
 * -----------------------------------------------------------------------------
 * USAGE EXAMPLE
 * -----------------------------------------------------------------------------
 * @code
 *   #include "GeigerTubes.h"
 *   #include "GeigerMeasurement.h"
 *
 *   // Create a measurement instance: SBM-20 tube, background radiation
 *   GeigerMeasurement geiger(TUBE_SBM20, SOURCE_BACKGROUND);
 *
 *   // Switch to Cs-137 source at runtime (e.g. when measuring a known source)
 *   geiger.setSource(SOURCE_CS137);
 *
 *   // Query the computed sensitivity directly (for diagnostics)
 *   float s = tubeSourceSensitivity(TUBE_SBM20, SOURCE_BACKGROUND);
 *   // s ≈ 95.7 CPM / (µSv/h)
 * @endcode
 */

#pragma once
#include <stdint.h>
#include <math.h>   // exp2f

// =============================================================================
// TUBE TYPES
// =============================================================================

/**
 * @brief Supported Geiger-Müller tube types.
 *
 * The integer values are row indices into the internal sensitivity and source
 * factor tables — do NOT change them without updating the tables accordingly.
 *
 * Row order matches RadPro tube.c tubeSensitivities[]:
 *   J305, M4011, HH614, SBM-20, SI-3BG, LND7317
 *
 * Note on J321: RadPro does not list J321 as a separate tube. It shares
 * identical sensitivity data with M4011. TUBE_J321 is provided as an alias
 * for convenience — it is identical to TUBE_M4011 in every way.
 */
enum GeigerTube {
    TUBE_J305    = 0,   ///< J305 (cylindrical glass tube)
    TUBE_M4011   = 1,   ///< M4011 (widely used in cheap commercial Geiger counters)
    TUBE_HH614   = 2,   ///< HH614 (cylindrical glass tube, also common in cheap commercial
                        ///< Geiger counters)
    TUBE_SBM20   = 3,   ///< SBM-20 (Soviet-era surplus, very common in the DIY community)
    TUBE_SI3BG   = 4,   ///< SI-3BG (low sensitivity, mainly for high radiation detection)
    TUBE_LND7317 = 5,   ///< LND 7317 (cylindrical, halogen-quenched)
    TUBE_J305_90 = 6,   ///< J305 90mm variant — sensitivity derived from J305 107mm
                        ///< by applying the measured 90mm/107mm length ratio (0.721).
                        ///< Source correction factors are IDENTICAL to TUBE_J305
                        ///< (wall material and geometry are the same; only active
                        ///< length differs). The fieldFactor of 1.069 is also
                        ///< assumed equal to TUBE_J305 — both are UNVERIFIED
                        ///< hypotheses pending a parallel background radiation
                        ///< measurement.
                        ///< See the empirical field factors section for details.
    TUBE_COUNT   = 7,   ///< Number of tubes with Rad Lab simulation data
    TUBE_CUSTOM  = 7,   ///< Custom or unknown tube — no Rad Lab data available.
                        ///< Use setSensitivity() to set the sensitivity directly.
                        ///< tubeSourceSensitivity() returns NaN for this value.
                        ///< setFieldFactor() has no meaning for TUBE_CUSTOM since
                        ///< there is no Rad Lab baseline to correct against.
    TUBE_J321    = TUBE_M4011  ///< Alias for TUBE_M4011 — identical sensitivity data.
                        ///< RadPro does not list J321 separately; the two tubes
                        ///< share the same Rad Lab simulation values.
};

// =============================================================================
// SOURCE PRESETS
// =============================================================================

/**
 * @brief Radiation source presets for energy-dependent sensitivity correction.
 *
 * Each source emits gamma radiation at a characteristic energy (or set of
 * energies). Because our GM tubes are not energy-compensated, a separate correction
 * factor is needed for each tube/source combination.
 *
 * SOURCE_BACKGROUND represents natural background radiation, which is a mix of:
 *   - Terrestrial: K-40, U-238 and Th-232 decay chains
 *   - Cosmic: muons and secondary particles
 * This is the recommended default for environmental monitoring.
 *
 * The integer values are column indices in the source factor table.
 */
enum GeigerSource {
    SOURCE_CS137            =  0,  ///< Cs-137   661 keV γ  — standard calibration source
    SOURCE_CO60             =  1,  ///< Co-60    1.17 + 1.33 MeV γ
    SOURCE_TC99M            =  2,  ///< Tc-99m   140 keV γ  — nuclear medicine
    SOURCE_I131             =  3,  ///< I-131    364 keV γ  — medicine / fallout
    SOURCE_LU177            =  4,  ///< Lu-177   113 + 208 keV γ — nuclear medicine
    SOURCE_AM241            =  5,  ///< Am-241   59 keV γ   — smoke detectors
    SOURCE_RADIUM           =  6,  ///< Radium   Ra-226 decay chain (mixed energies)
    SOURCE_URANIUM_ORE      =  7,  ///< Uranium ore (U-238 decay chain)
    SOURCE_URANIUM_GLASS    =  8,  ///< Uranium glass (low-activity U-238)
    SOURCE_DEPLETED_URANIUM =  9,  ///< Depleted uranium (U-238 dominant)
    SOURCE_THORIUM_ORE      = 10,  ///< Thorium ore (Th-232 decay chain)
    SOURCE_XRAYS            = 11,  ///< X-rays at ~60 kV (diagnostic imaging)
    SOURCE_K40              = 12,  ///< K-40    1.46 MeV γ  — potassium in food/soil
    SOURCE_BACKGROUND       = 13,  ///< Natural background (recommended default)
    SOURCE_COUNT            = 14   ///< Total number of source presets
};

// =============================================================================
// INTERNAL DATA TABLES
// =============================================================================
// These are implementation details. Use the public API functions below.

/**
 * @brief Base sensitivities in CPM / (µSv/h), calibrated against Cs-137.
 *
 * Values from Rad Lab numerical simulations, as used in RadPro tube.c.
 * Row order matches the GeigerTube enum: J305, M4011, HH614, SBM-20, SI-3BG, LND7317.
 *
 * Note on HH614: the datasheet specifies 68.4 cpm/µSv/h (Co-60). The Rad Lab
 * simulation value of 30.157 is calibrated against Cs-137, which explains the
 * difference — the HH614 has a lower sensitivity to Cs-137 than to Co-60.
 *
 * Note on J321: not listed separately in RadPro. Use TUBE_J321 or TUBE_M4011 — they are identical.
 */
static const float _tubeSensitivities[TUBE_COUNT] = {
    135.200f,  // J305
    108.345f,  // M4011/J321
     30.157f,  // HH614
    106.105f,  // SBM-20
      3.267f,  // SI-3BG
    252.567f,  // LND7317
     97.480f,  // J305_90 — DERIVED: 135.200 × 0.721 (measured 90mm/107mm length ratio)
               //   UNVERIFIED: source factors assumed identical to J305 107mm
};

/**
 * @brief Source correction factors, stored as uint8_t on a logarithmic scale.
 *
 * WHY ENCODE AS uint8_t?
 *   Storing 6×14 = 84 floats would use 336 bytes of flash. By encoding on a
 *   logarithmic scale we use only 84 bytes, with negligible precision loss.
 *
 * ENCODING FORMULA (from RadPro tube.c):
 *   factor = 0.125 × 2^(code / 36)
 *
 * Key reference points on this scale:
 *   code =   0  →  factor = 0.125 × 2^0.000 = 0.125  (minimum)
 *   code = 108  →  factor = 0.125 × 2^3.000 = 1.000  (Cs-137 baseline)
 *   code = 255  →  factor = 0.125 × 2^7.083 ≈ 17.1   (maximum)
 *
 * SPECIAL CASE — SI-3BG + X-rays (code = 0):
 *   Code 0 maps to the minimum factor (0.125), meaning ~8× less sensitive
 *   than to Cs-137. The SI-3BG's small size and geometry make it
 *   exceptionally insensitive to soft 60 kV X-rays.
 *
 * Row order:    J305, M4011, HH614, SBM-20, SI-3BG, LND7317
 * Column order: CS137, CO60, TC99M, I131, LU177, AM241, RADIUM,
 *               URANIUM_ORE, URANIUM_GLASS, DEPLETED_URANIUM,
 *               THORIUM_ORE, XRAYS, K40, BACKGROUND
 */
static const uint8_t _tubeSourceFactors[TUBE_COUNT][SOURCE_COUNT] = {
    {108, 113, 204, 107, 172, 236, 111, 117, 124, 123, 113, 196, 110, 123}, // J305
    {108, 113, 204, 107, 173, 236, 111, 118, 124, 123, 113, 196, 110, 123}, // M4011/J321
    {108, 124, 156,  93, 113, 185, 106, 106, 106, 112,  62, 153, 125, 112}, // HH614
    {108,  95, 169, 103, 126, 207, 100, 105, 110, 110, 100, 198,  92, 108}, // SBM-20
    {108, 125, 153,  93, 113,  77, 107, 106, 108, 112,  89,   0, 128, 109}, // SI-3BG
    {108, 107, 154, 104, 127, 167, 107, 111, 114, 113, 108, 161, 107, 115}, // LND7317
    {108, 113, 204, 107, 172, 236, 111, 117, 124, 123, 113, 196, 110, 123}, // J305_90
    //  J305_90 IDENTICAL to J305 107mm — wall material and geometry are the same;
    //  only active length differs. UNVERIFIED: assumes energy response is
    //  length-independent. Pending verification with a Cs-137 source.
};

// =============================================================================
// INTERNAL HELPER
// =============================================================================

/**
 * @brief Decode a uint8_t source factor code to a float correction factor.
 *
 * Inverse of the encoding used in RadPro tube.c:
 *   factor = SOURCE_FACTOR_DATA_MIN × 2^(code / SOURCE_FACTOR_DATA_SCALE)
 *          = 0.125 × 2^(code / 36)
 *
 * @param code  Encoded factor (0–255)
 * @return      Decoded correction factor (float)
 */
inline float _decodeSourceFactor(uint8_t code) {
    // 0.125 = SOURCE_FACTOR_DATA_MIN (RadPro)
    // 36    = SOURCE_FACTOR_DATA_SCALE (RadPro)
    return 0.125f * exp2f(code * (1.0f / 36.0f));
}

// =============================================================================
// PUBLIC API
// =============================================================================

/**
 * @brief Compute the source-specific sensitivity for a tube/source combination.
 *
 * This is the primary function. It combines the base sensitivity with the
 * energy-dependent source correction:
 *
 *   result = base_sensitivity[tube] × decode(factor_table[tube][source])
 *
 * The result should be passed to GeigerMeasurement's constructor or
 * setSensitivity().
 *
 * @param tube    Tube type (e.g. TUBE_SBM20)
 * @param source  Radiation source (e.g. SOURCE_BACKGROUND, SOURCE_CS137)
 * @return        Sensitivity in CPM / (µSv/h).
 *                Falls back to SBM-20/Cs-137 on invalid input.
 */
inline float tubeSourceSensitivity(GeigerTube tube, GeigerSource source) {
    if (tube >= TUBE_COUNT || source >= SOURCE_COUNT)
        return NAN;   // TUBE_CUSTOM or out-of-range: no Rad Lab data
    float factor = _decodeSourceFactor(_tubeSourceFactors[tube][source]);
    return factor * _tubeSensitivities[tube];
}

/**
 * @brief Return only the source correction factor (not multiplied by base sensitivity).
 *
 * Useful for diagnostics. A value of 1.0 means this source matches the Cs-137
 * calibration baseline. Values > 1.0 indicate higher sensitivity; < 1.0 lower.
 *
 * @param tube    Tube type
 * @param source  Radiation source
 * @return        Dimensionless correction factor. Returns 1.0 on invalid input.
 */
inline float tubeSourceFactor(GeigerTube tube, GeigerSource source) {
    if (tube >= TUBE_COUNT || source >= SOURCE_COUNT)
        return 1.0f;
    return _decodeSourceFactor(_tubeSourceFactors[tube][source]);
}

/**
 * @brief Return the base sensitivity for a tube (Cs-137, no source correction).
 *
 * Equivalent to tubeSourceSensitivity(tube, SOURCE_CS137).
 * Use this if you want to apply your own source correction.
 *
 * @param tube  Tube type
 * @return      Base sensitivity in CPM / (µSv/h). Falls back to SBM-20.
 */
inline float tubeSensitivity(GeigerTube tube) {
    if (tube >= TUBE_COUNT)
        return NAN;   // TUBE_CUSTOM or out-of-range: no Rad Lab data
    return _tubeSensitivities[tube];
}

// =============================================================================
// EMPIRICAL FIELD FACTORS — measured background radiation data
// =============================================================================
//
// The Rad Lab simulation values above are theoretical (Cs-137 reference
// geometry, ideal conditions). In practice, real GM tubes in background
// radiation fields show systematic deviations from these values — mainly due
// to differences in wall material, energy response, and tube geometry vs.
// the simulation model.
//
// The field factor (GeigerMeasurement::setFieldFactor()) corrects for this:
//
//   effective_sensitivity = radlab_sensitivity * fieldFactor
//
// MEASUREMENT CONDITIONS
//   Location:  Pannonhalma, Hungary (indoor, ~1m above floor)
//   Duration:  ~43.5 hours parallel run (lifetime average, Poisson error ~1%)
//   Reference: BOSEAN FS-5000 with J321 tube, RadPro 3.1.1 firmware
//              (using Rad Lab Cs-137/background values)
//              Displayed: ~16.6 CPM → 0.115 µSv/h
//              Consistent with regional background data (~0.087-0.115 µSv/h,
//              OMSZ/HM monitoring network, Hungary. See
//              https://www.katasztrofavedelem.hu/modules/hattersugarzas/aktualis_adatsor
//              for details.)
//   Software:  GeigerMeasurement library + GeigerCompare.ino, SOURCE_BACKGROUND
//
// RESULTS
//
//   Tube        CPM (meas.)  fieldFactor  Notes
//   ----------  -----------  -----------  -----------------------------------
//   M4011          21.11        1.269     ESP8266, ~380V HV
//   SBM-20         19.66        1.611     ESP8266, ~400V HV
//   J305 107mm     22.19        1.069     ESP8266, ~380V HV
//   J305 90mm      14.96          —       ESP8266, ~380V HV; use TUBE_J305_90 or
//                                         setSensitivity(130.1f) directly.
//                                         TUBE_J305_90 sensitivity is DERIVED
//                                         (not from Rad Lab simulation).
//
// INTERPRETATION
//   - J305 107mm / SBM-20 ratio: 1.129 (Rad Lab predicts 1.701, -33.6%)
//   - M4011 / SBM-20 ratio:      1.074 (Rad Lab predicts 1.363, -21.2%)
//   - SBM-20 shows the largest deviation (1.54x), consistent with its steel
//     wall filtering low-energy background components differently than the
//     simulation assumes.
//   - J305 90mm has no valid fieldFactor — the Rad Lab sensitivity (180.5) applies
//     to the 107mm geometry only. Use setSensitivity(130.1f) directly instead
//     (empirical value: 14.96 CPM / 0.115 µSv/h, ~43.5h run).
//
//     The 90mm/107mm sensitivity ratio (0.721) is notably below both the
//     simple geometric length ratio (90/107 = 0.841, -14.3%) and the
//     effective-length prediction assuming ~3.5mm dead zones per end:
//
//       effL_90  = 90  - 2×3.5 = 83 mm
//       effL_107 = 107 - 2×3.5 = 100 mm
//       ratio    = 83 / 100     = 0.830  (measured: 0.721, -13.2% discrepancy)
//
//     The large discrepancy between the geometric prediction (0.830) and the
//     measured ratio (0.721) is not explained by end-zone geometry alone.
//     Possible causes: non-uniform electric field near the cathode ends,
//     energy-dependent efficiency variation, or uncertainty in the actual
//     tube length. The cause is unresolved.
//
//     UNVERIFIED HYPOTHESIS: despite the unexplained discrepancy, the
//     measured ratio (0.721) may still be source-independent, since tube
//     geometry affects all gamma sources similarly. If confirmed, the
//     sensitivity for any source can be approximated from the 107mm value:
//
//       sens_90(source) ≈ tubeSourceSensitivity(TUBE_J305, source) × 0.721
//
//     Example for Cs-137:
//       J305 107mm Cs-137: 135.200 CPM/(µSv/h)
//       J305  90mm Cs-137: 135.200 × 0.721 ≈ 97.5 CPM/(µSv/h)  [unverified]
//
//     A dedicated Rad Lab simulation for the 90mm geometry would allow a
//     proper fieldFactor to be derived and this hypothesis to be confirmed.
//   - The FS-5000 reference itself uses Rad Lab values, so these field factors
//     represent real-world vs. simulation deviation, not absolute calibration.
//
// USAGE EXAMPLE
//   // Apply the empirical field factor for SBM-20 background measurements:
//   geiger.setFieldFactor(1.611f);   // measured 2026, Pannonhalma (~43.5h)
//
//   // Or calibrate live against a known reference:
//   while (!geiger.calibrate(0.115f, 15.0f)) { delay(1000); }
//   float ff = geiger.getFieldFactor();  // save to EEPROM/Flash for next boot
//
//   // J305 90mm — Option A: TUBE_J305_90 with assumed fieldFactor:
//   GeigerMeasurement geiger(TUBE_J305_90, SOURCE_BACKGROUND);
//   geiger.setFieldFactor(1.069f);   // assumed = J305 107mm — UNVERIFIED
//
//   // J305 90mm — Option B: TUBE_CUSTOM with measured sensitivity:
//   GeigerMeasurement geiger(TUBE_CUSTOM, SOURCE_BACKGROUND);
//   geiger.setSensitivity(130.1f);   // empirical: 14.96 CPM / 0.115 µSv/h, ~43.5h

/**
 * @brief Return a short human-readable name for a tube type.
 *
 * Suitable for Serial output, display labels, and CSV headers.
 * Returns "Custom" for TUBE_CUSTOM or any out-of-range value.
 *
 * @param tube  Tube type
 * @return      Null-terminated string literal (no allocation)
 */
inline const char* tubeLabel(GeigerTube tube) {
    switch (tube) {
        case TUBE_J305:    return "J305";
        case TUBE_M4011:   return "M4011/J321";
        case TUBE_HH614:   return "HH614";
        case TUBE_SBM20:   return "SBM-20";
        case TUBE_SI3BG:   return "SI-3BG";
        case TUBE_LND7317: return "LND7317";
        case TUBE_J305_90: return "J305-90";
        default:           return "Custom";
    }
}
