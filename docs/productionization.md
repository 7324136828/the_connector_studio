# Productionization decisions

The supplied skill is written around PDF conversion. The actual source is a
native Connector text chat client, so its architectural and lifecycle guidance
is applied to chat and session export. No unrelated PDF conversion workflow or
fabricated legacy Python implementation is introduced.

| Original native source | Python / React equivalent |
| --- | --- |
| `AppController.cpp`, `AppTypes.h` | Typed API requests, session records, React tabs/composer |
| `ConnectorClient.cpp` | `backend/app/services/connector.py`: /v1 normalization, discovery, bounded text/tool turns |
| `SessionPersistence.cpp` | `backend/app/services/lattice.py`: native v1 + trace/agent v2 encoding, UTF-8, limits, checksum |
| `WorkProjectStore.cpp` | `backend/app/services/workspace.py`: descriptors, managed files, safe paths, atomic saves |
| `ProjectResources.cpp` | Native skill/agent/MCP folder discovery and per-project preferences |
| `UIComponents.cpp` | Responsive React workspace, activity bar, sidebars, tabs, menus |
| `NewItemDialog.cpp` | Tabbed New dialog, recent projects, parent folder picker |
| `SettingsDialog.cpp` | Settings modal, test without save, URL, font, concurrency and project environment preferences |
| Request workers/cancellation | Async bounded jobs, per-session ownership, draft rollback, restart recovery |

The original source files remain intact. The Python application runs independently
and does not attempt to import or execute Windows C++ code.

The web port adds persistent draft/conversation storage and request history.
Native session-file compatibility is implemented from the source specification;
tests cover field roundtrips, header version, known FNV vectors, corrupt files
and collection limits. Native executable interoperability has not been exercised
in this environment.

The skill's pipeline guidance is implemented with UUID jobs, isolated system
temp folders, SQLite state, ZIP downloads, cancellation, expiry cleanup, and
restart recovery. ZIPs contain conversations and sessions rather than converted
PDF output. Real-time progress represents request stages; no simulated token
stream or estimated percentage is shown.

Setup creates the project .venv and uses backend and npm lockfiles. Development
starts both services; production serves the built UI from Python. Reload is
explicit because in-memory active request ownership cannot survive reload.
All processes are owned by the runner; the backend has a graceful shutdown
channel independent of Windows/POSIX console signaling.

This release targets a local, single-user deployment. Internet hosting requires
additional user isolation and identity controls documented in README.md.
MCP transport execution and extension loading are explicit future work; disabled
MCP controls are visible rather than pretending to establish connections.

The built-in filesystem picker lists directory metadata using Python filesystem
APIs, with search, pagination, and folder creation. The default permits any folder
accessible to the backend's operating-system account; Windows drives and POSIX /
are discoverable, and Windows network shares can be entered as paths. Explicit
FILE_BROWSER_ROOTS values opt into restricted roots. Project-internal writes
remain contained and reject overwrite. Choosing a parent folder controls where
new Work Projects are created. Local browser origins are accepted across valid
ports by the same policy used for CORS and write requests. The unused Plugins
activity button is removed.


SessionFiles centralizes native file opening, explicit saving, and conversation
autosave. Per-session locks serialize database mutations with file snapshots;
existing files are atomically replaced, and unrelated file changes are reported
before overwrite. Public session creation requires an existing project, and
project sessions are saved immediately on creation or opening. Global session
navigation and native upload/drop/paste import are removed; Explorer opens native
files within a project. Legacy orphan records remain available for explicit
association with a valid project, without creating new orphan sessions. Explorer files are validated and copied into the project's sessions
folder when needed, leaving the source intact; files already in the sessions
folder retain their name. Source and destination revisions prevent unrelated
changes from being overwritten, and reopening a source preserves the current
conversation. Ordinary opened folders support session storage where the OS
permits writes. A save during an active request stores the last completed
conversation with recoverable submitted text in its draft. Completion, rollback,
and restart recovery save file-backed sessions; disk-save errors preserve the
reply in SQLite and remain visible for retry.

The Markdown renderer uses remark-gfm for semantic tables and a conservative
normalizer for flattened table rows. Table regions scroll independently on narrow
screens. Workspace-wide Ctrl+S/Cmd+S flushes both draft and title edits before
saving the native session file.

ProjectMemory persists the last selected Connector configuration in a project's
`.memory/preferences.json`, and records each completed exchange in
`.memory/interactions/<job-id>.json`. Project-scoped locks serialize atomic writes;
strict schemas, size limits, UUID filenames, and symlink/junction checks keep
reads and writes inside that project's actual memory folder. Known credentials
are redacted, and memory folders are ignored by Git. Session creation and empty
session adoption inherit the project default without replacing an existing model.
Chat jobs capture their model and project when submitted. Bounded historical
context (eight exchanges, at most 16,000 characters) comes only from other
sessions in that project, and is never written into native session messages.
Failed or cancelled model requests never produce memory records. Memory failure retains
the completed response and exposes a session error and job log. The React viewer
loads preferences and memory by project ID and discards stale responses after
project switches.


ToolRuntime implements the five project tools, including the Max-only create_agent alias. Skill manifests are seeded
without overwriting existing definitions. Skill instructions are loaded only
from the active project and are size bounded. Calls validate names and arguments;
disabled functions cannot execute even if a provider requests them. Model output
is interpreted through bounded OpenAI-compatible function/tool turns, not arbitrary
Python expressions. Public trace steps record arguments, results and statuses;
provider reasoning fields are discarded before persistence or display.

Python environments are managed under app data with project UUID ownership,
validated markers and persistent project selection. Environment creation and
batch execution are serialized with cancellable locks. Subprocesses run in isolated
system temp directories, with limited stdout/stderr and deadlines. Windows job
objects (or taskkill fallback) and POSIX process groups stop descendants on
cancellation, timeout and completion. Script environments exclude Connector and
provider credentials. These processes retain the local account's filesystem
permissions; the deployment boundary remains a single-user local application.

AgentWorkers schedules bounded worker threads with independent asyncio loops
and HTTP clients. A global user setting limits active agents; waiting workers
are cancellable and settings changes wake them. Child agents cannot spawn further
agents, avoiding nested worker starvation. The parent awaits worker/subprocess
termination before cleaning temp files. Each agent run has a durable job record
and read-only hidden session in `.agent/runs/<session-id>/session.lattice`;
the reusable agent definition is stored separately. Child session mutation and
send APIs reject read-only records; their Stop API remains available. Child runs are opened through parent trace
cards and closing their tabs does not stop execution.

Lattice v1 remains the default for ordinary conversations. Version 2 adds bounded,
checksummed trace metadata and portable child snapshots with a fixed public field
schema. Import assigns fresh parent/child IDs and remaps trace references; running
snapshots become cancelled. Agent Task messages remain present in in-progress
file snapshots. Parent exports include refreshed child data, so runs can be viewed
after import without the original SQLite database. The original native v1 reader
does not understand v2. Real-process tests cover parallel worker limits, model
fallback, cancelled descendants, environment identity and portable trace imports;
browser tests cover trace expansion, read-only viewing and saved resources.


Task effort is captured in each durable job. Agent tools default off, lock below
Max, and require explicit user opt-in; generic agent creation is subject to the
same gate. Effort switches disable multi-agent skills outside SQLite session
transactions to avoid nested writer locks. New manifests are added to existing
managed projects without overwriting user definitions. Independent child Stop
returns a cancelled tool result to its parent, blocks automatic restart of that
agent for the task, and cancels executor-queued futures immediately. Parent Stop
still cancels the full task tree.

EffortContext preserves the original task, system instructions and enabled tools.
Medium retains bounded recent history; higher efforts synthesize public progress
at 100k estimated-token thresholds with retained windows of 100k/200k/250k.
Token estimates are UTF-8 JSON bytes/3 plus message overhead, with tool schemas
included. History trimming preserves complete tool-call/result groups. Synthesis
sources are fragmented into bounded non-executable inputs, with recursive work
limits, rather than sending oversized context during recovery. Max permits one
summary-of-summaries recovery before reporting a second failure. Each connector
turn has its own deadline; cancelling checks or synthesis propagates immediately
and prevents further continuation calls.

Public partial updates and phases persist with the session and its child snapshots.
Configuration attribution is captured per update, and duplicate final text is
hidden in the UI without removing saved intermediate data. Imports terminalize
active execution states and never resume tasks. Backend and browser tests cover
strict yes/no controls, tools on every work/check turn, context compaction,
recovery, cancel during checks/synthesis, individual child Stop, queued worker
Stop, native partial restoration and effort-gated controls.


User session files use portable title-based names, with readable collision
suffixes and unchanged internal UUID identity. Revision checks run before
renaming or replacing an owned file; exclusive staged publication and rollback
preserve existing conversations and external source copies on failure. Existing
UUID canonical files migrate on a successful save. Hidden agent run directories
retain their stable UUID layout. Native downloads use sanitized ASCII and RFC5987
UTF-8 filename headers.

Explorer folders toggle accessible expanded state per project and retain nested
collapse state through directory polling. Persisted public progress updates and
tool observations are shown inside collapsed conversation traces, including tasks
with no tool calls. Agent cards are inside the same expandable group. Private
provider reasoning continues to be discarded; this UI exposes public execution
activity, not hidden chain of thought.


Project recents are a separate SQLite setting, ordered by last create/open and
capped at five. Listing them removes confirmed missing or non-directory paths;
permission and transient filesystem errors retain the entry. Pruning only changes
recent pointers, preserving project catalog entries, sessions, and source files.
The list is displayed only in the New window's Projects tab.

Effort contexts retain an independent verbatim original user prompt. Work,
continuation checks, compaction, and Max summary-of-summaries recovery all use it.
Outbound work/check contexts are validated before dispatch and fail closed if
that prompt is absent or changed. Regression tests capture connector requests at
all non-Low efforts, including repeated identical prompts and multiline Unicode
text, and verify retained tool definitions throughout continuation.
