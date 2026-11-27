# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

## [1.1.2] - 2026-11-27

### Fixed

- The fixed modes read one count per window low. The rate was `(N − 1) / W`,
  the adaptive formula, where `(N − 1)` belongs to a window that ends at a pulse
  count reached; a fixed window counts every pulse in it, so `N / W` — as
  RadPro 3.2 has now changed it too. In `FIXED_60S` that was 1 CPM: 5% at a
  20 CPM background, 8% at 13 CPM. An empty window still reads 0, valid; a
  window with a single pulse now reads that pulse instead of 0.
- The fixed modes stopped at the buffer size. With more pulses in the window than
  `GEIGER_PULSE_BUFFER_SIZE`, the oldest had been overwritten and only the rest
  were counted: `FIXED_60S` could not read above 255 CPM with the default
  buffer, 1000 CPM read 75% low. Such a window is now measured over the span
  the buffer covers, from its oldest pulse to now.
- The fixed modes counted the time before a buffer clear as silence. After
  `reset()`, `setMode()` or an adaptive setter the window now grows from zero,
  so the first minute reads the rate instead of a ramp up to it (10 s after a
  clear, `FIXED_60S` had read 82% low).

  Adaptive modes are unchanged.

## [1.1.1]

### Fixed

- The ISR-shared counters are updated without compound or chained operations on
  `volatile`. C++20 deprecates both — a compound operation because its
  read-modify-write order is unspecified, a chained assignment because it reads
  back the value of a volatile store — and ESP32 Arduino core 3.x compiles at a
  standard that warns about them.

  Behaviour and generated code are unchanged; `_count++` becomes a read, an add
  and a write, and `_head = _count = 0` becomes two statements.

## [1.1.0] - 2026-09-20

### Added

- `tubeOperating()` and `tubeHasPlateau()` in `GeigerTubes.h`: datasheet plateau
  range, recommended voltage, absolute maximum and dead time per tube. Tube
  properties in the same sense as the sensitivities — true in any circuit, and
  needed by every project that drives a tube, which would otherwise each keep
  their own copy.

  Only sourced figures are included, and zero means "unknown" rather than zero
  volts: a plateau invented to fill a gap would report healthy supplies as
  faulty, and nothing downstream could tell. Individual fields are empty where a
  datasheet gives no value — the SI-3BG states no recommended point or dead
  time, and neither Chinese sheet states a dead time worth the name.

  `plateauMaxV` and `absoluteMaxV` are separate on purpose — above the plateau
  the counts are wrong, above the maximum the tube is being damaged.

### Changed

- The empirical field factors are replaced by a 214-hour four-tube run: J305
  1.030, M4011 1.279, SBM-20 1.558, and 122.2 CPM/(µSv/h) for the 90 mm J305.
  All four agree with the reference to within 1%, which is their counting
  precision.

  The previous figures were measured with three of the supplies a few volts
  above the plateau knee. Moving them to mid-plateau changed those factors by 3%
  and one by 10.6%, so the operating point is now stated alongside the numbers —
  along with the board and the dead-time setting, since a field factor absorbs
  all three.

### Fixed

- `calibrate()` now works for `TUBE_CUSTOM`. It previously skipped its entire
  body and returned `false` whenever the tube had no Rad Lab baseline, which is
  exactly the case where a reference measurement is the only way to obtain a
  sensitivity at all. The measured value is now written to `_sensitivity`
  regardless; the field factor is derived only where there is a baseline to
  divide by, and stays 1.0 otherwise — consistent with `setFieldFactor()`, which
  has never had any effect for a custom tube.

  No change for named tubes. For a custom tube, save and restore
  `getSensitivity()` / `setSensitivity()` rather than the field factor.

## [1.0.3] - 2026-08-28

### Fixed

- `getReading()` now sets `tubeAlive` and `timestampMs` on every return path.
  Two early returns in adaptive mode (zero window duration, and fewer than two
  pulses inside the window) previously returned the value-initialised struct,
  leaving `tubeAlive` false and `timestampMs` zero. Both are routine at
  background count rates with `ADAPTIVE_FAST`. The false `tubeAlive` reads as a
  dead tube, and a zero `timestampMs` passed to `RollingStats::addSample()`
  underflows its elapsed-time arithmetic, committing the entire ring buffer as
  NaN bins. `valid` still reports false on these paths — the numerical fields
  remain meaningless, as documented.

### Changed

- Minor documentation fix in README.md
- Documented that `tubeAlive` and `timestampMs` are set regardless of `valid`

## [1.0.2] - 2026-05-19

### Changed

- Updated install instructions
- Removed dependency from library.json
- Updated reference to RunningStatistics headers in README.md

## [1.0.1] - 2026-05-19

### Changed

- Follow the name change the companion library to RunningStatistics

## [1.0.0] - 2026-05-19

### Added

- First public release

[unreleased]: https://github.com/soosp/GeigerMeasurement/compare/1.1.2...HEAD
[1.1.2]: https://github.com/soosp/GeigerMeasurement/compare/1.1.1...1.1.2
[1.1.1]: https://github.com/soosp/GeigerMeasurement/compare/1.1.0...1.1.1
[1.1.0]: https://github.com/soosp/GeigerMeasurement/compare/1.0.3...1.1.0
[1.0.3]: https://github.com/soosp/GeigerMeasurement/compare/1.0.2...1.0.3
[1.0.2]: https://github.com/soosp/GeigerMeasurement/compare/1.0.1...1.0.2
[1.0.1]: https://github.com/soosp/GeigerMeasurement/compare/1.0.0...1.0.1
[1.0.0]: https://github.com/soosp/GeigerMeasurement/releases/tag/1.0.0
