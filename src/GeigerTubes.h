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
 *   #include <GeigerMeasurement.h>  // It includes GeigerTubes.h
 *
 *   // Create a measurement instance: SBM-20 tube, background radiation
 *   GeigerMeasurement geiger(TUBE_SBM20, SOURCE_BACKGROUND);
 *
 *   // Switch to Cs-137 source at runtime (e.g. when measuring a known source)
 *   geiger.setSource(SOURCE_CS137);
 *
 *   // Query the sensitivity directly (for diagnostics)
 *   float s = tubeSourceSensitivity(TUBE_SBM20, SOURCE_CS137);
 *   // s ≈ 106.105 CPM / (µSv/h)
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
    TUBE_COUNT   = 6,   ///< Number of tubes with Rad Lab simulation data
    TUBE_CUSTOM  = 6,   ///< Custom or unknown tube — no Rad Lab data available.
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
inline constexpr float _tubeSensitivities[TUBE_COUNT] = {
    135.200f,  // J305
    108.345f,  // M4011/J321
     30.157f,  // HH614
    106.105f,  // SBM-20
      3.267f,  // SI-3BG
    252.567f,  // LND7317
};

/**
 * @brief Source correction factors, stored as uint8_t on a logarithmic scale.
 *
 * WHY ENCODE AS uint8_t?
 *   Storing 6x14 = 84 floats would use 336 bytes of flash. By encoding on a
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
inline constexpr uint8_t _tubeSourceFactors[TUBE_COUNT][SOURCE_COUNT] = {
    {108, 113, 204, 107, 172, 236, 111, 117, 124, 123, 113, 196, 110, 123}, // J305
    {108, 113, 204, 107, 173, 236, 111, 118, 124, 123, 113, 196, 110, 123}, // M4011/J321
    {108, 124, 156,  93, 113, 185, 106, 106, 106, 112,  62, 153, 125, 112}, // HH614
    {108,  95, 169, 103, 126, 207, 100, 105, 110, 110, 100, 198,  92, 108}, // SBM-20
    {108, 125, 153,  93, 113,  77, 107, 106, 108, 112,  89,   0, 128, 109}, // SI-3BG
    {108, 107, 154, 104, 127, 167, 107, 111, 114, 113, 108, 161, 107, 115}, // LND7317
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
//   Duration:  214 hours, four tubes in parallel, undisturbed
//   Reference: BOSEAN FS-5000 with J321 tube, RadPro 3.1.1 firmware
//              (using Rad Lab Cs-137/background values)
//              24.81 µSv over 213:42:46 → 0.1161 µSv/h
//              Consistent with regional background data (~0.087-0.115 µSv/h,
//              OMSZ/HM monitoring network, Hungary. See
//              https://www.katasztrofavedelem.hu/modules/hattersugarzas/aktualis_adatsor
//              for details.)
//   Hardware:  ESP8266 on CAJOE-derived boards, each supply set to mid-plateau
//              (420-426 V), dead-time compensation off
//   Software:  GeigerMeasurement library, SOURCE_BACKGROUND
//
// RESULTS
//
//   Tube        CPM (meas.)  fieldFactor  vs reference   +-95%
//   ----------  -----------  -----------  ------------  ------
//   J305 107 mm    21.7         1.030        +0.27%      0.37%
//   M4011          21.5         1.279        +0.51%      0.37%
//   SBM-20         20.2         1.558        +0.61%      0.39%
//   J305 90 mm     14.3           —          -0.40%      0.46%
//                                use setSensitivity(122.2f).
//
//   All four agree to within 1% of the reference and of each other, which is
//   their counting precision. More time would not improve it: past this point
//   the reference's own accuracy is the limit.
//
// THREE QUALIFIERS, AND NONE OF THEM IS THE TUBE
//   These are not "the correction for this tube". A field factor absorbs
//   everything between a count and the displayed number, and the measurements
//   above show how much of it is not the glass:
//
//   1. OPERATING POINT. An earlier run had three of the supplies a few volts
//      above the plateau knee, where efficiency is at its most voltage-
//      sensitive. Moving them to mid-plateau changed their factors by 3%, and
//      one by 10.6%. Calibrating before the operating point is set measures the
//      operating point.
//   2. THE BOARD. Two tubes on identical boards tracked each other to 0.4% over
//      nine days. The one tube on an earlier board revision — same resistors,
//      same HV section, different topology — did not.
//   3. DEAD TIME OFF. At background the compensation is 0.003-0.006%, so this
//      changes nothing here, but the figures were measured without it.
//
//   A far better starting point than 1.0, and not a substitute for calibrating
//   a specific tube-and-board pair.
//
// INTERPRETATION
//   - SBM-20 still deviates most from Rad Lab (1.56x), consistent with its
//     steel wall responding to the low-energy background differently than the
//     simulation assumes.
//   - Over nine quiet days the SBM-20's day-to-day scatter against the other
//     tubes was 2.2x what counting statistics alone predict, while the two
//     glass tubes matched prediction. Suggestive of a real difference in what
//     the tubes respond to, and no more than suggestive: no spectrum was
//     measured, and a GM tube cannot measure one.
//   - J305 90 mm: not listed in the RadPro table, and not a shorter J305 — it
//     counts 34% below an M4011 of the same envelope, where the 1 mm diameter
//     difference accounts for at most half. Use TUBE_CUSTOM.
//   - The FS-5000 reference itself uses Rad Lab values, so these field factors
//     represent real-world vs. simulation deviation, not absolute calibration.
//
// USAGE EXAMPLE
//   // Apply the empirical field factor for SBM-20 background measurements:
//   geiger.setFieldFactor(1.558f);   // measured 2026, Pannonhalma (214 h)
//
//   // Or calibrate live against a known reference:
//   while (!geiger.calibrate(0.1161f, 15.0f)) { delay(1000); }
//   float ff = geiger.getFieldFactor();  // save to EEPROM/Flash for next boot
//
//   // J305 90 mm — Use TUBE_CUSTOM with measured sensitivity:
//   GeigerMeasurement geiger(TUBE_CUSTOM, SOURCE_BACKGROUND);
//   geiger.setSensitivity(122.2f);   // empirical: 14.3 CPM / 0.1161 µSv/h, 214 h

// =============================================================================
// OPERATING DATA — plateau, voltage limits, dead time
// =============================================================================

/**
 * @brief Datasheet operating figures for a tube.
 *
 * Here for the same reason the sensitivities are: these are properties of the
 * tube, not decisions an application makes. A plateau range is a fact about the
 * glass and the gas, it holds in any circuit, and every project that drives a
 * tube needs it — the alternative is each of them keeping its own copy and
 * getting it wrong separately.
 *
 * **A zero means no data, not zero volts.** Only sourced figures are here;
 * where no datasheet was to hand the fields are left empty rather than
 * estimated. A guess in a library is worse than a gap: a plateau set too narrow
 * reports a healthy supply as faulty, and nothing downstream can tell that the
 * number was invented.
 *
 * Where sources disagree, the widest credible plateau is taken. A window
 * narrower than the real plateau raises a fault on a working supply; one
 * slightly wider only notices a drift a little later, and drift of that size is
 * not subtle.
 *
 * @section limits Two limits, not one
 * `plateauMaxV` and `absoluteMaxV` mean different things and are easy to
 * conflate. Above the plateau the counts are wrong; above the absolute maximum
 * the tube is being damaged. A supply that cannot be regulated only needs the
 * first, to know whether to trust a reading; one that can should also respect
 * the second.
 */
struct GeigerTubeOperating {
    uint16_t plateauMinV;    ///< Lower end of the counting plateau [V]. 0 = unknown.
    uint16_t plateauMaxV;    ///< Upper end of the counting plateau [V]. 0 = unknown.
    /**
     * The operating point the datasheet names [V]. 0 = unknown.
     *
     * The manufacturer's figure, carried as given — the library neither uses it
     * nor endorses it. Worth knowing when choosing a tube, and worth reading
     * with care, because it is not always mid-plateau and the sheets do not
     * agree with each other: the J305's names 420 V in a 380-480 V plateau, the
     * M4011's names 380 V at the very bottom of 380-450.
     *
     * Mid-plateau is where a supply is least sensitive to drift, and at the
     * knee a few volts move the counting efficiency far more than the plateau
     * slope suggests. Whether a lower point buys tube life is a plausible guess
     * and not something any of these sheets states; at background it would be
     * moot either way, since 10^9 counts at 20 CPM is most of a century.
     */
    uint16_t recommendedV;
    uint16_t absoluteMaxV;   ///< Damage threshold [V]. 0 = unknown.
    uint16_t deadTimeUs;     ///< Datasheet dead time [us]. 0 = unknown or unspecified.
};

/**
 * @brief Operating data, row order matching the GeigerTube enum.
 *
 * Sources, row by row:
 *
 *   J305    Factory sheet Q/FG394.091-2003 (State Factory 772). Plateau
 *           380-480 V, recommended 420 V, minimum discharge 550 V. Seller
 *           listings for both the 107 mm and 90 mm tubes say 380-450, and a
 *           distributor table says 360-440; the factory sheet is the primary
 *           source and the widest.
 *   M4011   M4011 parameter sheet: working voltage 380-450 V, recommended
 *           380 V, plateau at least 80 V long, maximum 550 V. J321 shares it.
 *   HH614   Nanjing Hean datasheet: operating 400-500 V, recommended 420 V,
 *           maximum 550 V, dead time 15 us, plateau slope <=0.3%/V.
 *   SBM-20  ODO.339.172 TU: operating 350-475 V, recommended 400 V, minimum
 *           dead time 190 us at 400 V.
 *   SI-3BG  Soviet datasheet: working voltage 380-460 V, plateau 80 V long,
 *           slope 0.25%/V. No recommended point, maximum or dead time given.
 *           Note its intended use: a 300 R/h range tube that barely responds to
 *           background at all — a few tenths of a count per second.
 *   LND7317 LND datasheet: operating 475-675 V, recommended 500 V, maximum
 *           starting voltage 425 V, minimum dead time 40 us. A pancake with a
 *           2.0 mg/cm2 mica window — twenty times thinner than the SBM-20's
 *           steel, and correspondingly more responsive to beta. Its recommended
 *           anode resistor is 4.7 M (3.3 M minimum), not the 10 M common on
 *           CAJOE-style boards.
 *
 * Dead times are given only where a datasheet states one. The values circulating
 * for J305 and M4011 (~90 us) are convention rather than specification, and at
 * background the compensation they would enable is under 0.01% — not worth
 * carrying an unsourced number for.
 *
 * Where a datasheet does give one it is a *minimum*, both for the SBM-20 and the
 * LND7317. Measurements of individual LND7317s have come out several times its
 * 40 us, which is a reminder of what these figures are: a starting point for a
 * measurement, not a substitute for one.
 */
inline constexpr GeigerTubeOperating _tubeOperating[TUBE_COUNT] = {
    // plateau min, max, recommended, absolute max, dead time
    { 380, 480, 420, 550,   0 },   // J305
    { 380, 450, 380, 550,   0 },   // M4011/J321
    { 400, 500, 420, 550,  15 },   // HH614
    { 350, 475, 400,   0, 190 },   // SBM-20
    { 380, 460,   0,   0,   0 },   // SI-3BG
    { 475, 675, 500,   0,  40 },   // LND7317
};

/**
 * @brief Return the operating data for a tube.
 *
 * Every field is zero for TUBE_CUSTOM, for an out-of-range value, and for any
 * tube whose datasheet is not represented above. Check before use: a plateau of
 * 0-0 V would fault on any voltage at all.
 *
 * @param tube  Tube type
 * @return      Operating figures; all-zero when nothing is known.
 */
inline GeigerTubeOperating tubeOperating(GeigerTube tube) {
    if (tube >= TUBE_COUNT) return GeigerTubeOperating{0, 0, 0, 0, 0};
    return _tubeOperating[tube];
}

/**
 * @brief Whether a plateau range is known for this tube.
 *
 * Convenience for the common guard, since a plateau check is the usual reason
 * to ask. Equivalent to testing both plateau fields.
 *
 * @param tube  Tube type
 * @return      true if plateauMinV and plateauMaxV are both set.
 */
inline bool tubeHasPlateau(GeigerTube tube) {
    GeigerTubeOperating o = tubeOperating(tube);
    return o.plateauMinV != 0 && o.plateauMaxV != 0;
}

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
        default:           return "Custom";
    }
}
