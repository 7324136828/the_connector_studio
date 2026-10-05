#pragma once
#include "ConnectorClient.h"
#include <memory>
#include <string>
#include <vector>

namespace Lattice::Mcp
{
    // A successfully negotiated MCP connection. Releasing it disconnects without
    // waiting on the GUI thread; owned stdio processes are stopped in the background.
    class Connection
    {
    public:
        ~Connection();
        void Disconnect();
        bool IsAlive() const;
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
    private:
        struct State;
        explicit Connection(std::shared_ptr<State> state);
        std::shared_ptr<State> m_state;
        friend struct ClientAccess;
    };
    struct ConnectionResult
    {
        bool success = false;
        bool cancelled = false;
        std::wstring error;
        std::shared_ptr<Connection> connection;
    };

    // Worker-facing, bounded handshake only. No tools are invoked and failed
    // connections are not restarted. The user can explicitly enable again.
    ConnectionResult Connect(const std::wstring& folder,
        std::shared_ptr<Connector::Cancellation> cancellation = {},
        const std::wstring& serverKey = {});
    // Read-only discovery of named entries in servers/mcpServers. Missing or
    // invalid configuration returns an empty list; this never starts a server.
    std::vector<std::wstring> ServerKeys(const std::wstring& folder);
}
