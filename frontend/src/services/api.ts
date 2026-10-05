export type Effort = "low" | "medium" | "high" | "extra_high" | "max";

export interface Message {
  role: "user" | "assistant";
  author: string;
  time: string;
  text: string;
  job_id?: string;
  items?: string[];
  plan_steps?: { number: number; title: string; description: string }[];
}
export interface Session {
  id: string;
  title: string;
  project_id: string | null;
  project_name?: string;
  project_path?: string;
  draft: string;
  model: string;
  messages: Message[];
  attachments: string[];
  pending_job: string | null;
  created_at?: string;
  updated_at: string;
  saved_path: string | null;
  save_error?: string | null;
  memory_error?: string | null;
  effort?: Effort;
  live_updates?: LiveUpdate[];
  execution_state?: {
    phase: string;
    iteration: number;
    context_tokens?: number;
    summary_count?: number;
    retry_attempt?: number;
    retry_limit?: number;
    provider_response_timeout?: number;
    error?: string;
  };
  execution_traces?: ExecutionTraceRecord[];
  is_agent?: boolean;
  read_only?: boolean;
  hidden?: boolean;
  parent_session_id?: string;
  agent_name?: string;
  agent_status?: string;
}
export interface LiveUpdate {
  model?: string;
  id: string;
  job_id: string;
  text: string;
  created_at: string;
  kind: "partial" | "status";
}
export interface ExecutionStep {
  id: string;
  tool_name: string;
  arguments: Record<string, unknown>;
  result: unknown;
  status: string;
  started_at?: string | null;
  completed_at?: string | null;
  summary?: string;
  agent_session_id?: string;
  agent_name?: string;
  model?: string;
}
export interface ExecutionTraceRecord {
  job_id: string;
  status: string;
  created_at?: string | null;
  completed_at?: string | null;
  steps: ExecutionStep[];
}
export interface Project {
  id: string;
  name: string;
  path: string;
  managed: boolean;
}
export interface ProjectPreferences {
  project_id: string;
  default_model: string;
  error: string | null;
}
export interface ProjectEnvironments {
  project_id: string;
  selected: string;
  environments: {
    name: string;
    path: string;
    python: string;
    ready: boolean;
  }[];
}
export interface ProjectMemory extends ProjectPreferences {
  interaction_count: number;
  interactions: {
    job_id: string;
    session_id: string;
    model: string;
    completed_at: string;
    user_text: string;
    assistant_text: string;
  }[];
  context_limit: number;
}
export interface Resource {
  id: string;
  name: string;
  kind: "skill" | "agent" | "mcp";
  enabled: boolean;
  disabled_reason?: string;
  priority: number;
  description: string;
}
export interface Job {
  id: string;
  effort?: Effort;
  provider_response_timeout?: number;
  provider_timeout_retries?: number;
  session_id: string;
  filename: string;
  file_size: number;
  kind: string;
  status: string;
  progress: number;
  created_at: string;
  completed_at: string | null;
  error_message: string | null;
  logs: string[];
  download_available: boolean;
  hidden?: boolean;
}
export interface Connection {
  server_url: string;
  font_size: number;
  workspace_root: string;
  max_parallel_agents: number;
  provider_response_timeout?: number;
  provider_timeout_retries?: number;
}
export interface Model {
  id: string;
  name: string;
}
export interface Entry {
  name: string;
  path: string;
  directory: boolean;
  depth: number;
}

export async function api<T>(
  path: string,
  method = "GET",
  body?: unknown,
): Promise<T> {
  const response = await fetch("/api" + path, {
    method,
    headers:
      body instanceof FormData
        ? undefined
        : { "Content-Type": "application/json" },
    body:
      body instanceof FormData
        ? body
        : body === undefined
          ? undefined
          : JSON.stringify(body),
  });
  if (!response.ok) {
    const payload = await response.json().catch(() => null);
    const detail = payload?.detail;
    throw new Error(
      typeof detail === "string"
        ? detail
        : "Request failed (" +
          response.status +
          "). Check the input and try again.",
    );
  }
  return response.json() as Promise<T>;
}
