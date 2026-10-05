# Connector Studio

A React UI and Python FastAPI port of the native Connector Studio in
`original-project/the_connector_studio/`. Everything needed to develop and run
the web app lives at the repository root. The original C++ source is preserved.

The workspace includes project and session menus, a file explorer, session tabs,
per-session drafts and model choices, Markdown chat, connection settings, resource
preferences, project tools, concurrent agents, expandable execution traces, a persistent
request ledger, cancellation, and downloadable exports.

## Start

Requires Python 3.11+ and Node.js 22.12+ or 24+ with npm.

Windows:

```powershell
.\setup.bat
.\run.bat
```

Linux/macOS:

```bash
bash setup.sh
bash run.sh
```

Both setup launchers dispatch `setup.py`, which creates/reuses the project's
`.venv`, upgrades pip, installs the locked backend and frontend dependencies,
checks TypeScript, builds React, and creates `.env` without overwriting an
existing file. An active Conda/virtual environment does not replace this project's
environment.

Both run launchers dispatch `run.py`. Development starts FastAPI and Vite
concurrently. Ports default to 8000 and 5173; occupied ports advance to the next
available port. The runner prints the actual URL. Ctrl+C stops the owned services.
The backend receives a graceful shutdown command; process-tree termination is
the fallback. Backend reload is opt-in with `--reload`, because a restart
interrupts active requests.

For the built application:

```powershell
.\run.bat --production
```

```bash
bash run.sh --production
```

This serves the compiled React bundle and API together from Python at the backend
URL; no Vite server is needed. This follows [Vite's production build guidance](https://vite.dev/guide/build)
and [FastAPI's static file support](https://fastapi.tiangolo.com/tutorial/static-files/).

## Connect a model

Start your Connector server separately. The default URL is
`http://127.0.0.1:8301`. Use **Connection settings**, **Test Connection**, then
**Save**. A server root or `/v1` URL is accepted. Choose an active configuration
from the model selector, then send a message. Each project remembers the last
configuration you select in `.memory/preferences.json`. New project sessions
inherit that choice after reopening or restarting the app; existing and imported
sessions keep their own selected model. If a remembered configuration becomes
unavailable, it stays visible so you can choose a replacement.

The Python adapter discovers models through `GET /v1/models` and sends
non-streaming text requests to `POST /v1/chat/completions`. Stop cancels the
request, removes its pending user entry, and restores submitted text alongside
any newer draft. Switching tabs preserves response ownership. Connector errors
keep the conversation intact. Managed project conversations support OpenAI-compatible
function tool calls, including repeated tool/result turns. Standalone conversations
remain text-only.

Connector/provider credentials belong on the server. An optional
`CONNECTOR_API_KEY` is read from the backend environment and is never returned
to the frontend or included in request logs.

## Projects, sessions, and files

**Project > New Project** opens the native-style **New** window with Files,
Sessions, and Projects tabs, an item type list, a name field, a parent folder
field, and recent projects. **Browse...** opens the built-in filesystem browser:
use Home/Workspace, available Desktop/Documents/Downloads shortcuts, and local
drives, or enter an absolute folder path (including a Windows network share).
Navigate folders, search the current directory, switch grid/list views, and
select a parent folder. You can also create a folder inside the picker. Browsing
and folder creation work anywhere the backend's operating-system account has
permission.

Creating a Work Project writes `project.connector`, `files/`, and `sessions/`
inside the selected parent. Existing names are rejected. **Open Existing...** or
**Project > Open Project** uses the same browser to open an existing folder;
ordinary folders can be browsed without creating a descriptor. The New window's
Projects tab is the only place that lists recent projects. It keeps the five most
recently created or opened folders, with the latest first. Reopening a folder moves
it to the top. Missing folders are removed from this list; pruning never deletes
project records, conversations, or files. Closing a project closes the explorer
association.

Open or create a project before starting a conversation. **New Session** and **+**
create a blank chat in the selected project; **Session > New Session** opens the
New window's Sessions tab to name it. These controls are disabled without a
project, and the backend rejects session creation without a valid existing project.
The startup workspace offers project creation and opening. Conversations are opened
through the project's Explorer and tabs; there is no global Conversations list,
Open Session window, or native upload/import action. Dropping or pasting a
`.lattice` file does not import a session.

New sessions and project files opened as conversations immediately save a native
`.lattice` file in the project's `sessions/` folder. Filenames use the session title:
`test-1` saves as `test-1.lattice`. Duplicate names get a readable suffix such as
`test-1 (2).lattice`; invalid filename characters are replaced safely. Renaming a
session renames its saved file while keeping the conversation ID. Source copies
outside the canonical session folder stay intact. Existing UUID-named sessions
move to title-based filenames on their next successful save. Native downloads
also use readable title-based names. Ordinary opened folders support this when
your account has write permission.

The Files tab creates a named text file in a Work Project's `files/` folder.
Conversations and drafts persist automatically in SQLite. **Save** writes the
active project session file. **Ctrl+S** (or **Cmd+S** on macOS) saves the active
session from anywhere in the workspace, including pending title and draft edits.
Each completed conversation automatically saves back to its file. File-save
failures appear with a manual Save retry; the completed reply remains in SQLite.
Closing a tab retains the project's saved conversation and cancels active work.

The Python serializer preserves the original v1 magic, field order, little-endian
encoding, bounds checks, and FNV-1a checksum for ordinary conversations. Sessions
with execution traces or agent snapshots use a version 2 extension; this app reads
both versions, while the original native v1 client cannot read extended v2 files.
To open a native session from another installation, place it inside a project and
open it through Explorer. Corrupt files leave existing sessions unchanged. Native
project folders can be opened anywhere your operating-system account can access;
an optional FILE_BROWSER_ROOTS override can restrict them.

Clicking a `.lattice` file in the project explorer opens its conversation in a
tab and automatically saves it in the project's `sessions/` folder with a
readable filename based on the session title. Files elsewhere in the project
are copied into it, leaving the source intact; later saves update the copy.
Reopening the source or saved copy reuses the same session. Other explorer files,
dropped ordinary files, and the attachment button add file names/context mentions. Contents are not uploaded to a provider,
matching the native text client's behavior. Click a folder's row or arrow to
collapse or expand its contents; Enter and Space work when the row is focused.
Nested folders retain their state when a parent is reopened. Collapse state is
kept separately for each project during the current app visit and survives file
list refreshes.

Every project has its own `.memory/` folder. Completed user/model exchanges are
saved as UTF-8 JSON files in `.memory/interactions/`, including the model and
completion time. **Project Memory** in the Explorer opens a viewer of the most
recent exchanges and the project's default configuration. Later chats receive
up to eight recent exchanges from other sessions in the same project, bounded
to 16,000 characters of historical context. The current session's messages
already provide its own history. Drafts, cancelled requests, failed model requests,
and legacy standalone sessions do not add interaction records. Memory never reads
another project's folder or any global user history. Recognizable credential
values are redacted from memory records and recall context; `.memory/` is
excluded from Git. Memory errors appear in the UI and job logs while preserving
the completed conversation in SQLite and its session file.

Chat Markdown supports GFM tables, including aligned headers and formatted
cells. Clear single-line tables with flattened row breaks are restored for
rendering; code blocks and ordinary pipe-delimited prose are preserved. Wide
tables scroll within the message on smaller screens.

The Session sidebar discovers skills, agents, and MCP folders using the native
folder aliases. Skill/agent toggles and agent order persist per project.
Enabled project skills contribute instructions to tool-enabled conversations.
The built-in tool toggles control which functions the model may call; multi-agent
tools require Max effort and explicit opt-in.
MCP definitions can be created and viewed; their transport execution, third-party
plugin execution, native dictation, and native item-type registration are not
implemented in this web port. The unused Plugins activity button has been removed.
Use your OS dictation in the composer for speech input.

The directory browser and project persistence APIs include:

| Method | Endpoint | Behavior |
| --- | --- | --- |
| GET | `/api/filesystem/roots` | Local drives or configured roots, plus existing shortcuts |
| GET | `/api/filesystem/list?path=...` | Paginated immediate directory entries with optional search |
| POST | `/api/filesystem/folders` | Create a named folder under any accessible parent |
| POST | `/api/projects` | Create a Work Project using `name` and optional `parent_path` |
| POST | `/api/projects/{id}/sessions/open` | Open a project-relative `.lattice` file and ensure storage in its `sessions/` folder |
| GET | `/api/projects` | List up to five existing recent project folders in last-opened order |
| POST | `/api/sessions` | Create a session; an existing `project_id` is required |
| POST | `/api/sessions/{id}/open` | Open a project session; an explicit valid `project_id` can associate a legacy orphan |
| GET | `/api/projects/{id}/preferences` | Read the project's remembered default configuration |
| GET | `/api/projects/{id}/memory` | Read the project's latest completed interaction records |
| GET / POST / PATCH | `/api/projects/{id}/environments` | List / create / select managed Python environments |
| GET | `/api/sessions?include_agents=true` | Include hidden read-only agent sessions for trace viewers |
| PATCH | `/api/sessions/{id}` | Set session effort (`low`, `medium`, `high`, `extra_high`, `max`) when idle |
| GET / PATCH | `/api/projects/{id}/resources` | Read resources with `?effort=...`; toggles send an `effort` field and enforce Max-only agent opt-in |
| POST | `/api/jobs/{id}/discard` | Stop a parent task and its agents, or stop one child agent independently |

Listings do not execute a shell or return file contents. The operating system
enforces read/write permissions. If FILE_BROWSER_ROOTS is explicitly set to a list
of folders, paths and symlinks outside those folders are rejected.

## Project tools and agents

New Work Projects receive five built-in skills in `.skill/`. Python, batch and
conversation-based creation are enabled by default; `create_agent` and
`run_agent` start off. Existing managed projects receive missing manifests when
the backend starts or the project opens, without replacing custom definitions:

| Tool | Behavior |
| --- | --- |
| `run_python_script` | Run supplied Python code in the selected project virtual environment; return stdout, stderr and exit code |
| `run_batch_script` | Run a Windows batch script or POSIX shell script, with Python and pip resolving to the selected environment |
| `create_tool_from_conversation` | Create a new skill or MCP definition; agent definitions additionally require Max effort and `create_agent` opt-in. Existing definitions are never replaced |
| `create_agent` | At Max effort, save a reusable project agent definition after the user enables this skill |
| `run_agent` | At Max effort, run a new or existing named project agent after the user enables this skill, using its requested configuration when available or the parent configuration otherwise |

In **Connection settings**, choose the project's **Python environment**, or create
another named environment. The default environment is created on first execution.
Selection persists in `.tools/settings.json`; managed virtual environments live
under the app data directory's `environments/<project-id>/`, outside source files.
For installation, the model can call `run_batch_script` with
`python -m pip install PACKAGE`. Batch calls using the same environment are
serialized to prevent simultaneous package changes.

**Maximum parallel agents** controls concurrent agent worker threads, from 1 to
16 (default 4), across all conversations. Additional agents wait for a slot.
Independent `run_agent` calls in one response run together; each child receives
its own task, project memory, agent instructions and enabled tools. Child agents
do not recursively launch further agents. Model configurations must be exposed
by the connected server, and the selected configuration must support function
calling to use these tools.

Agent definitions live in `.agent/<name>/`; each execution is saved immediately
and throughout its run as `.agent/runs/<session-id>/session.lattice`. Child sessions
stay hidden from the normal session list and History until **View trace** opens
them. Their tabs are read-only: tasks, replies, status and expandable tool actions
remain inspectable, with Save/Copy/Export available. Closing the child tab leaves
its execution running. **Stop agent** on its read-only tab cancels just that agent,
including queued work, and lets the parent and other agents continue. The parent
will not restart that stopped agent during the same task. **Stop** on the parent
cancels its queued/running agents and their subprocess trees.

**Execution trace** is collapsed by default. Expand it to see public progress
summaries, agent cards, tool names, arguments and observations; individual tool
actions also expand/collapse. Public progress is available even when a task has
no tool calls. These summaries describe observable work, not private model
chain of thought. Private provider reasoning is neither displayed nor persisted. While a request runs, only its current trace
appears above the message input. Completed traces stay with their assistant reply
in the conversation; cancelled or failed traces without a final reply stay in the
conversation at their execution time. Sending another message keeps those earlier
traces in place. Traces remain in SQLite and session files;
parent v2 files include child snapshots so export/import retains inspectable
agent runs. Imported active snapshots become cancelled and never resume work.

Scripts run as the backend's operating-system account, so they can access files
that account can access. This is local automation, not an OS sandbox. Working
files use isolated system temp job folders; timeouts, output limits and
cancellation control the process tree. Provider credentials are excluded from
script environments and recognizable credentials are redacted from tool output
and generated definitions. Created MCP definitions save metadata only.

## Task effort, continuation, and partial results

Choose **Effort** beside the model before sending a task. The level is saved with
the session and captured when a task starts; changing tabs or another session's
settings does not change an active task. Stop the current task before changing
its effort or model. Low is the default.

| Effort | Completion and context policy |
| --- | --- |
| Low | Run tool turns as needed, then return the model's first final answer. No continuation check |
| Medium | After each final answer, ask whether useful work remains. Keep recent conversation and enabled tools within approximately 100,000 tokens |
| High | Continue after each final answer; summarize earlier public interaction at each 100,000-token threshold, retaining summaries and current work within approximately 100,000 tokens |
| Extra high | The same continuation and 100,000-token summarization threshold, with approximately 200,000 tokens of retained context |
| Max | The same threshold, with approximately 250,000 tokens of retained context, optional multi-agent tools, and one summary-of-summaries recovery attempt if the model stops responding |

For every level above Low, the harness asks:

```text
Can you continue? Respond with only JSON {"continue":"yes"} or {"continue":"no"}.
```

`yes` requests more concrete work on the original task, using the same enabled
tool definitions. `no` ends processing immediately and commits the latest final
answer. Every work request and continuation check retains a separate, verbatim
copy of the original submitted prompt, including after history trimming, repeated
continuations, context summaries, and Max recovery. Summarization requests include
that same original prompt; a summary never replaces it. If it cannot fit or the
pinned prompt is missing or changed, execution reports an error instead of sending
a request without it. System instructions and enabled tools are also retained.
Ambiguous or malformed controls are errors rather than an implicit decision to
continue. Max checks only after all of that turn's delegated agents have finished
or been stopped. If a Max work/check request stops responding, the harness
summarizes public history and earlier summaries, asks again whether to continue,
and either stops on `no` or continues from the compacted context on `yes`. There
is one automatic recovery attempt per task; a second failed response is reported
with the partial results retained.

Token counts are estimates from UTF-8 JSON bytes divided by three plus message
overhead, because connected models can use different tokenizers. These are
harness budgets rather than guarantees about a provider's model context limit.
Summaries and recent interaction share the effort's retained-context window;
tool schemas count toward the window. Medium drops the oldest complete history
units when needed. Higher levels produce public progress summaries from bounded
chunks; task constraints stay pinned, and executable tool calls stay paired with
all their observations. Summary requests do not execute tools. The actual work
and continuation checks continue to receive the enabled tool definitions.

**Max does not automatically enable multi-agent mode.** Select Max, then explicitly
enable `run_agent` and/or `create_agent` in the Session sidebar. Below Max these
skills and agent controls are unchecked and locked. Lowering effort switches the
multi-agent skills off; returning to Max requires opting in again. Agent creation
through `create_tool_from_conversation` cannot bypass the effort or opt-in rules.
`run_agent` opt-in allows a new named agent to be created for its task. Child agents
inherit the parent effort, while recursive agent launching remains disabled.
These checks are enforced in the backend as well as the UI. Enabled tool selection
is captured at task start and kept across continuation and context compaction.

Public intermediate replies appear in the conversation as each model turn
arrives. Public context summaries and saved progress history stay inside the
collapsed execution trace until expanded. The working indicator shows the
current phase and iteration, while expandable traces show tool observations
and agent activity.
Effort, partial replies, their configuration, and execution state persist in
SQLite and the session's version 2 `.lattice` metadata, including child snapshots
and exports. If a final reply repeats an intermediate reply, it is shown once.
Cancelled partials remain visible after reload, Save, Copy or export. Private
provider reasoning is never displayed or recorded.

At **any effort level**, the submit control becomes **Stop** in the same position
while work runs, including during continuation checks, summaries and recovery.
Agent tabs show **Stop agent** there while their read-only task runs. Stop cancels
in-flight connector requests, queued/running worker threads and owned subprocesses;
cleanup waits for processes to terminate even if Stop is pressed repeatedly.
No later continuation message is sent after cancellation. Submitted text is
restored to the parent's draft, and already displayed partial results remain.

## Jobs and exports

**History** refreshes every three seconds and shows job ID, session name, type,
size, timestamps, elapsed time, state and errors. **Continue** reopens its
conversation; **Discard** cancels active work and deletes owned temporary files.
Chat completions and **Export ZIP** create archives containing a native session,
Markdown conversation, JSON snapshot, and sanitized progress logs.

Each job has an isolated folder under the operating system's temp directory.
Generated ZIPs and
scratch output never go into the source checkout. Exports expire after 24 hours
by default, while history and conversations remain available. Export the saved
session again after expiry. Restart recovery marks interrupted jobs failed,
restores drafts, and removes stale temporary work.

## Configuration and deployment boundary

Copy `.env.example` to `.env` or let setup do it. Shell variables take precedence.

| Variable | Default | Purpose |
| --- | --- | --- |
| `BACKEND_PORT` | `8000` | First backend port to try |
| `FRONTEND_PORT` | `5173` | First frontend port to try |
| `CONNECTOR_URL` | `http://127.0.0.1:8301` | Initial Connector URL |
| `CONNECTOR_ALLOWED_HOSTS` | `localhost,127.0.0.1,::1` | Explicit outbound Connector host allowlist |
| `CONNECTOR_API_KEY` | empty | Optional Connector bearer credential |
| `STUDIO_DATA_DIR` | OS user data folder | SQLite, legacy saved sessions, managed tool environments |
| `WORKSPACE_ROOT` | user data folder / `projects` | Default parent folder for new projects |
| `FILE_BROWSER_ROOTS` | `*` (any accessible folder) | Optional root restriction; use absolute paths separated by `;` on Windows or `:` on Linux/macOS. Unset, empty, or `*` leaves OS permissions in control. |
| `JOB_TTL_SECONDS` | `86400` | Temporary export retention |

Windows stores app data under `%LOCALAPPDATA%/ConnectorStudioWeb`.
Linux/macOS use `${XDG_DATA_HOME:-~/.local/share}/connector-studio`.
Use absolute paths for overrides. A previously saved connection URL takes
precedence over the initial `CONNECTOR_URL` until changed in Settings.

The runner binds to loopback. This is a **single-user, single-process local
application**, with bounded uploads, four simultaneous jobs, request timeouts,
host/origin checks and project-file containment. Enabled project tools execute
Python and shell commands under the local account.
It is not an authenticated multi-tenant service. Before public hosting, add
authentication/authorization, per-user storage, a shared durable job queue,
TLS/reverse-proxy configuration, quotas, and database backup/retention policies.
Do not start multiple backend workers against the same data directory: active
job ownership and cancellation are maintained by one process.

Local HTTP/HTTPS frontend origins on localhost, 127.0.0.1, and [::1] work on any
valid port, including when the runner selects a free port. CORS_ORIGINS can list
additional exact origins. Restart the running backend after configuration or
backend code changes; the normal runner does not enable automatic reload.

## Verification

Install optional browser test tooling:

```powershell
.\setup.bat --with-e2e
.venv\Scripts\python.exe -m unittest discover -s backend/tests -v
.venv\Scripts\python.exe scripts/smoke_run.py
.venv\Scripts\python.exe scripts/audit_secrets.py
npm --prefix frontend run typecheck
npm --prefix frontend run build
npm --prefix e2e run typecheck
npm --prefix e2e test
```

On Linux/macOS replace the Python path with `.venv/bin/python`.
Browser tests use Edge on Windows/Linux and WebKit on macOS. Install Edge or
change the Playwright project to Chromium. On macOS run
`npm --prefix e2e run install:webkit` once.

Tests use isolated system temp data, dynamically selected ports, and a
deterministic mock Connector. They do not invoke a paid provider.
See [secrets.md](secrets.md) for audit scope and [docs/productionization.md](docs/productionization.md)
for the original-source mapping and productionization decisions.

## Layout

```text
backend/app/              FastAPI routes, schemas, services, SQLite store
backend/serve.py          Backend lifecycle and parent shutdown channel
backend/tests/            Backend integration and format tests
frontend/                 React + TypeScript + Vite UI
e2e/                      Browser tests and local Connector fixture
scripts/                  Startup smoke check and secret pattern audit
docs/                     Port mapping and engineering guidance
setup.py / setup.*        Environment creation and frontend build
run.py / run.*            Service supervision and production mode
original-project/         Preserved native C++ implementation
skill/productionization/  User-supplied productionization guidance
```

Apache 2.0. See [LICENSE](LICENSE).
