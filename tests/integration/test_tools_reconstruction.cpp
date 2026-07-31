#include <gtest/gtest.h>

#include "core/diff_session.h"
#include "core/session.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tools.h"

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

using renderdoc::core::DiffSession;
using renderdoc::core::Session;
using renderdoc::mcp::ToolContext;
using renderdoc::mcp::ToolRegistry;
namespace tools = renderdoc::mcp::tools;
namespace fs = std::filesystem;

#ifdef _WIN32
static void openReconstructionCaptureImpl(Session* session);

#pragma warning(push)
#pragma warning(disable: 4611)
static bool openReconstructionCaptureSEH(Session* session) {
    __try {
        openReconstructionCaptureImpl(session);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#pragma warning(pop)

static void openReconstructionCaptureImpl(Session* session) {
    session->open(TEST_RDC_PATH);
}
#endif

class ReconstructionToolTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        tools::registerSessionTools(registry);
        tools::registerEventTools(registry);
        tools::registerResourceTools(registry);
        tools::registerReconstructionTools(registry);

#ifdef _WIN32
        if (!openReconstructionCaptureSEH(&session)) {
            skipAll = true;
            return;
        }
#else
        session.open(TEST_RDC_PATH);
#endif

        auto draws = registry.callTool(
            "list_draws", ToolContext{session, diffSession}, {});
        ASSERT_TRUE(draws.contains("draws"));
        ASSERT_FALSE(draws["draws"].empty());
        firstDraw = draws["draws"][0]["eventId"].get<uint32_t>();

        auto resources = registry.callTool(
            "list_resources", ToolContext{session, diffSession},
            {{"type", "Texture"}});
        if (resources.contains("resources") && !resources["resources"].empty())
            textureId = resources["resources"][0]["resourceId"].get<std::string>();
    }

    static void TearDownTestSuite() {
        session.close();
    }

    void SetUp() override {
        if (skipAll)
            GTEST_SKIP() << "RenderDoc replay not available";
    }

    static Session session;
    static DiffSession diffSession;
    static ToolRegistry registry;
    static uint32_t firstDraw;
    static std::string textureId;
    static bool skipAll;
};

Session ReconstructionToolTest::session;
DiffSession ReconstructionToolTest::diffSession;
ToolRegistry ReconstructionToolTest::registry;
uint32_t ReconstructionToolTest::firstDraw = 0;
std::string ReconstructionToolTest::textureId;
bool ReconstructionToolTest::skipAll = false;

TEST(ReconstructionToolRegistrationTest, RegistersP0Tools) {
    ToolRegistry localRegistry;
    tools::registerReconstructionTools(localRegistry);

    EXPECT_TRUE(localRegistry.hasTool("export_shader_binary"));
    EXPECT_TRUE(localRegistry.hasTool("get_descriptor_bindings"));
    EXPECT_TRUE(localRegistry.hasTool("export_texture_raw"));
    EXPECT_TRUE(localRegistry.hasTool("export_bound_buffer"));
    EXPECT_TRUE(localRegistry.hasTool("get_d3d12_pipeline_state_full"));
    EXPECT_TRUE(localRegistry.hasTool("export_draw_reconstruction_bundle"));

    const auto definitions = localRegistry.getToolDefinitions();
    ASSERT_EQ(definitions.size(), 6u);
    for (const auto& definition : definitions) {
        EXPECT_TRUE(definition.contains("name"));
        EXPECT_TRUE(definition.contains("description"));
        EXPECT_TRUE(definition.contains("inputSchema"));
        EXPECT_EQ(definition["inputSchema"]["type"], "object");
    }
}

TEST_F(ReconstructionToolTest, DescriptorBindingsReturnResolvedSchema) {
    auto result = registry.callTool(
        "get_descriptor_bindings", ToolContext{session, diffSession},
        {{"eventId", firstDraw}});
    EXPECT_EQ(result["eventId"], firstDraw);
    EXPECT_TRUE(result.contains("bindings"));
    EXPECT_TRUE(result["bindings"].is_array());
    EXPECT_EQ(result["count"], result["bindings"].size());
}

TEST_F(ReconstructionToolTest, ShaderBinaryWritesOriginalBytes) {
    fs::path output =
        fs::temp_directory_path() / "renderdoc_mcp_shader_binary_test";
    fs::remove_all(output);

    auto result = registry.callTool(
        "export_shader_binary", ToolContext{session, diffSession},
        {{"eventId", firstDraw}, {"stage", "vs"},
         {"outputDir", output.string()}});

    EXPECT_GT(result["byteSize"].get<uint64_t>(), 0u);
    EXPECT_TRUE(fs::exists(result["path"].get<std::string>()));
    EXPECT_TRUE(result.contains("encoding"));
    EXPECT_TRUE(result.contains("checksum"));

    fs::remove_all(output);
}

TEST_F(ReconstructionToolTest, RawTextureWritesMetadataAndBytes) {
    if (textureId.empty())
        GTEST_SKIP() << "No texture resource found";

    fs::path output =
        fs::temp_directory_path() / "renderdoc_mcp_raw_texture_test";
    fs::remove_all(output);

    auto result = registry.callTool(
        "export_texture_raw", ToolContext{session, diffSession},
        {{"eventId", firstDraw}, {"resourceId", textureId},
         {"outputDir", output.string()}, {"allSubresources", false}});

    EXPECT_EQ(result["subresourceCount"], 1);
    EXPECT_TRUE(fs::exists(output / "metadata.json"));
    EXPECT_GT(result["byteSize"].get<uint64_t>(), 0u);

    fs::remove_all(output);
}
