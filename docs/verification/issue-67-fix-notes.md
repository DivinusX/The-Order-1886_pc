# Issue #67: carpet/window flicker and cutscene stutter after v0.2.56

## Measured cause (Windows 11, RTX 5050 Laptop, v0.2.56 runtime, 2026-11-25)

The v0.2.56 face-spot fix (#27) added a slope-scaled redraw depth bias, read from
`ORDER1886_REDRAW_SLOPE_BIAS`. Tested in the carpet room and on face close-ups by starting
the runtime directly (the launcher strips inherited `ORDER1886_*` variables, see
`runtime_process_environment` / `DIAGNOSTIC_ENVIRONMENT` in `order1886.py`):

| `ORDER1886_REDRAW_SLOPE_BIAS` | Carpet | Faces |
|---|---|---|
| v0.2.56 default | whole carpet pops in and out (floor drawn over it) | clean |
| 0 | stays | black spots (#27) back |
| **0.25** | **stays** | **clean** |

## Recommended fix

**In the real v0.2.56 source (`7202b31`, not on GitHub): change the default of
`ORDER1886_REDRAW_SLOPE_BIAS` to 0.25.** That is exactly the configuration tested above,
in that build's own units.

This tree (`release/v0.2.49` + the two fixes re-implemented) is the fallback if that source
is not at hand. Its slope term is the Vulkan `depthBiasSlopeFactor` with default 0.25; it
matches the tested build only if v0.2.56 also passes the value straight to Vulkan. Re-check
the carpet room and a face close-up after building it.

**Status of this tree: not compiled, not run.**

## Changes in this tree

| File | Change |
|---|---|
| `vk_rasterizer.cpp` (`RefreshDepthState`) | Redraw bias gets a slope term, default 0.25; optional clamp `ORDER1886_REDRAW_BIAS_CLAMP` (default 0 = off). |
| `vk_instance.h` | `IsDepthBiasClampSupported()` for the optional clamp. |
| `vk_async_compiler.h` | `AsyncJob::WaitFor(timeout)`. |
| `vk_pipeline_cache.{h,cpp}` | #61 re-implemented as a bounded wait: token bucket, at most 6 ms at once, refilled at 15 % of elapsed time; not ready in time = skipped as in v0.2.49. |

## Knobs

| Variable | Default | Meaning |
|---|---|---|
| `ORDER1886_REDRAW_DEPTH_BIAS` | 16 | constant redraw bias, 0 = whole redraw bias off |
| `ORDER1886_REDRAW_SLOPE_BIAS` | 0.25 | slope factor, 0 = v0.2.49 behaviour |
| `ORDER1886_REDRAW_BIAS_CLAMP` | 0 | cap on the total bias (depth-range units), 0 = none |
| `ORDER1886_SHADER_WAIT_MS` | 6 | max wait at once for a compile, 0 = no wait |
| `ORDER1886_SHADER_WAIT_SHARE` | 0.15 | share of elapsed time waiting may use |

## Still open

- Cutscene stutter: not A/B tested yet. v0.2.56 reads `ORDER1886_COMPILE_WAIT_MS`; run
  `Test issue 67.ps1 -Mode 3` (wait off) against mode 1 on a fresh shader cache.
