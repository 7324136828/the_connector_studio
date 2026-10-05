import MarkdownMessage from "./MarkdownMessage";
import type {
  ExecutionStep,
  ExecutionTraceRecord,
  LiveUpdate,
} from "../services/api";

function statusLabel(status: string) {
  return status
    .replaceAll("_", " ")
    .replace(/^./, (value) => value.toUpperCase());
}

function formatValue(value: unknown) {
  if (value === undefined || value === null)
    return "No observation recorded yet.";
  return typeof value === "string" ? value : JSON.stringify(value, null, 2);
}

function agentDetails(step: ExecutionStep) {
  const result =
    step.result && typeof step.result === "object"
      ? (step.result as Record<string, unknown>)
      : {};
  const sessionId =
    step.agent_session_id ||
    (typeof result.agent_session_id === "string"
      ? result.agent_session_id
      : "");
  return {
    sessionId,
    name:
      step.agent_name ||
      (typeof result.agent_name === "string" ? result.agent_name : "Agent"),
    model: step.model || (typeof result.model === "string" ? result.model : ""),
  };
}

export default function ExecutionTrace({
  trace,
  updates = [],
  onOpenAgent,
}: {
  trace: ExecutionTraceRecord;
  updates?: LiveUpdate[];
  onOpenAgent: (id: string) => void;
}) {
  const agents = trace.steps.filter(
    (step) => step.tool_name === "run_agent" || step.agent_session_id,
  );
  return (
    <section
      className="execution-trace"
      aria-label="Execution trace"
      data-testid="execution-trace"
      data-job-id={trace.job_id}
    >
      <details className="trace-details">
        <summary>
          <strong>Execution trace</strong>
          <span>
            {!!trace.steps.length && (
              <>
                {trace.steps.length}{" "}
                {trace.steps.length === 1 ? "action" : "actions"}
              </>
            )}
            {!!updates.length && (
              <>
                {trace.steps.length ? " \u00b7 " : ""}
                {updates.length} {updates.length === 1 ? "update" : "updates"}
              </>
            )}
            {!trace.steps.length && !updates.length && "0 actions"}
          </span>
          <span className={"trace-status " + trace.status}>
            {statusLabel(trace.status)}
          </span>
        </summary>
        <div className="trace-content">
          <p className="trace-description">
            Public progress summaries, tool actions, arguments, and observations.
          </p>
          {!!agents.length && (
            <div className="trace-agent-grid" aria-label="Agent activity">
              {agents.map((step) => {
                const agent = agentDetails(step);
                return (
                  <article
                    className="trace-agent-card"
                    key={step.id}
                    data-testid="agent-card"
                  >
                    <div className="trace-agent-heading">
                      <span className="trace-agent-avatar" aria-hidden="true">
                        {agent.name.slice(0, 1).toUpperCase()}
                      </span>
                      <div>
                        <strong>{agent.name}</strong>
                        <span>{agent.model || "Read-only agent session"}</span>
                      </div>
                      <span className={"trace-status " + step.status}>
                        {statusLabel(step.status)}
                      </span>
                    </div>
                    {step.summary && <p>{step.summary}</p>}
                    <button
                      className="text-button"
                      disabled={!agent.sessionId}
                      onClick={() => onOpenAgent(agent.sessionId)}
                    >
                      View trace
                    </button>
                  </article>
                );
              })}
            </div>
          )}
          {!!updates.length && (
            <section
              className="trace-progress"
              aria-label="Public progress summaries"
            >
              <h4>Progress summaries</h4>
              {updates.map((update) => (
                <article
                  className="trace-progress-entry"
                  key={update.id}
                  data-testid="trace-progress-update"
                >
                  <div className="trace-progress-meta">
                    <span>
                      {update.kind === "status"
                        ? "Context summary"
                        : "Progress update"}
                    </span>
                    <time dateTime={update.created_at}>
                      {new Date(update.created_at).toLocaleTimeString()}
                    </time>
                  </div>
                  <MarkdownMessage text={update.text} />
                </article>
              ))}
            </section>
          )}
          <ol className="trace-step-list">
            {trace.steps.map((step) => (
              <li key={step.id}>
                <details className="trace-step">
                  <summary>
                    <code>{step.tool_name}</code>
                    <span className={"trace-status " + step.status}>
                      {statusLabel(step.status)}
                    </span>
                  </summary>
                  {step.summary && (
                    <p className="trace-step-summary">{step.summary}</p>
                  )}
                  <dl>
                    <dt>Arguments</dt>
                    <dd>
                      <pre>{formatValue(step.arguments)}</pre>
                    </dd>
                    <dt>Observations</dt>
                    <dd>
                      <pre>{formatValue(step.result)}</pre>
                    </dd>
                    <dt>Status</dt>
                    <dd>{statusLabel(step.status)}</dd>
                    {step.started_at && (
                      <>
                        <dt>Started</dt>
                        <dd>
                          <time dateTime={step.started_at}>
                            {new Date(step.started_at).toLocaleString()}
                          </time>
                        </dd>
                      </>
                    )}
                    {step.completed_at && (
                      <>
                        <dt>Completed</dt>
                        <dd>
                          <time dateTime={step.completed_at}>
                            {new Date(step.completed_at).toLocaleString()}
                          </time>
                        </dd>
                      </>
                    )}
                  </dl>
                </details>
              </li>
            ))}
          </ol>
          {!trace.steps.length && !updates.length && (
            <p className="muted">Waiting for the first tool action.</p>
          )}
        </div>
      </details>
    </section>
  );
}
