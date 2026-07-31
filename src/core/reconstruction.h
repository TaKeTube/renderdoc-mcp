#pragma once

#include "core/types.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace renderdoc::core {

class Session;

// Export the original captured shader container (DXBC, DXIL, SPIR-V, etc.).
nlohmann::json exportShaderBinary(Session& session,
                                  uint32_t eventId,
                                  ShaderStage stage,
                                  const std::string& outputDir);

// Resolve reflection bindings to their real descriptors/resources at an event.
nlohmann::json getDescriptorBindings(Session& session, uint32_t eventId);

// Export one or all raw texture subresources. Returned data is the tightly-packed
// byte layout produced by RenderDoc's GetTextureData.
nlohmann::json exportTextureRaw(Session& session,
                                uint32_t eventId,
                                ResourceId resourceId,
                                const std::string& outputDir,
                                bool allSubresources,
                                uint32_t mip,
                                uint32_t slice,
                                uint32_t sample);

// Export the exact byte range backing a shader binding. Root constants are
// exported directly from D3D12 root parameter state when no buffer backs them.
nlohmann::json exportBoundBuffer(Session& session,
                                 uint32_t eventId,
                                 ShaderStage stage,
                                 const std::string& bindingKind,
                                 uint32_t bindingIndex,
                                 uint32_t arrayElement,
                                 const std::string& outputDir);

// Export API-specific D3D12 state needed to recreate a draw.
nlohmann::json getD3D12PipelineStateFull(Session& session,
                                         uint32_t eventId,
                                         bool includeAllResourceStates);

// Create a self-contained draw reconstruction bundle. If preEventId is omitted,
// the greatest action event ID lower than eventId is used.
nlohmann::json exportDrawReconstructionBundle(Session& session,
                                               uint32_t eventId,
                                               std::optional<uint32_t> preEventId,
                                               const std::string& outputDir);

} // namespace renderdoc::core
