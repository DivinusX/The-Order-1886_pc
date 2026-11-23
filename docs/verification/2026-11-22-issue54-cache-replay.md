# Issue #54 — a rejected cached pipeline stopped the second boot

Date: 2026-11-22. Branch: `fix/issue-54-cache-runtime`.

The report
(https://github.com/DivinusX/a-port-of-the-order-1886-thats-actually-alive/issues/54)
ran v0.2.44 and v0.2.45 on Windows: the first session played, every later session
died while starting with

```
GPU [Debug] vk_graphics_pipeline.cpp:512 GraphicsPipeline: Assertion Failed!
GPU log: Failed to create graphics pipeline: ErrorInitializationFailed
STOP: GPU library assertion failed (see GPU log above)
```

and clearing the shader cache made it play again. The reporter's guess was that the
new version needs the cache cleared. It does not: the cache is allowed to contain a
pipeline the current driver refuses.

## Cause

`PipelineCache::WarmUp` replays the pipeline keys recorded by earlier sessions before
the first frame. A pipeline built during live play can carry guest state that differs
from what the serialized key can describe (render targets, stage specialization,
driver-specific limits). The same build can therefore write a key that its next boot
cannot rebuild, and drivers differ in what they accept: NVIDIA returns
`VK_ERROR_INITIALIZATION_FAILED`, RADV can even fault inside `libvulkan_radeon.so`.

The `GraphicsPipeline` constructor treated that replay result as fatal
(`ASSERT_MSG(... eSuccess ...)`), so one unusable entry aborted the whole boot. The
previous work-around — clear the cache — also threw away every good pipeline.

## Fix

- A cached graphics pipeline that fails to build during preload is skipped and left
  unpublished. Any non-success result counts, not only `ERROR_INITIALIZATION_FAILED`.
- The same rule applies to cached compute pipelines (they had the identical assertion).
- Every caller checks `Handle()` before publishing stages, so a skipped entry never
  becomes a null pipeline in `graphics_pipelines` / `compute_pipelines`.
- The key stays in the store; the normal draw path rebuilds that pipeline from live
  guest state and rewrites the entry, so the cache heals itself.
- The end-of-warm-up log says how many entries were skipped instead of asking the
  player to regenerate the cache.

Per-build cache isolation and the launcher's exit-23 quarantine (commit `0cf3c0a`)
stay as a second line of defence: they act on a whole cache only when a native crash
still happens, not on the reported rejected-pipeline path.

## Verification

Headless (gamescope, RADV GFX1201, CUSA00785 01.02) with the real 37-pipeline cache
from the v0.2.45 test state (`out/cache-update.WRp0GH`), copied to a scratch
`ORDER1886_GPU_USER_DIR` so the store could be controlled:

| Run | Result |
| --- | --- |
| Control, unaffected build | `Pipeline warm-up: 37 of 37 pipelines in 0.00 s` |
| Every graphics replay forced to `ERROR_INITIALIZATION_FAILED` (temporary verification hook in the preload path, removed before commit) | 24 graphics skipped with `Skipping a cached graphics pipeline the driver rejected (ErrorInitializationFailed)`, 13 compute replayed, warm-up completed, `GPU: window and Vulkan presenter ready`, frames presented |

The fault injection used the same constructor branch the reporter's driver reaches;
the pre-fix binary aborts there. The hook only forced the result code and was not
committed.

Checks:

- `pipeline-binary-cache-test`, `async-pipeline-cache-test`, `order-spirv-test` pass.
- `python3 -m unittest discover -s tests -p "test_order1886*.py"`: 505 tests, only
  the two errors from the uncommitted issue #53 display work.

## What players see

Nothing. The second boot after an update plays; no cache needs to be cleared, and the
first pipeline that needed rebuilding does it on demand.
