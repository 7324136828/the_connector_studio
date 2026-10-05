#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ConnectorTest
{
    struct Request
    {
        std::string method, path, body;
        std::map<std::string, std::string> headers;
    };
    struct Response
    {
        int status = 200;
        std::string body;
        int delayMs = 0;
        std::map<std::string, std::string> headers;
        bool streaming = false;
        int keepOpenMs = 0;
    };
    class MockConnectorServer
    {
    public:
        using Handler = std::function<Response(const Request&)>;
        explicit MockConnectorServer(Handler handler = {}) : m_handler(std::move(handler))
        {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed");
            m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (m_listen == INVALID_SOCKET) Fail();
            BOOL exclusive = TRUE;
            setsockopt(m_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
            sockaddr_in address{};
            address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(m_listen, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(m_listen, SOMAXCONN) == SOCKET_ERROR) Fail();
            int size = sizeof(address);
            if (getsockname(m_listen, reinterpret_cast<sockaddr*>(&address), &size) == SOCKET_ERROR) Fail();
            m_url = L"http://127.0.0.1:" + std::to_wstring(ntohs(address.sin_port));
            m_accept = std::thread([this] { Accept(); });
        }
        MockConnectorServer(const MockConnectorServer&) = delete;
        MockConnectorServer& operator=(const MockConnectorServer&) = delete;
        ~MockConnectorServer()
        {
            m_stop.store(true);
            if (m_accept.joinable()) m_accept.join();
            if (m_listen != INVALID_SOCKET) closesocket(m_listen);
            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                for (const auto client : m_clients) shutdown(client, SD_BOTH);
            }
            for (auto& thread : m_workers) if (thread.joinable()) thread.join();
            WSACleanup();
        }
        const std::wstring& Url() const { return m_url; }
        std::vector<Request> Requests() const
        { std::lock_guard<std::mutex> lock(m_mutex); return m_requests; }
        std::size_t RequestCount() const
        { std::lock_guard<std::mutex> lock(m_mutex); return m_requests.size(); }
        bool WaitForRequests(std::size_t count, int timeoutMs = 3000)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_received.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return m_requests.size() >= count; });
        }
        void SetHandler(Handler handler)
        { std::lock_guard<std::mutex> lock(m_mutex); m_handler = std::move(handler); }
        static Response Default(const Request& request)
        {
            if (request.method == "GET" && request.path == "/v1/models")
                return {200, R"({"object":"list","data":[{"id":"studio-test","name":"Studio test"},{"id":"second-model"}]})", 0, {}};
            if (request.method == "POST" && request.path == "/v1/chat/completions")
                return {200, R"({"choices":[{"message":{"role":"assistant","content":"Connector test reply"},"finish_reason":"stop"}]})", 0, {}};
            return {404, R"({"error":{"message":"Missing test endpoint"}})", 0, {}};
        }
    private:
        SOCKET m_listen = INVALID_SOCKET;
        std::wstring m_url;
        std::atomic<bool> m_stop{false};
        std::thread m_accept;
        std::vector<std::thread> m_workers;
        std::vector<SOCKET> m_clients;
        mutable std::mutex m_mutex, m_clientsMutex;
        std::condition_variable m_received;
        std::vector<Request> m_requests;
        Handler m_handler;
        [[noreturn]] void Fail()
        { if (m_listen != INVALID_SOCKET) closesocket(m_listen); WSACleanup(); throw std::runtime_error("Native mock listener failed"); }
        void Accept()
        {
            while (!m_stop.load())
            {
                fd_set ready; FD_ZERO(&ready); FD_SET(m_listen, &ready);
                timeval timeout{0, 50000};
                const int selected = select(0, &ready, nullptr, nullptr, &timeout);
                if (selected <= 0) continue;
                const auto client = accept(m_listen, nullptr, nullptr);
                if (client == INVALID_SOCKET) continue;
                if (m_stop.load()) { closesocket(client); break; }
                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex); m_clients.push_back(client);
                }
                m_workers.emplace_back([this, client] {
                    Serve(client);
                    {
                        std::lock_guard<std::mutex> lock(m_clientsMutex);
                        m_clients.erase(std::remove(m_clients.begin(), m_clients.end(), client), m_clients.end());
                    }
                    closesocket(client);
                });
            }
        }
        bool ReadRequest(SOCKET client, Request& request)
        {
            const DWORD timeout = 200;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            std::string bytes;
            char buffer[8192];
            std::size_t boundary = std::string::npos, contentLength = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!m_stop.load() && std::chrono::steady_clock::now() < deadline)
            {
                const int read = recv(client, buffer, sizeof(buffer), 0);
                if (!read) return false;
                if (read < 0) { if (WSAGetLastError() == WSAETIMEDOUT) continue; return false; }
                bytes.append(buffer, static_cast<std::size_t>(read));
                if (bytes.size() > 17 * 1024 * 1024) return false;
                if (boundary == std::string::npos)
                {
                    boundary = bytes.find("\r\n\r\n");
                    if (boundary == std::string::npos) { if (bytes.size() > 65536) return false; continue; }
                    std::istringstream header(bytes.substr(0, boundary));
                    std::string first; std::getline(header, first);
                    std::istringstream line(first); line >> request.method >> request.path;
                    for (std::string field; std::getline(header, field);)
                    {
                        const auto colon = field.find(':'); if (colon == std::string::npos) continue;
                        auto key = field.substr(0, colon), value = field.substr(colon + 1);
                        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
                        if (!value.empty() && value.back() == '\r') value.pop_back();
                        request.headers[key] = value;
                    }
                    if (request.headers.count("content-length"))
                    { try { contentLength = std::stoull(request.headers["content-length"]); } catch (...) { return false; } }
                    if (contentLength > 16 * 1024 * 1024) return false;
                }
                if (bytes.size() >= boundary + 4 + contentLength)
                { request.body = bytes.substr(boundary + 4, contentLength); return true; }
            }
            return false;
        }
        void Serve(SOCKET client)
        {
            Request request;
            if (!ReadRequest(client, request)) return;
            Handler handler;
            {
                std::lock_guard<std::mutex> lock(m_mutex); m_requests.push_back(request); handler = m_handler;
            }
            m_received.notify_all();
            Response response;
            try { response = handler ? handler(request) : Default(request); }
            catch (...) { response = {500, R"({"error":{"message":"Fixture handler error"}})", 0, {}}; }
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(response.delayMs);
            while (!m_stop.load() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (m_stop.load()) return;
            bool customContentType = false;
            for (const auto& header : response.headers)
            {
                auto name = header.first;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (name == "content-type") customContentType = true;
            }
            std::string bytes = "HTTP/1.1 " + std::to_string(response.status) + " Test\r\n";
            if (!customContentType) bytes += "Content-Type: application/json\r\n";
            if (!response.streaming) bytes += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
            bytes += "Connection: close\r\n";
            for (const auto& header : response.headers) bytes += header.first + ": " + header.second + "\r\n";
            bytes += "\r\n" + response.body;
            std::size_t sent = 0;
            while (sent < bytes.size() && !m_stop.load())
            {
                const int count = send(client, bytes.data() + sent, static_cast<int>((std::min)(bytes.size() - sent, static_cast<std::size_t>(65536))), 0);
                if (count <= 0) return;
                sent += static_cast<std::size_t>(count);
            }
            const auto keepUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(response.keepOpenMs);
            while (!m_stop.load() && std::chrono::steady_clock::now() < keepUntil) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
}
