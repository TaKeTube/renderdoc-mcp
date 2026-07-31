#include "core/reconstruction.h"

#include "core/errors.h"
#include "core/events.h"
#include "core/resource_id.h"
#include "core/session.h"

#include <renderdoc_replay.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace renderdoc::core {

namespace {

using json = nlohmann::json;

struct BoundShader {
    ::ResourceId resourceId;
    const ::ShaderReflection* reflection = nullptr;
};

struct ResolvedBinding {
    ::ShaderStage stage = ::ShaderStage::Count;
    DescriptorType descriptorType = DescriptorType::Unknown;
    DescriptorCategory category = DescriptorCategory::Unknown;
    uint16_t reflectionIndex = DescriptorAccess::NoShaderBinding;
    uint32_t arrayElement = 0;
    bool staticallyUnused = false;
    ::ResourceId descriptorStore;
    uint32_t descriptorByteOffset = 0;
    uint32_t descriptorByteSize = 0;
    std::string name;
    uint32_t bindPoint = 0;
    uint32_t bindSpace = 0;
    uint32_t reflectedByteSize = 0;
    Descriptor descriptor;
    SamplerDescriptor sampler;
    bool hasDescriptor = false;
    bool hasSampler = false;
};

template <typename T>
std::string enumString(const T& value) {
    return std::string(ToStr(value).c_str());
}

std::string resourceIdString(::ResourceId id) {
    return "ResourceId::" + std::to_string(toResourceId(id));
}

std::string resourceIdString(ResourceId id) {
    return "ResourceId::" + std::to_string(id);
}

std::string stageString(::ShaderStage stage) {
    switch (stage) {
        case ::ShaderStage::Vertex: return "vs";
        case ::ShaderStage::Hull: return "hs";
        case ::ShaderStage::Domain: return "ds";
        case ::ShaderStage::Geometry: return "gs";
        case ::ShaderStage::Pixel: return "ps";
        case ::ShaderStage::Compute: return "cs";
        case ::ShaderStage::Task: return "as";
        case ::ShaderStage::Mesh: return "ms";
        default: return "unknown";
    }
}

std::string stageString(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return "vs";
        case ShaderStage::Hull: return "hs";
        case ShaderStage::Domain: return "ds";
        case ShaderStage::Geometry: return "gs";
        case ShaderStage::Pixel: return "ps";
        case ShaderStage::Compute: return "cs";
    }
    return "unknown";
}

::ShaderStage toRenderDocStage(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return ::ShaderStage::Vertex;
        case ShaderStage::Hull: return ::ShaderStage::Hull;
        case ShaderStage::Domain: return ::ShaderStage::Domain;
        case ShaderStage::Geometry: return ::ShaderStage::Geometry;
        case ShaderStage::Pixel: return ::ShaderStage::Pixel;
        case ShaderStage::Compute: return ::ShaderStage::Compute;
    }
    return ::ShaderStage::Count;
}

std::string bindingKind(DescriptorCategory category) {
    switch (category) {
        case DescriptorCategory::ConstantBlock: return "constantBuffer";
        case DescriptorCategory::ReadOnlyResource: return "readOnlyResource";
        case DescriptorCategory::ReadWriteResource: return "readWriteResource";
        case DescriptorCategory::Sampler: return "sampler";
        default: return "unknown";
    }
}

DescriptorCategory categoryFromBindingKind(const std::string& kind) {
    if (kind == "constantBuffer") return DescriptorCategory::ConstantBlock;
    if (kind == "readOnlyResource") return DescriptorCategory::ReadOnlyResource;
    if (kind == "readWriteResource") return DescriptorCategory::ReadWriteResource;
    throw CoreError(CoreError::Code::InternalError,
                    "Unknown binding kind: " + kind);
}

void validateOutputDir(const std::string& outputDir) {
    if (outputDir.empty())
        throw CoreError(CoreError::Code::InvalidPath, "Output directory cannot be empty.");

    fs::path raw(outputDir);
    for (const auto& part : raw) {
        if (part == "..")
            throw CoreError(CoreError::Code::InvalidPath,
                            "Output directory must not contain '..': " + outputDir);
    }
}

std::string sanitizeFilename(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (unsigned char ch : input) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.')
            result.push_back(static_cast<char>(ch));
        else
            result.push_back('_');
    }
    if (result.empty()) result = "unnamed";
    return result;
}

std::string bytesToHex(const byte* data, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result[i * 2] = digits[(data[i] >> 4) & 0xF];
        result[i * 2 + 1] = digits[data[i] & 0xF];
    }
    return result;
}

std::string bytesToHex(const bytebuf& data) {
    return bytesToHex(data.data(), data.size());
}

uint64_t fnv1a64(const byte* data, size_t size) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex64(uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::string checksum(const bytebuf& data) {
    return hex64(fnv1a64(data.data(), data.size()));
}

void writeBytes(const fs::path& path, const byte* data, size_t size) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw CoreError(CoreError::Code::ExportFailed,
                        "Failed to open output file: " + path.string());
    if (size > 0)
        output.write(reinterpret_cast<const char*>(data),
                     static_cast<std::streamsize>(size));
    if (!output)
        throw CoreError(CoreError::Code::ExportFailed,
                        "Failed to write output file: " + path.string());
}

void writeBytes(const fs::path& path, const bytebuf& data) {
    writeBytes(path, data.data(), data.size());
}

void writeJson(const fs::path& path, const json& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    if (!output)
        throw CoreError(CoreError::Code::ExportFailed,
                        "Failed to open JSON output file: " + path.string());
    output << value.dump(2);
    if (!output)
        throw CoreError(CoreError::Code::ExportFailed,
                        "Failed to write JSON output file: " + path.string());
}

json formatJson(const ResourceFormat& format) {
    return {
        {"name", std::string(format.Name().c_str())},
        {"type", enumString(format.type)},
        {"componentType", enumString(format.compType)},
        {"componentCount", format.compCount},
        {"componentByteWidth", format.compByteWidth},
        {"elementByteSize", format.ElementSize()},
        {"blockCompressed", format.BlockFormat()},
        {"bgraOrder", format.BGRAOrder()},
        {"srgbCorrected", format.SRGBCorrected()},
        {"yuvSubsampling", format.YUVSubsampling()},
        {"yuvPlaneCount", format.YUVPlaneCount()}
    };
}

json swizzleJson(const TextureSwizzle4& swizzle) {
    return {
        {"r", enumString(swizzle.red)},
        {"g", enumString(swizzle.green)},
        {"b", enumString(swizzle.blue)},
        {"a", enumString(swizzle.alpha)}
    };
}

json pixelValueJson(const ::PixelValue& value) {
    return {
        {"float", {value.floatValue[0], value.floatValue[1],
                   value.floatValue[2], value.floatValue[3]}},
        {"uint", {value.uintValue[0], value.uintValue[1],
                  value.uintValue[2], value.uintValue[3]}},
        {"int", {value.intValue[0], value.intValue[1],
                 value.intValue[2], value.intValue[3]}}
    };
}

json descriptorJson(const Descriptor& descriptor) {
    return {
        {"type", enumString(descriptor.type)},
        {"flags", enumString(descriptor.flags)},
        {"format", formatJson(descriptor.format)},
        {"resourceId", resourceIdString(descriptor.resource)},
        {"secondaryResourceId", resourceIdString(descriptor.secondary)},
        {"viewId", resourceIdString(descriptor.view)},
        {"byteOffset", descriptor.byteOffset},
        {"byteSize", descriptor.byteSize},
        {"counterByteOffset", descriptor.counterByteOffset},
        {"bufferStructCount", descriptor.bufferStructCount},
        {"elementByteSize", descriptor.elementByteSize},
        {"minLODClamp", descriptor.minLODClamp},
        {"firstSlice", descriptor.firstSlice},
        {"numSlices", descriptor.numSlices},
        {"firstMip", descriptor.firstMip},
        {"numMips", descriptor.numMips},
        {"swizzle", swizzleJson(descriptor.swizzle)},
        {"textureType", enumString(descriptor.textureType)}
    };
}

json samplerJson(const SamplerDescriptor& sampler) {
    return {
        {"objectId", resourceIdString(sampler.object)},
        {"type", enumString(sampler.type)},
        {"addressU", enumString(sampler.addressU)},
        {"addressV", enumString(sampler.addressV)},
        {"addressW", enumString(sampler.addressW)},
        {"compareFunction", enumString(sampler.compareFunction)},
        {"filter", {
            {"minify", enumString(sampler.filter.minify)},
            {"magnify", enumString(sampler.filter.magnify)},
            {"mip", enumString(sampler.filter.mip)},
            {"function", enumString(sampler.filter.filter)}
        }},
        {"maxAnisotropy", sampler.maxAnisotropy},
        {"minLOD", sampler.minLOD},
        {"maxLOD", sampler.maxLOD},
        {"mipBias", sampler.mipBias},
        {"borderColor", pixelValueJson(sampler.borderColorValue)},
        {"borderColorType", enumString(sampler.borderColorType)},
        {"srgbBorder", sampler.srgbBorder},
        {"seamlessCubemaps", sampler.seamlessCubemaps},
        {"unnormalizedCoordinates", sampler.unnormalized},
        {"creationTimeConstant", sampler.creationTimeConstant},
        {"swizzle", swizzleJson(sampler.swizzle)}
    };
}

const ActionDescription* findAction(const rdcarray<ActionDescription>& actions,
                                    uint32_t eventId) {
    for (const auto& action : actions) {
        if (action.eventId == eventId) return &action;
        if (!action.children.empty()) {
            if (const auto* found = findAction(action.children, eventId))
                return found;
        }
    }
    return nullptr;
}

void findPreviousEvent(const rdcarray<ActionDescription>& actions,
                       uint32_t eventId,
                       uint32_t& previous) {
    for (const auto& action : actions) {
        if (action.eventId < eventId && action.eventId > previous)
            previous = action.eventId;
        if (!action.children.empty())
            findPreviousEvent(action.children, eventId, previous);
    }
}

json actionJson(const ActionDescription& action) {
    json outputs = json::array();
    for (const auto& output : action.outputs) {
        if (output != ::ResourceId::Null())
            outputs.push_back(resourceIdString(output));
    }
    return {
        {"eventId", action.eventId},
        {"actionId", action.actionId},
        {"name", std::string(action.customName.c_str())},
        {"flags", enumString(action.flags)},
        {"numIndices", action.numIndices},
        {"numInstances", action.numInstances},
        {"baseVertex", action.baseVertex},
        {"indexOffset", action.indexOffset},
        {"vertexOffset", action.vertexOffset},
        {"instanceOffset", action.instanceOffset},
        {"drawIndex", action.drawIndex},
        {"dispatchDimension", {action.dispatchDimension[0],
                               action.dispatchDimension[1],
                               action.dispatchDimension[2]}},
        {"dispatchThreadsDimension", {action.dispatchThreadsDimension[0],
                                      action.dispatchThreadsDimension[1],
                                      action.dispatchThreadsDimension[2]}},
        {"dispatchBase", {action.dispatchBase[0],
                          action.dispatchBase[1],
                          action.dispatchBase[2]}},
        {"outputs", outputs},
        {"depthOutput", resourceIdString(action.depthOut)}
    };
}

BoundShader getBoundShader(IReplayController* controller, ::ShaderStage stage) {
    APIProperties properties = controller->GetAPIProperties();
    BoundShader result;

    if (properties.pipelineType == GraphicsAPI::D3D12) {
        const auto* state = controller->GetD3D12PipelineState();
        if (!state) return result;
        const D3D12Pipe::Shader* shader = nullptr;
        switch (stage) {
            case ::ShaderStage::Vertex: shader = &state->vertexShader; break;
            case ::ShaderStage::Hull: shader = &state->hullShader; break;
            case ::ShaderStage::Domain: shader = &state->domainShader; break;
            case ::ShaderStage::Geometry: shader = &state->geometryShader; break;
            case ::ShaderStage::Pixel: shader = &state->pixelShader; break;
            case ::ShaderStage::Compute: shader = &state->computeShader; break;
            default: break;
        }
        if (shader) {
            result.resourceId = shader->resourceId;
            result.reflection = shader->reflection;
        }
    } else if (properties.pipelineType == GraphicsAPI::D3D11) {
        const auto* state = controller->GetD3D11PipelineState();
        if (!state) return result;
        const D3D11Pipe::Shader* shader = nullptr;
        switch (stage) {
            case ::ShaderStage::Vertex: shader = &state->vertexShader; break;
            case ::ShaderStage::Hull: shader = &state->hullShader; break;
            case ::ShaderStage::Domain: shader = &state->domainShader; break;
            case ::ShaderStage::Geometry: shader = &state->geometryShader; break;
            case ::ShaderStage::Pixel: shader = &state->pixelShader; break;
            case ::ShaderStage::Compute: shader = &state->computeShader; break;
            default: break;
        }
        if (shader) {
            result.resourceId = shader->resourceId;
            result.reflection = shader->reflection;
        }
    } else if (properties.pipelineType == GraphicsAPI::Vulkan) {
        const auto* state = controller->GetVulkanPipelineState();
        if (!state) return result;
        const VKPipe::Shader* shader = nullptr;
        switch (stage) {
            case ::ShaderStage::Vertex: shader = &state->vertexShader; break;
            case ::ShaderStage::Hull: shader = &state->tessControlShader; break;
            case ::ShaderStage::Domain: shader = &state->tessEvalShader; break;
            case ::ShaderStage::Geometry: shader = &state->geometryShader; break;
            case ::ShaderStage::Pixel: shader = &state->fragmentShader; break;
            case ::ShaderStage::Compute: shader = &state->computeShader; break;
            default: break;
        }
        if (shader) {
            result.resourceId = shader->resourceId;
            result.reflection = shader->reflection;
        }
    } else if (properties.pipelineType == GraphicsAPI::OpenGL) {
        const auto* state = controller->GetGLPipelineState();
        if (!state) return result;
        const GLPipe::Shader* shader = nullptr;
        switch (stage) {
            case ::ShaderStage::Vertex: shader = &state->vertexShader; break;
            case ::ShaderStage::Hull: shader = &state->tessControlShader; break;
            case ::ShaderStage::Domain: shader = &state->tessEvalShader; break;
            case ::ShaderStage::Geometry: shader = &state->geometryShader; break;
            case ::ShaderStage::Pixel: shader = &state->fragmentShader; break;
            case ::ShaderStage::Compute: shader = &state->computeShader; break;
            default: break;
        }
        if (shader) {
            result.resourceId = shader->shaderResourceId;
            result.reflection = shader->reflection;
        }
    }

    return result;
}

void fillBindingIdentity(ResolvedBinding& binding,
                         const ::ShaderReflection* reflection) {
    if (!reflection || binding.reflectionIndex == DescriptorAccess::NoShaderBinding)
        return;

    const uint16_t index = binding.reflectionIndex;
    switch (binding.category) {
        case DescriptorCategory::ConstantBlock:
            if (index < reflection->constantBlocks.count()) {
                const auto& item = reflection->constantBlocks[index];
                binding.name = item.name.c_str();
                binding.bindPoint = item.fixedBindNumber;
                binding.bindSpace = item.fixedBindSetOrSpace;
                binding.reflectedByteSize = item.byteSize;
            }
            break;
        case DescriptorCategory::ReadOnlyResource:
            if (index < reflection->readOnlyResources.count()) {
                const auto& item = reflection->readOnlyResources[index];
                binding.name = item.name.c_str();
                binding.bindPoint = item.fixedBindNumber;
                binding.bindSpace = item.fixedBindSetOrSpace;
            }
            break;
        case DescriptorCategory::ReadWriteResource:
            if (index < reflection->readWriteResources.count()) {
                const auto& item = reflection->readWriteResources[index];
                binding.name = item.name.c_str();
                binding.bindPoint = item.fixedBindNumber;
                binding.bindSpace = item.fixedBindSetOrSpace;
            }
            break;
        case DescriptorCategory::Sampler:
            if (index < reflection->samplers.count()) {
                const auto& item = reflection->samplers[index];
                binding.name = item.name.c_str();
                binding.bindPoint = item.fixedBindNumber;
                binding.bindSpace = item.fixedBindSetOrSpace;
            }
            break;
        default:
            break;
    }
}

std::vector<ResolvedBinding> resolveBindings(IReplayController* controller) {
    std::vector<ResolvedBinding> result;
    const auto& accesses = controller->GetDescriptorAccess();
    result.reserve(accesses.size());

    for (const auto& access : accesses) {
        ResolvedBinding binding;
        binding.stage = access.stage;
        binding.descriptorType = access.type;
        binding.category = CategoryForDescriptorType(access.type);
        binding.reflectionIndex = access.index;
        binding.arrayElement = access.arrayElement;
        binding.staticallyUnused = access.staticallyUnused;
        binding.descriptorStore = access.descriptorStore;
        binding.descriptorByteOffset = access.byteOffset;
        binding.descriptorByteSize = access.byteSize;

        const auto shader = getBoundShader(controller, access.stage);
        fillBindingIdentity(binding, shader.reflection);

        rdcarray<DescriptorRange> ranges;
        ranges.push_back(DescriptorRange(access));

        if (binding.category == DescriptorCategory::Sampler) {
            auto samplers = controller->GetSamplerDescriptors(access.descriptorStore, ranges);
            if (!samplers.empty()) {
                binding.sampler = samplers[0];
                binding.hasSampler = true;
            }
        } else {
            auto descriptors = controller->GetDescriptors(access.descriptorStore, ranges);
            if (!descriptors.empty()) {
                binding.descriptor = descriptors[0];
                binding.hasDescriptor = true;
            }
        }

        result.push_back(std::move(binding));
    }

    return result;
}

json resolvedBindingJson(const ResolvedBinding& binding) {
    json result = {
        {"stage", stageString(binding.stage)},
        {"descriptorType", enumString(binding.descriptorType)},
        {"bindingKind", bindingKind(binding.category)},
        {"reflectionIndex", binding.reflectionIndex},
        {"arrayElement", binding.arrayElement},
        {"staticallyUnused", binding.staticallyUnused},
        {"name", binding.name},
        {"register", binding.bindPoint},
        {"space", binding.bindSpace},
        {"reflectedByteSize", binding.reflectedByteSize},
        {"descriptorStore", resourceIdString(binding.descriptorStore)},
        {"descriptorByteOffset", binding.descriptorByteOffset},
        {"descriptorByteSize", binding.descriptorByteSize}
    };
    if (binding.hasDescriptor)
        result["descriptor"] = descriptorJson(binding.descriptor);
    if (binding.hasSampler)
        result["sampler"] = samplerJson(binding.sampler);
    if (binding.category == DescriptorCategory::ConstantBlock &&
        binding.reflectedByteSize > 0) {
        result["d3d12CbvSize"] =
            (static_cast<uint64_t>(binding.reflectedByteSize) + 255ull) &
            ~255ull;
    }
    return result;
}

const TextureDescription* findTexture(IReplayController* controller,
                                      ::ResourceId resourceId) {
    const auto& textures = controller->GetTextures();
    for (const auto& texture : textures)
        if (texture.resourceId == resourceId)
            return &texture;
    return nullptr;
}

const BufferDescription* findBuffer(IReplayController* controller,
                                    ::ResourceId resourceId) {
    const auto& buffers = controller->GetBuffers();
    for (const auto& buffer : buffers)
        if (buffer.resourceId == resourceId)
            return &buffer;
    return nullptr;
}

json textureDescriptionJson(const TextureDescription& texture) {
    return {
        {"resourceId", resourceIdString(texture.resourceId)},
        {"format", formatJson(texture.format)},
        {"dimension", texture.dimension},
        {"textureType", enumString(texture.type)},
        {"width", texture.width},
        {"height", texture.height},
        {"depth", texture.depth},
        {"mips", texture.mips},
        {"arraySize", texture.arraysize},
        {"cubemap", texture.cubemap},
        {"msaaSamples", texture.msSamp},
        {"msaaQuality", texture.msQual},
        {"byteSize", texture.byteSize},
        {"creationFlags", enumString(texture.creationFlags)}
    };
}

bool usesFourByFourBlocks(const ResourceFormat& format) {
    switch (format.type) {
        case ResourceFormatType::BC1:
        case ResourceFormatType::BC2:
        case ResourceFormatType::BC3:
        case ResourceFormatType::BC4:
        case ResourceFormatType::BC5:
        case ResourceFormatType::BC6:
        case ResourceFormatType::BC7:
        case ResourceFormatType::ETC2:
        case ResourceFormatType::EAC:
            return true;
        default:
            return false;
    }
}

json rawSubresourceLayout(const TextureDescription& texture,
                          uint32_t mip,
                          const bytebuf& data) {
    uint32_t width = std::max(1u, texture.width >> mip);
    uint32_t height = std::max(1u, texture.height >> mip);
    uint32_t depth = texture.dimension == 3
        ? std::max(1u, texture.depth >> mip)
        : 1u;

    uint64_t rowCount = height;
    std::string rowUnit = "texelRow";
    if (usesFourByFourBlocks(texture.format)) {
        rowCount = std::max<uint64_t>(1, (height + 3u) / 4u);
        rowUnit = "4x4BlockRow";
    } else if (texture.format.type == ResourceFormatType::ASTC ||
               texture.format.type == ResourceFormatType::PVRTC) {
        rowCount = 0;
        rowUnit = "formatSpecificBlockRow";
    }

    json result = {
        {"layout", "RenderDocGetTextureDataTightlyPacked"},
        {"width", width},
        {"height", height},
        {"depth", depth},
        {"byteLength", data.size()},
        {"rowUnit", rowUnit}
    };

    if (depth > 0 && data.size() % depth == 0)
        result["slicePitch"] = data.size() / depth;
    else
        result["slicePitch"] = nullptr;

    if (rowCount > 0 && depth > 0 &&
        data.size() % (rowCount * depth) == 0) {
        result["rowCountPerSlice"] = rowCount;
        result["rowPitch"] = data.size() / (rowCount * depth);
    } else {
        result["rowCountPerSlice"] = nullptr;
        result["rowPitch"] = nullptr;
    }

    return result;
}

json exportTextureRawAtCurrentEvent(IReplayController* controller,
                                    uint32_t eventId,
                                    ::ResourceId resourceId,
                                    const fs::path& outputDir,
                                    bool allSubresources,
                                    uint32_t requestedMip,
                                    uint32_t requestedSlice,
                                    uint32_t requestedSample) {
    const auto* texture = findTexture(controller, resourceId);
    if (!texture)
        throw CoreError(CoreError::Code::InvalidResourceId,
                        resourceIdString(resourceId) + " is not a texture.");

    if (!allSubresources) {
        if (requestedMip >= texture->mips)
            throw CoreError(CoreError::Code::InternalError, "Texture mip is out of range.");
        if (texture->dimension != 3 && requestedSlice >= texture->arraysize)
            throw CoreError(CoreError::Code::InternalError, "Texture slice is out of range.");
        if (requestedSample >= texture->msSamp)
            throw CoreError(CoreError::Code::InternalError, "Texture sample is out of range.");
    }

    fs::create_directories(outputDir);

    json subresources = json::array();
    uint64_t totalBytes = 0;

    uint32_t mipBegin = allSubresources ? 0 : requestedMip;
    uint32_t mipEnd = allSubresources ? texture->mips : requestedMip + 1;
    uint32_t sampleBegin = allSubresources ? 0 : requestedSample;
    uint32_t sampleEnd = allSubresources ? texture->msSamp : requestedSample + 1;

    for (uint32_t mip = mipBegin; mip < mipEnd; ++mip) {
        uint32_t sliceCount = texture->dimension == 3 ? 1u : texture->arraysize;
        uint32_t sliceBegin = allSubresources ? 0 : requestedSlice;
        uint32_t sliceEnd = allSubresources ? sliceCount : requestedSlice + 1;

        for (uint32_t slice = sliceBegin; slice < sliceEnd; ++slice) {
            for (uint32_t sample = sampleBegin; sample < sampleEnd; ++sample) {
                Subresource subresource(mip, slice, sample);
                bytebuf data = controller->GetTextureData(resourceId, subresource);

                std::ostringstream filename;
                filename << "mip_" << mip << "_slice_" << slice
                         << "_sample_" << sample << ".bin";
                fs::path outputPath = outputDir / filename.str();
                writeBytes(outputPath, data);

                json entry = {
                    {"mip", mip},
                    {"slice", slice},
                    {"sample", sample},
                    {"path", outputPath.filename().generic_string()},
                    {"checksumAlgorithm", "fnv1a64"},
                    {"checksum", checksum(data)}
                };
                entry.update(rawSubresourceLayout(*texture, mip, data));
                subresources.push_back(std::move(entry));
                totalBytes += data.size();
            }
        }
    }

    json metadata = {
        {"schemaVersion", 1},
        {"eventId", eventId},
        {"stateTiming", "immediatelyAfterEvent"},
        {"texture", textureDescriptionJson(*texture)},
        {"subresources", subresources},
        {"subresourceCount", subresources.size()},
        {"totalExportedBytes", totalBytes}
    };
    fs::path metadataPath = outputDir / "metadata.json";
    writeJson(metadataPath, metadata);

    return {
        {"eventId", eventId},
        {"resourceId", resourceIdString(resourceId)},
        {"outputDir", outputDir.string()},
        {"metadataPath", metadataPath.string()},
        {"subresourceCount", subresources.size()},
        {"byteSize", totalBytes}
    };
}

bool getResolvedBufferRange(IReplayController* controller,
                            const ResolvedBinding& binding,
                            uint64_t& offset,
                            uint64_t& size) {
    if (!binding.hasDescriptor ||
        binding.descriptor.resource == ::ResourceId::Null()) {
        return false;
    }

    const auto* buffer = findBuffer(controller, binding.descriptor.resource);
    if (!buffer)
        return false;

    offset = std::min<uint64_t>(binding.descriptor.byteOffset, buffer->length);
    uint64_t available = buffer->length - offset;
    size = binding.category == DescriptorCategory::ConstantBlock &&
                   binding.reflectedByteSize > 0
               ? binding.reflectedByteSize
               : binding.descriptor.byteSize;
    if (size == 0 || size == ~0ull || size > available)
        size = available;
    return true;
}

bytebuf getResolvedBufferBytes(IReplayController* controller,
                               const ResolvedBinding& binding,
                               uint64_t& offset,
                               uint64_t& size) {
    if (!getResolvedBufferRange(controller, binding, offset, size))
        return {};
    return controller->GetBufferData(binding.descriptor.resource, offset, size);
}

const bytebuf* findRootConstants(IReplayController* controller,
                                 const ResolvedBinding& binding) {
    const auto* state = controller->GetD3D12PipelineState();
    if (!state || binding.category != DescriptorCategory::ConstantBlock)
        return nullptr;
    for (const auto& parameter : state->rootSignature.parameters) {
        if (!parameter.constants.empty() &&
            parameter.reg == binding.bindPoint &&
            parameter.space == binding.bindSpace) {
            return &parameter.constants;
        }
    }
    return nullptr;
}

json writeResolvedBuffer(IReplayController* controller,
                         uint32_t eventId,
                         const ResolvedBinding& binding,
                         const fs::path& outputDir) {
    fs::create_directories(outputDir);
    std::string baseName = stageString(binding.stage) + "_" +
                           bindingKind(binding.category) + "_" +
                           std::to_string(binding.reflectionIndex) + "_" +
                           sanitizeFilename(binding.name);
    fs::path outputPath = outputDir / (baseName + ".bin");

    uint64_t offset = 0;
    uint64_t size = 0;
    bytebuf data = getResolvedBufferBytes(controller, binding, offset, size);

    json result = resolvedBindingJson(binding);
    result["eventId"] = eventId;

    if (!data.empty() ||
        (binding.hasDescriptor &&
         binding.descriptor.resource != ::ResourceId::Null() &&
         findBuffer(controller, binding.descriptor.resource))) {
        writeBytes(outputPath, data);
        result["source"] = "bufferResource";
        result["resourceId"] = resourceIdString(binding.descriptor.resource);
        result["byteOffset"] = offset;
        result["byteSize"] = data.size();
        result["path"] = outputPath.string();
        result["checksumAlgorithm"] = "fnv1a64";
        result["checksum"] = checksum(data);
    } else if (const bytebuf* constants = findRootConstants(controller, binding)) {
        writeBytes(outputPath, *constants);
        result["source"] = "d3d12RootConstants";
        result["resourceId"] = resourceIdString(::ResourceId::Null());
        result["byteOffset"] = 0;
        result["byteSize"] = constants->size();
        result["path"] = outputPath.string();
        result["checksumAlgorithm"] = "fnv1a64";
        result["checksum"] = checksum(*constants);
    } else {
        throw CoreError(CoreError::Code::ExportFailed,
                        "Binding '" + binding.name +
                        "' is not backed by a buffer or D3D12 root constants.");
    }

    fs::path metadataPath = outputDir / (baseName + ".json");
    writeJson(metadataPath, result);
    result["metadataPath"] = metadataPath.string();
    return result;
}

} // anonymous namespace

nlohmann::json exportShaderBinary(Session& session,
                                  uint32_t eventId,
                                  ShaderStage stage,
                                  const std::string& outputDir) {
    validateOutputDir(outputDir);
    gotoEvent(session, eventId);
    auto* controller = session.controller();

    BoundShader shader = getBoundShader(controller, toRenderDocStage(stage));
    if (!shader.reflection ||
        shader.resourceId == ::ResourceId::Null()) {
        throw CoreError(CoreError::Code::NoShaderBound,
                        "No shader is bound at stage '" + stageString(stage) + "'.");
    }
    if (shader.reflection->rawBytes.empty()) {
        throw CoreError(CoreError::Code::ExportFailed,
                        "The bound shader reflection has no original raw bytes.");
    }

    std::string encoding = enumString(shader.reflection->encoding);
    std::string extension = ".bin";
    if (shader.reflection->encoding == ::ShaderEncoding::DXIL) extension = ".dxil";
    else if (shader.reflection->encoding == ::ShaderEncoding::DXBC) extension = ".dxbc";
    else if (shader.reflection->encoding == ::ShaderEncoding::SPIRV) extension = ".spv";

    fs::path directory(outputDir);
    fs::create_directories(directory);
    fs::path outputPath = directory / ("shader_" + stageString(stage) + extension);
    writeBytes(outputPath, shader.reflection->rawBytes);

    json result = {
        {"eventId", eventId},
        {"stage", stageString(stage)},
        {"resourceId", resourceIdString(shader.resourceId)},
        {"entryPoint", std::string(shader.reflection->entryPoint.c_str())},
        {"encoding", encoding},
        {"path", outputPath.string()},
        {"byteSize", shader.reflection->rawBytes.size()},
        {"checksumAlgorithm", "fnv1a64"},
        {"checksum", checksum(shader.reflection->rawBytes)}
    };
    writeJson(directory / ("shader_" + stageString(stage) + ".json"), result);
    return result;
}

nlohmann::json getDescriptorBindings(Session& session, uint32_t eventId) {
    gotoEvent(session, eventId);
    auto* controller = session.controller();
    auto bindings = resolveBindings(controller);

    json entries = json::array();
    for (const auto& binding : bindings)
        entries.push_back(resolvedBindingJson(binding));

    return {
        {"eventId", eventId},
        {"api", enumString(controller->GetAPIProperties().pipelineType)},
        {"bindings", entries},
        {"count", entries.size()}
    };
}

nlohmann::json exportTextureRaw(Session& session,
                                uint32_t eventId,
                                ResourceId resourceId,
                                const std::string& outputDir,
                                bool allSubresources,
                                uint32_t mip,
                                uint32_t slice,
                                uint32_t sample) {
    validateOutputDir(outputDir);
    gotoEvent(session, eventId);
    return exportTextureRawAtCurrentEvent(
        session.controller(), eventId, fromResourceId(resourceId),
        fs::path(outputDir), allSubresources, mip, slice, sample);
}

nlohmann::json exportBoundBuffer(Session& session,
                                 uint32_t eventId,
                                 ShaderStage stage,
                                 const std::string& requestedKind,
                                 uint32_t bindingIndex,
                                 uint32_t arrayElement,
                                 const std::string& outputDir) {
    validateOutputDir(outputDir);
    gotoEvent(session, eventId);
    auto* controller = session.controller();

    DescriptorCategory requestedCategory =
        categoryFromBindingKind(requestedKind);
    ::ShaderStage requestedStage = toRenderDocStage(stage);
    auto bindings = resolveBindings(controller);

    for (const auto& binding : bindings) {
        if (binding.stage == requestedStage &&
            binding.category == requestedCategory &&
            binding.reflectionIndex == bindingIndex &&
            binding.arrayElement == arrayElement) {
            return writeResolvedBuffer(controller, eventId, binding,
                                       fs::path(outputDir));
        }
    }

    throw CoreError(
        CoreError::Code::TargetNotFound,
        "No descriptor binding matched stage=" + stageString(stage) +
        ", kind=" + requestedKind +
        ", reflectionIndex=" + std::to_string(bindingIndex) +
        ", arrayElement=" + std::to_string(arrayElement));
}

namespace {

json viewportJson(const ::Viewport& viewport) {
    return {
        {"enabled", viewport.enabled},
        {"x", viewport.x},
        {"y", viewport.y},
        {"width", viewport.width},
        {"height", viewport.height},
        {"minDepth", viewport.minDepth},
        {"maxDepth", viewport.maxDepth}
    };
}

json scissorJson(const ::Scissor& scissor) {
    return {
        {"enabled", scissor.enabled},
        {"x", scissor.x},
        {"y", scissor.y},
        {"width", scissor.width},
        {"height", scissor.height}
    };
}

json blendEquationJson(const BlendEquation& equation) {
    return {
        {"source", enumString(equation.source)},
        {"destination", enumString(equation.destination)},
        {"operation", enumString(equation.operation)}
    };
}

json colorBlendJson(const ColorBlend& blend) {
    return {
        {"enabled", blend.enabled},
        {"logicOperationEnabled", blend.logicOperationEnabled},
        {"logicOperation", enumString(blend.logicOperation)},
        {"writeMask", static_cast<uint32_t>(blend.writeMask)},
        {"color", blendEquationJson(blend.colorBlend)},
        {"alpha", blendEquationJson(blend.alphaBlend)}
    };
}

json stencilFaceJson(const StencilFace& stencil) {
    return {
        {"failOperation", enumString(stencil.failOperation)},
        {"depthFailOperation", enumString(stencil.depthFailOperation)},
        {"passOperation", enumString(stencil.passOperation)},
        {"function", enumString(stencil.function)},
        {"reference", stencil.reference},
        {"compareMask", stencil.compareMask},
        {"writeMask", stencil.writeMask}
    };
}

json shaderStateJson(const D3D12Pipe::Shader& shader) {
    json result = {
        {"resourceId", resourceIdString(shader.resourceId)},
        {"stage", stageString(shader.stage)}
    };
    if (shader.reflection) {
        result["entryPoint"] = std::string(shader.reflection->entryPoint.c_str());
        result["encoding"] = enumString(shader.reflection->encoding);
        result["rawByteSize"] = shader.reflection->rawBytes.size();
        result["rawChecksumAlgorithm"] = "fnv1a64";
        result["rawChecksum"] = checksum(shader.reflection->rawBytes);
    }
    return result;
}

json rootParamJson(const D3D12Pipe::RootParam& parameter) {
    json constantsWords = json::array();
    for (size_t offset = 0; offset + sizeof(uint32_t) <= parameter.constants.size();
         offset += sizeof(uint32_t)) {
        uint32_t word = 0;
        std::memcpy(&word, parameter.constants.data() + offset, sizeof(word));
        constantsWords.push_back(word);
    }

    json ranges = json::array();
    for (const auto& range : parameter.tableRanges) {
        ranges.push_back({
            {"category", enumString(range.category)},
            {"space", range.space},
            {"baseRegister", range.baseRegister},
            {"count", range.count},
            {"tableByteOffset", range.tableByteOffset},
            {"appended", range.appended}
        });
    }

    return {
        {"visibility", enumString(parameter.visibility)},
        {"space", parameter.space},
        {"register", parameter.reg},
        {"constantsByteSize", parameter.constants.size()},
        {"constantsHex", bytesToHex(parameter.constants)},
        {"constantsWords", constantsWords},
        {"rootDescriptor", descriptorJson(parameter.descriptor)},
        {"descriptorHeap", resourceIdString(parameter.heap)},
        {"descriptorHeapByteOffset", parameter.heapByteOffset},
        {"tableRanges", ranges}
    };
}

json rootSignatureJson(const D3D12Pipe::RootSignature& rootSignature) {
    json parameters = json::array();
    for (const auto& parameter : rootSignature.parameters)
        parameters.push_back(rootParamJson(parameter));

    json staticSamplers = json::array();
    for (const auto& sampler : rootSignature.staticSamplers) {
        staticSamplers.push_back({
            {"visibility", enumString(sampler.visibility)},
            {"space", sampler.space},
            {"register", sampler.reg},
            {"sampler", samplerJson(sampler.descriptor)}
        });
    }

    return {
        {"resourceId", resourceIdString(rootSignature.resourceId)},
        {"parameters", parameters},
        {"staticSamplers", staticSamplers},
        {"note", "Root-signature creation flags are not exposed by D3D12Pipe::RootSignature; "
                 "the bundle's serializedBlob field contains either the original bytes or "
                 "RenderDoc's decoded UnpackedSignature fallback."}
    };
}

void addDescriptorResources(const Descriptor& descriptor,
                            std::set<::ResourceId>& resources) {
    if (descriptor.resource != ::ResourceId::Null())
        resources.insert(descriptor.resource);
    if (descriptor.secondary != ::ResourceId::Null())
        resources.insert(descriptor.secondary);
    if (descriptor.view != ::ResourceId::Null())
        resources.insert(descriptor.view);
}

json d3d12PipelineJson(IReplayController* controller,
                       uint32_t eventId,
                       bool includeAllResourceStates) {
    const auto* state = controller->GetD3D12PipelineState();
    if (!state)
        throw CoreError(CoreError::Code::InternalError,
                        "The capture is not using D3D12.");

    std::set<::ResourceId> referencedResources;
    auto addResource = [&](::ResourceId id) {
        if (id != ::ResourceId::Null()) referencedResources.insert(id);
    };

    addResource(state->pipelineResourceId);
    addResource(state->rootSignature.resourceId);

    json descriptorHeaps = json::array();
    for (const auto& heap : state->descriptorHeaps) {
        descriptorHeaps.push_back(resourceIdString(heap));
        addResource(heap);
    }

    json layouts = json::array();
    for (const auto& layout : state->inputAssembly.layouts) {
        layouts.push_back({
            {"semanticName", std::string(layout.semanticName.c_str())},
            {"semanticIndex", layout.semanticIndex},
            {"format", formatJson(layout.format)},
            {"inputSlot", layout.inputSlot},
            {"byteOffset", layout.byteOffset},
            {"perInstance", layout.perInstance},
            {"instanceDataStepRate", layout.instanceDataStepRate}
        });
    }

    json vertexBuffers = json::array();
    for (size_t slot = 0; slot < state->inputAssembly.vertexBuffers.size(); ++slot) {
        const auto& buffer = state->inputAssembly.vertexBuffers[slot];
        if (buffer.resourceId == ::ResourceId::Null() && buffer.byteSize == 0)
            continue;
        vertexBuffers.push_back({
            {"slot", slot},
            {"resourceId", resourceIdString(buffer.resourceId)},
            {"byteOffset", buffer.byteOffset},
            {"byteSize", buffer.byteSize},
            {"byteStride", buffer.byteStride}
        });
        addResource(buffer.resourceId);
    }

    const auto& indexBuffer = state->inputAssembly.indexBuffer;
    addResource(indexBuffer.resourceId);
    json inputAssembly = {
        {"topology", enumString(state->inputAssembly.topology)},
        {"indexStripCutValue", state->inputAssembly.indexStripCutValue},
        {"layouts", layouts},
        {"vertexBuffers", vertexBuffers},
        {"indexBuffer", {
            {"resourceId", resourceIdString(indexBuffer.resourceId)},
            {"byteOffset", indexBuffer.byteOffset},
            {"byteSize", indexBuffer.byteSize},
            {"byteStride", indexBuffer.byteStride}
        }}
    };

    json shaders = json::array();
    const D3D12Pipe::Shader* shaderStages[] = {
        &state->vertexShader, &state->hullShader, &state->domainShader,
        &state->geometryShader, &state->pixelShader, &state->computeShader,
        &state->ampShader, &state->meshShader
    };
    for (const auto* shader : shaderStages) {
        if (shader->resourceId == ::ResourceId::Null()) continue;
        shaders.push_back(shaderStateJson(*shader));
        addResource(shader->resourceId);
    }

    json streamOutputs = json::array();
    for (const auto& output : state->streamOut.outputs) {
        streamOutputs.push_back({
            {"resourceId", resourceIdString(output.resourceId)},
            {"byteOffset", output.byteOffset},
            {"byteSize", output.byteSize},
            {"writtenCountResourceId", resourceIdString(output.writtenCountResourceId)},
            {"writtenCountByteOffset", output.writtenCountByteOffset}
        });
        addResource(output.resourceId);
        addResource(output.writtenCountResourceId);
    }

    json viewports = json::array();
    for (const auto& viewport : state->rasterizer.viewports)
        viewports.push_back(viewportJson(viewport));

    json scissors = json::array();
    for (const auto& scissor : state->rasterizer.scissors)
        scissors.push_back(scissorJson(scissor));

    const auto& rasterState = state->rasterizer.state;
    addResource(rasterState.shadingRateImage);
    json rasterizer = {
        {"sampleMask", state->rasterizer.sampleMask},
        {"viewports", viewports},
        {"scissors", scissors},
        {"state", {
            {"fillMode", enumString(rasterState.fillMode)},
            {"cullMode", enumString(rasterState.cullMode)},
            {"frontCCW", rasterState.frontCCW},
            {"depthBias", rasterState.depthBias},
            {"depthBiasClamp", rasterState.depthBiasClamp},
            {"slopeScaledDepthBias", rasterState.slopeScaledDepthBias},
            {"depthClip", rasterState.depthClip},
            {"lineRasterMode", enumString(rasterState.lineRasterMode)},
            {"forcedSampleCount", rasterState.forcedSampleCount},
            {"conservativeRasterization",
             enumString(rasterState.conservativeRasterization)},
            {"baseShadingRate", {rasterState.baseShadingRate.first,
                                 rasterState.baseShadingRate.second}},
            {"shadingRateCombiners", {
                enumString(rasterState.shadingRateCombiners.first),
                enumString(rasterState.shadingRateCombiners.second)}},
            {"shadingRateImage", resourceIdString(rasterState.shadingRateImage)}
        }}
    };

    const auto& depthStencil = state->outputMerger.depthStencilState;
    json depthStencilJson = {
        {"depthEnable", depthStencil.depthEnable},
        {"depthWrites", depthStencil.depthWrites},
        {"depthBoundsEnable", depthStencil.depthBoundsEnable},
        {"depthFunction", enumString(depthStencil.depthFunction)},
        {"stencilEnable", depthStencil.stencilEnable},
        {"frontFace", stencilFaceJson(depthStencil.frontFace)},
        {"backFace", stencilFaceJson(depthStencil.backFace)},
        {"minDepthBounds", depthStencil.minDepthBounds},
        {"maxDepthBounds", depthStencil.maxDepthBounds}
    };

    const auto& blend = state->outputMerger.blendState;
    json blends = json::array();
    for (const auto& targetBlend : blend.blends)
        blends.push_back(colorBlendJson(targetBlend));
    json blendJson = {
        {"alphaToCoverage", blend.alphaToCoverage},
        {"independentBlend", blend.independentBlend},
        {"blendFactor", {blend.blendFactor[0], blend.blendFactor[1],
                         blend.blendFactor[2], blend.blendFactor[3]}},
        {"targets", blends}
    };

    json renderTargets = json::array();
    for (size_t slot = 0; slot < state->outputMerger.renderTargets.size(); ++slot) {
        const auto& target = state->outputMerger.renderTargets[slot];
        if (target.resource == ::ResourceId::Null()) continue;
        renderTargets.push_back({
            {"slot", slot},
            {"descriptor", descriptorJson(target)}
        });
        addDescriptorResources(target, referencedResources);
    }
    addDescriptorResources(state->outputMerger.depthTarget, referencedResources);

    json rootSignature = rootSignatureJson(state->rootSignature);
    for (const auto& parameter : state->rootSignature.parameters) {
        addResource(parameter.heap);
        addDescriptorResources(parameter.descriptor, referencedResources);
    }

    auto resolvedBindings = resolveBindings(controller);
    for (const auto& binding : resolvedBindings) {
        addResource(binding.descriptorStore);
        if (binding.hasDescriptor)
            addDescriptorResources(binding.descriptor, referencedResources);
        if (binding.hasSampler)
            addResource(binding.sampler.object);
    }

    addResource(state->predication.resourceId);
    json predication = {
        {"resourceId", resourceIdString(state->predication.resourceId)},
        {"offset", state->predication.offset},
        {"skipIfZero", state->predication.skipIfZero}
    };

    json resourceStates = json::array();
    for (const auto& resource : state->resourceStates) {
        if (!includeAllResourceStates &&
            referencedResources.find(resource.resourceId) == referencedResources.end())
            continue;
        json states = json::array();
        for (const auto& subresourceState : resource.states)
            states.push_back(std::string(subresourceState.name.c_str()));
        resourceStates.push_back({
            {"resourceId", resourceIdString(resource.resourceId)},
            {"subresourceStates", states}
        });
    }

    return {
        {"schemaVersion", 1},
        {"api", "D3D12"},
        {"eventId", eventId},
        {"pipelineResourceId", resourceIdString(state->pipelineResourceId)},
        {"descriptorHeaps", descriptorHeaps},
        {"rootSignature", rootSignature},
        {"inputAssembly", inputAssembly},
        {"shaders", shaders},
        {"streamOut", {
            {"rasterizedStream", state->streamOut.rasterizedStream},
            {"outputs", streamOutputs}
        }},
        {"rasterizer", rasterizer},
        {"outputMerger", {
            {"depthStencil", depthStencilJson},
            {"blend", blendJson},
            {"renderTargets", renderTargets},
            {"depthTarget", descriptorJson(state->outputMerger.depthTarget)},
            {"depthReadOnly", state->outputMerger.depthReadOnly},
            {"stencilReadOnly", state->outputMerger.stencilReadOnly}
        }},
        {"predication", predication},
        {"resourceStates", resourceStates},
        {"resourceStateScope", includeAllResourceStates ? "allLiveResources"
                                                       : "pipelineAndDescriptorReferenced"}
    };
}

json structuredObjectJson(const SDObject* object) {
    if (!object) return nullptr;

    json result = {
        {"name", std::string(object->name.c_str())},
        {"typeName", std::string(object->type.name.c_str())},
        {"basicType", enumString(object->type.basetype)},
        {"byteSize", object->type.byteSize}
    };

    if (object->NumChildren() > 0) {
        json children = json::array();
        for (size_t i = 0; i < object->NumChildren(); ++i)
            children.push_back(structuredObjectJson(object->GetChild(i)));
        result["children"] = std::move(children);
    } else if (object->IsResource()) {
        result["value"] = resourceIdString(object->AsResourceId());
    } else if (object->IsString()) {
        result["value"] = std::string(object->AsString().c_str());
    } else if (object->IsFloat()) {
        result["value"] = object->AsDouble();
    } else if (object->IsInt()) {
        result["value"] = object->AsInt64();
    } else if (object->IsUInt() || object->IsEnum()) {
        result["value"] = object->AsUInt64();
        if (!object->data.str.empty())
            result["display"] = std::string(object->data.str.c_str());
    } else if (object->IsBuffer()) {
        result["bufferIndex"] = object->AsUInt64();
    } else if (object->IsNULL()) {
        result["value"] = nullptr;
    }

    return result;
}

json exportRootSignatureBlob(IReplayController* controller,
                             ::ResourceId rootSignatureId,
                             const fs::path& outputPath) {
    const SDFile& structuredFile = controller->GetStructuredFile();
    uint32_t candidateCount = 0;
    for (const SDChunk* chunk : structuredFile.chunks) {
        if (!chunk) continue;
        std::string chunkName = chunk->name.c_str();
        if (chunkName.find("CreateRootSignature") == std::string::npos)
            continue;
        ++candidateCount;

        const SDObject* signatureObject =
            chunk->FindChild("pRootSignature");
        if (!signatureObject || !signatureObject->IsResource() ||
            signatureObject->AsResourceId() != rootSignatureId)
            continue;

        const SDObject* blobObject = chunk->FindChild("pBlobWithRootSignature");
        const SDObject* lengthObject = chunk->FindChild("blobLengthInBytes");
        const SDObject* unpackedObject = chunk->FindChild("UnpackedSignature");

        json result = {
            {"available", false},
            {"semanticDescriptionAvailable", unpackedObject != nullptr},
            {"resourceId", resourceIdString(rootSignatureId)},
            {"sourceChunk", chunkName},
            {"candidateCount", candidateCount},
            {"structuredBufferCount", structuredFile.buffers.size()},
            {"decodedSignature", structuredObjectJson(unpackedObject)}
        };

        if (lengthObject)
            result["capturedBlobByteSize"] = lengthObject->AsUInt64();

        if (blobObject && blobObject->IsBuffer()) {
            uint64_t bufferIndex = blobObject->AsUInt64();
            result["structuredBufferIndex"] = bufferIndex;
            if (bufferIndex < structuredFile.buffers.size() &&
                structuredFile.buffers[bufferIndex]) {
                const bytebuf& blob = *structuredFile.buffers[bufferIndex];
                writeBytes(outputPath, blob);
                result["available"] = true;
                result["path"] = outputPath.string();
                result["byteSize"] = blob.size();
                result["checksumAlgorithm"] = "fnv1a64";
                result["checksum"] = checksum(blob);
            } else {
                result["reason"] =
                    "RenderDoc's public structured file exposes the decoded root signature "
                    "but does not retain this Important() byte buffer. Use decodedSignature "
                    "to serialize a semantically equivalent root signature.";
            }
        }

        return result;
    }

    return {
        {"available", false},
        {"semanticDescriptionAvailable", false},
        {"resourceId", resourceIdString(rootSignatureId)},
        {"reason", "No matching Device_CreateRootSignature structured chunk was found."},
        {"candidateCount", candidateCount}
    };
}

std::pair<uint64_t, std::string> checksumFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {0, ""};

    uint64_t hash = 14695981039346656037ull;
    uint64_t size = 0;
    char buffer[64 * 1024];
    while (input) {
        input.read(buffer, sizeof(buffer));
        std::streamsize count = input.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<uint8_t>(buffer[i]);
            hash *= 1099511628211ull;
        }
        size += static_cast<uint64_t>(count);
    }
    return {size, hex64(hash)};
}

json collectFileChecksums(const fs::path& root) {
    json files = json::array();
    if (!fs::exists(root)) return files;

    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        fs::path relative = fs::relative(entry.path(), root);
        if (relative == "manifest.json" || relative == "checksums.json")
            continue;
        paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());

    for (const auto& path : paths) {
        auto [size, hash] = checksumFile(path);
        files.push_back({
            {"path", fs::relative(path, root).generic_string()},
            {"byteSize", size},
            {"algorithm", "fnv1a64"},
            {"checksum", hash}
        });
    }
    return files;
}

} // anonymous namespace

nlohmann::json getD3D12PipelineStateFull(Session& session,
                                         uint32_t eventId,
                                         bool includeAllResourceStates) {
    gotoEvent(session, eventId);
    auto* controller = session.controller();
    if (controller->GetAPIProperties().pipelineType != GraphicsAPI::D3D12)
        throw CoreError(CoreError::Code::InternalError,
                        "get_d3d12_pipeline_state_full requires a D3D12 capture.");
    return d3d12PipelineJson(controller, eventId, includeAllResourceStates);
}

nlohmann::json exportDrawReconstructionBundle(
    Session& session,
    uint32_t eventId,
    std::optional<uint32_t> requestedPreEventId,
    const std::string& outputDir) {
    validateOutputDir(outputDir);
    auto* controller = session.controller();
    if (controller->GetAPIProperties().pipelineType != GraphicsAPI::D3D12)
        throw CoreError(CoreError::Code::InternalError,
                        "Draw reconstruction bundles currently require a D3D12 capture.");

    const auto& roots = controller->GetRootActions();
    const ActionDescription* action = findAction(roots, eventId);
    if (!action)
        throw CoreError(CoreError::Code::InvalidEventId,
                        "Event ID " + std::to_string(eventId) + " was not found.");
    if (!(action->flags & ActionFlags::Drawcall))
        throw CoreError(CoreError::Code::InvalidEventId,
                        "Event ID " + std::to_string(eventId) + " is not a draw call.");

    uint32_t preEventId = requestedPreEventId.value_or(0);
    if (!requestedPreEventId)
        findPreviousEvent(roots, eventId, preEventId);
    if (preEventId >= eventId)
        throw CoreError(CoreError::Code::InvalidEventId,
                        "preEventId must be lower than eventId.");

    fs::path root(outputDir);
    if (fs::exists(root)) {
        if (!fs::is_directory(root))
            throw CoreError(CoreError::Code::InvalidPath,
                            "Bundle output path exists and is not a directory: " +
                            root.string());
        if (!fs::is_empty(root))
            throw CoreError(CoreError::Code::InvalidPath,
                            "Bundle output directory must be empty to prevent stale "
                            "resources from entering the manifest: " + root.string());
    }
    fs::create_directories(root);
    fs::create_directories(root / "shaders");
    fs::create_directories(root / "resources" / "buffers");
    fs::create_directories(root / "resources" / "textures");
    fs::create_directories(root / "resources" / "root_constants");
    fs::create_directories(root / "outputs" / "pre");
    fs::create_directories(root / "outputs" / "post");

    json errors = json::array();
    json draw = actionJson(*action);
    draw["preEventId"] = preEventId;
    draw["indirectFlattening"] = {
        {"supported", true},
        {"suggestedDirectCall",
         (action->flags & ActionFlags::Indexed)
             ? "DrawIndexedInstanced"
             : "DrawInstanced"},
        {"note", "The decoded ActionDescription fields can be replayed directly when exact "
                 "ExecuteIndirect command-stream semantics are not required."}
    };
    writeJson(root / "draw.json", draw);

    // Resolve all command state and descriptors at the target draw.
    gotoEvent(session, eventId);
    json pipeline = d3d12PipelineJson(controller, eventId, false);
    writeJson(root / "pipeline_d3d12.json", pipeline);

    auto resolvedBindings = resolveBindings(controller);
    json bindings = json::array();
    for (const auto& binding : resolvedBindings)
        bindings.push_back(resolvedBindingJson(binding));
    writeJson(root / "bindings.json", {
        {"eventId", eventId},
        {"bindings", bindings},
        {"count", bindings.size()}
    });

    const auto* targetState = controller->GetD3D12PipelineState();
    json rootSignatureBlob = exportRootSignatureBlob(
        controller, targetState->rootSignature.resourceId,
        root / "root_signature.bin");
    writeJson(root / "root_signature.json", {
        {"state", rootSignatureJson(targetState->rootSignature)},
        {"serializedBlob", rootSignatureBlob}
    });
    if (!rootSignatureBlob.value("available", false) &&
        !rootSignatureBlob.value("semanticDescriptionAvailable", false)) {
        errors.push_back(
            "Neither the original root-signature blob nor its decoded structured "
            "description was found.");
    }

    // Shader binaries are independent of resource pre/post state.
    json shaderExports = json::array();
    const ShaderStage stages[] = {
        ShaderStage::Vertex, ShaderStage::Hull, ShaderStage::Domain,
        ShaderStage::Geometry, ShaderStage::Pixel, ShaderStage::Compute
    };
    for (ShaderStage stage : stages) {
        try {
            shaderExports.push_back(
                exportShaderBinary(session, eventId, stage,
                                   (root / "shaders").string()));
        } catch (const CoreError& error) {
            if (error.code() != CoreError::Code::NoShaderBound)
                errors.push_back("Shader " + stageString(stage) + ": " + error.what());
        }
    }

    // Export root constants while target root-argument state is current.
    gotoEvent(session, eventId);
    json bufferExports = json::array();
    for (const auto& binding : resolvedBindings) {
        if (binding.category != DescriptorCategory::ConstantBlock)
            continue;
        if (binding.hasDescriptor &&
            binding.descriptor.resource != ::ResourceId::Null())
            continue;
        try {
            bufferExports.push_back(writeResolvedBuffer(
                controller, eventId, binding,
                root / "resources" / "root_constants"));
        } catch (const std::exception& error) {
            errors.push_back("Root constants '" + binding.name + "': " + error.what());
        }
    }

    // Resource contents used by the draw must be captured from the pre-draw state.
    gotoEvent(session, preEventId);
    std::set<::ResourceId> exportedTextures;
    std::set<std::tuple<::ResourceId, uint64_t, uint64_t>> exportedBufferRanges;
    json textureExports = json::array();

    for (const auto& binding : resolvedBindings) {
        if (!binding.hasDescriptor ||
            binding.descriptor.resource == ::ResourceId::Null())
            continue;

        ::ResourceId resourceId = binding.descriptor.resource;
        if (findTexture(controller, resourceId)) {
            if (!exportedTextures.insert(resourceId).second)
                continue;
            try {
                fs::path textureDir =
                    root / "resources" / "textures" /
                    ("ResourceId__" + std::to_string(toResourceId(resourceId)));
                textureExports.push_back(exportTextureRawAtCurrentEvent(
                    controller, preEventId, resourceId, textureDir,
                    true, 0, 0, 0));
            } catch (const std::exception& error) {
                errors.push_back("Texture " + resourceIdString(resourceId) +
                                 ": " + error.what());
            }
        } else if (findBuffer(controller, resourceId)) {
            uint64_t offset = 0;
            uint64_t size = 0;
            if (!getResolvedBufferRange(controller, binding, offset, size))
                continue;
            auto key = std::make_tuple(resourceId, offset, size);
            if (!exportedBufferRanges.insert(key).second)
                continue;
            try {
                bufferExports.push_back(writeResolvedBuffer(
                    controller, preEventId, binding,
                    root / "resources" / "buffers"));
            } catch (const std::exception& error) {
                errors.push_back("Buffer '" + binding.name + "': " + error.what());
            }
        }
    }

    // Capture render/depth outputs before and after the draw.
    json preOutputs = json::array();
    json postOutputs = json::array();
    std::set<::ResourceId> outputResources;
    for (const auto& output : action->outputs)
        if (output != ::ResourceId::Null())
            outputResources.insert(output);
    if (action->depthOut != ::ResourceId::Null())
        outputResources.insert(action->depthOut);

    gotoEvent(session, preEventId);
    for (const auto& resourceId : outputResources) {
        try {
            fs::path resourceDir =
                root / "outputs" / "pre" /
                ("ResourceId__" + std::to_string(toResourceId(resourceId)));
            preOutputs.push_back(exportTextureRawAtCurrentEvent(
                controller, preEventId, resourceId, resourceDir,
                true, 0, 0, 0));
        } catch (const std::exception& error) {
            errors.push_back("Pre-output " + resourceIdString(resourceId) +
                             ": " + error.what());
        }
    }

    gotoEvent(session, eventId);
    for (const auto& resourceId : outputResources) {
        try {
            fs::path resourceDir =
                root / "outputs" / "post" /
                ("ResourceId__" + std::to_string(toResourceId(resourceId)));
            postOutputs.push_back(exportTextureRawAtCurrentEvent(
                controller, eventId, resourceId, resourceDir,
                true, 0, 0, 0));
        } catch (const std::exception& error) {
            errors.push_back("Post-output " + resourceIdString(resourceId) +
                             ": " + error.what());
        }
    }

    json checksums = {
        {"algorithm", "fnv1a64"},
        {"files", collectFileChecksums(root)}
    };
    writeJson(root / "checksums.json", checksums);

    json fileList = json::array();
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() &&
            entry.path().filename() != "manifest.json") {
            fileList.push_back(fs::relative(entry.path(), root).generic_string());
        }
    }
    std::sort(fileList.begin(), fileList.end());

    json manifest = {
        {"schemaVersion", 1},
        {"capturePath", session.capturePath()},
        {"api", "D3D12"},
        {"eventId", eventId},
        {"preEventId", preEventId},
        {"stateTiming", {
            {"inputs", "immediatelyAfterPreEvent"},
            {"pipelineAndBindings", "immediatelyAfterDrawEvent"},
            {"referenceOutputs", "immediatelyAfterDrawEvent"}
        }},
        {"draw", "draw.json"},
        {"pipeline", "pipeline_d3d12.json"},
        {"bindings", "bindings.json"},
        {"rootSignature", "root_signature.json"},
        {"shaders", shaderExports},
        {"buffers", bufferExports},
        {"textures", textureExports},
        {"preOutputs", preOutputs},
        {"postOutputs", postOutputs},
        {"checksums", "checksums.json"},
        {"files", fileList},
        {"errors", errors},
        {"complete", errors.empty()}
    };
    writeJson(root / "manifest.json", manifest);

    gotoEvent(session, eventId);

    return {
        {"eventId", eventId},
        {"preEventId", preEventId},
        {"outputDir", root.string()},
        {"manifestPath", (root / "manifest.json").string()},
        {"fileCount", fileList.size() + 1},
        {"shaderCount", shaderExports.size()},
        {"bufferCount", bufferExports.size()},
        {"textureCount", textureExports.size()},
        {"preOutputCount", preOutputs.size()},
        {"postOutputCount", postOutputs.size()},
        {"complete", errors.empty()},
        {"errors", errors}
    };
}

} // namespace renderdoc::core
