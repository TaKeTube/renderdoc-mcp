---
name: renderdoc-mcp
description: Analyze and reconstruct RenderDoc GPU frame captures with renderdoc-mcp MCP tools. Use when Codex needs to inspect .rdc captures, diagnose black screens or visual artifacts, explain frame structure, inspect or recreate specific draw calls, extract exact shader/resource data, or investigate GPU rendering and performance issues.
---

# RenderDoc MCP

Use renderdoc-mcp to analyze GPU frame captures and debug rendering problems.

Always use the MCP server named `renderdoc-mcp` for tool calls.

When you need shell-based or batch workflows outside the MCP tool surface, use `renderdoc-cli` from `PATH`.

## Analysis Framework

Every analysis task follows this flow:

```text
1. Understand goal -> what does the user want to know?
2. Route -> pick the workflow before broad data collection
3. Open -> load or capture the frame
4. Gather -> collect only the context required by that workflow
5. Execute -> drill down with verification at each step
6. Summarize -> present findings with evidence
```

## Phase 1: Route and Open

Choose a workflow from the user's goal:

| User goal | Workflow |
|-----------|----------|
| "Screen is black" or "nothing renders" | Black Screen Diagnosis |
| "Colors are wrong" or "there are artifacts" | Visual Artifact Diagnosis |
| "Performance is bad" or "too slow" | Performance Analysis |
| "Explain what this frame does" | Frame Walkthrough |
| "Debug this specific draw call" | Targeted Draw Inspection |
| "Recreate this draw" or "extract exact draw resources" | Draw Reconstruction |
| "Compare two captures" or "what changed between frames" | Frame Regression Diagnosis |
| General or unclear request | Ask the user what they want to investigate |

### Opening a Capture

From file: call `open_capture` with the `.rdc` path.

From app: call `capture_frame` to launch the app, inject RenderDoc, capture a frame, and auto-open it.

Verification: check the returned event count. If it is `0`, the capture is empty and you should report that immediately.

Error recovery:
- If `open_capture` fails, verify the path exists and points to a valid `.rdc` file.
- If `capture_frame` fails, check the executable path, whether the app needs admin privileges, whether it exits immediately, and whether `delayFrames` should be increased.

## Phase 2: Gather Route-Specific Context

For a general, frame-wide, visual, or performance investigation, call the
following independent tools in parallel:

| Tool | What it tells you |
|------|-------------------|
| `get_capture_info` | API, GPU, driver, event count |
| `get_stats` | Per-pass draw and triangle counts, top draws, largest resources |
| `get_log` | Validation errors and debug messages; check HIGH severity first |
| `list_passes` | Frame structure: pass names and draw counts |

For a known event or reconstruction target, do not gather global statistics or
the complete pass list by default. Start with the target event and the tools
listed by its workflow. Use `get_stats`, `get_log`, or `list_passes` later only
when they help identify the target or diagnose a failure.

For frame-wide gathering, summarize:
- Which graphics API is in use?
- How many passes and draws are present?
- Are there any HIGH-severity validation errors?
- Which passes or draws look most expensive?

Use that summary as the working context for the rest of the analysis.

## Diagnostic Workflows

### Black Screen Diagnosis

```text
list_draws
  draws = 0?
    -> No geometry submitted. Check:
       - list_events for Clear or Dispatch events
       - get_log for pipeline creation or binding errors
       - report "No draw calls found" with likely causes
  draws > 0?
    -> goto_event for the last draw and get_pipeline_state in parallel
       no render target bound?
         -> report that output goes nowhere
       render target bound?
         -> export_render_target
            render target has content?
              -> likely a present or swapchain issue; inspect Present-related events
            render target is black?
              -> inspect bindings and shaders:
                 - get_bindings
                 - get_shader ps
                 - get_shader vs
```

Parallel opportunity: `goto_event` and `get_pipeline_state` can run in parallel when they target the same `eventId`.

### Visual Artifact Diagnosis

```text
Identify the problematic draw, either from the user or by exporting render targets
  -> goto_event for that draw
  -> get_pipeline_state and get_bindings in parallel
     - inspect blend state
     - inspect render target format
     - inspect bound textures
     - export suspicious textures when needed
     - inspect shaders:
       - get_shader ps mode=disasm
       - get_shader ps mode=reflect
       - search_shaders if you need similar shader matches
```

If multiple draws look suspicious, show the candidate event IDs and names, export their render targets, and ask the user which one looks wrong.

### Performance Analysis

```text
Start from get_stats
  -> inspect top draws by triangle count
  -> goto_event and get_draw_info for heavy draws
  -> inspect pipeline complexity with get_pipeline_state
  -> inspect shader reflection with get_shader vs/ps mode=reflect
  -> inspect oversized resources with get_resource_info
  -> inspect the heaviest pass with get_pass_info
  -> look for redundant draws with similar shaders and resources
```

Report issues by impact. For each one, state what it is, where it occurs, how severe it is, and what the likely improvement is.

### Frame Walkthrough

```text
list_passes
  -> for each important pass:
     - get_pass_info
     - goto_event for the first draw and get_pipeline_state in parallel
     - describe the pass inputs, shaders, and outputs
     - export_render_target to show the pass result
  -> end with a narrative from start to finish
```

Parallel opportunity: when passes are independent analysis tasks, inspect two or three in parallel.

### Pixel-Level Diagnosis

When investigating why a pixel has the wrong color or is missing:

1. **pick_pixel** — Read the current pixel color to confirm the issue
2. **pixel_history** — Find which draws modified this pixel, check if any were culled/discarded
3. **debug_pixel** — Trace the fragment shader execution to find where the wrong value comes from
4. **get_texture_stats** — Check if input textures have unexpected ranges (NaN, all-zero, etc.)

### Shader Debugging

When a draw produces wrong output:

1. **debug_vertex** / **debug_pixel** — Trace shader execution with mode="summary" first
2. If inputs look wrong, check bindings with **get_bindings**
3. If logic seems wrong, re-run with mode="trace" for step-by-step execution

### Frame Regression Diagnosis

When comparing two captures to find rendering differences:

1. `diff_open` captureA captureB → Load both captures
2. `diff_summary` → Quick overview: any differences? Check `divergedAt` field
3. `diff_draws` → Which draws changed/added/removed?
4. `diff_pipeline "MarkerPath"` → What pipeline state changed at that draw?
5. `diff_framebuffer` with `diffOutput` → Pixel-level visual comparison
6. `diff_close` → Clean up

### Targeted Draw Inspection

When the user specifies an event ID or draw name:

```text
goto_event + get_pipeline_state + get_bindings in parallel
  -> describe:
     - vertex shader with get_shader vs mode=reflect
     - pixel shader with get_shader ps mode=reflect
     - bound textures from bindings
     - render targets from pipeline state
     - viewport from pipeline state
  -> go deeper when needed:
     - get_shader vs/ps mode=disasm
     - export_render_target
     - export_texture
     - get_draw_info
```

### Draw Reconstruction

Before extracting exact resources or recreating a draw in a standalone program,
read [references/draw-reconstruction.md](references/draw-reconstruction.md)
completely. It documents the six reconstruction tools, exact parameter
semantics, bundle limits, runtime preflight, standalone replay, upload rules,
output initialization, comparison, and mismatch triage.

Use this core workflow:

```text
open_capture
  -> get_draw_info for the target event
  -> choose the pre-draw event
  -> get_shader reflection + get_descriptor_bindings
     + get_d3d12_pipeline_state_full in parallel
  -> export_draw_reconstruction_bundle
  -> audit export completeness and unsupported pipeline features
  -> preflight Shader Model and D3D12 runtime compatibility
  -> build and run the standalone replay
  -> read back and compare against outputs/post
```

Key rules:

- Use the actual child draw as `eventId`, not an `ExecuteIndirect` wrapper.
- Use the event representing resource contents immediately before the draw as
  `preEventId`; pass it explicitly when exact timing matters.
- Use a new or empty bundle output directory.
- Treat pipeline/bindings as draw-event state, inputs as pre-event contents,
  and post outputs as references.
- Prefer the one-call bundle for handoff; use individual tools to inspect or
  repeat one export.
- Treat `manifest.complete=true` as export completeness, not proof that a
  standalone replay is complete or correct.
- Recreate descriptor-table layout and the complete child root state; do not
  compact descriptors or flatten an indirect draw before restoring that state.
- Initialize every dependent output from `outputs/pre`; do not substitute a
  clear unless the draw provably overwrites every relevant texel and channel.
- Do not export mesh data for a non-indexed draw whose vertex shader generates
  geometry solely from system values such as `SV_VertexID` and
  `SV_InstanceID`.
- Finish only after output readback matches `outputs/post` byte-for-byte, or a
  format-aware comparison meets a stated tolerance and explains the remaining
  difference.

## Verification Checkpoints

Apply these checks throughout the analysis:

| After this step | Verify |
|----------------|--------|
| `open_capture` or `capture_frame` | Event count is greater than 0 |
| `get_log` | HIGH severity messages are investigated first |
| `list_draws` | Draw count matches expectations |
| `get_pipeline_state` | Shaders are bound and a render target exists |
| `get_bindings` | Expected resources are bound and not null |
| `get_shader` returns empty | The stage may not be bound at this event; try a different stage or event |
| `export_render_target` | The image is not unexpectedly all black or all white |
| `export_shader_binary` | Byte size is non-zero and the checksum/container encoding are recorded |
| `export_texture_raw` | Every requested subresource and its metadata/checksum exist |
| `export_bound_buffer` | The range matches the descriptor; constant buffers use reflected size and aligned CBV size |
| `export_draw_reconstruction_bundle` | `complete` is true, `errors` is empty, every manifest file exists, and unsupported replay requirements are identified |
| Standalone draw replay | Device/PSO creation has no unexplained debug errors; output is read back; exact or format-aware comparison against `outputs/post` is recorded |
| Each phase | Summarize what was found, ruled out, and what comes next |

## Error Recovery

| Error | Recovery |
|-------|----------|
| `open_capture` file not found | Verify the path and ask for the correct file if needed |
| `open_capture` invalid file | The file may be corrupted or not be an `.rdc`; ask for a new capture |
| `capture_frame` app exits immediately | Check `cmdLine`, `workingDir`, and startup requirements |
| `capture_frame` no frame captured | Increase `delayFrames` and verify the app actually renders to a window |
| `get_shader` empty result | No shader is bound for that stage at this event; try another stage or event |
| `get_pipeline_state` no render target | Some draws do not output to render targets; inspect draw flags |
| `export_render_target` index out of range | Check how many render targets are bound and use a valid index from `0` to `7` |
| `get_resource_info` invalid `ResourceId` | Call `list_resources` first to find a valid ID |
| Reconstruction output directory is not empty | Choose a new directory or explicitly empty the intended directory outside the MCP tool |
| Root-signature blob is unavailable | Rebuild from `serializedBlob.decodedSignature` when `semanticDescriptionAvailable` is true |
| Raw texture row pitch is null | Apply format-specific block/plane rules using the recorded dimensions and `byteLength` |
| Repeated GPU-buffer exports differ only in unread tail data | Compare the shader-accessed range before treating replay as non-deterministic |
| `CreateGraphicsPipelineState` returns `E_INVALIDARG` | Print `ID3D12InfoQueue` messages first; check Shader Model/runtime compatibility, root-signature compatibility, output formats, sample count, and topology |
| Standalone output is unchanged or black | Verify `outputs/pre` upload, transitions, descriptor-table bases, root addresses, viewport/scissor, and draw arguments |
| Any tool says no capture is open | Call `open_capture` first |

## When to Ask the User

Ask before proceeding when:
- Multiple draw calls could be the source of the problem.
- The analysis is ambiguous and you need the user to confirm the most likely hypothesis.
- The user's goal is still unclear after initial context gathering.
- You found a likely root cause but need confirmation before narrowing further.

Do not ask when:
- The next diagnostic step is obvious.
- More data will clearly narrow the problem.
- Exporting an image or texture will give better evidence.

## Tool Reference

### Session

| Tool | Purpose |
|------|---------|
| `open_capture` | Load an `.rdc` file for analysis |
| `capture_frame` | Launch the app, inject RenderDoc, capture a frame, and auto-open it |

### Navigation and Events

| Tool | Purpose |
|------|---------|
| `list_events` | List all events, including draws and non-draws |
| `list_draws` | List draw calls only |
| `goto_event` | Navigate to an event and update current state |
| `get_draw_info` | Retrieve detailed information for one draw call |

### Pipeline and Bindings

| Tool | Purpose |
|------|---------|
| `get_pipeline_state` | Inspect bound shaders, render targets, depth state, and viewports |
| `get_bindings` | Inspect constant buffers, textures, UAVs, and samplers |

### Shaders

| Tool | Purpose |
|------|---------|
| `get_shader` | Retrieve disassembly or reflection |
| `list_shaders` | List unique shaders with usage counts |
| `search_shaders` | Search across shader disassembly text |

### Resources and Passes

| Tool | Purpose |
|------|---------|
| `list_resources` | List GPU resources with optional filtering |
| `get_resource_info` | Inspect a single resource in detail |
| `list_passes` | List render passes with draw counts |
| `get_pass_info` | List draws within one pass |

### Info and Diagnostics

| Tool | Purpose |
|------|---------|
| `get_capture_info` | Inspect API, GPU, driver, and event count |
| `get_stats` | Inspect per-pass breakdowns, top draws, and large resources |
| `get_log` | Inspect debug and validation messages |

### Export

| Tool | Purpose |
|------|---------|
| `export_render_target` | Export the current event's render target as PNG |
| `export_texture` | Export a texture resource as PNG |
| `export_buffer` | Export buffer data as a binary file |

### Pixel & Debug

| Tool | Key Parameters | Purpose |
|------|----------------|---------|
| `pixel_history` | `x`, `y`, `eventId` (opt), `targetIndex` (opt) | Query which draws modified a pixel up to an event; includes shader output, post-blend value, and pass/fail status |
| `pick_pixel` | `x`, `y`, `eventId` (opt), `targetIndex` (opt) | Read the RGBA value of a single pixel; returns float, uint, and int representations |
| `debug_pixel` | `eventId`, `x`, `y`, `mode` (summary/trace), `primitive` (opt) | Debug the fragment shader at a pixel; summary returns inputs/outputs, trace adds step-by-step execution |
| `debug_vertex` | `eventId`, `vertexId`, `mode` (summary/trace), `instance` (opt), `index` (opt), `view` (opt) | Debug the vertex shader for a specific vertex; summary or full trace |
| `debug_thread` | `eventId`, `groupX/Y/Z`, `threadX/Y/Z`, `mode` (summary/trace) | Debug a compute shader thread at a workgroup and thread coordinate |
| `get_texture_stats` | `resourceId`, `mip` (opt), `slice` (opt), `histogram` (opt), `eventId` (opt) | Get min/max pixel values and an optional 256-bucket RGBA histogram; useful for detecting NaN or all-zero textures |

### Shader Hot-Editing

| Tool | Purpose |
|------|---------|
| `shader_encodings` | List supported shader compilation encodings |
| `shader_build` | Compile shader source code, returns a shaderId |
| `shader_replace` | Replace shader at a given event/stage with a built shader |
| `shader_restore` | Restore a single shader to its original |
| `shader_restore_all` | Restore all replaced shaders and free resources |

### Extended Export

| Tool | Purpose |
|------|---------|
| `export_mesh` | Export post-transform vertex data as OBJ or JSON |
| `export_snapshot` | Export complete draw state (pipeline, shaders, and render targets) |
| `get_resource_usage` | Query how a resource is used across all events |

### Exact Draw Reconstruction

| Tool | Key Parameters | Purpose |
|------|----------------|---------|
| `export_shader_binary` | `eventId`, `stage`, `outputDir` (opt) | Export the original captured shader container and checksum |
| `get_descriptor_bindings` | `eventId` | Resolve reflection entries to actual descriptors, resources, views, ranges, and samplers |
| `export_texture_raw` | `eventId`, `resourceId`, `outputDir` (opt), subresource options | Export tightly packed raw texture bytes and layout metadata |
| `export_bound_buffer` | `eventId`, `stage`, `bindingKind`, `bindingIndex`, `arrayElement` (opt), `outputDir` (opt) | Export the exact buffer binding range or root constants |
| `get_d3d12_pipeline_state_full` | `eventId`, `includeAllResourceStates` (opt) | Export D3D12 PSO, root, IA, rasterizer, OM, descriptor, predication, and resource-state data |
| `export_draw_reconstruction_bundle` | `eventId`, `outputDir`, `preEventId` (opt) | Export a D3D12 draw package with descriptor-bound inputs and pre/post outputs; audit IA, indirect, specialized-pipeline, and MSAA requirements before calling it replay-complete |

### CI Assertions

| Tool | Purpose |
|------|---------|
| `assert_pixel` | Validate pixel RGBA value with configurable tolerance |
| `assert_state` | Validate a pipeline state field against an expected value |
| `assert_image` | Compare two PNG images pixel-by-pixel |
| `assert_count` | Validate resource, draw, or event counts |
| `assert_clean` | Validate no debug messages above a given severity |

### Diff / Comparison

| Tool | Key Parameters | Purpose |
|------|----------------|---------|
| `diff_open` | `captureA`, `captureB` | Open two captures for side-by-side comparison |
| `diff_close` | — | Close the diff session and free resources |
| `diff_summary` | — | High-level summary with multi-level checking; check `divergedAt` field |
| `diff_draws` | — | Compare draw call sequences using LCS alignment; reports changed/added/removed draws |
| `diff_resources` | — | Compare GPU resource lists between the two captures |
| `diff_stats` | — | Compare per-pass statistics between the two captures |
| `diff_pipeline` | `marker` | Compare pipeline state at a matched draw identified by marker path |
| `diff_framebuffer` | `eidA`, `eidB`, `target` (opt), `threshold` (opt), `diffOutput` (opt) | Pixel-level render target comparison with optional diff image output |
