#include "mcp/mcp_server.h"
#include "mcp/tool_registry.h"
#include "core/session.h"
#include "core/diff_session.h"
#include "core/errors.h"
#include <stdexcept>

using json = nlohmann::json;

namespace renderdoc::mcp {

// ── Injection constructor ──────────────────────────────────────────────────

McpServer::McpServer(core::Session& session, core::DiffSession& diffSession, ToolRegistry& registry)
    : m_session(&session)
    , m_diffSession(&diffSession)
    , m_registry(&registry)
    , m_initializeReceived(false)
    , m_initialized(false)
{
}

McpServer::~McpServer() = default;

void McpServer::shutdown()
{
    if(m_session)
        m_session->close();
    if(m_diffSession)
        m_diffSession->close();
}

// ── JSON-RPC helpers ────────────────────────────────────────────────────────

json McpServer::makeResponse(const json& id, const json& result)
{
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = result;
    return resp;
}

json McpServer::makeError(const json& id, int code, const std::string& message)
{
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["error"]["code"] = code;
    resp["error"]["message"] = message;
    return resp;
}

json McpServer::makeToolResult(const json& data, bool isError)
{
    json result;
    json content;
    content["type"] = "text";

    if(data.is_string())
        content["text"] = data.get<std::string>();
    else
    {
        content["text"] = data.dump();
        if(data.is_object())
            result["structuredContent"] = data;
    }

    result["content"] = json::array({content});
    if(isError)
        result["isError"] = true;
    return result;
}

// ── Message dispatch ────────────────────────────────────────────────────────

json McpServer::handleMessage(const json& msg)
{
    // Check for valid JSON-RPC 2.0
    if(!msg.contains("jsonrpc") || msg["jsonrpc"] != "2.0")
        return makeError(msg.value("id", json(nullptr)), -32600, "Invalid Request: missing jsonrpc 2.0");

    std::string method = msg.value("method", "");
    bool isNotification = !msg.contains("id");
    json id = msg.value("id", json(nullptr));

    // Route methods
    if(method == "initialize")
    {
        if(m_initializeReceived)
            return makeError(id, -32600, "Server already initialized");
        return handleInitialize(msg);
    }
    else if(method == "notifications/initialized")
    {
        if(m_initializeReceived)
            m_initialized = true;
        return nullptr;  // No response for notifications
    }
    else if(method == "ping")
    {
        if(isNotification)
            return nullptr;
        return makeResponse(id, json::object());
    }
    else if(method == "shutdown")
    {
        shutdown();
        return makeResponse(id, json::object());
    }
    else if(method == "tools/list" || method == "tools/call")
    {
        if(!m_initialized && !isNotification)
            return makeError(id, -32002, "Server not initialized");
        if(method == "tools/list")
            return handleToolsList(msg);
        return handleToolsCall(msg);
    }
    else if(isNotification)
        return nullptr;  // Unknown notifications are silently ignored
    else
        return makeError(id, -32601, "Method not found: " + method);
}

json McpServer::handleBatch(const json& arr)
{
    // JSON-RPC batching was removed from MCP in 2025-06-18. Preserve it only
    // for a session that explicitly negotiated the 2025-03-26 revision.
    if(m_protocolVersion != "2025-03-26")
        return makeError(
            nullptr, -32600,
            "Invalid Request: JSON-RPC batching is not supported by the negotiated MCP protocol");

    if(!arr.is_array() || arr.empty())
        return makeError(nullptr, -32600, "Invalid Request: batch must be a non-empty array");

    // initialize must always be a standalone request, including in 2025-03-26.
    for(const auto& msg : arr)
    {
        if(msg.is_object() && msg.value("method", "") == "initialize")
            return makeError(nullptr, -32600, "Invalid Request: initialize must not appear in a JSON-RPC batch");
    }

    json responses = json::array();
    for(const auto& msg : arr)
    {
        if(!msg.is_object())
        {
            responses.push_back(makeError(nullptr, -32600, "Invalid Request: batch element is not an object"));
            continue;
        }
        json resp = handleMessage(msg);
        if(!resp.is_null())
            responses.push_back(resp);
    }

    // If all were notifications, return nothing
    if(responses.empty())
        return nullptr;

    return responses;
}

// ── MCP method handlers ─────────────────────────────────────────────────────

json McpServer::handleInitialize(const json& msg)
{
    json id = msg.value("id", json(nullptr));

    // Negotiate the newest mutually supported protocol version. MCP requires
    // servers to echo a supported client version, or advertise their latest
    // supported version so the client can decide whether to continue.
    static constexpr const char* kLatestProtocolVersion = "2025-11-25";
    static constexpr const char* kPreviousProtocolVersion = "2025-06-18";
    static constexpr const char* kLegacyProtocolVersion = "2025-03-26";
    std::string negotiatedVersion = kLatestProtocolVersion;

    json params = msg.value("params", json::object());
    if(!params.is_object())
        return makeError(id, -32602, "Invalid params: initialize params must be an object");

    if (params.contains("protocolVersion")) {
        if(!params["protocolVersion"].is_string())
            return makeError(id, -32602, "Invalid params: protocolVersion must be a string");
        std::string clientVersion = params["protocolVersion"].get<std::string>();
        if (clientVersion == kLatestProtocolVersion ||
            clientVersion == kPreviousProtocolVersion ||
            clientVersion == kLegacyProtocolVersion)
            negotiatedVersion = clientVersion;
    }

    m_protocolVersion = negotiatedVersion;
    m_initializeReceived = true;

    json result;
    result["protocolVersion"] = negotiatedVersion;
    result["capabilities"]["tools"] = json::object();
    result["serverInfo"]["name"] = "renderdoc-mcp";
    result["serverInfo"]["title"] = "RenderDoc MCP";
    result["serverInfo"]["version"] = "1.0.0";
    result["serverInfo"]["description"] =
        "Inspect, debug, compare, export, and reconstruct RenderDoc GPU captures.";
    result["serverInfo"]["websiteUrl"] =
        "https://github.com/JiaboLi-GitHub/renderdoc-mcp";
    result["instructions"] =
        "Open a RenderDoc capture before calling capture-dependent tools.";

    return makeResponse(id, result);
}

json McpServer::handleToolsList(const json& msg)
{
    json id = msg.value("id", json(nullptr));
    json result;
    result["tools"] = m_registry->getToolDefinitions();
    return makeResponse(id, result);
}

json McpServer::handleToolsCall(const json& msg)
{
    json id = msg.value("id", json(nullptr));
    json params = msg.value("params", json::object());
    if(!params.is_object())
        return makeError(id, -32602, "Invalid params: tools/call params must be an object");

    if(!params.contains("name") || !params["name"].is_string() ||
       params["name"].get_ref<const std::string&>().empty())
        return makeError(id, -32602, "Invalid params: missing tool name");

    std::string toolName = params["name"].get<std::string>();
    json arguments =
        params.contains("arguments") ? params["arguments"] : json::object();

    try
    {
        ToolContext ctx{*m_session, *m_diffSession};
        json rawResult = m_registry->callTool(toolName, ctx, arguments);
        return makeResponse(id, makeToolResult(rawResult));
    }
    catch(const UnknownToolError& e)
    {
        // Finding a tool is a protocol-level failure.
        return makeError(id, -32602, std::string("Invalid params: ") + e.what());
    }
    catch(const InvalidParamsError& e)
    {
        // MCP 2025-11-25 clarifies that tool input validation failures are tool
        // execution errors so the model can inspect and correct the arguments.
        // Preserve the older protocol-level behavior for negotiated revisions.
        if(m_protocolVersion == "2025-11-25")
            return makeResponse(
                id, makeToolResult(std::string("Invalid arguments: ") + e.what(), true));
        return makeError(id, -32602, std::string("Invalid params: ") + e.what());
    }
    catch(const core::CoreError& e)
    {
        // Core-level error: no capture open, invalid event id, etc.
        return makeResponse(id, makeToolResult(std::string(e.what()), true));
    }
    catch(const std::exception& e)
    {
        // Tool-level error: renderdoc API failure, etc.
        return makeResponse(id, makeToolResult(std::string(e.what()), true));
    }
}

} // namespace renderdoc::mcp
