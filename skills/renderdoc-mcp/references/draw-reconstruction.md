# Exact Draw Reconstruction

Use this workflow to turn a captured draw into a standalone D3D12 replay, to
extract exact GPU inputs, or to prepare a reconstruction package for another
agent.

## Contents

- [Definition of done](#definition-of-done)
- [Workflow](#workflow)
- [Choose event timing](#choose-event-timing)
- [Export a reconstruction bundle](#export-a-reconstruction-bundle)
- [Audit bundle coverage](#audit-bundle-coverage)
- [Use individual tools](#use-individual-tools)
- [Preflight shaders and the D3D12 runtime](#preflight-shaders-and-the-d3d12-runtime)
- [Build the standalone replay](#build-the-standalone-replay)
- [Upload buffers](#upload-buffers)
- [Upload textures](#upload-textures)
- [Handle typeless and depth-stencil resources](#handle-typeless-and-depth-stencil-resources)
- [Recreate root state and descriptors](#recreate-root-state-and-descriptors)
- [Initialize outputs from the pre-draw state](#initialize-outputs-from-the-pre-draw-state)
- [Reproduce the draw](#reproduce-the-draw)
- [Decide whether mesh export is needed](#decide-whether-mesh-export-is-needed)
- [Verify the result](#verify-the-result)
- [Triage mismatches](#triage-mismatches)
- [Troubleshoot exports and replay](#troubleshoot-exports-and-replay)

## Definition of done

Do not stop at bundle export. Treat `manifest.json.complete=true` as export
completeness only.

Finish reconstruction when:

1. the standalone program creates the device, root signature, resources, PSO,
   and command list without unexplained debug-layer errors;
2. it issues the captured draw with the captured input and output pre-state;
3. it reads back every output needed for validation; and
4. the readback matches `outputs/post` byte-for-byte, or a format-aware
   comparison meets a stated tolerance and explains the remaining difference.

## Workflow

Use task-local names instead of copying identifiers from another capture:

```text
CAPTURE_PATH
TARGET_EVENT_ID
PRE_EVENT_ID
BUNDLE_DIR
REPLAY_DIR
```

Follow this sequence:

```text
open_capture(CAPTURE_PATH)
  -> get_draw_info(TARGET_EVENT_ID)
  -> inspect bound shader reflection
  -> choose PRE_EVENT_ID
  -> get_descriptor_bindings(TARGET_EVENT_ID)
     + get_d3d12_pipeline_state_full(TARGET_EVENT_ID) in parallel
  -> export_draw_reconstruction_bundle
  -> audit manifest, files, state timing, and unsupported features
  -> preflight Shader Model and D3D12 runtime
  -> build and run the standalone replay
  -> read back outputs
  -> compare with outputs/post
```

Do not call `get_stats` or enumerate the whole pass list for a known event
unless they help diagnose a failure.

## Choose event timing

- Set `eventId` to the actual draw event. Use its decoded
  `ActionDescription` for vertex/index count, instance count, offsets, outputs,
  and draw flags.
- Set `preEventId` to an event lower than `eventId` that represents input and
  output contents immediately before the draw.
- For a flattened `ExecuteIndirect` draw, normally use the wrapper event
  immediately before its child draw as `preEventId`.
- Omit `preEventId` only when selecting the greatest prior action event is
  sufficient.
- Read `manifest.json.stateTiming`: pipeline and bindings come from the draw
  event, input bytes come from immediately after the pre-event, and reference
  outputs come from immediately after the draw event.
- Re-check timing when a resource ID is invalid at the pre-event or when an
  exported GPU-written resource changes across nearby events.

## Export a reconstruction bundle

Call `export_draw_reconstruction_bundle` with:

| Parameter | Value |
|-----------|-------|
| `eventId` | `TARGET_EVENT_ID` |
| `preEventId` | `PRE_EVENT_ID` |
| `outputDir` | `BUNDLE_DIR` |

Use a new or empty `outputDir`. The exporter rejects a non-empty directory so
stale files cannot enter the manifest.

Use the exported files as follows:

| Path | Reconstruction data |
|------|---------------------|
| `draw.json` | Decoded draw arguments and direct-call flattening guidance for indirect draws |
| `pipeline_d3d12.json` | PSO ID, shaders, root arguments, descriptor heaps, IA, rasterizer, viewport/scissor, blend, depth/stencil, RTV/DSV descriptors, predication, and relevant resource states |
| `bindings.json` | Reflection bindings resolved to descriptor stores, descriptor offsets, resources/views, ranges, and samplers |
| `root_signature.json` | Current root state and either serialized root-signature bytes or decoded `UnpackedSignature` |
| `shaders/` | Original captured shader containers |
| `resources/buffers/` | Bound CBV/SRV/UAV ranges |
| `resources/root_constants/` | D3D12 root-constant bytes when present |
| `resources/textures/` | Raw mip/slice/sample bytes and layout metadata |
| `outputs/pre/` | Render/depth output contents before the draw |
| `outputs/post/` | Reference output contents after the draw |
| `checksums.json` | FNV-1a64 checksums for exported artifacts |
| `manifest.json` | File inventory, state timing, export results, errors, and `complete` |

Require `manifest.complete=true`, an empty `errors` array, existing files, and
matching sizes/checksums before implementing replay.

## Audit bundle coverage

The exporter and `get_d3d12_pipeline_state_full` are D3D12-only. The bundle
contains descriptor-bound resources, shaders, state, and outputs, but is not
unconditionally self-contained for every D3D12 pipeline.

Before claiming replay completeness, detect:

- raw IA vertex/index buffers required by user vertex attributes or indexed
  draws;
- indirect command signatures and argument/count buffers when exact command
  stream behavior matters;
- root constants, CBVs, SRVs, or UAVs written by an indirect command;
- mesh/task shaders, ray tracing, acceleration structures, or other specialized
  pipelines;
- MSAA inputs or outputs that cannot be initialized with an ordinary
  buffer-to-texture copy;
- predication or queries whose behavior must be reproduced rather than
  flattened;
- aliased resources or transitions involving live resources omitted by the
  default referenced-resource state list.

Use `export_mesh` for validation, not as a universal substitute for exact raw IA
buffers. If required data is not exported by the MCP surface, state the missing
artifact explicitly instead of inventing it.

## Use individual tools

Use individual tools to inspect one component or repeat one failed export.

### Export an original shader container

Call `export_shader_binary` with `TARGET_EVENT_ID`, a bound stage (`vs`, `hs`,
`ds`, `gs`, `ps`, or `cs`), and an output directory.

Preserve the returned encoding, entry point, byte size, and checksum. Treat a
`DXBC` file header as a container signature; a DXIL shader commonly uses a DXBC
container.

### Resolve descriptors to resources

Call `get_descriptor_bindings` with `TARGET_EVENT_ID`.

Reconstruct from the returned descriptor store, descriptor byte offset,
resource/view ID, view format, buffer offset/size, mip/slice range, and sampler
state. Do not infer the actual resource from register numbers alone.

### Export exact texture bytes

Call `export_texture_raw` with:

| Parameter | Value |
|-----------|-------|
| `eventId` | `PRE_EVENT_ID` for pre-draw input contents |
| `resourceId` | the resource resolved from `bindings.json` |
| `outputDir` | a new texture directory |
| `allSubresources` | `true`, unless a proven subset is sufficient |

For one subresource, set `allSubresources=false` and pass `mip`, `slice`, and
`sample`.

Read `metadata.json` for format, dimensions, array/cubemap/MSAA properties,
per-subresource byte length, row or block-row pitch when derivable, slice
pitch, and checksum. Treat each `.bin` as the tightly packed result of RenderDoc
`GetTextureData`, not a PNG/DDS file or an API upload buffer with row-alignment
padding.

### Export one bound buffer

Call `export_bound_buffer` with `PRE_EVENT_ID`, a shader stage,
`bindingKind`, reflection-array `bindingIndex`, optional `arrayElement`, and an
output directory.

Use `constantBuffer`, `readOnlyResource`, or `readWriteResource` for
`bindingKind`. Treat `bindingIndex` as the index in the corresponding shader
reflection array, not necessarily the shader register. The tool writes the
resolved descriptor byte range or D3D12 root-constant bytes.

For a GPU-written list or buffer, determine the elements actually accessed by
the draw and shader. Ignore checksum differences confined to unread or
undefined tail elements.

### Export full D3D12 state

Call `get_d3d12_pipeline_state_full` with `TARGET_EVENT_ID` and normally keep
`includeAllResourceStates=false`. Set it to true only when diagnosing
transitions, aliasing, or predication involving otherwise unrelated live
resources.

## Preflight shaders and the D3D12 runtime

Use the original captured shader containers. Do not recompile or modify them to
hide a runtime incompatibility.

Before PSO creation:

1. Determine each shader's profile and Shader Model from exported metadata,
   disassembly, or D3D12 debug diagnostics.
2. Check whether the OS D3D12 runtime accepts that Shader Model.
3. When required, locate a compatible DirectX Agility SDK. Prefer the version
   used by the captured application when it is available.
4. Place the matching `D3D12Core.dll` in the replay deployment and export
   `D3D12SDKVersion` and `D3D12SDKPath` from the executable.
5. Enable the D3D12 debug layer in diagnostic builds.
6. Query and print `ID3D12InfoQueue` messages when device, root-signature, PSO,
   resource, or command-list creation fails.

Treat `CreateGraphicsPipelineState -> E_INVALIDARG` plus a shader-version
message as a runtime preflight failure, not evidence that captured PSO fields
should be guessed or changed.

## Build the standalone replay

Implement the smallest program that preserves observable draw behavior:

1. Select the D3D12 runtime and create the device, queue, allocator, and command
   list.
2. Load the captured shader containers as opaque bytes.
3. Create resources with compatible resource formats, dimensions, sample
   counts, flags, and initial states.
4. Upload input resources and `outputs/pre`.
5. Recreate descriptor heaps, descriptors, root signature, and root arguments.
6. Recreate the PSO, viewport, scissor, topology, blend constants, stencil
   reference, and render/depth attachments.
7. Transition resources into the states recorded for the draw.
8. Issue the direct draw or reproduce the original indirect behavior.
9. Transition outputs for readback, copy them to readback buffers, wait for GPU
   completion, and write deterministic output files.

Preserve source bundle files. Generate derived upload buffers or converted
resources in a separate replay directory so provenance remains auditable.

## Upload buffers

- Copy only the exported byte range at the captured logical offset.
- Treat `reflectedByteSize` as shader-visible CB data.
- Allocate and describe a CBV using `d3d12CbvSize`, aligned to 256 bytes.
- Copy the reflected bytes and zero-fill remaining CBV padding.
- Preserve structured/raw view stride, first element, element count, format,
  and SRV/UAV flags.
- Preserve root CBV/SRV/UAV GPU virtual-address offsets.
- Compare only shader-accessed bytes when exported GPU-written tails are
  undefined.

## Upload textures

Treat exported raw texture files as tight data. D3D12 buffer-to-texture copies
require placed footprints whose rows may be padded.

For every required subresource:

1. Create the destination texture using a resource format compatible with the
   captured view format.
2. Call `GetCopyableFootprints` for the destination resource.
3. Determine tight source rows or BC block rows from format, dimensions, and
   metadata.
4. Copy each tight source row into the corresponding padded upload-footprint
   row; do not copy the file as one opaque padded allocation.
5. Use D3D12 subresource ordering:

   ```text
   subresource =
       mip
       + arraySlice * mipCount
       + planeSlice * mipCount * arraySize
   ```

6. Treat cubemaps as six array slices per cube.
7. Issue `CopyTextureRegion` with the placed footprint.
8. Transition from `COPY_DEST` to the shader-visible captured state.

Do not assume `byteLength` equals the D3D12 upload allocation size. Distinguish
texel rows from BC block rows, resource format from view format, array slices
from depth slices, and ordinary color data from depth/stencil planes.

Do not initialize an MSAA texture through a normal buffer-to-MSAA copy. Recreate
or resolve the observable sample data with an API-valid path, or record MSAA
initialization as an unsupported requirement.

## Handle typeless and depth-stencil resources

Preserve typeless resource formats when multiple typed views or depth-target
behavior are observable.

RenderDoc may export a packed depth/stencil representation that cannot be copied
directly into D3D12 depth and stencil planes.

- If the shader observes only depth, extract equivalent depth values and use a
  compatible depth-plane SRV representation such as `R32_FLOAT`.
- If the shader observes stencil, plane-specific loads, atomics, or depth-target
  behavior, recreate the typeless resource and upload each required plane
  correctly.
- Do not use a semantic substitution merely because it is convenient.
- Prove any substitution by comparing final output against `outputs/post`.

## Recreate root state and descriptors

### Root signatures

1. Use the exported root-signature `.bin` when
   `serializedBlob.available=true`.
2. Otherwise require `serializedBlob.semanticDescriptionAvailable=true`.
3. Rebuild and serialize an equivalent root signature from
   `serializedBlob.decodedSignature`.

Preserve root-signature version and flags, parameter order and types, shader
visibility, descriptor range flags, base registers, register spaces, explicit
and append offsets, root descriptor flags, and static samplers.

### Descriptor tables

- Create descriptor heaps large enough for exported table ranges.
- Preserve logical descriptor positions; do not compact used entries.
- Preserve table range offsets, append semantics, register spaces, flags, and
  shader visibility.
- Create SRV/UAV/CBV/sampler/RTV/DSV views with the captured view format and
  range, not only the underlying resource format.
- Bind the correct table-base GPU handle and root parameter index.
- Restore all child root constants and root descriptors before a flattened
  indirect draw.

## Initialize outputs from the pre-draw state

Treat `outputs/pre` as draw input whenever blending, channel write masks, depth
or stencil testing, partial coverage, load operations, or read-modify-write
behavior can preserve previous contents.

Upload every relevant pre-output subresource before the draw. Do not clear an
attachment unless the exported pre-state is known to equal that clear value or
the draw provably overwrites every relevant texel, sample, and channel.

Match resource/view formats, dimensions, sample count, subresources, and initial
states. A visually plausible clear is not an exact substitute.

## Reproduce the draw

Restore:

- PSO and root signature;
- descriptor heaps and tables;
- root constants and root descriptors;
- IA topology, index format, vertex/index buffer views when used;
- viewport and scissor;
- render targets, depth/stencil view, blend factor, stencil reference, and
  primitive topology;
- resource states required at draw time;
- vertex/index count, instance count, start offsets, base vertex, and start
  instance.

Flatten an indirect child to `DrawInstanced` or `DrawIndexedInstanced` only
after applying the complete root, descriptor, and draw state captured at the
child event. If exact command-signature behavior matters, recreate the command
signature and argument/count buffers instead.

## Decide whether mesh export is needed

Ignore stale vertex/index-buffer bindings when the draw and shader do not use
them.

Skip mesh export when all conditions hold:

- the draw is non-indexed;
- shader reflection contains no user vertex attributes; and
- the vertex shader generates geometry from system values such as
  `SV_VertexID` and `SV_InstanceID`.

Preserve vertex/instance counts and export every buffer explicitly read by the
vertex shader. Use `export_mesh` only as a validation artifact in this case.

Export exact raw IA ranges when the draw is indexed or reflection shows user
vertex inputs. Do not assume post-transform OBJ/JSON data can replace raw IA
data in an exact replay.

## Verify the result

Validate in this order:

1. Compare raw output byte length and bytes.
2. Compare a cryptographic hash such as SHA-256.
3. Decode the captured format and report maximum/mean error and differing
   texel/channel counts.
4. Produce a visual image and difference heatmap when they help interpretation.

Use raw or format-aware comparison as authoritative. PNG-only comparison is
insufficient for HDR, integer, depth, typeless, compressed, or multisampled
resources.

Record:

- reference and replay file paths;
- format, dimensions, subresource, and row-pitch interpretation;
- hashes;
- exact-match result or numeric tolerance;
- differing byte/texel counts and maximum error;
- every semantic resource substitution.

## Triage mismatches

Investigate in this order:

1. **PSO creation fails**
   - print `ID3D12InfoQueue`;
   - check Shader Model and Agility runtime;
   - check root-signature compatibility, RT/DS formats, sample count, and
     topology.
2. **Output is unchanged or black**
   - verify `outputs/pre` upload and transitions;
   - verify descriptor-table base handles and root GPU addresses;
   - verify viewport/scissor and draw arguments.
3. **Large global mismatch**
   - check resource/view formats, CB offsets, root parameter order,
     blend/depth/stencil state, and missing resources.
4. **Localized mismatch**
   - check mip/slice/cube/plane order, sampler state, row pitch, and pre-event
     timing.
5. **Small numeric mismatch**
   - use format-aware metrics;
   - check runtime/driver precision and semantic substitutions.
6. **Still unresolved**
   - capture the standalone replay with RenderDoc and compare its draw against
     the original capture.

## Troubleshoot exports and replay

| Problem | Action |
|---------|--------|
| Output directory is not empty | Choose a new directory or explicitly empty the intended directory outside the MCP tool |
| Bundle reports `complete=false` | Inspect `errors` and missing manifest paths before writing replay code |
| Root-signature blob is unavailable | Rebuild from `decodedSignature` when `semanticDescriptionAvailable=true` |
| Raw row pitch is null | Apply format-specific block/plane rules using dimensions and `byteLength` |
| GPU-buffer checksums vary | Compare only the shader-accessed range before diagnosing replay non-determinism |
| A shader stage export fails | Confirm the stage is bound at `TARGET_EVENT_ID` |
| A resource ID is invalid at the pre-event | Re-check descriptor timing and select the correct `PRE_EVENT_ID` |
| Pre/post outputs are identical | Confirm the draw writes that attachment and consider blending, masks, culling, and discarded fragments |
| `CreateGraphicsPipelineState` returns `E_INVALIDARG` | Print `ID3D12InfoQueue`; check Shader Model/runtime, root signature, output formats, sample count, and topology |
| Texture appears scrambled | Check tight-row to placed-footprint copying, BC block rows, and subresource order |
| Colors are plausible but not exact | Check `outputs/pre`, blend state, view formats, sampler state, and semantic substitutions |
| Required IA/indirect/specialized data is absent | Report the missing artifact and use a dedicated exporter before calling the replay complete |
