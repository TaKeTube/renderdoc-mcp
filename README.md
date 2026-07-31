<p align="center">
  <img src="docs/logo.png" alt="renderdoc-mcp" width="400"><br>
  <b>AI-native GPU frame debugging for RenderDoc</b><br><br>
  <a href="https://github.com/JiaboLi-GitHub/renderdoc-mcp/actions/workflows/release.yml"><img src="https://github.com/JiaboLi-GitHub/renderdoc-mcp/actions/workflows/release.yml/badge.svg" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License: MIT"></a><br>
  <a href="https://oosmetrics.com/repo/JiaboLi-GitHub/renderdoc-mcp"><img src="https://api.oosmetrics.com/api/v1/badge/achievement/5ab44894-4c44-4fee-903d-9dd6f86d2d46.svg" alt="oosmetrics: Top 3 in Graphics by acceleration"></a>
  <a href="https://oosmetrics.com/repo/JiaboLi-GitHub/renderdoc-mcp"><img src="https://api.oosmetrics.com/api/v1/badge/achievement/90a5455c-2c8f-43c3-868d-61c5b0811f78.svg" alt="oosmetrics: Top 6 in Backend by acceleration"></a><br>
  <b>English</b> | <a href="README-CN.md">简体中文</a>
</p>

---

renderdoc-mcp is an MCP server and CLI that wraps the RenderDoc replay API into **65 structured tools**, letting AI assistants (Claude, Codex, etc.) open `.rdc` captures, inspect GPU frames, debug shaders/pixels, compare captures, and extract exact draw data for standalone reconstruction — all without manual UI.

## Demo

<p align="center">
  <img src="docs/demo/demo-en.png" alt="Demo" width="600">
</p>

## Features

| Area | What you can do |
|------|----------------|
| Session & Capture | Open captures, live-capture frames, inspect metadata |
| Frame Navigation | List events/draws, jump to any event |
| Pipeline & Shaders | Inspect pipeline state, bindings, shader source, constant buffers |
| Resources & Passes | Analyze frame structure, pass dependencies, resource usage |
| Pixel & Shader Debug | Pixel history, pick pixel, debug pixel/vertex/thread |
| Export | Render targets, textures, buffers, meshes, snapshots |
| Exact Draw Reconstruction | Original shader containers, resolved descriptors, raw resources, full D3D12 state, and pre/post outputs |
| Diff & Assertions | Compare captures, assert pixels/state/images for CI |

## Exact Draw Reconstruction

For a known D3D12 draw, renderdoc-mcp can export the captured state and exact
resource bytes needed to build a standalone replay:

```text
target draw + pre-draw event
  -> export shaders, descriptors, resources, root/PSO state, and pre/post outputs
  -> recreate the draw in a standalone D3D12 program
  -> read back and compare against the captured post-draw output
```

| Tool | Purpose |
|------|---------|
| `export_draw_reconstruction_bundle` | Export a draw package with manifest, checksums, inputs, state, and pre/post outputs |
| `export_shader_binary` | Preserve the original captured DXBC/DXIL shader container |
| `get_descriptor_bindings` | Resolve shader bindings to actual descriptors, resources, views, ranges, and samplers |
| `export_texture_raw` | Export tightly packed raw texture subresources and layout metadata |
| `export_bound_buffer` | Export the exact CBV/SRV/UAV range or root constants |
| `get_d3d12_pipeline_state_full` | Export D3D12 PSO, root, IA, rasterizer, output-merger, predication, and resource state |

`manifest.complete=true` means the export is complete; it does not by itself
prove that a standalone replay is correct. Exact reconstruction finishes when
the replay output matches `outputs/post` byte-for-byte, or a format-aware
comparison meets a documented tolerance.

The current bundle exporter is D3D12-only. Audit raw IA buffers, indirect
command data, mesh/task or ray-tracing pipelines, and MSAA initialization before
calling a package replay-complete. See the
[exact draw reconstruction workflow](skills/renderdoc-mcp/references/draw-reconstruction.md)
for event timing, Agility SDK preflight, texture upload, output initialization,
descriptor layout, and mismatch triage.

## Download

Get the latest package from [GitHub Releases](https://github.com/JiaboLi-GitHub/renderdoc-mcp/releases). The zip contains:

| File | Description |
|------|-------------|
| `bin/renderdoc-mcp.exe` | MCP server (stdio) |
| `bin/renderdoc-cli.exe` | CLI for shell & CI |
| `bin/renderdoc.dll` / `renderdoc.json` | Bundled RenderDoc runtime |
| `skills/renderdoc-mcp/` | Codex workflow skill |
| `install-codex.ps1` | One-click Codex Desktop installer |

## Client Configuration

### Codex Desktop

The installer auto-configures `~/.codex/config.toml`. Or add manually:

```toml
[mcp_servers.renderdoc-mcp]
command = 'renderdoc-mcp.exe'
args = []
```

### Claude Code

```json
{
  "mcpServers": {
    "renderdoc-mcp": {
      "command": "renderdoc-mcp.exe",
      "args": []
    }
  }
}
```

## Build From Source

```bash
cmake -B build -DRENDERDOC_DIR=<path-to-renderdoc-source>
cmake --build build --config Release
```

| Variable | Required | Description |
|----------|----------|-------------|
| `RENDERDOC_DIR` | Yes | RenderDoc source root |
| `RENDERDOC_BUILD_DIR` | No | RenderDoc build output (if non-standard location) |

## Architecture

![Architecture](docs/architecture.png)

## License

[MIT](LICENSE). RenderDoc itself is under its [own license](https://github.com/baldurk/renderdoc/blob/v1.x/LICENSE.md).
