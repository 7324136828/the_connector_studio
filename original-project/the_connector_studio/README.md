# Connector Studio

Connector Studio is a native C++17 Windows desktop adaptation of the design in `original-project/`. The application uses Win32, Direct2D, DirectWrite and WinHTTP. Its executable uses Windows system libraries, with no Python, Node.js, React or browser-engine runtime. Networked chat connects to a separately managed Connector server.

The dark workspace keeps the reference design's activity bar, explorer and session sidebars, tabs, conversation view, plan cards, composer, purple accents and custom menus. Layout and input handling use device-independent coordinates so native controls and drawn controls stay aligned when Windows display scaling changes.

## Run

Use Windows 10 version 1703 or newer, or Windows 11, on x64. After building, run:

```powershell
.\build\x64\Release\ConnectorStudio.exe
```

Alternatively, extract the portable ZIP from `dist/` and open `ConnectorStudio\ConnectorStudio.exe`. Release builds statically link the MSVC runtime; a separate Visual C++ Redistributable installation is not required. The application runs as the current user.

Start Connector separately, then open the Settings gear in Studio. The default server URL is `http://127.0.0.1:8301`; an HTTP/HTTPS server root or `/v1` base URL is accepted. **Test Connection** asynchronously checks model discovery without saving the edited settings or sending a completion request. **Save** persists the normalized URL and **Chat font size** in `%LOCALAPPDATA%\ConnectorStudio\connection.json`. Cancel leaves the saved settings unchanged. Font choices range from the original 11.5 through 24; chat Markdown and the composer resize together. A font-only change preserves drafts, selected models and pending replies. Select an active configuration from the model menu before sending a message; raw upstream model names are available only when Connector exposes them as active configuration IDs.

Studio discovers models on launch and when connection settings are saved. A font-only save leaves the existing model list in place. Click the model selector in the composer or Session sidebar to open a list of the active models retrieved from Connector. The menu remains visible while models are loading and when discovery fails or returns an empty list, with **Refresh models** and **Connection settings** actions. Opening a disconnected model selector retries discovery; Refresh models reloads the current server's list. Long lists scroll with the mouse wheel, scrollbar or keyboard navigation. Only successfully discovered, available model rows can be selected; status rows are disabled. An empty model list means the server has no active configuration available for chat.

A disconnected server or unavailable selection leaves the draft intact and shows a connection/request error. Completions run in background workers and return a full text response. The send button becomes a square Stop control while a reply is pending; clicking it cancels that session's request. Each reply belongs to the submitting session when tabs change. Failure or cancellation removes the submitted pending entry and restores its text alongside any newer draft; closing its session discards late results. Changing the server URL cancels pending requests and refreshes model discovery. Saving during a request writes the real user message; a later reply marks the session changed again.

## Build and verify

Install Visual Studio 2022/2026 or Visual Studio Build Tools with **Desktop development with C++** and a Windows 10/11 SDK. Tests also use CMake, available through the **C++ CMake tools for Windows** component.

The PowerShell build script locates Visual Studio with `vswhere`, chooses the installed toolset, and works from an ordinary PowerShell prompt. Builds default to two parallel jobs; use `-MaxParallelJobs 1` for a sequential build or choose a higher limit when appropriate:

```powershell
.\scripts\build.ps1 -Configuration Debug -RunTests
.\scripts\build.ps1 -Configuration Release -RunTests
```

Application binaries are written to `build\x64\Debug\` and `build\x64\Release\`. The thirteen native CTest suites cover session serialization, controller behavior, layout/hit testing, real Win32 interactions, item registration, project storage, native dialogs, Connector HTTP transport, conversation ownership, project resource discovery, Markdown and MCP connections. Network regressions use local mock servers and an owned native stdio fixture; they do not invoke paid providers or download command packages. Checks remain active in Release test builds. Each suite receives a separate working directory and `CONNECTOR_STUDIO_TEST_ROOT` under the CMake build directory's `test-workspaces/` folder.

To build while an existing application binary is running, choose a separate output directory inside the repository. This also isolates CMake state and test fixtures:

```powershell
.\scripts\build.ps1 -Configuration Debug -RunTests -OutputRoot build\x64\agent-font-copy
.\scripts\build.ps1 -Configuration Release -RunTests -OutputRoot build\x64\agent-font-copy
```

These commands write binaries to `build\x64\agent-font-copy\Debug\` and `build\x64\agent-font-copy\Release\`.

Open `ConnectorStudio.sln` for Visual Studio development. The solution builds the application; the scripts/CMake also build tests. When opening it in Visual Studio 2022, select the installed v143 platform toolset in project properties. For a direct MSBuild command from a Developer PowerShell prompt:

```powershell
msbuild .\ConnectorStudio.sln /m:2 /p:Configuration=Release /p:Platform=x64
```

Add `/p:PlatformToolset=v143` when using Visual Studio 2022. The PowerShell build script handles this choice automatically.

CMake/Ninja presets are available from an **x64 Developer PowerShell** prompt with CMake and Ninja on `PATH`:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug

cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

## Package

The packaging script builds Release, runs the tests, then creates a ZIP containing the executable, README, license, third-party notices and package metadata. It writes a SHA-256 checksum beside the archive:

```powershell
.\scripts\package.ps1
```

After a verified Release build, use `-SkipBuild` to package that binary, or `-IncludeSymbols` to include the PDB. Distribution artifacts are placed in `dist/`. Fresh staging directories ensure test executables and stale build files are excluded. The package is portable and unsigned; installer distribution, signing and automatic updates are not implemented.

Use an explicit executable path when packaging a verified isolated build:

```powershell
.\scripts\package.ps1 -SkipBuild -ExecutablePath build\x64\agent-font-copy\Release\ConnectorStudio.exe
```

## Working with projects and sessions

The application starts with an empty workspace: no project, file tree, session tabs or conversation history. Choose **Project > New Project** to open the native dark New dialog. Its **Files**, **Sessions** and **Projects** tabs list registered types, accept a name and location, and create the selected item. The dialog can also open an existing or recent project folder. Creating a project keeps the conversation area empty until a session is explicitly created or opened.

**Work Project** creates a named folder containing `project.connector`, `files/` and `sessions/`. **Text File** creates an empty file in the project's `files/` folder and refreshes the explorer. **Work Session** creates a saved, blank `.lattice` session in `sessions/` and opens its tab. Files and sessions require a managed Work Project location. Existing names are rejected without overwriting them. Opening a Work Project loads its saved sessions; opening an ordinary folder displays its files without creating a chat. The explorer's disclosure controls expand/collapse folders. Item metadata uses `.connector-item` sidecar files, which are hidden from the explorer.

The **Project** menu also closes the current project or shows recent projects. **Session > New Session** creates a blank, unsaved chat; **Session > Open Session** loads an individual saved conversation. The Session menu saves and closes chat sessions. Closing the last session removes the conversation controls while an explicitly opened project can remain in the explorer.

Each session has its own conversation, draft, model selection, attachments and scroll position. Switching sessions preserves unsent drafts. Closing a changed session asks whether to save it. Open and Save use native Windows dialogs and versioned `.lattice` files containing UTF-8 text, structured messages/plan steps, project/context paths and preferences. Saves use atomic replacement; loads validate size bounds, version and checksum before changing state. Failed or corrupt file loads preserve the current session. Save sessions explicitly with Ctrl+S; conversations are not automatically restored after an application restart. Legacy text/JSON exports cannot be imported as sessions.

Recent project paths are stored in `%LOCALAPPDATA%\ConnectorStudio\recent-projects.txt`, with the previous recent-project list loaded when needed. Sessions created through the New dialog save back to their project path; unsaved chats choose a location in the Save dialog, initially the current managed project's `sessions/` folder when available. The `.lattice` extension and file format remain compatible with earlier saved sessions.

Type in the composer and press **Enter** to send; **Shift+Enter** inserts a newline. Use **Escape** to dismiss menus. Keyboard shortcuts are handled while the composer has focus:

| Shortcut | Action |
| --- | --- |
| Ctrl+Shift+N | Open New dialog on Projects |
| Ctrl+Shift+W | Close the project |
| Ctrl+N | New session |
| Ctrl+O | Open session |
| Ctrl+S | Save session |
| Ctrl+W | Close the active session |
| Ctrl+Shift+C | Copy the active chat |
| Alt+P / Alt+S | Open Project / Session menu |
| Ctrl+F / F3 | Find text across conversations / find next |
| Ctrl+L | Focus the composer |
| Ctrl+1 / Ctrl+2 / Ctrl+3 | Files / Session / Plugins sidebar |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous session |
| Tab / Shift+Tab | Move keyboard focus forward / backward |

The paperclip selects files through a native dialog. Clicking a real file in the explorer or dropping files into the window also attaches their paths and adds editable `@filename` mentions to the draft. The context chip shows attached paths and can clear them. File contents are not uploaded or sent to an AI provider.

The microphone focuses the composer and invokes Windows dictation; speech input depends on the Windows language/speech setup.

Conversation text is rendered using native DirectWrite Markdown layouts: headings, bold/italic emphasis, inline and fenced code, lists, quotes, links and tables. Tables wrap within the conversation width. Saved messages retain their original Markdown text. Rendering does not load remote images or execute HTML. The title search button has been removed; Ctrl+F and F3 still open native conversation search.

Use **Copy chat** in the conversation heading or **Session > Copy chat** (Ctrl+Shift+C) to copy the active conversation with author labels. Each message also has a **Copy** button for its text. Copies preserve Markdown, Unicode and structured message content; unsent drafts are excluded. Windows receives Unicode text with standard line endings. A brief status confirms success or explains a clipboard error. Empty conversations have no enabled copy action.

## Project skills, agents and MCP servers

The Session sidebar discovers immediate child folders beneath these names in the open project's root:

| Resource | Folder names | Control |
| --- | --- | --- |
| Skills | `skill`, `skills`, `.skill`, `.skills` | Enable/disable |
| Agents | `agent`, `agents`, `.agent`, `.agents` | Enable/disable and move up/down in priority order |
| MCP servers | `mcp`, `.mcp`, `mcps`, `.mcps` | Connect/disconnect |

New skills, agents and MCP servers start disabled. No Add skill button or single-agent selector is shown. Agent priorities start in name order; new agents append to the existing saved order. Toggling an agent preserves its priority, and reordering preserves its enabled state. Existing project preferences without agent flags retain their ordering and start with agents off. Changes are saved per project in `%LOCALAPPDATA%\ConnectorStudio\resource-preferences\`. Resource folders are periodically rediscovered while the project is open. Folder discovery alone does not run commands or read skill instructions. Skill choices and agent enablement/priorities are preferences for future agent execution; this text client does not yet execute their instructions.

Place `mcp.json` in each detected MCP child folder. A `servers` map exposes each named server as an independent toggle, for example:

```json
{
  "servers": {
    "github": {
      "type": "http",
      "url": "https://api.githubcopilot.com/mcp"
    },
    "playwright": {
      "command": "npx",
      "args": ["-y", "@microsoft/mcp-server-playwright"]
    }
  }
}
```

Enabling an MCP server attempts a native HTTP or stdio protocol connection in a background worker. The row shows Connecting, Connected or its failure reason. A failed connection turns the entry off and stops retrying; enable it explicitly to try again. Successful saved entries attempt one connection when their project is opened. Disabling an entry or closing its project releases its connection and any Studio-owned command process. Command servers require their executable/runtime to be installed separately; Studio itself remains entirely C++. An HTTP entry can include a `headers` object, such as `{"Authorization":"Bearer YOUR_TOKEN"}`; a command entry can include an `env` object. Credentials stay in the local configuration. Authentication-required endpoints need valid credentials; there is no OAuth sign-in window yet. No MCP tools are invoked by chat responses in this release.

## Current integration boundaries

Connector Studio is a text chat client for Connector's `/v1` API. It discovers active configuration models through `GET /v1/models` and sends the active conversation to `POST /v1/chat/completions` with `stream: false`. The model ID selects a server-owned routing configuration; upstream credentials and routing settings belong to Connector. Completion calls do not create Connector web sessions. Studio does not start, install or bundle the server. Model discovery determines text connection readiness; Connector's speech health can be unavailable independently. Tool-call responses produce an error instead of executing them.

The native New dialog organizes registered item types into **Files**, **Sessions** and **Projects**, with built-in **Text File**, **Work Session** and **Work Project** types. Future register-file/session/project skills can supply declarative definitions through `AppController::RegisterItemType`; definitions are saved in `%LOCALAPPDATA%\ConnectorStudio\item-types.registry`. Registration adds a type definition and does not execute instructions, scripts or provider calls.

The plugin registry starts empty. Studio does not call `/api/agent/run`, execute tools or edit files from model responses. The Plan before edits and Auto-run safe tools controls are removed; their legacy serialized fields remain readable for session compatibility. There is no third-party plugin loader or extension marketplace. The custom drawn interface also does not yet expose a full UI Automation accessibility tree.

## Source layout

| Path | Purpose |
| --- | --- |
| `src/main.cpp` | Win32 lifecycle, keyboard/mouse handling, composer and DPI changes |
| `src/AppController.*` | Projects, sessions, commands and connected response ownership |
| `src/AppTypes.h` | Native state and layout data |
| `src/AppIdentity.h` | Application identity and compatible settings paths |
| `src/UIComponents.*` | Layout, drawing and hit testing |
| `src/Direct2DContext.*` | Direct2D/DirectWrite resources and PNG rendering |
| `src/SessionPersistence.*` | Validated session file serialization |
| `src/ItemTypeRegistry.*` | Registered file, session and project item types |
| `src/WorkProjectStore.*` | Project metadata and contained session storage |
| `src/NewItemDialog.*` | Native dark New dialog and previews |
| `src/ConnectorClient.*` | WinHTTP discovery/completions, cancellation and URL settings |
| `src/ProjectResources.*` | Project folder discovery and saved enablement/priority preferences |
| `src/McpClient.*` | Native HTTP/stdio MCP connection lifecycle |
| `src/MarkdownRenderer.*` | Bounded Markdown parsing and native DirectWrite rendering |
| `src/ChatCopy.*` | Markdown-preserving message and conversation copy formatting |
| `src/SettingsDialog.*` | Native dark server URL and chat font Settings dialog |
| `third_party/nlohmann/` | Compiled-in JSON parser and its MIT license |
| `resources/` | DPI/execution manifest and executable version information |
| `tests/` | Native regression suites |
| `original-project/` | Original React design reference; excluded from the native build/package |

Screenshot mode renders the app's client UI to a PNG for visual inspection. The default preview shows the empty launch state; `--new-session` explicitly creates a blank chat for inspecting session controls and model menus. `--screenshot-new` captures the native New dialog and its real Windows child controls; `--category=files` or `--category=sessions` selects a tab, with Projects as the default. `--screenshot-settings` captures the native URL Settings dialog. Native window-frame behavior is checked by the interaction regression suite.

```powershell
.\build\x64\Release\ConnectorStudio.exe --screenshot=build\native-files.png
.\build\x64\Release\ConnectorStudio.exe --screenshot=build\native-session.png --sidebar=session --new-session
.\build\x64\Release\ConnectorStudio.exe --screenshot=build\native-plugins.png --sidebar=plugins
.\build\x64\Release\ConnectorStudio.exe --screenshot=build\native-project-menu.png --menu=project
.\build\x64\Release\ConnectorStudio.exe --screenshot=build\native-session-menu.png --menu=session
.\build\x64\Release\ConnectorStudio.exe --screenshot=build\native-model-menu.png --menu=model --new-session
.\build\x64\Release\ConnectorStudio.exe --screenshot=build\native-sidebar-model-menu.png --sidebar=session --menu=sidebar-model --new-session
.\build\x64\Release\ConnectorStudio.exe --screenshot-new=build\new-project.png
.\build\x64\Release\ConnectorStudio.exe --screenshot-new=build\new-file.png --category=files
.\build\x64\Release\ConnectorStudio.exe --screenshot-new=build\new-session.png --category=sessions
.\build\x64\Release\ConnectorStudio.exe --screenshot-settings=build\connector-settings.png
```

The project is licensed under Apache 2.0; see [LICENSE](LICENSE). Third-party attribution is included in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
