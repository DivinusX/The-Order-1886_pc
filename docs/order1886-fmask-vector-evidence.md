# FMask vector replacement evidence

The `9e0c` cold validation run in
`user/order1886/diagnostics/graphics-final-layer-mapping-validation` reached the
60-second watchdog. Its `cs_0x00000000804aeead_3.spv` contains the only invalid
vector extraction found among the run's 233 SPIR-V dumps:

```text
%2542 = OpConstantComposite <vec2> 0x76543210 0xfedcba98
%2545 = OpCompositeExtract <f32> %2542 2
```

Those words identify the existing FMask identity replacement. It produced two
components although both `ImageSampleRaw` and `ImageRead` declare `F32x4` results.
The invalid instruction is a producer type mismatch, independent of image
coordinate dimensionality.

The correction produces four components according to the descriptor's explicit
component selectors. R/G select the established low/high identity payload words;
Zero/One select their explicit integer values. Unestablished B/A payload selectors
remain unsupported and fail before module emission. No general extraction clamp
or guessed missing-channel value was added.

[Mesa's GFX6 FMask descriptor construction](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/418c4cfa6708a0e0b1175e72fb8fd27d3ca1615a/src/amd/common/ac_descriptors.c#262)
uses UINT format and explicitly selects X for all four output components. This
provides a concrete supported case in which Z must select the low payload word.

The captured log retains 16 shader-origin trace records for `0x804aeead`, but none
contains an FMask descriptor. The exact failing permutation's selector values
therefore remain unavailable. The fix does not establish that the game requests
only the supported selectors, or that the existing identity model implements all
FMask behavior.

The CPU regression compiles the exact production replacement body against a
four-component result contract and executes all-X, R/G/Zero/One, and explicit
unsupported-selector cases. The descriptor fixture also checks the shared
payload helper. FMask selectors participate in specialization keys so a changed
selector cannot reuse a stale constant replacement. Runtime validation remains
required after rebuilding with a new shader-binary cache version.

The integrated binary-11 runtime (`917a09ca...`) was subsequently tested. Its
cold run stops explicitly at selectors R=0 G=0 B=4 A=6; the blocking cache-reload
run and portable package stop at R=0 G=2 B=6 A=2. The background reload stops at
R=1 G=2 B=3 A=7. These values are captured by the new assertion; the failing raw
descriptor words and fetch ancestry still need capture. Unsupported B/A values
are not replaced by guessed constants. The invalid module is no longer emitted
on these runs because the unsupported descriptor fails earlier; this does not
establish complete FMask emulation. See `order1886-runtime-status.md` for run
identities and the remaining integer-filtering validation error.

## Issue #18 capture: the glyph-upload shader's descriptor is unbound

The v0.2.45 report (RTX 5080, 4K render width, DLSS, unlocked frame rate) stopped
after a cutscene in compute shader `0xa7b66f58` — the glyph-upload shader covered by
`order1886_image_mode.h` — with `Unsupported FMask payload selectors R=4 G=5 B=6 A=7`.
This run finally has the context the earlier evidence asked for:

- fetch: `load_mask=0xff`, offsets 44..51 (all eight words from user data),
  `flat_dwords=54`, `fetched=true`
- raw descriptor words:
  `00000000 3f800000 3e3851ec 94e00fac 4575db04 00000010 98b51e10 00000010`
- decoded with `AmdGpu::Image`: `Address()=0`, `base_address=0`, `type=9`,
  `data_format=56`, `num_format=15`, selectors Red/Green/Blue/Alpha.

The descriptor is a null sharp, not an FMask surface: the base address is zero and
the other fields are unrelated values (`0x3f800000` is `1.0f`). `Rasterizer::
BindTextures` already treats T# address 0 as a null image, so nothing in the guest
requests FMask coverage here; this is the inactive image path of that shader, whose
mode guard lives in `order1886_image_mode.h`.

The pass now separates the two states. An unbound sharp whose selectors the identity
model cannot resolve is replaced with a zero sample and keeps its FMask binding
contract, exactly as a null texture sample reads zero. A bound descriptor with
unestablished B/A selectors still records the failure context and asserts before
module emission — no blanket return-zero and no guessed payload words. The CPU
regression now compiles the production body for bound R/G/Zero/One, bound B/A
(rejected) and unbound identity selectors (zero).

