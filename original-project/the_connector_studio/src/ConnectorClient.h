#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <utility>

namespace Lattice::Connector
{
    struct Settings
    {
        std::wstring serverUrl = L"http://127.0.0.1:8301";
        float chatFontSize = 11.5f;
    };
    struct Model { std::wstring id; std::wstring name; };
    struct ChatMessage { std::wstring role; std::wstring content; };
    class Cancellation
    {
    public:
        void Cancel() { m_cancelled.store(true); }
        bool IsCancelled() const { return m_cancelled.load(); }
    private:
        std::atomic<bool> m_cancelled{false};
    };
    struct DiscoveryResult
    {
        bool success = false;
        bool cancelled = false;
        std::vector<Model> models;
        std::wstring error;
    };
    struct CompletionResult
    {
        bool success = false;
        bool cancelled = false;
        std::wstring text;
        std::wstring error;
    };
    struct HttpResult
    {
        bool success = false, cancelled = false;
        unsigned long status = 0;
        std::string body;
        std::wstring error, contentType, mcpSessionId;
    };
    // Worker-only transport for an absolute HTTP endpoint. stopAfter may finish
    // an SSE response once its matching JSON-RPC result has arrived.
    HttpResult ExchangeJson(const std::wstring& url, const std::wstring& method,
        const std::string& body, const std::vector<std::pair<std::wstring, std::wstring>>& headers = {},
        const std::shared_ptr<Cancellation>& cancel = {}, unsigned timeoutSeconds = 20,
        const std::function<bool(const std::string&)>& stopAfter = {});

    // Accepts a server root or a /v1 base. Discovery/completions use its /v1 API.
    bool NormalizeServerUrl(const std::wstring& input, std::wstring& normalized, std::wstring& error);
    std::wstring ApiBaseUrl(const std::wstring& normalized);
    std::wstring DefaultSettingsPath();
    bool SaveSettings(const std::wstring& path, const Settings& settings, std::wstring& error);
    bool LoadSettings(const std::wstring& path, Settings& settings, std::wstring& error);

    // Blocking worker-facing functions. Internally asynchronous WinHTTP allows
    // cancellation without closing a synchronous request from another thread.
    DiscoveryResult DiscoverModels(const Settings& settings, const std::shared_ptr<Cancellation>& cancel = {});
    CompletionResult Complete(const Settings& settings, const std::wstring& model,
        const std::vector<ChatMessage>& messages, const std::shared_ptr<Cancellation>& cancel = {});
}
