# OrcaSlicer v2.5.0 Native Modules — Development Checkpoint

Date: 2026-11-20
Canonical upstream baseline: `40ecc66f78ed72dc368e0c1ebe40d77f00cc8a19`
Development branch: `feature/painted-surface-layer-time-smooth-v250`
Frozen recovery checkpoint: `checkpoint/native-modules-2026-11-20-52eec1f`
Checkpoint commit: `52eec1fb1470d013e30cdae8ba244324dcf8723f`
Review PR: https://github.com/yinq83583-prog/0001/pull/2 (draft; release/orcaslicer-v2.5.0 base)

## Implemented on development branch

- Painted Surface Connector: native painting gizmo, independent paint annotation stored in 3MF, geometry generation for facing surfaces and optional down-to-bed regions.
- Layer Time Smooth: native C++ G-code feedrate post-processing, print configuration entries, preview processor feedrate replay.
- Regression tests: standalone GCC `-Wall -Wextra -Werror` checks for both modules; passing as of the checkpoint.
- CMake integration for both native libraries and the connector gizmo.

## Why CI appeared stuck

1. `.github/workflows/build_all.yml` had `concurrency.cancel-in-progress: ${{ github.event_name == 'pull_request' }}`. Any new commit on PR #2 cancelled its preceding long-running Build all job before dependent Windows x64 compilation could finish.
2. Clang-Tidy failures on PR #2 showed failure in `Restore the newest cached deps`, with `fail-on-cache-miss: true`. This is missing dependency cache, NOT proof of Clang-Tidy violations in native code.
3. `Build all` launches expensive multi-platform dependency compilation. Standalone native tests are much quicker and have passed.

Mitigation committed: set `concurrency.cancel-in-progress: false` in `.github/workflows/build_all.yml` on the development branch. This prevents incremental PR pushes from invalidating already-started heavy build jobs, but more than one build may consume CI runner time.

## Remaining validation

- Successful full OrcaSlicer Windows 11 x64 build and GUI launch.
- Functional tests: 3MF roundtrip and undo/redo for connector painting.
- Watertight/manifold geometry and collision checks on real model surfaces.
- Preview of generated connectors and live viewport feedback.
- Final G-code feedrate, time-limit and machine compatibility validation with actual OrcaSlicer output.
- Clang-Tidy on a runner with a usable dependencies cache.

Do not merge or use for physical printing until these checks pass. Preserve main and SLA/LCD modules.
