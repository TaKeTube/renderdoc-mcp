#include "mcp/tools/tools.h"

#include "core/reconstruction.h"
#include "core/session.h"
#include "mcp/serialization.h"
#include "mcp/tool_registry.h"

#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

namespace renderdoc::mcp::tools {

namespace {

std::string defaultOutputDir(const core::Session& session, const std::string& leaf) {
    return (fs::path(session.exportDir()) / leaf).string();
}

} // namespace

void registerReconstructionTools(ToolRegistry& registry) {
    registry.registerTool({
        "export_shader_binary",
        "Export the original captured shader container bytes (DXBC, DXIL, SPIR-V, etc.) "
        "for a bound shader stage at an event.",
        {{"type", "object"},
         {"properties", {
             {"eventId", {{"type", "integer"}, {"description", "Event ID"}}},
             {"stage", {{"type", "string"}, {"enum", nlohmann::json::array({"vs", "hs", "ds", "gs", "ps", "cs"})}}},
             {"outputDir", {{"type", "string"}, {"description", "Output directory (optional)"}}}
         }},
         {"required", nlohmann::json::array({"eventId", "stage"})}},
        [](ToolContext& ctx, const nlohmann::json& args) {
            uint32_t eventId = args["eventId"].get<uint32_t>();
            auto stage = parseShaderStage(args["stage"].get<std::string>());
            std::string outputDir = args.value(
                "outputDir",
                defaultOutputDir(ctx.session, "shader-binary-" + std::to_string(eventId)));
            return core::exportShaderBinary(ctx.session, eventId, stage, outputDir);
        }
    });

    registry.registerTool({
        "get_descriptor_bindings",
        "Resolve shader reflection bindings to the actual descriptor stores, resources, views, "
        "buffer ranges, texture subresource ranges, and sampler state at an event.",
        {{"type", "object"},
         {"properties", {
             {"eventId", {{"type", "integer"}, {"description", "Event ID"}}}
         }},
         {"required", nlohmann::json::array({"eventId"})}},
        [](ToolContext& ctx, const nlohmann::json& args) {
            return core::getDescriptorBindings(
                ctx.session, args["eventId"].get<uint32_t>());
        }
    });

    registry.registerTool({
        "export_texture_raw",
        "Export exact raw texture bytes from RenderDoc GetTextureData with format and tightly-packed "
        "layout metadata. Can export one subresource or the whole texture.",
        {{"type", "object"},
         {"properties", {
             {"eventId", {{"type", "integer"}, {"description", "Resource state immediately after this event"}}},
             {"resourceId", {{"type", "string"}, {"description", "Texture ResourceId"}}},
             {"outputDir", {{"type", "string"}, {"description", "Output directory (optional)"}}},
             {"allSubresources", {{"type", "boolean"}, {"description", "Export every mip/slice/sample"}, {"default", false}}},
             {"mip", {{"type", "integer"}, {"description", "Mip when allSubresources=false"}, {"default", 0}, {"minimum", 0}}},
             {"slice", {{"type", "integer"}, {"description", "Array/cubemap slice when allSubresources=false"}, {"default", 0}, {"minimum", 0}}},
             {"sample", {{"type", "integer"}, {"description", "MSAA sample when allSubresources=false"}, {"default", 0}, {"minimum", 0}}}
         }},
         {"required", nlohmann::json::array({"eventId", "resourceId"})}},
        [](ToolContext& ctx, const nlohmann::json& args) {
            uint32_t eventId = args["eventId"].get<uint32_t>();
            core::ResourceId resourceId =
                parseResourceId(args["resourceId"].get<std::string>());
            std::string outputDir = args.value(
                "outputDir",
                defaultOutputDir(ctx.session,
                                 "raw-texture-" + std::to_string(resourceId) +
                                 "-event-" + std::to_string(eventId)));
            return core::exportTextureRaw(
                ctx.session, eventId, resourceId, outputDir,
                args.value("allSubresources", false),
                args.value("mip", 0u),
                args.value("slice", 0u),
                args.value("sample", 0u));
        }
    });

    registry.registerTool({
        "export_bound_buffer",
        "Export the exact byte range backing a shader CBV/SRV/UAV binding. "
        "D3D12 root constants are exported when the binding has no buffer resource.",
        {{"type", "object"},
         {"properties", {
             {"eventId", {{"type", "integer"}, {"description", "Event ID"}}},
             {"stage", {{"type", "string"}, {"enum", nlohmann::json::array({"vs", "hs", "ds", "gs", "ps", "cs"})}}},
             {"bindingKind", {{"type", "string"},
                              {"enum", nlohmann::json::array({"constantBuffer", "readOnlyResource", "readWriteResource"})}}},
             {"bindingIndex", {{"type", "integer"}, {"description", "Reflection-array index"}, {"minimum", 0}}},
             {"arrayElement", {{"type", "integer"}, {"description", "Binding array element"}, {"default", 0}, {"minimum", 0}}},
             {"outputDir", {{"type", "string"}, {"description", "Output directory (optional)"}}}
         }},
         {"required", nlohmann::json::array({"eventId", "stage", "bindingKind", "bindingIndex"})}},
        [](ToolContext& ctx, const nlohmann::json& args) {
            uint32_t eventId = args["eventId"].get<uint32_t>();
            std::string outputDir = args.value(
                "outputDir",
                defaultOutputDir(ctx.session, "bound-buffer-" + std::to_string(eventId)));
            return core::exportBoundBuffer(
                ctx.session, eventId,
                parseShaderStage(args["stage"].get<std::string>()),
                args["bindingKind"].get<std::string>(),
                args["bindingIndex"].get<uint32_t>(),
                args.value("arrayElement", 0u),
                outputDir);
        }
    });

    registry.registerTool({
        "get_d3d12_pipeline_state_full",
        "Get D3D12-specific reconstruction state: PSO ID, root signature and root arguments, "
        "IA, rasterizer, blend/depth/stencil, RTV/DSV descriptors, predication, and resource states.",
        {{"type", "object"},
         {"properties", {
             {"eventId", {{"type", "integer"}, {"description", "Event ID"}}},
             {"includeAllResourceStates", {{"type", "boolean"},
                                           {"description", "Include states for every live resource instead of only referenced resources"},
                                           {"default", false}}}
         }},
         {"required", nlohmann::json::array({"eventId"})}},
        [](ToolContext& ctx, const nlohmann::json& args) {
            return core::getD3D12PipelineStateFull(
                ctx.session,
                args["eventId"].get<uint32_t>(),
                args.value("includeAllResourceStates", false));
        }
    });

    registry.registerTool({
        "export_draw_reconstruction_bundle",
        "Export a self-contained D3D12 draw reconstruction bundle with exact shader containers, "
        "resolved descriptors, raw input resources, full pipeline/root state, pre-draw outputs, "
        "post-draw reference outputs, manifest, and checksums. The output directory must be new "
        "or empty so stale files cannot enter the manifest.",
        {{"type", "object"},
         {"properties", {
             {"eventId", {{"type", "integer"}, {"description", "Draw event ID"}}},
             {"preEventId", {{"type", "integer"},
                             {"description", "Event representing draw pre-state; defaults to previous action"}}},
             {"outputDir", {{"type", "string"},
                            {"description", "New or empty bundle output directory"}}}
         }},
         {"required", nlohmann::json::array({"eventId", "outputDir"})}},
        [](ToolContext& ctx, const nlohmann::json& args) {
            std::optional<uint32_t> preEventId;
            if (args.contains("preEventId"))
                preEventId = args["preEventId"].get<uint32_t>();
            return core::exportDrawReconstructionBundle(
                ctx.session,
                args["eventId"].get<uint32_t>(),
                preEventId,
                args["outputDir"].get<std::string>());
        }
    });
}

} // namespace renderdoc::mcp::tools
