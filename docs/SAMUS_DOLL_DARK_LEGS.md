# Dark inventory legs: candidate fix and on-device verification

## Leading hypothesis: non-invariant multipass depth

This is a code-supported hypothesis, **not a reproduced or confirmed fix**.

`CModelData::MultiLightingDrawCallback` (`src/MetroidPrime/CModelData.cpp:161`)
draws the same geometry three times: lights disabled with zero-alpha depth
flags, lights enabled with alpha blending, then additive blending. The latter
two passes compare depth but do not write it. `CCubeMaterial.cpp:493` selects
`GX_LEQUAL` for these flags. Lighting changes the generated shader variant.

`extern/aurora/lib/gx/shader.cpp:1026` emits the same model-view/projection
expressions, but its `VertexOutput.pos` previously had no `@invariant`.
WGSL does not otherwise guarantee identical position arithmetic across shader
variants. Compiler reassociation/contraction can therefore change depth enough
that a later lit pass fails against its own depth prepass. Resolution changes
alter sample locations and interpolated depth rounding; integer scales are not
inherently protected. This fits GPU/compiler specificity better than a blanket
claim that fractional EFB scale selects corrupt mips.

The candidate patch adds `@invariant` to the generated position output. It
preserves depth tests, texture data, sampler settings, and all scale choices.
WGSL section 12.10 explicitly permits its use on a shared vertex-output /
fragment-input structure (it has no effect on the fragment input).

`CSamusDoll.cpp:386-400` renders substituted body models and then a separate
boots model, each via MultiLightingDraw. Boots select a separate ANCS character
(`BuildSuitModelDataBoots`, line 198). Their geometry/materials can differ;
the C++ alone does **not** establish which texture IDs cover the dark pixels.
`VerifyCurrentShader` / `RemapMaterialData` select material sets without an EFB
scale branch. Do not assume the legs use CMPR or a particular mip chain.

## Decisive test on the affected 8060S

Keep the same save, suit, pose, camera, lighting and texture replacements.
Compare original and patched builds at 1x, 1.5x and 2x, restarting between
builds so shader modules/pipelines are rebuilt. Record the actual render and
logical viewport dimensions, not just the requested scale. Check Dawn errors.

1. Capture a bad original-build frame in RenderDoc. Use pixel history on a
   dark leg pixel to identify the depth-only, alpha-lit and additive events.
   Record their event IDs, shader IDs, depth compare/write state, vertex/index
   inputs, transform uniforms, and pre/post-VS positions. Check whether the
   lit fragments fail depth against the *same* surface's prepass. Inspect the
   bound textures and individual mip levels for these events and a good torso
   event. This identifies the actual leg resources without guessing assets.
2. Diagnostic only, on the original build: in
   `CModelData::MultiLightingDrawCallback`, change the two
   `.DepthCompareUpdate(true, false)` calls to
   `.DepthCompareUpdate(false, false)`. Do not ship this; it breaks occlusion.
   Restored leg color plus failed self-depth in pixel history strongly supports
   the depth hypothesis. Verify that the invariant patch restores color with
   the original depth tests. If depth still fails, compare the actual geometry
   and uniforms: invariance cannot fix differing inputs or overlapping models.
3. If the lit events pass depth but sample dark texels, test sampling instead,
   independently of step 2. In `TextureBind::get_descriptor`
   (`extern/aurora/lib/gfx/texture.cpp`), temporarily force the returned
   `.maxAnisotropy = 1`. Restart. If only this restores color and captured mips
   are valid, suspect the anisotropic sampler/driver path. Log final min/mag/mip
   filters, LOD clamps, anisotropy, `ref->hasArbitraryMips`, `ref->isReplacement`,
   format, dimensions and mip count for the captured leg texture. Arbitrary
   mips already force anisotropy to 1 at line 323; GX_ANISO_4 alone proves nothing.
4. With anisotropy still disabled, temporarily set both returned LOD clamps
   to 0, then to each valid integer mip index. Restart for each run. A bad
   integer level implicates its data; valid integer levels but bad trilinear
   interpolation implicate filtering/LOD (not necessarily corrupt data).
   At `new_static_texture_2d` immediately before the upload at line 137, log
   label, mip, dimensions, offset, bytesPerRow, heightBlocks and dataSize;
   hash/dump that exact CPU slice and compare it with GPU texture readback.
   CPU bad = loading/decoding; CPU good but GPU bad = upload/backend. If only
   implicit sampling fails, inspect generated UVs/derivatives and the bias
   from `shader_info.cpp::texture_size_bias` for the captured draw.

## Other findings and limits

- CMPR and GameCube RGBA8 are decoded on the CPU into RGBA8 here, not uploaded
  as native BC1. The upload loop uses per-mip dimensions and format block sizes;
  no fractional-EFB-dependent row-stride calculation was found.
- There is a separate mip-tail concern: `DolphinCTexture.cpp:85-91` and
  `:301-308` read mip sizes rounded to 4x4 regardless of GX format, whereas
  Aurora's CMPR decoder consumes complete 8x8 tiles (32 bytes). A 4x4 CMPR
  tail is counted as only 8 bytes by the reader. Inspect the actual TXTR
  layout before changing this: the asset may use a compact tail, or may never
  include such levels. This is not evidence that the observed leg draw samples
  such a tail. Do not silently change asset offsets as part of this patch.
- The existing replacement-only LOD-bias experiment is left unchanged. Zero
  explicit bias does not prevent fractional implicit LOD at any render scale.
- Local verification: C++ syntax-only compilation of `shader.cpp` passed
  using the configured include paths/defines. `ninja` is unavailable in this
  environment; no full build, generated-WGSL execution or GPU confirmation was
  performed. No executable was published and no commit was made.
