import { useCallback, useEffect, useRef, useState } from "react";
import MarkdownMessage from "./components/MarkdownMessage";
import ExecutionTrace from "./components/ExecutionTrace";
import NewItemDialog from "./components/NewItemDialog";
import FileBrowser, { useDialogFocus } from "./components/FileBrowser";
import {
  api,
  type Connection,
  type Entry,
  type Effort,
  type ExecutionTraceRecord,
  type Job,
  type Model,
  type Project,
  type ProjectMemory,
  type ProjectPreferences,
  type ProjectEnvironments,
  type Resource,
  type Session,
} from "./services/api";

type Dialog =
  | ""
  | "project"
  | "open"
  | "file"
  | "new-session"
  | "settings"
  | "memory";
const effortLevels: { value: Effort; label: string }[] = [
  { value: "low", label: "Low" },
  { value: "medium", label: "Medium" },
  { value: "high", label: "High" },
  { value: "extra_high", label: "Extra high" },
  { value: "max", label: "Max" },
];
const needsMaxEffort = (resource: Resource) =>
  resource.kind === "agent" ||
  ["run_agent", "create_agent"].includes(resource.name);

function traceTime(
  trace: ExecutionTraceRecord,
  session: Session,
  fallback: string,
) {
  return (
    [
      trace.created_at,
      trace.completed_at,
      ...trace.steps.flatMap((step) => [step.started_at, step.completed_at]),
      ...session.messages
        .filter((message) => message.job_id === trace.job_id)
        .map((message) => message.time),
      ...(session.live_updates || [])
        .filter((update) => update.job_id === trace.job_id)
        .map((update) => update.created_at),
    ].reduce<string>(
      (latest, time) =>
        time && Number.isFinite(Date.parse(time)) &&
        (!latest || Date.parse(time) > Date.parse(latest))
          ? time
          : latest,
      "",
    ) || fallback
  );
}

export default function App() {
  const [sessions, setSessions] = useState<Session[]>([]);
  const [tabs, setTabs] = useState<string[]>([]);
  const [activeId, setActiveId] = useState("");
  const [projects, setProjects] = useState<Project[]>([]);
  const [project, setProject] = useState<Project | null>(null);
  const [files, setFiles] = useState<Entry[]>([]);
  const [collapsedFolders, setCollapsedFolders] = useState<
    Record<string, Set<string>>
  >({});
  const projectCollapsedFolders = project
    ? collapsedFolders[project.id] || new Set<string>()
    : new Set<string>();
  const visibleFiles = files.filter((entry) => {
    const segments = entry.path.split("/");
    return !segments.slice(0, -1).some((_, index) =>
      projectCollapsedFolders.has(segments.slice(0, index + 1).join("/")),
    );
  });
  const toggleExplorerFolder = (path: string) => {
    if (!project) return;
    setCollapsedFolders((previous) => {
      const collapsed = new Set(previous[project.id] || []);
      if (collapsed.has(path)) collapsed.delete(path);
      else collapsed.add(path);
      return { ...previous, [project.id]: collapsed };
    });
  };
  const [resources, setResources] = useState<Resource[]>([]);
  const [projectPreferences, setProjectPreferences] =
    useState<ProjectPreferences | null>(null);
  const [memoryState, setMemoryState] = useState<{
    project_id: string;
    data: ProjectMemory | null;
    loading: boolean;
    error: string;
  } | null>(null);
  const [memoryRefresh, setMemoryRefresh] = useState(0);
  const [models, setModels] = useState<Model[]>([]);
  const [modelError, setModelError] = useState("");
  const [jobs, setJobs] = useState<Job[]>([]);
  const [sidebar, setSidebar] = useState<"files" | "session">("files");
  const [panelOpen, setPanelOpen] = useState(false);
  const [screen, setScreen] = useState<"workspace" | "history">("workspace");
  const [dialog, setDialog] = useState<Dialog>("");
  const [connection, setConnection] = useState<Connection>({
    server_url: "http://127.0.0.1:8301",
    font_size: 14,
    workspace_root: "",
    max_parallel_agents: 4,
  });
  const [settingsDraft, setSettingsDraft] = useState(connection);
  const [environmentState, setEnvironmentState] = useState<{
    project_id: string;
    data: ProjectEnvironments | null;
    loading: boolean;
    error: string;
  } | null>(null);
  const [environmentName, setEnvironmentName] = useState("");
  const [defaultParentPath, setDefaultParentPath] = useState("");
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const [busy, setBusy] = useState(false);
  const dirty = useRef(new Map<string, string>());
  const dirtyTitles = useRef(new Map<string, string>());
  const shortcutHandler = useRef<(event: KeyboardEvent) => void>(() => {});
  const editVersions = useRef(new Map<string, number>());
  const refreshSequence = useRef(0);
  const saving = useRef(new Map<string, Promise<void>>());
  const attachmentRef = useRef<HTMLInputElement>(null);
  const conversation = useRef<HTMLDivElement>(null);
  const selectedProjectId = useRef<string | null>(null);
  const preferencesSequence = useRef(0);
  selectedProjectId.current = project?.id ?? null;
  const active = sessions.find((s) => s.id === activeId);
  const activeReadOnly = !!(active?.read_only || active?.is_agent);
  const activeEffort = active?.effort || "low";
  const resourceProjectId = active?.project_id || project?.id || null;
  const resourceScope = useRef("");
  const resourceMutation = useRef(false);
  resourceScope.current = (resourceProjectId || "") + ":" + activeEffort;
  const replyTraceIndices = new Map<string, number>();
  active?.messages.forEach((message, index) => {
    if (message.role === "assistant" && message.job_id)
      replyTraceIndices.set(message.job_id, index);
  });
  const progressUpdates = (active?.live_updates || []).filter(
    (update) =>
      update.kind === "status" ||
      !active?.messages.some(
        (message) =>
          message.role === "assistant" &&
          message.job_id === update.job_id &&
          message.text === update.text,
      ),
  );
  const executionTraces = [...(active?.execution_traces || [])];
  const tracedJobs = new Set(executionTraces.map((trace) => trace.job_id));
  for (const update of progressUpdates) {
    if (tracedJobs.has(update.job_id)) continue;
    const job = jobs.find((value) => value.id === update.job_id);
    executionTraces.push({
      job_id: update.job_id,
      status:
        active?.pending_job === update.job_id
          ? "running"
          : job?.status === "failed"
            ? "failed"
            : ["discarded", "cancelled"].includes(job?.status || "")
              ? "cancelled"
              : replyTraceIndices.has(update.job_id)
                ? "completed"
                : "saved",
      created_at: update.created_at,
      steps: [],
    });
    tracedJobs.add(update.job_id);
  }
  const currentExecutionTrace = executionTraces.find(
    (trace) =>
      trace.job_id === active?.pending_job &&
      ["queued", "running"].includes(trace.status),
  );
  const conversationItems = [
    ...(active?.messages || []).map((message, index) => ({
      id: "message-" + index,
      kind: "message" as const,
      index,
      message,
      created_at: message.time,
    })),
    ...progressUpdates
      .filter((update) => update.kind === "partial")
      .map((update) => ({
        id: "update-" + update.id,
        kind: "update" as const,
        update,
        created_at: update.created_at,
      })),
    ...executionTraces
      .filter(
        (trace) =>
          trace.job_id !== currentExecutionTrace?.job_id &&
          !replyTraceIndices.has(trace.job_id),
      )
      .map((trace) => ({
        id: "trace-" + trace.job_id,
        kind: "trace" as const,
        trace,
        // Imports without any related timestamps use a stable session anchor.
        created_at: traceTime(
          trace,
          active!,
          active!.created_at || active!.messages[0]?.time || active!.updated_at,
        ),
      })),
  ].sort((a, b) => Date.parse(a.created_at) - Date.parse(b.created_at));
  const visibleJobs = jobs.filter((job) => {
    const session = sessions.find((s) => s.id === job.session_id);
    return (
      !(
        job.hidden ||
        job.kind === "agent" ||
        session?.hidden ||
        session?.is_agent
      ) || tabs.includes(job.session_id)
    );
  });
  const environmentProjectId = active?.project_id || project?.id || null;
  const environmentProjectName =
    environmentProjectId === active?.project_id
      ? active.project_name ||
        projects.find((p) => p.id === environmentProjectId)?.name ||
        "Conversation project"
      : project?.name || "";
  const selectedEnvironmentProject = useRef<string | null>(null);
  selectedEnvironmentProject.current =
    dialog === "settings" ? environmentProjectId : null;
  const environmentSequence = useRef(0);
  const selectedEnvironments =
    environmentState?.project_id === environmentProjectId
      ? environmentState
      : null;
  const activeJob = jobs.find((j) => j.id === active?.pending_job);
  const activeProjectName = active?.project_id
    ? active.project_name ||
      projects.find((p) => p.id === active.project_id)?.name ||
      "Project conversation"
    : "Personal workspace";
  const selectedPreferences =
    projectPreferences?.project_id === project?.id ? projectPreferences : null;
  const rememberedModel = selectedPreferences?.default_model || "";
  const rememberedModelName =
    models.find((m) => m.id === rememberedModel)?.name || rememberedModel;

  const guard = async (action: () => Promise<void>) => {
    setError("");
    setBusy(true);
    try {
      await action();
    } catch (e) {
      setError(e instanceof Error ? e.message : "Something went wrong");
    } finally {
      setBusy(false);
    }
  };
  const flush = useCallback(async (id: string) => {
    while (saving.current.has(id)) await saving.current.get(id);
    const draft = dirty.current.get(id);
    const title = dirtyTitles.current.get(id);
    if (draft === undefined && title === undefined) return;
    if (title !== undefined && !title.trim())
      throw new Error("Enter a session title before saving.");
    const operation = api<Session>("/sessions/" + id, "PATCH", {
      ...(draft === undefined ? {} : { draft }),
      ...(title === undefined ? {} : { title }),
    }).then(() => {
      if (draft !== undefined && dirty.current.get(id) === draft)
        dirty.current.delete(id);
      if (title !== undefined && dirtyTitles.current.get(id) === title)
        dirtyTitles.current.delete(id);
    });
    saving.current.set(id, operation);
    try {
      await operation;
    } finally {
      if (saving.current.get(id) === operation) saving.current.delete(id);
    }
  }, []);
  const refresh = useCallback(async () => {
    const sequence = ++refreshSequence.current;
    const startedVersions = new Map(editVersions.current);
    const pendingDrafts = new Set(dirty.current.keys());
    const pendingTitles = new Set(dirtyTitles.current.keys());
    const pendingSaves = new Set(saving.current.keys());
    const [serverSessions, serverJobs, serverProjects] = await Promise.all([
      api<Session[]>("/sessions?include_agents=true"),
      api<Job[]>("/jobs"),
      api<Project[]>("/projects"),
    ]);
    if (sequence !== refreshSequence.current) return;
    setSessions((previous) => {
      if (sequence !== refreshSequence.current) return previous;
      return serverSessions.map((s) => {
        const local = previous.find((p) => p.id === s.id);
        const changed =
          editVersions.current.get(s.id) !== startedVersions.get(s.id);
        const pending = saving.current.has(s.id) || pendingSaves.has(s.id);
        return {
          ...s,
          draft:
            dirty.current.get(s.id) ??
            (local && (changed || pending || pendingDrafts.has(s.id))
              ? local.draft
              : s.draft),
          title:
            dirtyTitles.current.get(s.id) ??
            (local && (changed || pending || pendingTitles.has(s.id))
              ? local.title
              : s.title),
        };
      });
    });
    setJobs(serverJobs);
    setProjects(serverProjects);
  }, []);
  const loadModels = useCallback(async () => {
    try {
      setModels(await api<Model[]>("/models"));
      setModelError("");
    } catch (e) {
      setModels([]);
      setModelError(e instanceof Error ? e.message : "Models unavailable");
    }
  }, []);
  const loadProjectPreferences = useCallback(async (projectId: string) => {
    const sequence = ++preferencesSequence.current;
    try {
      const preferences = await api<ProjectPreferences>(
        "/projects/" + projectId + "/preferences",
      );
      if (
        selectedProjectId.current === projectId &&
        sequence === preferencesSequence.current
      )
        setProjectPreferences(preferences);
    } catch (e) {
      if (
        selectedProjectId.current === projectId &&
        sequence === preferencesSequence.current
      )
        setProjectPreferences({
          project_id: projectId,
          default_model: "",
          error:
            e instanceof Error
              ? e.message
              : "Unable to read this project's saved model.",
        });
    }
  }, []);
  const loadEnvironments = useCallback(async (projectId: string) => {
    const sequence = ++environmentSequence.current;
    try {
      const data = await api<ProjectEnvironments>(
        "/projects/" + projectId + "/environments",
      );
      if (
        selectedEnvironmentProject.current === projectId &&
        sequence === environmentSequence.current
      )
        setEnvironmentState({
          project_id: projectId,
          data,
          loading: false,
          error: "",
        });
    } catch (e) {
      if (
        selectedEnvironmentProject.current === projectId &&
        sequence === environmentSequence.current
      )
        setEnvironmentState({
          project_id: projectId,
          data: null,
          loading: false,
          error:
            e instanceof Error
              ? e.message
              : "Unable to read the project's Python environments.",
        });
    }
  }, []);
  useEffect(() => {
    let alive = true;
    void refresh().catch((e) => setError(e.message));
    void api<Connection>("/settings")
      .then((s) => {
        if (alive) {
          setConnection(s);
          setSettingsDraft(s);
        }
      })
      .catch((e) => setError(e.message));
    void loadModels();
    const timer = setInterval(() => {
      if (alive) void refresh().catch((e) => setError(e.message));
    }, 3000);
    const draftTimer = setInterval(() => {
      for (const id of new Set([
        ...dirty.current.keys(),
        ...dirtyTitles.current.keys(),
      ]))
        void flush(id).catch((e) => setError(e.message));
    }, 800);
    return () => {
      alive = false;
      clearInterval(timer);
      clearInterval(draftTimer);
    };
  }, [flush, refresh, loadModels]);
  useEffect(() => {
    if (!project) {
      setFiles([]);
      return;
    }
    let cancelled = false;
    setFiles([]);
    const load = async () => {
      try {
        const fileList = await api<Entry[]>(
          "/projects/" + project.id + "/files",
        );
        if (!cancelled) setFiles(fileList);
      } catch (e) {
        if (!cancelled)
          setError(e instanceof Error ? e.message : "Project unavailable");
      }
    };
    void load();
    const timer = setInterval(() => void load(), 5000);
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  }, [project]);
  useEffect(() => {
    setResources([]);
    if (!resourceProjectId) return;
    let cancelled = false;
    const load = async () => {
      try {
        const result = await api<Resource[]>(
          "/projects/" +
            resourceProjectId +
            "/resources?effort=" +
            activeEffort,
        );
        if (!cancelled && !resourceMutation.current) setResources(result);
      } catch (e) {
        if (!cancelled)
          setError(
            e instanceof Error ? e.message : "Project tools unavailable",
          );
      }
    };
    void load();
    const timer = setInterval(() => void load(), 5000);
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  }, [resourceProjectId, activeEffort]);
  useEffect(() => {
    setProjectPreferences(null);
    if (!project) return;
    void loadProjectPreferences(project.id);
    const timer = setInterval(
      () => void loadProjectPreferences(project.id),
      5000,
    );
    return () => {
      clearInterval(timer);
      preferencesSequence.current++;
    };
  }, [project?.id, loadProjectPreferences]);
  useEffect(() => {
    if (dialog !== "memory" || !project) {
      setMemoryState(null);
      return;
    }
    const projectId = project.id;
    let cancelled = false;
    let sequence = 0;
    setMemoryState({
      project_id: projectId,
      data: null,
      loading: true,
      error: "",
    });
    const load = async () => {
      const request = ++sequence;
      try {
        const memory = await api<ProjectMemory>(
          "/projects/" + projectId + "/memory",
        );
        if (!cancelled && request === sequence)
          setMemoryState({
            project_id: projectId,
            data: memory,
            loading: false,
            error: "",
          });
      } catch (e) {
        if (!cancelled && request === sequence)
          setMemoryState((previous) => ({
            project_id: projectId,
            data: previous?.project_id === projectId ? previous.data : null,
            loading: false,
            error:
              e instanceof Error
                ? e.message
                : "Unable to load this project's memory.",
          }));
      }
    };
    void load();
    const timer = setInterval(() => void load(), 3000);
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  }, [project?.id, dialog, memoryRefresh]);
  useEffect(() => {
    conversation.current?.scrollTo({
      top: conversation.current.scrollHeight,
      behavior: "smooth",
    });
  }, [
    activeId,
    active?.messages.length,
    active?.live_updates?.length,
    active?.pending_job,
  ]);
  useEffect(() => {
    setEnvironmentName("");
    if (dialog !== "settings" || !environmentProjectId) {
      setEnvironmentState(null);
      return;
    }
    setEnvironmentState({
      project_id: environmentProjectId,
      data: null,
      loading: true,
      error: "",
    });
    void loadEnvironments(environmentProjectId);
    return () => {
      environmentSequence.current++;
    };
  }, [dialog, environmentProjectId, loadEnvironments]);

  const updateLocal = (id: string, changes: Partial<Session>) =>
    setSessions((old) =>
      old.map((s) => (s.id === id ? { ...s, ...changes } : s)),
    );
  const changeDraft = (id: string, draft: string) => {
    editVersions.current.set(id, (editVersions.current.get(id) ?? 0) + 1);
    dirty.current.set(id, draft);
    updateLocal(id, { draft });
  };
  useEffect(() => {
    const listener = (event: KeyboardEvent) => shortcutHandler.current(event);
    window.addEventListener("keydown", listener);
    return () => window.removeEventListener("keydown", listener);
  }, []);
  const changeTitle = (id: string, title: string) => {
    editVersions.current.set(id, (editVersions.current.get(id) ?? 0) + 1);
    dirtyTitles.current.set(id, title);
    updateLocal(id, { title });
  };
  const openTab = (session: Session) => {
    setTabs((old) => (old.includes(session.id) ? old : [...old, session.id]));
    setActiveId(session.id);
    setScreen("workspace");
    setDialog("");
  };
  const receiveSession = (
    session: Session,
    startedVersions = new Map(editVersions.current),
  ) => {
    refreshSequence.current++;
    setSessions((old) => {
      const local = old.find((s) => s.id === session.id);
      const changed =
        editVersions.current.get(session.id) !==
        startedVersions.get(session.id);
      const current = {
        ...session,
        draft:
          dirty.current.get(session.id) ??
          (local && changed ? local.draft : session.draft),
        title:
          dirtyTitles.current.get(session.id) ??
          (local && changed ? local.title : session.title),
      };
      return local
        ? old.map((s) => (s.id === session.id ? current : s))
        : [current, ...old];
    });
  };
  const openSession = (existing: Session) =>
    guard(async () => {
      if (!existing.project_id && !project)
        throw new Error("Open a project before opening this conversation.");
      await flush(existing.id);
      const startedVersions = new Map(editVersions.current);
      const session = await api<Session>(
        "/sessions/" + existing.id + "/open",
        "POST",
        !existing.project_id && project ? { project_id: project.id } : {},
      );
      receiveSession(session, startedVersions);
      openTab(session);
    });
  const openAgentSession = (id: string) =>
    guard(async () => {
      const session = await api<Session>("/sessions/" + id);
      receiveSession(session);
      openTab(session);
    });
  const newSession = () =>
    guard(async () => {
      if (!project)
        throw new Error("Open a project before creating a conversation.");
      const session = await api<Session>("/sessions", "POST", {
        title: "New session",
        project_id: project.id,
      });
      receiveSession(session);
      openTab(session);
    });
  const closeSession = (id: string) =>
    guard(async () => {
      await flush(id);
      const session = sessions.find((s) => s.id === id);
      if (session?.pending_job && !session.read_only && !session.is_agent)
        await api("/jobs/" + session.pending_job + "/discard", "POST");
      const remaining = tabs.filter((tab) => tab !== id);
      setTabs(remaining);
      if (activeId === id) setActiveId(remaining.at(-1) ?? "");
      await refresh();
    });
  const saveSession = () =>
    guard(async () => {
      if (!active) return;
      await flush(active.id);
      await api("/sessions/" + active.id + "/save", "POST");
      await refresh();
      setNotice("Session saved.");
      setDialog("");
    });
  shortcutHandler.current = (event) => {
    if (
      (event.ctrlKey || event.metaKey) &&
      event.key.toLowerCase() === "s" &&
      active &&
      screen === "workspace" &&
      !dialog
    ) {
      event.preventDefault();
      if (!event.repeat && !busy) void saveSession();
    }
  };
  const openExplorerEntry = (entry: Entry) =>
    guard(async () => {
      if (entry.directory) return;
      if (!project || !entry.path.toLowerCase().endsWith(".lattice")) {
        await attach([entry.path]);
        return;
      }
      for (const id of new Set([
        ...dirty.current.keys(),
        ...dirtyTitles.current.keys(),
        ...saving.current.keys(),
      ]))
        await flush(id);
      const startedVersions = new Map(editVersions.current);
      const session = await api<Session>(
        "/projects/" + project.id + "/sessions/open",
        "POST",
        { path: entry.path },
      );
      receiveSession(session, startedVersions);
      openTab(session);
      setPanelOpen(false);
    });
  const send = () =>
    guard(async () => {
      if (
        !active?.project_id ||
        activeReadOnly ||
        !active.model ||
        active.pending_job
      )
        return;
      const text = dirty.current.get(active.id) ?? active.draft;
      if (!text.trim()) return;
      await flush(active.id);
      await api<Job>("/sessions/" + active.id + "/messages", "POST", {
        text,
        model: active.model,
        effort: activeEffort,
      });
      if (dirty.current.get(active.id) === text)
        dirty.current.delete(active.id);
      await refresh();
    });
  const stop = (id: string) =>
    guard(async () => {
      await api("/jobs/" + id + "/discard", "POST");
      await refresh();
    });
  const exportSession = () =>
    guard(async () => {
      if (!active) return;
      await flush(active.id);
      await api("/sessions/" + active.id + "/export", "POST");
      await refresh();
      setScreen("history");
    });
  const attach = async (names: string[]) => {
    if (activeReadOnly) {
      setError(
        "Agent sessions are read-only. Open a conversation before attaching files.",
      );
      return;
    }
    if (!active?.project_id) {
      setError("Create or open a conversation in a project first.");
      return;
    }
    if (!names.length) return;
    const attachments = [...new Set([...active.attachments, ...names])].slice(
      0,
      100,
    );
    await api("/sessions/" + active.id, "PATCH", { attachments });
    updateLocal(active.id, { attachments });
    changeDraft(
      active.id,
      active.draft + " " + names.map((n) => "@" + n).join(" "),
    );
  };
  const copy = (text: string) =>
    guard(async () => {
      await navigator.clipboard.writeText(text);
      setNotice("Copied to clipboard.");
    });
  const showDialog = (next: Dialog) => {
    if (["project", "open", "file", "new-session"].includes(next)) {
      void guard(async () => {
        const roots = await api<{ default_path: string }>("/filesystem/roots");
        setDefaultParentPath(roots.default_path);
        setDialog(next);
      });
      return;
    }
    if (next === "settings") {
      void guard(async () => {
        const current = await api<Connection>("/settings");
        setConnection(current);
        setSettingsDraft(current);
        setDialog(next);
      });
      return;
    }
    setError("");
    setNotice("");
    setSettingsDraft(connection);
    setDialog(next);
  };
  const openProject = async (path: string) => {
    const next = await api<Project>("/projects/open", "POST", { path });
    setProject(next);
    setScreen("workspace");
    setPanelOpen(false);
    await refresh();
    setDialog("");
  };
  const createItem = async (
    kind: "file" | "session" | "project",
    name: string,
    parentPath: string,
  ) => {
    setBusy(true);
    try {
      if (kind === "project") {
        const next = await api<Project>("/projects", "POST", {
          name,
          parent_path: parentPath,
        });
        setProject(next);
        setScreen("workspace");
        setPanelOpen(false);
        await refresh();
      } else if (kind === "file") {
        if (!project)
          throw new Error("Open a Work Project before creating a file.");
        await api("/projects/" + project.id + "/files", "POST", { name });
        setFiles(await api<Entry[]>("/projects/" + project.id + "/files"));
      } else {
        if (!project)
          throw new Error("Open a project before creating a Work Session.");
        const session = await api<Session>("/sessions", "POST", {
          title: name,
          project_id: project.id,
        });
        receiveSession(session);
        openTab(session);
      }
      setDialog("");
    } finally {
      setBusy(false);
    }
  };
  const submitDialog = () =>
    guard(async () => {
      for (const id of new Set([
        ...dirty.current.keys(),
        ...dirtyTitles.current.keys(),
      ]))
        await flush(id);
      await api("/settings", "POST", {
        server_url: settingsDraft.server_url,
        font_size: settingsDraft.font_size,
        max_parallel_agents: settingsDraft.max_parallel_agents,
      });
      setConnection(settingsDraft);
      await loadModels();
      await refresh();
      setDialog("");
    });
  const toggleResource = (resource: Resource, direction?: "up" | "down") =>
    guard(async () => {
      if (!resourceProjectId || activeReadOnly || active?.pending_job) return;
      if (activeEffort !== "max" && needsMaxEffort(resource)) return;
      const scope = resourceScope.current;
      const enabled = direction ? resource.enabled : !resource.enabled;
      resourceMutation.current = true;
      if (!direction)
        setResources((previous) =>
          previous.map((entry) =>
            entry.id === resource.id ? { ...entry, enabled } : entry,
          ),
        );
      try {
        const result = await api<Resource[]>(
          "/projects/" + resourceProjectId + "/resources",
          "PATCH",
          { id: resource.id, effort: activeEffort, enabled, direction },
        );
        if (resourceScope.current === scope) setResources(result);
      } catch (problem) {
        if (resourceScope.current === scope && !direction)
          setResources((previous) =>
            previous.map((entry) =>
              entry.id === resource.id
                ? { ...entry, enabled: resource.enabled }
                : entry,
            ),
          );
        throw problem;
      } finally {
        resourceMutation.current = false;
      }
    });
  const selectModel = (model: string) =>
    guard(async () => {
      if (!active || activeReadOnly) return;
      const startedVersions = new Map(editVersions.current);
      const session = await api<Session>("/sessions/" + active.id, "PATCH", {
        model,
      });
      receiveSession(session, startedVersions);
      if (
        session.project_id === selectedProjectId.current &&
        session.project_id
      )
        await loadProjectPreferences(session.project_id);
    });
  const selectEffort = (effort: Effort) =>
    guard(async () => {
      if (!active || activeReadOnly || active.pending_job) return;
      const startedVersions = new Map(editVersions.current);
      const session = await api<Session>("/sessions/" + active.id, "PATCH", {
        effort,
      });
      receiveSession(session, startedVersions);
    });
  const selectEnvironment = (name: string) =>
    guard(async () => {
      if (!environmentProjectId) return;
      await api(
        "/projects/" + environmentProjectId + "/environments",
        "PATCH",
        { name },
      );
      await loadEnvironments(environmentProjectId);
    });
  const createEnvironment = () =>
    guard(async () => {
      if (!environmentProjectId || !environmentName.trim()) return;
      await api("/projects/" + environmentProjectId + "/environments", "POST", {
        name: environmentName.trim(),
      });
      await loadEnvironments(environmentProjectId);
      setEnvironmentName("");
    });

  return (
    <div
      className="studio"
      data-panel-open={panelOpen}
      style={
        { "--chat-font": connection.font_size + "px" } as React.CSSProperties
      }
      onDragOver={(e) => e.preventDefault()}
      onDrop={(e) => {
        e.preventDefault();
        if (activeReadOnly) {
          setError(
            "Agent sessions are read-only. Open a conversation before dropping files.",
          );
          return;
        }
        const dropped = Array.from(e.dataTransfer.files);
        const names = dropped
          .filter((file) => !file.name.toLowerCase().endsWith(".lattice"))
          .map((file) => file.name);
        if (names.length) void guard(() => attach(names));
      }}
    >
      <header className="topbar">
        <span className="brand-mark">◈</span>
        <strong>Connector Studio</strong>
        <details className="menu">
          <summary>Project</summary>
          <div
            className="dropdown"
            onClick={(e) => {
              (e.currentTarget.parentElement as HTMLDetailsElement).open =
                false;
            }}
          >
            <button onClick={() => showDialog("project")}>New Project</button>
            <button onClick={() => showDialog("open")}>Open Project</button>
            <button disabled={!project} onClick={() => setProject(null)}>
              Close Project
            </button>
          </div>
        </details>
        <details className="menu">
          <summary>Session</summary>
          <div
            className="dropdown"
            onClick={(e) => {
              (e.currentTarget.parentElement as HTMLDetailsElement).open =
                false;
            }}
          >
            <button
              disabled={!project || busy}
              onClick={() => showDialog("new-session")}
            >
              New Session
            </button>
            <button disabled={!active} onClick={saveSession}>
              Save Session
            </button>
            <button disabled={!active} onClick={exportSession}>
              Export ZIP
            </button>
            <button
              disabled={!active}
              onClick={() => active && void closeSession(active.id)}
            >
              Close Session
            </button>
          </div>
        </details>
        <div className="nav">
          <button
            className={screen === "workspace" ? "selected" : ""}
            onClick={() => setScreen("workspace")}
          >
            Workspace
          </button>
          <button
            className={screen === "history" ? "selected" : ""}
            onClick={() => setScreen("history")}
          >
            History <span>{visibleJobs.length}</span>
          </button>
        </div>
        <button
          aria-label="Connection settings"
          onClick={() => showDialog("settings")}
        >
          ⚙
        </button>
      </header>
      <div className="workspace">
        <nav className="activity" aria-label="Side panels">
          {(["files", "session"] as const).map((mode, i) => (
            <button
              key={mode}
              aria-label={mode === "session" ? "Session controls" : mode}
              className={sidebar === mode ? "active" : ""}
              onClick={() => {
                setSidebar(mode);
                setPanelOpen(sidebar === mode ? !panelOpen : true);
              }}
            >
              {["▤", "◉"][i]}
            </button>
          ))}
        </nav>
        <aside className="sidebar">
          <div className="sidebar-title">
            {sidebar === "files" ? "EXPLORER" : sidebar.toUpperCase()}
            <button
              aria-label="New session"
              disabled={!project || busy}
              onClick={newSession}
            >
              ＋
            </button>
          </div>
          {sidebar === "files" && (
            <>
              {project ? (
                <>
                  <div className="project-name">
                    ▾ {project.name}
                    <button
                      onClick={() => showDialog("file")}
                      disabled={!project.managed}
                      title="New text file"
                    >
                      ＋
                    </button>
                  </div>
                  <div className="project-memory-controls">
                    <button
                      className="project-memory-button"
                      onClick={() => showDialog("memory")}
                    >
                      Project Memory
                    </button>
                    <p className="project-model-hint">
                      {rememberedModel ? (
                        <>
                          <strong>Default model: {rememberedModelName}</strong>
                          <span>
                            Used for new conversations in this project.
                          </span>
                        </>
                      ) : (
                        "Select a model to remember it for new project conversations."
                      )}
                    </p>
                    {selectedPreferences?.error && (
                      <p className="project-memory-error" role="alert">
                        {selectedPreferences.error}
                      </p>
                    )}
                  </div>
                  <div className="file-list">
                    {visibleFiles.map((f) => (
                      <button
                        key={f.path}
                        className={
                          f.directory ? "explorer-folder" : "explorer-file"
                        }
                        style={{ paddingLeft: 16 + f.depth * 12 }}
                        title={f.path}
                        aria-label={f.directory ? f.name : undefined}
                        aria-expanded={
                          f.directory
                            ? !projectCollapsedFolders.has(f.path)
                            : undefined
                        }
                        onClick={() =>
                          f.directory
                            ? toggleExplorerFolder(f.path)
                            : void openExplorerEntry(f)
                        }
                      >
                        <span className="explorer-entry-icon" aria-hidden="true">
                          {f.directory
                            ? projectCollapsedFolders.has(f.path)
                              ? "\u25b8 \u25b1"
                              : "\u25be \u25b1"
                            : "\u25a7"}
                        </span>{" "}
                        {f.name}
                      </button>
                    ))}
                  </div>
                </>
              ) : (
                <div className="sidebar-empty">
                  <p>No project open</p>
                  <p>Organize files and conversations in a Work Project.</p>
                  <button
                    className="secondary"
                    onClick={() => showDialog("project")}
                  >
                    New Project
                  </button>
                </div>
              )}
            </>
          )}
          {sidebar === "session" && (
            <>
              <h3>MODEL</h3>
              <select
                aria-label="Sidebar model"
                value={active?.model ?? ""}
                disabled={
                  activeReadOnly || busy || !active || !!active.pending_job
                }
                onChange={(e) => void selectModel(e.target.value)}
              >
                <option value="">Select configuration</option>
                {active?.model &&
                  !models.some((m) => m.id === active.model) && (
                    <option value={active.model}>
                      {active.model} (unavailable)
                    </option>
                  )}
                {models.map((m) => (
                  <option key={m.id} value={m.id}>
                    {m.name}
                  </option>
                ))}
              </select>
              <button className="text-button" onClick={loadModels}>
                Refresh models
              </button>
              <p className="muted">
                {modelError ||
                  (models.length
                    ? models.length + " configurations available"
                    : "No active configurations")}
              </p>
              {active?.project_id && (
                <p className="muted model-project-hint">
                  The selected model is remembered for {activeProjectName}.
                </p>
              )}
              <h3>EFFORT</h3>
              <select
                aria-label="Sidebar effort"
                value={activeEffort}
                disabled={
                  activeReadOnly || busy || !active || !!active.pending_job
                }
                onChange={(e) => void selectEffort(e.target.value as Effort)}
              >
                {effortLevels.map((level) => (
                  <option key={level.value} value={level.value}>
                    {level.label}
                  </option>
                ))}
              </select>
              <p className="muted effort-hint">
                {activeEffort === "low"
                  ? "One final response."
                  : "Checks whether more work is needed after each response."}
                {activeEffort === "max"
                  ? " Multi-agent tools are optional; enable them below."
                  : " Multi-agent tools require Max effort."}
              </p>
              {(["skill", "agent", "mcp"] as const).map((kind) => (
                <section key={kind}>
                  <h3>
                    {kind === "mcp" ? "MCP SERVERS" : kind.toUpperCase() + "S"}
                  </h3>
                  {resources
                    .filter((r) => r.kind === kind)
                    .map((r) => (
                      <div className="resource" key={r.id}>
                        <label title={r.description}>
                          <input
                            type="checkbox"
                            aria-label={r.name}
                            checked={
                              activeEffort !== "max" && needsMaxEffort(r)
                                ? false
                                : r.enabled
                            }
                            disabled={
                              activeReadOnly ||
                              busy ||
                              !!active?.pending_job ||
                              kind === "mcp" ||
                              (activeEffort !== "max" && needsMaxEffort(r))
                            }
                            onChange={() => void toggleResource(r)}
                          />
                          <span>
                            {r.name}
                            {activeEffort !== "max" && needsMaxEffort(r) && (
                              <small className="resource-restriction">
                                Requires Max effort
                              </small>
                            )}
                          </span>
                        </label>
                        {kind === "agent" && (
                          <span>
                            <button
                              aria-label={"Move " + r.name + " up"}
                              disabled={
                                activeReadOnly ||
                                busy ||
                                !!active?.pending_job ||
                                activeEffort !== "max"
                              }
                              onClick={() => void toggleResource(r, "up")}
                            >
                              ↑
                            </button>
                            <button
                              aria-label={"Move " + r.name + " down"}
                              disabled={
                                activeReadOnly ||
                                busy ||
                                !!active?.pending_job ||
                                activeEffort !== "max"
                              }
                              onClick={() => void toggleResource(r, "down")}
                            >
                              ↓
                            </button>
                          </span>
                        )}
                      </div>
                    ))}
                  {!resources.some((r) => r.kind === kind) && (
                    <p className="muted">
                      {project
                        ? "No " + kind + " folders found."
                        : "Open a project to discover resources."}
                    </p>
                  )}
                </section>
              ))}
              <p className="muted">
                Skills and agents are available to the model as tools. MCP
                execution is unavailable in this web version.
              </p>
            </>
          )}
        </aside>
        <main className="main">
          {screen === "history" ? (
            <section className="history">
              <div className="heading">
                <div>
                  <span className="eyebrow">ACTIVITY</span>
                  <h1>Request history</h1>
                  <p>
                    Track requests, reconnect to conversations, and download
                    session exports.
                  </p>
                </div>
                <button
                  className="secondary"
                  onClick={() => void guard(refresh)}
                >
                  Refresh
                </button>
              </div>
              {!visibleJobs.length ? (
                <div className="empty-history">
                  Your requests and exports will appear here.
                </div>
              ) : (
                <div className="table-scroll">
                  <table>
                    <thead>
                      <tr>
                        <th>Session / Job ID</th>
                        <th>Type / Size</th>
                        <th>Created</th>
                        <th>Elapsed</th>
                        <th>Status</th>
                        <th>Actions</th>
                      </tr>
                    </thead>
                    <tbody>
                      {visibleJobs.map((j) => (
                        <tr key={j.id}>
                          <td>
                            {j.filename}
                            <small>{j.id.slice(0, 8)}</small>
                          </td>
                          <td>
                            {j.kind}
                            <small>{j.file_size} bytes</small>
                          </td>
                          <td>{new Date(j.created_at).toLocaleString()}</td>
                          <td>
                            {Math.max(
                              0,
                              Math.round(
                                ((j.completed_at
                                  ? new Date(j.completed_at).getTime()
                                  : Date.now()) -
                                  new Date(j.created_at).getTime()) /
                                  1000,
                              ),
                            )}
                            s
                          </td>
                          <td>
                            <span className={"badge " + j.status}>
                              {j.status.replace("_", " ")}
                            </span>
                            {j.error_message && (
                              <small className="error-text">
                                {j.error_message}
                              </small>
                            )}
                          </td>
                          <td>
                            <div className="row-actions">
                              <button
                                disabled={
                                  !project &&
                                  !sessions.find(
                                    (session) => session.id === j.session_id,
                                  )?.project_id
                                }
                                onClick={() => {
                                  const session = sessions.find(
                                    (s) => s.id === j.session_id,
                                  );
                                  if (session) {
                                    if (session.is_agent || session.read_only)
                                      void openAgentSession(session.id);
                                    else void openSession(session);
                                  }
                                }}
                              >
                                Continue
                              </button>
                              {["queued", "in_progress"].includes(j.status) &&
                                j.kind !== "agent" &&
                                !sessions.find((s) => s.id === j.session_id)
                                  ?.read_only && (
                                  <button onClick={() => void stop(j.id)}>
                                    Discard
                                  </button>
                                )}
                              {j.download_available && (
                                <a
                                  href={"/api/jobs/" + j.id + "/download-zip"}
                                  download
                                >
                                  Download ZIP
                                </a>
                              )}
                            </div>
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )}
            </section>
          ) : (
            <>
              <div className="tabs" role="tablist" aria-label="Open sessions">
                {tabs.map((id) => {
                  const session = sessions.find((s) => s.id === id);
                  return (
                    session && (
                      <div
                        className={"tab " + (activeId === id ? "active" : "")}
                        key={id}
                      >
                        <button
                          role="tab"
                          aria-selected={id === activeId}
                          onClick={() => setActiveId(id)}
                        >
                          ◌ {session.title}
                        </button>
                        <button
                          aria-label={"Close " + session.title}
                          onClick={() => void closeSession(id)}
                        >
                          ×
                        </button>
                      </div>
                    )
                  );
                })}
                <button
                  aria-label="Add session"
                  disabled={!project || busy}
                  onClick={newSession}
                >
                  ＋
                </button>
              </div>
              {active ? (
                <div className="chat">
                  <div className="chat-heading">
                    <div>
                      <span className="eyebrow">{activeProjectName}</span>
                      {project && active.project_id !== project.id && (
                        <span className="chat-project-context">
                          Explorer: {project.name}
                        </span>
                      )}
                      <input
                        aria-label="Session title"
                        disabled={activeReadOnly}
                        value={active.title}
                        maxLength={200}
                        onChange={(e) => changeTitle(active.id, e.target.value)}
                        onBlur={() =>
                          void flush(active.id).catch((e) =>
                            setError(
                              e instanceof Error
                                ? e.message
                                : "Unable to save title changes.",
                            ),
                          )
                        }
                      />
                      {activeReadOnly && (
                        <span className="agent-session-badge">
                          Read-only agent session ·{" "}
                          {active.agent_status || "Trace available"}
                        </span>
                      )}
                    </div>
                    <div>
                      <button
                        onClick={() =>
                          void copy(
                            conversationItems
                              .flatMap((item) =>
                                item.kind === "message"
                                  ? [
                                      item.message.author +
                                        ":\n" +
                                        item.message.text,
                                    ]
                                  : item.kind === "update" &&
                                      item.update.kind === "partial"
                                    ? [
                                        (item.update.model || active.model) +
                                          " (intermediate result):\n" +
                                          item.update.text,
                                      ]
                                    : [],
                              )
                              .join("\n\n"),
                          )
                        }
                        disabled={
                          !active.messages.length &&
                          !active.live_updates?.some(
                            (update) => update.kind === "partial",
                          )
                        }
                      >
                        Copy chat
                      </button>
                      <button
                        onClick={saveSession}
                        disabled={busy}
                        title="Save session (Ctrl+S / Cmd+S)"
                      >
                        Save
                      </button>
                      <button onClick={exportSession} disabled={busy}>
                        Export ZIP
                      </button>
                    </div>
                  </div>
                  {active.save_error && (
                    <p className="session-save-error" role="alert">
                      {active.save_error} Use Save or Ctrl+S to retry.
                    </p>
                  )}
                  {active.memory_error && (
                    <p
                      className="session-save-error memory-save-error"
                      role="alert"
                    >
                      {active.memory_error} Your conversation is still saved in
                      this app.
                    </p>
                  )}
                  <div className="conversation" ref={conversation}>
                    {!conversationItems.length && (
                      <div className="welcome">
                        <span className="welcome-logo">◈</span>
                        <span className="eyebrow">
                          YOUR NEXT IDEA STARTS HERE
                        </span>
                        <h1>Let's make a connection.</h1>
                        <p>
                          Choose an active Connector configuration and start a
                          conversation.
                          <br />
                          Your projects, drafts, and chats stay in your
                          workspace.
                        </p>
                        <div className="welcome-cards">
                          <div>
                            <b>01 / Connect</b>
                            <p>Set your Connector server URL in Settings.</p>
                          </div>
                          <div>
                            <b>02 / Explore</b>
                            <p>
                              Keep related files and sessions in a project.
                            </p>
                          </div>
                          <div>
                            <b>03 / Create</b>
                            <p>Chat, save, and export your conversation.</p>
                          </div>
                        </div>
                      </div>
                    )}
                    {conversationItems.map((item) => {
                      if (item.kind === "trace") {
                        return (
                          <div className="conversation-trace" key={item.id}>
                            <ExecutionTrace
                              trace={item.trace}
                              updates={progressUpdates.filter(
                                (update) => update.job_id === item.trace.job_id,
                              )}
                              onOpenAgent={(id) => void openAgentSession(id)}
                            />
                          </div>
                        );
                      }
                      if (item.kind === "update") {
                        const update = item.update;
                        return (
                          <article
                            className="message assistant partial-result"
                            key={item.id}
                            data-testid="partial-result"
                          >
                            <div className="avatar" aria-hidden="true">
                              P
                            </div>
                            <div className="message-body">
                              <div className="message-meta">
                                <strong>{update.model || active.model}</strong>
                                <span className="partial-result-label">
                                  Intermediate result
                                </span>
                                <time>
                                  {new Date(
                                    update.created_at,
                                  ).toLocaleTimeString()}
                                </time>
                                <button onClick={() => void copy(update.text)}>
                                  Copy
                                </button>
                              </div>
                              <MarkdownMessage text={update.text} />
                            </div>
                          </article>
                        );
                      }
                      const m = item.message;
                      return (
                        <article className={"message " + m.role} key={item.id}>
                          <div className="avatar">
                            {m.role === "user" ? "Y" : "◈"}
                          </div>
                          <div className="message-body">
                            <div className="message-meta">
                              <strong>{m.author}</strong>
                              <time>
                                {new Date(m.time).toLocaleTimeString()}
                              </time>
                              <button onClick={() => void copy(m.text)}>
                                Copy
                              </button>
                            </div>
                            <MarkdownMessage text={m.text} />
                            {m.role === "assistant" &&
                              executionTraces.filter(
                                  (trace) =>
                                    trace.job_id === m.job_id &&
                                    replyTraceIndices.get(trace.job_id) ===
                                      item.index &&
                                    trace.job_id !== currentExecutionTrace?.job_id,
                                )
                                .map((trace) => (
                                  <ExecutionTrace
                                    key={trace.job_id}
                                    trace={trace}
                                    updates={progressUpdates.filter(
                                      (update) => update.job_id === trace.job_id,
                                    )}
                                    onOpenAgent={(id) =>
                                      void openAgentSession(id)
                                    }
                                  />
                                ))}
                            {m.items?.map((item, index) => (
                              <p key={index}>{item}</p>
                            ))}
                            {m.plan_steps?.map((step) => (
                              <div className="plan-step" key={step.number}>
                                <b>
                                  {step.number}. {step.title}
                                </b>
                                <p>{step.description}</p>
                              </div>
                            ))}
                          </div>
                        </article>
                      );
                    })}
                    {(active.pending_job ||
                      ["queued", "running", "in_progress"].includes(
                        active.agent_status || "",
                      )) && (
                      <div className="working" role="status">
                        <span className="spinner" />
                        {active.execution_state?.phase
                          ? active.execution_state.phase.replaceAll("_", " ") +
                            (active.execution_state.iteration
                              ? " · Pass " + active.execution_state.iteration
                              : "")
                          : activeReadOnly
                            ? "Agent is working…"
                            : "Connector is working…"}
                      </div>
                    )}
                  </div>
                  <div className="composer-wrap">
                    {currentExecutionTrace && (
                      <ExecutionTrace
                        key={currentExecutionTrace.job_id}
                        trace={currentExecutionTrace}
                        updates={progressUpdates.filter(
                          (update) =>
                            update.job_id === currentExecutionTrace.job_id,
                        )}
                        onOpenAgent={(id) => void openAgentSession(id)}
                      />
                    )}
                    {activeJob && (
                      <details className="job-log">
                        <summary>
                          Request progress · {activeJob.progress}%
                        </summary>
                        <progress value={activeJob.progress} max={100} />
                        <pre>{activeJob.logs.join("\n")}</pre>
                      </details>
                    )}
                    {activeReadOnly ? (
                      <div className="agent-readonly-notice" role="status">
                        <p>
                          This agent session is read-only. You can inspect its
                          results and stop its work at any time.
                        </p>
                        {active.parent_session_id && (
                          <button
                            className="secondary"
                            onClick={() =>
                              void openAgentSession(active.parent_session_id!)
                            }
                          >
                            Return to parent conversation
                          </button>
                        )}
                        <div className="composer agent-readonly-composer">
                          <div className="composer-controls">
                            <span className="muted">
                              {active.model} ·{" "}
                              {
                                effortLevels.find(
                                  (level) => level.value === activeEffort,
                                )?.label
                              }{" "}
                              effort
                            </span>
                            <span className="grow" />
                            {active.pending_job && (
                              <button
                                className="send-button"
                                aria-label="Stop agent"
                                onClick={() => void stop(active.pending_job!)}
                              >
                                ■ Stop
                              </button>
                            )}
                          </div>
                        </div>
                      </div>
                    ) : (
                      <>
                        {!!active.attachments.length && (
                          <div className="attachments">
                            {active.attachments.map((name) => (
                              <span key={name}>▧ {name}</span>
                            ))}
                            <button
                              onClick={() =>
                                void guard(async () => {
                                  await api("/sessions/" + active.id, "PATCH", {
                                    attachments: [],
                                  });
                                  updateLocal(active.id, { attachments: [] });
                                })
                              }
                            >
                              Clear
                            </button>
                          </div>
                        )}
                        <div className="composer">
                          <textarea
                            disabled={busy || !active.project_id}
                            aria-label="Message"
                            placeholder="Ask a question, explore an idea…"
                            value={active.draft}
                            onChange={(e) =>
                              changeDraft(active.id, e.target.value)
                            }
                            onKeyDown={(e) => {
                              if (
                                e.key === "Enter" &&
                                !e.shiftKey &&
                                !e.nativeEvent.isComposing
                              ) {
                                e.preventDefault();
                                if (!active.pending_job) void send();
                              }
                            }}
                          />
                          <div className="composer-controls">
                            <button
                              aria-label="Attach files"
                              disabled={busy || !active.project_id}
                              title="Attach file names as context"
                              onClick={() => attachmentRef.current?.click()}
                            >
                              ＋
                            </button>
                            <select
                              aria-label="Model"
                              value={active.model}
                              disabled={busy || !!active.pending_job}
                              onChange={(e) => void selectModel(e.target.value)}
                            >
                              <option value="">Select configuration</option>
                              {active.model &&
                                !models.some((m) => m.id === active.model) && (
                                  <option value={active.model}>
                                    {active.model} (unavailable)
                                  </option>
                                )}
                              {models.map((m) => (
                                <option value={m.id} key={m.id}>
                                  {m.name}
                                </option>
                              ))}
                            </select>
                            <select
                              aria-label="Effort"
                              title="Task effort"
                              className="effort-select"
                              value={activeEffort}
                              disabled={busy || !!active.pending_job}
                              onChange={(e) =>
                                void selectEffort(e.target.value as Effort)
                              }
                            >
                              {effortLevels.map((level) => (
                                <option key={level.value} value={level.value}>
                                  {level.label} effort
                                </option>
                              ))}
                            </select>
                            <button
                              aria-label="Refresh models"
                              onClick={loadModels}
                            >
                              ↻
                            </button>
                            <span className="grow" />
                            {active.pending_job ? (
                              <button
                                className="send-button"
                                aria-label="Stop response"
                                onClick={() => void stop(active.pending_job!)}
                              >
                                ■ Stop
                              </button>
                            ) : (
                              <button
                                className="send-button"
                                disabled={
                                  busy ||
                                  !active.project_id ||
                                  !active.draft.trim() ||
                                  !models.some((m) => m.id === active.model)
                                }
                                onClick={send}
                              >
                                ↑ Send
                              </button>
                            )}
                          </div>
                        </div>
                        <p className="composer-hint">
                          {modelError ? (
                            <button onClick={() => showDialog("settings")}>
                              Connector disconnected · Open settings
                            </button>
                          ) : (
                            "Enter to send · Shift + Enter for a new line · File attachments add names only"
                          )}
                        </p>
                      </>
                    )}
                  </div>
                </div>
              ) : (
                <div className="welcome empty-workspace">
                  <span className="welcome-logo">◈</span>
                  <span className="eyebrow">CONNECTOR STUDIO</span>
                  <h1>A workspace for your conversations.</h1>
                  {project ? (
                    <>
                      <p>Start a conversation in {project.name}.</p>
                      <button
                        className="primary"
                        disabled={busy}
                        onClick={newSession}
                      >
                        New Session
                      </button>
                      <p className="muted">
                        Open saved conversations from the project's sessions
                        folder in Explorer.
                      </p>
                    </>
                  ) : (
                    <>
                      <p>Create or open a project to start a conversation.</p>
                      <button
                        className="primary"
                        onClick={() => showDialog("project")}
                      >
                        Create a project
                      </button>
                      <button
                        className="text-button"
                        onClick={() => showDialog("open")}
                      >
                        Open a project
                      </button>
                    </>
                  )}
                </div>
              )}
            </>
          )}
        </main>
      </div>
      {(error || notice) && (
        <div
          className={"toast " + (error ? "error" : "")}
          role={error ? "alert" : "status"}
        >
          {error || notice}
          <button
            aria-label="Dismiss notification"
            onClick={() => {
              setError("");
              setNotice("");
            }}
          >
            ×
          </button>
        </div>
      )}
      <footer className="statusbar">
        <span>◈ {project?.name ?? "No project open"}</span>
        <span>
          {models.length ? "● Connector ready" : "○ Connector offline"} ·{" "}
          {tabs.length} open sessions
        </span>
        <span>React + Python</span>
      </footer>
      <input
        hidden
        multiple
        ref={attachmentRef}
        disabled={activeReadOnly || !active?.project_id}
        type="file"
        onChange={(e) => {
          void guard(() =>
            attach(Array.from(e.target.files ?? []).map((f) => f.name)),
          );
          e.target.value = "";
        }}
      />
      {(dialog === "project" ||
        dialog === "file" ||
        dialog === "new-session") && (
        <NewItemDialog
          category={
            dialog === "project"
              ? "projects"
              : dialog === "file"
                ? "files"
                : "sessions"
          }
          projects={projects}
          initialParentPath={defaultParentPath || connection.workspace_root}
          currentProject={project}
          busy={busy}
          onClose={() => setDialog("")}
          onCreate={createItem}
          onOpenProject={openProject}
        />
      )}
      {dialog === "open" && (
        <FileBrowser
          initialPath={
            project?.path || defaultParentPath || connection.workspace_root
          }
          onClose={() => setDialog("")}
          onSelect={openProject}
        />
      )}
      {dialog === "memory" && project && (
        <ProjectMemoryDialog
          key={project.id}
          project={project}
          memory={
            memoryState?.project_id === project.id ? memoryState.data : null
          }
          loading={
            memoryState?.project_id === project.id ? memoryState.loading : true
          }
          error={
            memoryState?.project_id === project.id ? memoryState.error : ""
          }
          onClose={() => setDialog("")}
          onRefresh={() => setMemoryRefresh((value) => value + 1)}
        />
      )}
      {dialog === "settings" && (
        <div className="modal-backdrop" onClick={() => setDialog("")}>
          <section
            className="modal"
            role="dialog"
            aria-modal="true"
            aria-labelledby="dialog-title"
            onClick={(e) => e.stopPropagation()}
            onKeyDown={(e) => {
              if (e.key === "Escape") setDialog("");
            }}
          >
            <button
              className="modal-close"
              aria-label="Close dialog"
              onClick={() => setDialog("")}
            >
              &times;
            </button>
            <span className="eyebrow">CONNECTOR STUDIO</span>
            <h2 id="dialog-title">Connection settings</h2>
            <form
              onSubmit={(e) => {
                e.preventDefault();
                void submitDialog();
              }}
            >
              <label>
                Connector server URL
                <input
                  autoFocus
                  value={settingsDraft.server_url}
                  onChange={(e) =>
                    setSettingsDraft({
                      ...settingsDraft,
                      server_url: e.target.value,
                    })
                  }
                  required
                />
              </label>
              <p className="muted">
                Connector runs separately and exposes /v1/models and
                /v1/chat/completions.
              </p>
              <label>
                Chat font size
                <select
                  value={settingsDraft.font_size}
                  onChange={(e) =>
                    setSettingsDraft({
                      ...settingsDraft,
                      font_size: Number(e.target.value),
                    })
                  }
                >
                  {[11, 12, 14, 16, 18, 20, 24].map((size) => (
                    <option key={size}>{size}</option>
                  ))}
                </select>
              </label>
              <label>
                Maximum parallel agents
                <input
                  type="number"
                  min={1}
                  max={16}
                  step={1}
                  required
                  value={settingsDraft.max_parallel_agents ?? 4}
                  onChange={(e) =>
                    setSettingsDraft({
                      ...settingsDraft,
                      max_parallel_agents: Number(e.target.value),
                    })
                  }
                />
              </label>
              <p className="muted">Limit how many agents can work at once.</p>
              <fieldset className="environment-settings" disabled={busy}>
                <legend>
                  Python environments
                  {environmentProjectName
                    ? " · " + environmentProjectName
                    : ""}
                </legend>
                {environmentProjectId ? (
                  <>
                    {selectedEnvironments?.error && (
                      <p className="project-memory-error" role="alert">
                        {selectedEnvironments.error}
                      </p>
                    )}
                    <label>
                      Selected environment
                      <select
                        aria-label="Python environment"
                        value={selectedEnvironments?.data?.selected || ""}
                        disabled={
                          !selectedEnvironments?.data ||
                          selectedEnvironments.loading
                        }
                        onChange={(e) =>
                          void selectEnvironment(e.target.value)
                        }
                      >
                        {!selectedEnvironments?.data && (
                          <option value="">Loading environments...</option>
                        )}
                        {selectedEnvironments?.data?.environments.map(
                          (environment) => (
                            <option
                              key={environment.name}
                              value={environment.name}
                            >
                              {environment.name}
                              {environment.ready
                                ? ""
                                : " (created on first script)"}
                            </option>
                          ),
                        )}
                      </select>
                    </label>
                    {selectedEnvironments?.data && (
                      <p className="muted">
                        Scripts use this project's selected Python
                        environment.{" "}
                        {selectedEnvironments.data.environments.find(
                          (environment) =>
                            environment.name ===
                            selectedEnvironments.data?.selected,
                        )?.ready
                          ? "Ready to run."
                          : "The selected environment is prepared when a script first runs."}
                      </p>
                    )}
                    <div className="environment-create">
                      <label>
                        New environment name
                        <input
                          value={environmentName}
                          maxLength={64}
                          placeholder="analysis"
                          onChange={(e) => setEnvironmentName(e.target.value)}
                        />
                      </label>
                      <button
                        type="button"
                        className="secondary"
                        disabled={
                          !environmentName.trim() ||
                          selectedEnvironments?.loading
                        }
                        onClick={() => void createEnvironment()}
                      >
                        Create environment
                      </button>
                    </div>
                  </>
                ) : (
                  <p className="muted">
                    Open a project to select or create a Python environment.
                  </p>
                )}
              </fieldset>
              <button
                type="button"
                className="secondary"
                disabled={busy}
                onClick={() =>
                  void guard(async () => {
                    const result = await api<{ models: Model[] }>(
                      "/settings/test",
                      "POST",
                      {
                        server_url: settingsDraft.server_url,
                        font_size: settingsDraft.font_size,
                        max_parallel_agents:
                          settingsDraft.max_parallel_agents,
                      },
                    );
                    setNotice(
                      "Connection successful \u00b7 " +
                        result.models.length +
                        " configurations",
                    );
                  })
                }
              >
                Test Connection
              </button>
              <p className="muted">
                Workspace root: {connection.workspace_root}
              </p>
              <div className="modal-actions">
                <button type="button" onClick={() => setDialog("")}>
                  Cancel
                </button>
                <button className="primary" disabled={busy}>
                  {busy ? "Working..." : "Save"}
                </button>
              </div>
            </form>
          </section>
        </div>
      )}
    </div>
  );
}

function ProjectMemoryDialog({
  project,
  memory,
  loading,
  error,
  onClose,
  onRefresh,
}: {
  project: Project;
  memory: ProjectMemory | null;
  loading: boolean;
  error: string;
  onClose: () => void;
  onRefresh: () => void;
}) {
  const ref = useRef<HTMLElement>(null);
  useDialogFocus(ref, onClose);
  const problem = error || memory?.error;
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <section
        ref={ref}
        className="modal memory-modal"
        role="dialog"
        aria-modal="true"
        aria-labelledby="project-memory-title"
        aria-describedby="project-memory-description"
        tabIndex={-1}
        onClick={(event) => event.stopPropagation()}
      >
        <button
          className="modal-close"
          aria-label="Close project memory"
          onClick={onClose}
          data-dialog-autofocus
        >
          &times;
        </button>
        <span className="eyebrow">{project.name}</span>
        <h2 id="project-memory-title">Project Memory</h2>
        <p id="project-memory-description" className="memory-description">
          Recent exchanges are automatically recalled in conversations in this
          project.
        </p>
        <div className="memory-summary">
          <span>
            <strong>Default model:</strong>{" "}
            {memory?.default_model || "No model selected"}
          </span>
          {memory && (
            <span>Saved interactions: {memory.interaction_count}</span>
          )}
        </div>
        {problem && (
          <p className="project-memory-error" role="alert">
            {problem}
          </p>
        )}
        <div className="memory-records" aria-busy={loading}>
          {loading && (
            <p className="muted" role="status">
              Loading project memory...
            </p>
          )}
          {!loading && !problem && !memory?.interactions.length && (
            <p className="memory-empty">
              Completed conversations will appear here.
            </p>
          )}
          {memory && memory.interaction_count > memory.interactions.length && (
            <p className="muted">
              Showing the latest {memory.interactions.length} of{" "}
              {memory.interaction_count} saved interactions.
            </p>
          )}
          {memory?.interactions.map((interaction, index) => (
            <article
              className="memory-interaction"
              key={interaction.job_id}
              data-testid="memory-interaction"
              aria-label={"Saved interaction " + (index + 1)}
            >
              <div className="memory-interaction-meta">
                <strong>{interaction.model || "Model not recorded"}</strong>
                <time dateTime={interaction.completed_at}>
                  {new Date(interaction.completed_at).toLocaleString()}
                </time>
              </div>
              <span className="memory-label">You</span>
              <p className="memory-user-text">{interaction.user_text}</p>
              <div className="message-body memory-assistant">
                <span className="memory-label">Reply</span>
                <MarkdownMessage text={interaction.assistant_text} />
              </div>
            </article>
          ))}
        </div>
        <div className="modal-actions memory-actions">
          <button className="secondary" onClick={onRefresh} disabled={loading}>
            Refresh
          </button>
          <button className="primary" onClick={onClose}>
            Close
          </button>
        </div>
      </section>
    </div>
  );
}
