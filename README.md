# Connector Studio

Connector Studio is a native C++17 Windows desktop adaptation of the design in `original-project/`. The application uses Win32, Direct2D and DirectWrite. Its executable needs Windows system libraries only: no Python, Node.js, React, browser engine or web server is used at runtime.

The dark workspace keeps the reference design's activity bar, explorer and session sidebars, tabs, conversation view, plan cards, composer, purple accents and custom menus. Layout and input handling use device-independent coordinates so native controls and drawn controls stay aligned when Windows display scaling changes.

## Run

Use Windows 10 version 1703 or newer, or Windows 11, on x64. After building, run:

```powershell
.\build\x64\Release\ConnectorStudio.exe
```

Alternatively, extract the portable ZIP from `dist/` and open `ConnectorStudio\ConnectorStudio.exe`. Release builds statically link the MSVC runtime; a separate Visual C++ Redistributable installation is not required. The application runs as the current user.

## Build and verify

Install Visual Studio 2022/2026 or Visual Studio Build Tools with **Desktop development with C++** and a Windows 10/11 SDK. Tests also use CMake, available through the **C++ CMake tools for Windows** component.

The PowerShell build script locates Visual Studio with `vswhere`, chooses the installed toolset, and works from an ordinary PowerShell prompt:

```powershell
.\scripts\build.ps1 -Configuration Debug -RunTests
.\scripts\build.ps1 -Configuration Release -RunTests
```

Application binaries are written to `build\x64\Debug\` and `build\x64\Release\`. The seven native CTest suites cover session serialization, controller behavior, layout/hit testing, real Win32 composer/window interactions, item type registration, project storage and the native New dialog. Checks remain active in Release test builds. Each suite receives a separate working directory and `CONNECTOR_STUDIO_TEST_ROOT` under the CMake build directory's `test-workspaces/` folder.

Open `ConnectorStudio.sln` for Visual Studio development. The solution builds the application; the scripts/CMake also build tests. When opening it in Visual Studio 2022, select the installed v143 platform toolset in project properties. For a direct MSBuild command from a Developer PowerShell prompt:

```powershell
msbuild .\ConnectorStudio.sln /m /p:Configuration=Release /p:Platform=x64
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

The packaging script builds Release, runs the tests, then creates a ZIP containing the executable, README, license and package metadata. It writes a SHA-256 checksum beside the archive:

```powershell
.\scripts\package.ps1
```

After a verified Release build, use `-SkipBuild` to package that binary, or `-IncludeSymbols` to include the PDB. Distribution artifacts are placed in `dist/`. Fresh staging directories ensure test executables and stale build files are excluded. The package is portable and unsigned; installer distribution, signing and automatic updates are not implemented.

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
| Alt+P / Alt+S | Open Project / Session menu |
| Ctrl+F / F3 | Find text across conversations / find next |
| Ctrl+L | Focus the composer |
| Ctrl+1 / Ctrl+2 / Ctrl+3 | Files / Session / Plugins sidebar |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous session |
| Tab / Shift+Tab | Move keyboard focus forward / backward |

The paperclip selects files through a native dialog. Clicking a real file in the explorer or dropping files into the window also attaches their paths and adds editable `@filename` mentions to the draft. The context chip shows attached paths and can clear them. File contents are not uploaded or sent to an AI provider.

The microphone focuses the composer and invokes Windows dictation; speech input depends on the Windows language/speech setup.

## Current integration boundaries

The application currently uses an explicitly labeled offline assistant. Model selection and agent controls store session preferences; no external AI provider, API key storage or automatic code-editing agent is connected. Sending a message produces an offline explanation rather than a remote model response.

The native New dialog organizes registered item types into **Files**, **Sessions** and **Projects**, with built-in **Text File**, **Work Session** and **Work Project** types. Future register-file/session/project skills can supply declarative definitions through `AppController::RegisterItemType`; definitions are saved in `%LOCALAPPDATA%\ConnectorStudio\item-types.registry`. Registration adds a type definition and does not execute instructions, scripts or provider calls. LLM/provider execution still requires a separate integration.

Plugin and custom-skill registries start empty. Adding a custom skill selects a Markdown instruction file and attaches its path; the skill registry exists for the current application process, while attached paths can be saved with the session. Instructions are not executed. There is no third-party plugin loader, tool execution engine or extension marketplace. Provider and extension integration would require additional implementation. The custom drawn interface also does not yet expose a full UI Automation accessibility tree.

## Source layout

| Path | Purpose |
| --- | --- |
| `src/main.cpp` | Win32 lifecycle, keyboard/mouse handling, composer and DPI changes |
| `src/AppController.*` | Projects, sessions, commands and offline responses |
| `src/AppTypes.h` | Native state and layout data |
| `src/AppIdentity.h` | Application identity and compatible settings paths |
| `src/UIComponents.*` | Layout, drawing and hit testing |
| `src/Direct2DContext.*` | Direct2D/DirectWrite resources and PNG rendering |
| `src/SessionPersistence.*` | Validated session file serialization |
| `src/ItemTypeRegistry.*` | Registered file, session and project item types |
| `src/WorkProjectStore.*` | Project metadata and contained session storage |
| `src/NewItemDialog.*` | Native dark New dialog and previews |
| `resources/` | DPI/execution manifest and executable version information |
| `tests/` | Native regression suites |
| `original-project/` | Original React design reference; excluded from the native build/package |

Screenshot mode renders the app's client UI to a PNG for visual inspection. The default preview shows the empty launch state; `--new-session` explicitly creates a blank chat for inspecting session controls and model menus. `--screenshot-new` captures the native New dialog and its real Windows child controls; `--category=files` or `--category=sessions` selects a tab, with Projects as the default. Native window-frame behavior is checked by the interaction regression suite.

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
```

The project is licensed under Apache 2.0; see [LICENSE](LICENSE).
