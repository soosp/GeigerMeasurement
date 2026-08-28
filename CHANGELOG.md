# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

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
