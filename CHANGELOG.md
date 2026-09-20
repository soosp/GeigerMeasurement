# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

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

[unreleased]: https://github.com/soosp/GeigerMeasurement/compare/1.0.3...HEAD
[1.0.3]: https://github.com/soosp/GeigerMeasurement/compare/1.0.2...1.0.3
[1.0.2]: https://github.com/soosp/GeigerMeasurement/compare/1.0.1...1.0.2
[1.0.1]: https://github.com/soosp/GeigerMeasurement/compare/1.0.0...1.0.1
[1.0.0]: https://github.com/soosp/GeigerMeasurement/releases/tag/1.0.0
