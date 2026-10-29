# Terrain profile and draw-state update, 2026-10-29

The user captured `build/virgl-profile.log` after the producer-wait and shader
selection updates. The most draw-heavy context is 9. Four consecutive reporting
windows near the end of the capture show approximately:

| Metric | Per two-second window |
| --- | --- |
| Host dispatch wall time | 951–960 ms |
| Host dispatch thread CPU time | 937–946 ms |
| DRAW_VBO dispatch | 180,000 draws, 555 ms |
| GL error checks | 34.5 ms |
| Context switching inside submission | 19–26 ms |

This demonstrates substantial host CPU cost in draw dispatch. Error polling
accounts for about 1.7% of elapsed time in these windows, so disabling it would
not explain the approximately ninefold terrain/sky FPS difference. CPU time is
close to dispatch wall time; these measurements do not show the dispatch calls
spending most of their time blocked. GPU execution, guest Java/Mesa CPU time,
fence retirement outside dispatch and scheduling are not measured here.

The capture does not contain explicit terrain/sky markers, so every reporting
window cannot be assigned to a camera direction. The last two windows contain
fewer draw calls and more submissions; they must not be treated as a controlled
before/after comparison of driver versions.

## Draw-state changes

`scripts/virgl/draw-state-overhead.patch` removes two sources of redundant GL
calls on the draw path:

- Cache the effective program binding by numeric ID and pipeline/program type.
  Consecutive draws using that binding skip repeated `glUseProgram` and
  `glBindProgramPipeline`. Every renderer make-current path invalidates the
  cache, including switches to/from the separate blitter context. Program
  destruction also invalidates it to handle deleted bindings and reused names.
  The separate fence-thread surfaceless context does not access this cache.
- Direct draws leave indirect argument bindings alone: direct OpenGL drawing
  commands do not read those buffers. Indirect draws still bind their validated
  argument resources every time, so transfers or earlier draws cannot leave
  stale bindings affecting them.

The host builder and normal launcher's prerequisites include the new patch.
A running QEMU retains its mapped library until restarted. No game settings,
world data, guest draw ordering or GL error checks were changed.

## Validation

The local renderer builds and its repeated build is idempotent. Source-backed
state tests exercise 180,000 repeated direct draws, program changes, program vs
pipeline IDs, unbinding, context/blitter switches, deletion/name reuse and
indirect draws following changed argument bindings. Removing context-switch
invalidation fails the regression. Existing vertex-format selection,
retired-binding and missing-buffer guard regressions pass.

The live host crash-guard test passes, with valid subsequent geometry and pixel
readback. A private VM with the new library passes 32 frames of 64 rebuilt
chunks, indexed quads, textures, depth/alpha and fog pixel checks. Local live
tests use TCG and host llvmpipe, since the execution environment has no KVM or
host GPU access. The user's Minecraft FPS improvement is not yet measured.

For an uncaptured FPS comparison, restart using `make run` and compare the same
position, view and settings after chunk loading settles. For a new profile,
preserve the baseline and write a different log:

```sh
AVORY_VIRGL_PROFILE=1 make run 2>build/virgl-profile-after.log
python3 scripts/summarize-virgl-profile.py build/virgl-profile-after.log
```

Host profiling is opt-in and reads no clocks when disabled. It reports
per-context two-second windows, thread CPU time, dispatch wall time, context
switching, error checks and the eight command types with the highest host API
time. Short-lived contexts flush remaining samples on destruction. Profiling
itself adds overhead; API timings include driver blocking and do not measure
GPU execution.
