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

## Follow-up investigation and recovery — 2026-11-20

Active development head at this checkpoint: `5db5c291f4ff6a91e1bd95b7de58c83dbfa24be5`.

Additional frozen recovery branch:
`checkpoint/native-modules-2026-11-20-5db5c29` (points exactly to the active head above; the earlier `52eec1f` checkpoint is retained).

### CI queue diagnosis

- The full `Build all` run #26 for commit `de1575ecf539` was still actively building dependencies when inspected. The Windows x64 dependency build had reached approximately 80% of 125 Assimp compilation tasks in its log at 2026-11-20 03:35:53 UTC; Linux, macOS, and Flatpak jobs were also active. This is long-running dependency compilation, not a C++ source error.
- The latest full `Build all` run #29 for `5db5c291` was pending behind that active run. The workflow uses the per-PR concurrency group `build-all-pull_request-2` with `cancel-in-progress: false`. GitHub permits one running and one pending run per concurrency group; newer pending runs replace older pending runs, while the active run is allowed to finish. The latest head will therefore build after the active run releases the group.
- The focused `Native Windows x64 build` run #2 was also still building Windows dependencies; run #5 for the latest head was pending behind it. This focused workflow duplicates part of the full Build all Windows x64 work and should be monitored for redundant runner use.
- Native module regression tests passed on commit `f0e5dbab832a`; the run for `5db5c291` was queued at the time of inspection.

### Corrected profile-check diagnosis

The previous PR profile check was **not** failing because the legacy printer map was outdated. The log shows:
`fatal: couldn't find remote ref refs/tags/v2.4.2`.
The check attempted to fetch the release tag from the fork's `origin`, but that tag is absent in the fork. The tag exists in the canonical `OrcaSlicer/OrcaSlicer` repository. The workflow is now changed to fetch `v2.4.2` from that upstream repository, and the entire fetch/archive/check command is kept in the step log pipeline so future failures show their real cause.

### Clang-Tidy diagnosis

The previous Clang-Tidy run failed at `Restore the newest cached deps` because no compatible dependency cache was available. The configure and clang-tidy execution steps were skipped, so this run produced no C++ diagnostics. Re-run after the active dependency build/cache work completes; do not classify this as a code-clean result or as a C++ defect.

### Next validation sequence

1. Let the already-running full Build all and focused Windows x64 jobs finish without pushing unnecessary commits.
2. Confirm the latest-head Build all run starts after the active per-PR concurrency slot is released.
3. Confirm the fixed profile check can fetch upstream `v2.4.2` and run the compatibility-map comparison.
4. Re-run Clang-Tidy after a matching Linux dependency cache is available.
5. Inspect actual Windows x64 source compilation diagnostics, then fix only concrete compiler/test failures.
6. Keep PR #2 draft and do not merge until full build, 3MF persistence, undo/redo, geometry manifoldness, and real G-code validation pass.

## Further CI fixes — 2026-11-20

The follow-up review identified additional causes of unnecessary CI churn and fixed them together:

- **Clang-Tidy cache wait:** the workflow previously marked a PR as changing dependencies only when `deps/` changed, while `ci_deps_cache.sh` also fingerprints `.github/`. Because this PR changes workflow files, Clang-Tidy incorrectly looked for a base-branch cache instead of waiting for this PR's own Build all dependency job. It then failed at cache restore before running C++ analysis. The changed-file check now treats either `deps/` or `.github/` changes as requiring the PR's own cache, and Clang-Tidy no longer cancels its active run on each new commit.
- **Profile-check queue:** added a per-PR concurrency group that lets the active check finish and retains only the newest pending run. The legacy-tag fetch now targets upstream and captures fetch/archive/check output in one log.
- **Duplicate native tests:** the native module test workflow now runs on pull requests and manual dispatch, not both push and pull_request for the same PR commit. It also uses a per-PR concurrency group.
- **Duplicate Windows build:** the focused Windows x64 workflow is now manual-only because Build all already performs the full Windows x64 matrix. This prevents future duplicate dependency builds while preserving a manual diagnostic build.

The active full Build all run #26 and the focused Windows x64 run #2 were already in progress before these changes; they were left untouched. The latest full Build all run is expected to wait behind the active per-PR build group and then validate the current PR head. Existing queued runs created before these workflow changes may still complete or be superseded by newer pending runs.

## Geometry safety validation — 2026-11-20

A code review found that a painted region on a vertical wall can collapse to a zero-area footprint when projected onto the build plate. The previous implementation could emit an open or zero-volume connector shell because it only rejected degenerate individual triangles.

The connector generator now validates every downward connector and bridge as a closed, consistently oriented, non-zero-volume triangle shell before adding it to the model. Downward connectors also reject painted regions that intersect or approach the build plate below the configured minimum height. Unsupported geometry is counted as skipped rather than emitted as a broken printable volume.

Regression coverage added for vertical patches (must skip), sloped patches (must remain watertight), and patches intersecting the bed (must skip). The existing horizontal, facing-bridge, and mismatched-loop tests now check edge pairing, winding consistency, and non-zero signed volume.
