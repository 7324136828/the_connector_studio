import { test, expect, type Page } from "@playwright/test";
import { existsSync, readFileSync } from "node:fs";
import { basename, join } from "node:path";
import { selectProject, openProjectSession, writeProjectSnapshot } from "../helpers/project-sessions";
import type {
  Project,
  ProjectEnvironments,
  Resource,
  Session,
} from "../../frontend/src/services/api";

test.setTimeout(60000);

async function projectSession(page: Page, label: string) {
  const projectResponse = await page.request.post("/api/projects", {
    data: { name: label + " " + Date.now() },
  });
  expect(projectResponse.status()).toBe(201);
  const project: Project = await projectResponse.json();
  await page.goto("/");
  await selectProject(page, project);
  const created = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/sessions") &&
      response.request().method() === "POST",
  );
  await page.getByRole("button", { name: "Add session", exact: true }).click();
  const response = await created;
  expect(response.status()).toBe(201);
  const session: Session = await response.json();
  return { project, session };
}

async function sendToolPrompt(page: Page, prompt: string, reply: string) {
  if (prompt === "tool parallel agents" || prompt.startsWith("tool create ")) {
    await page.getByLabel("Effort", { exact: true }).selectOption("max");
    await expect(page.getByLabel("Effort", { exact: true })).toHaveValue("max");
    await page
      .getByRole("button", { name: "Session controls", exact: true })
      .click();
    for (const name of ["run_agent", "create_agent"]) {
      const tool = page.getByLabel(name, { exact: true });
      await expect(tool).toBeEnabled();
      await tool.check();
      await expect(tool).toBeChecked();
    }
  }
  await page.getByLabel("Model", { exact: true }).selectOption("test-config");
  const submitted = page.waitForResponse(
    (response) =>
      /\/api\/sessions\/[^/]+\/messages$/.test(response.url()) &&
      response.request().method() === "POST",
  );
  await page.getByLabel("Message", { exact: true }).fill(prompt);
  await page.getByRole("button", { name: /Send$/ }).click();
  const response = await submitted;
  expect(response.status()).toBe(202);
  const job = await response.json();
  await expect
    .poll(
      async () =>
        (await (await page.request.get("/api/jobs/" + job.id)).json()).status,
      { timeout: 45000 },
    )
    .toBe("completed");
  await expect(page.locator(".message.assistant:not(.partial-result)").last()).toContainText(reply, {
    timeout: 15000,
  });
  await expect(page.getByRole("button", { name: /Send$/ })).toBeVisible();
  return job;
}

test("managed environment selection and concurrency persist, and Python stdout is inspectable in an expandable trace", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1280, height: 1100 });
  const { project } = await projectSession(page, "Python tool settings");
  const initial: ProjectEnvironments = await (
    await page.request.get("/api/projects/" + project.id + "/environments")
  ).json();
  expect(initial.selected).toBe("default");
  expect(
    initial.environments.find((environment) => environment.name === "default")
      ?.ready,
  ).toBe(false);
  await page
    .getByRole("button", { name: "Connection settings", exact: true })
    .click();
  const dialog = page.getByRole("dialog", {
    name: "Connection settings",
    exact: true,
  });
  await expect(dialog.locator("legend")).toContainText(project.name);
  const concurrency = dialog.getByLabel("Maximum parallel agents", {
    exact: true,
  });
  await expect(concurrency).toHaveAttribute("min", "1");
  await expect(concurrency).toHaveAttribute("max", "16");
  await concurrency.fill("2");
  await dialog
    .getByLabel("New environment name", { exact: true })
    .fill("analysis");
  const created = page.waitForResponse(
    (response) =>
      response
        .url()
        .endsWith("/api/projects/" + project.id + "/environments") &&
      response.request().method() === "POST",
  );
  await dialog
    .getByRole("button", { name: "Create environment", exact: true })
    .click();
  expect((await created).status()).toBe(201);
  await dialog
    .getByLabel("Python environment", { exact: true })
    .selectOption("analysis");
  await expect(
    dialog.getByLabel("Python environment", { exact: true }),
  ).toHaveValue("analysis");
  await expect(dialog).toContainText("Ready to run.");
  const selectedDefault = page.waitForResponse(
    (response) =>
      response
        .url()
        .endsWith("/api/projects/" + project.id + "/environments") &&
      response.request().method() === "PATCH",
  );
  await dialog
    .getByLabel("Python environment", { exact: true })
    .selectOption("default");
  expect((await selectedDefault).status()).toBe(200);
  await expect(
    dialog.getByLabel("Python environment", { exact: true }),
  ).toHaveValue("default");
  await expect(dialog).toContainText(
    "The selected environment is prepared when a script first runs.",
  );
  const lazyDefault: ProjectEnvironments = await (
    await page.request.get("/api/projects/" + project.id + "/environments")
  ).json();
  expect(lazyDefault.selected).toBe("default");
  expect(
    lazyDefault.environments.find(
      (environment) => environment.name === "default",
    )?.ready,
  ).toBe(false);
  await dialog
    .getByLabel("Python environment", { exact: true })
    .selectOption("analysis");
  await expect(
    dialog.getByLabel("Python environment", { exact: true }),
  ).toHaveValue("analysis");
  await page.screenshot({
    path: "test-results/tool-settings.png",
    fullPage: true,
  });
  const saved = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/settings") &&
      response.request().method() === "POST",
  );
  await dialog.getByRole("button", { name: "Save", exact: true }).click();
  expect((await saved).status()).toBe(200);
  await expect(dialog).not.toBeVisible();
  await page
    .getByRole("button", { name: "Connection settings", exact: true })
    .click();
  await expect(
    dialog.getByLabel("Maximum parallel agents", { exact: true }),
  ).toHaveValue("2");
  await expect(
    dialog.getByLabel("Python environment", { exact: true }),
  ).toHaveValue("analysis");
  await dialog
    .getByRole("button", { name: "Close dialog", exact: true })
    .click();
  const environments: ProjectEnvironments = await (
    await page.request.get("/api/projects/" + project.id + "/environments")
  ).json();
  const analysis = environments.environments.find(
    (environment) => environment.name === "analysis",
  )!;
  expect(analysis.ready).toBe(true);
  expect(existsSync(analysis.python)).toBeTruthy();
  await sendToolPrompt(
    page,
    "tool python",
    "Python execution complete: fixture-42.",
  );
  const trace = page.getByTestId("execution-trace");
  await expect(trace).toHaveCount(1);
  await expect(trace.locator(".trace-content")).not.toBeVisible();
  await trace.locator(".trace-details > summary").click();
  await trace.locator(".trace-step > summary").click();
  await expect(trace.getByText("Arguments", { exact: true })).toBeVisible();
  await expect(trace.getByText("Observations", { exact: true })).toBeVisible();
  await expect(trace.locator("pre").first()).toContainText(
    "print('fixture-42')",
  );
  await expect(trace.locator("pre").last()).toContainText("fixture-42");
  await expect(trace.locator("pre").last()).toContainText(
    '"environment": "analysis"',
  );
  await expect(page.locator("body")).not.toContainText(
    "fixture-private-reasoning",
  );
  await page.screenshot({
    path: "test-results/tool-trace.png",
    fullPage: true,
  });
  await trace.locator(".trace-details > summary").click();
  await expect(trace.locator(".trace-content")).not.toBeVisible();
});

test("agent cards open hidden read-only sessions, preserve their IDs, and closing a child does not cancel work", async ({
  page,
}) => {
  const { session } = await projectSession(page, "Parallel agent views");
  const parentJob = await sendToolPrompt(
    page,
    "tool parallel agents",
    "Parallel agent work complete.",
  );
  const parentTrace = await expandTrace(page, parentJob.id);
  const cards = parentTrace.getByTestId("agent-card");
  await expect(cards).toHaveCount(2);
  await expect(cards.filter({ hasText: "Alpha" })).toContainText(
    "test-config-alt",
  );
  await expect(cards.filter({ hasText: "Beta" })).toContainText("test-config");
  await expect(
    page.locator(".session-row").filter({ hasText: /Alpha|Beta/ }),
  ).toHaveCount(0);
  await page.getByRole("button", { name: /^History/ }).click();
  await expect(
    page.locator(".history tbody tr").filter({ hasText: /Alpha|Beta/ }),
  ).toHaveCount(0);
  await page.getByRole("button", { name: "Workspace", exact: true }).click();
  await expandTrace(page, parentJob.id);
  const parent: Session = await (
    await page.request.get("/api/sessions/" + session.id)
  ).json();
  const alphaId = parent
    .execution_traces!.flatMap((trace) => trace.steps)
    .find((step) => step.agent_name === "Alpha")!.agent_session_id!;
  const opened = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/sessions/" + alphaId) &&
      response.request().method() === "GET",
  );
  await cards
    .filter({ hasText: "Alpha" })
    .getByRole("button", { name: "View trace", exact: true })
    .click();
  const child: Session = await (await opened).json();
  expect(child.id).toBe(alphaId);
  expect(child.parent_session_id).toBe(session.id);
  expect(child.read_only).toBe(true);
  expect(child.hidden).toBe(true);
  await expect(page.getByLabel("Session title", { exact: true })).toHaveValue(
    "Alpha",
  );
  await expect(
    page.getByLabel("Session title", { exact: true }),
  ).toBeDisabled();
  await expect(page.getByLabel("Message", { exact: true })).toHaveCount(0);
  await expect(
    page.getByRole("button", { name: "Attach files", exact: true }),
  ).toHaveCount(0);
  await expect(page.locator(".message.assistant:not(.partial-result)")).toContainText(
    "test-config-alt",
  );
  await page
    .getByRole("button", { name: "Session controls", exact: true })
    .click();
  await expect(
    page.getByLabel("Sidebar model", { exact: true }),
  ).toBeDisabled();
  await page
    .locator("header summary")
    .filter({ hasText: /^Session$/ })
    .click();
  await expect(page.getByRole("button", { name: "Open Session", exact: true })).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Import .lattice", exact: true })).toHaveCount(0);
  await page.locator("header summary").filter({ hasText: /^Session$/ }).click();
  const saved = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/sessions/" + alphaId + "/save") &&
      response.request().method() === "POST",
  );
  await page.keyboard.press("Control+s");
  expect((await saved).status()).toBe(200);
  await page.screenshot({
    path: "test-results/agent-readonly.png",
    fullPage: true,
  });
  const discarded: string[] = [];
  page.on("request", (request) => {
    if (/\/discard$/.test(request.url())) discarded.push(request.url());
  });
  await page.getByRole("button", { name: "Close Alpha", exact: true }).click();
  await expandTrace(page, parentJob.id);
  await expect(page.getByTestId("agent-card")).toHaveCount(2);
  expect(discarded).toEqual([]);
  const reopened = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/sessions/" + alphaId) &&
      response.request().method() === "GET",
  );
  await page
    .getByTestId("agent-card")
    .filter({ hasText: "Alpha" })
    .getByRole("button", { name: "View trace", exact: true })
    .click();
  expect((await (await reopened).json()).id).toBe(alphaId);
  await page
    .getByRole("button", { name: "Return to parent conversation", exact: true })
    .click();
  await expect(page.getByLabel("Message", { exact: true })).toBeVisible();
  await expandTrace(page, parentJob.id);
  await expect(page.getByTestId("agent-card")).toHaveCount(2);
  await expect(page.locator("body")).not.toContainText(
    "fixture-private-reasoning",
  );
  await page.screenshot({
    path: "test-results/agent-cards.png",
    fullPage: true,
  });
});

test("native version-two downloads and project file copies preserve public traces and reopen hidden agent snapshots", async ({
  page,
}) => {
  const { project, session } = await projectSession(
    page,
    "Native agent trace storage",
  );
  await sendToolPrompt(
    page,
    "tool parallel agents",
    "Parallel agent work complete.",
  );
  const stored: Session = await (
    await page.request.get("/api/sessions/" + session.id)
  ).json();
  const disk = readFileSync(stored.saved_path!);
  expect(disk.readUInt32LE(8)).toBe(2);
  expect(disk.toString("utf8")).not.toContain("fixture-private-reasoning");
  await page.reload();
  await selectProject(page, project);
  await page
    .locator(".file-list")
    .getByTitle("sessions/" + basename(stored.saved_path!), { exact: true })
    .click();
  await expandTrace(page, stored.execution_traces![0].job_id);
  await expect(page.getByTestId("agent-card")).toHaveCount(2);
  const download = await page.request.get(
    "/api/sessions/" + session.id + "/download",
  );
  expect(download.status()).toBe(200);
  const buffer = await download.body();
  expect(buffer.readUInt32LE(8)).toBe(2);
  const path = writeProjectSnapshot(project, buffer, "agent-conversation.lattice");
  await selectProject(page, project);
  const opened = page.waitForResponse((response) => response.url().endsWith("/api/projects/" + project.id + "/sessions/open") && response.request().method() === "POST");
  await page.locator(".file-list").getByTitle(path, { exact: true }).click();
  const response = await opened;
  expect(response.status()).toBe(200);
  const imported: Session = await response.json();
  expect(imported.id).not.toBe(session.id);
  expect(imported.execution_traces).toHaveLength(1);
  expect(imported.execution_traces![0].steps).toHaveLength(2);
  await expandTrace(page, imported.execution_traces![0].job_id);
  await expect(page.getByTestId("agent-card")).toHaveCount(2);
  const alphaId = imported.execution_traces![0].steps.find(
    (step) => step.agent_name === "Alpha",
  )!.agent_session_id!;
  const childResponse = await page.request.get("/api/sessions/" + alphaId);
  expect(childResponse.status()).toBe(200);
  const child: Session = await childResponse.json();
  expect(child.parent_session_id).toBe(imported.id);
  expect(child.is_agent).toBe(true);
  expect(child.hidden).toBe(true);
  await page
    .getByTestId("agent-card")
    .filter({ hasText: "Alpha" })
    .getByRole("button", { name: "View trace", exact: true })
    .click();
  await expect(
    page.getByLabel("Session title", { exact: true }),
  ).toBeDisabled();
  await expect(page.locator(".message.assistant:not(.partial-result)")).toContainText(
    "Agent completed fixture agent alpha",
  );
  await expect(page.getByTestId("execution-trace")).toHaveCount(1);
  await expect(page.locator("body")).not.toContainText(
    "fixture-private-reasoning",
  );
});

test("the five built-in skills can create reusable project skill, agent, and MCP definitions from a conversation", async ({
  page,
}) => {
  const { project } = await projectSession(page, "Conversation-created tools");
  const builtins = [
    "run_python_script",
    "run_batch_script",
    "create_tool_from_conversation",
    "create_agent",
    "run_agent",
  ];
  const initial: Resource[] = await (
    await page.request.get("/api/projects/" + project.id + "/resources")
  ).json();
  expect(
    initial
      .filter((resource) => resource.kind === "skill")
      .map((resource) => resource.name)
      .sort(),
  ).toEqual(builtins.sort());
  for (const kind of ["skill", "agent", "mcp"] as const) {
    await sendToolPrompt(
      page,
      "tool create " + kind,
      "Created the requested project resource.",
    );
    const resources: Resource[] = await (
      await page.request.get("/api/projects/" + project.id + "/resources")
    ).json();
    expect(
      resources.some(
        (resource) =>
          resource.kind === kind && resource.name === "fixture-" + kind,
      ),
    ).toBeTruthy();
    const filename =
      kind === "skill"
        ? "SKILL.md"
        : kind === "agent"
          ? "AGENT.md"
          : "mcp.json";
    const path = join(project.path, "." + kind, "fixture-" + kind, filename);
    expect(existsSync(path)).toBeTruthy();
    expect(readFileSync(path, "utf8")).not.toContain(
      "fixture-private-reasoning",
    );
  }
  await expect(page.getByTestId("execution-trace")).toHaveCount(3);
  await page
    .getByRole("button", { name: "Session controls", exact: true })
    .click();
  for (const name of builtins)
    await expect(page.getByLabel(name, { exact: true })).toBeChecked();
  await expect(page.getByLabel("fixture-mcp", { exact: true })).toBeDisabled();
});

async function startTracedPrompt(page: Page, text: string) {
  const submitted = page.waitForResponse(
    (response) =>
      /\/api\/sessions\/[^/]+\/messages$/.test(response.url()) &&
      response.request().method() === "POST",
  );
  await page.getByLabel("Message", { exact: true }).fill(text);
  await page.getByRole("button", { name: /Send$/ }).click();
  const response = await submitted;
  expect(response.status()).toBe(202);
  return response.json();
}

function traceSelector(jobId: string) {
  return '[data-testid="execution-trace"][data-job-id="' + jobId + '"]';
}

async function expandTrace(page: Page, jobId: string) {
  const trace = page.locator(traceSelector(jobId));
  await expect(trace).toHaveCount(1);
  const details = trace.locator(".trace-details").first();
  if (!(await details.evaluate((element) => element.hasAttribute("open"))))
    await details.locator(":scope > summary").click();
  await expect(details).toHaveAttribute("open", "");
  return trace;
}

async function reopenProjectSession(
  page: Page,
  project: Project,
  session: Session,
) {
  const stored: Session = await (
    await page.request.get("/api/sessions/" + session.id)
  ).json();
  await page.reload();
  await selectProject(page, project);
  await page
    .locator(".file-list")
    .getByTitle("sessions/" + basename(stored.saved_path!), { exact: true })
    .click();
  await expect(page.getByLabel("Message", { exact: true })).toBeVisible();
}

test("completed traces stay in conversation during a second send, and cancelled traces leave the composer and persist in history", async ({
  page,
}) => {
  const { project, session } = await projectSession(
    page,
    "Historical trace placement",
  );
  const completed = await sendToolPrompt(
    page,
    "tool python",
    "Python execution complete: fixture-42.",
  );
  const completedSelector = traceSelector(completed.id);
  await expect(
    page.locator(".conversation").locator(completedSelector),
  ).toHaveCount(1);
  await expect(
    page.locator(".composer-wrap").getByTestId("execution-trace"),
  ).toHaveCount(0);
  const pending = await startTracedPrompt(page, "tool trace slow");
  const pendingSelector = traceSelector(pending.id);
  await expect(
    page.locator(".composer-wrap").locator(pendingSelector),
  ).toBeVisible({ timeout: 15000 });
  await expect(
    page
      .locator(".composer-wrap")
      .locator(pendingSelector)
      .locator(".trace-step"),
  ).toHaveCount(1, { timeout: 15000 });
  await expect(
    page.locator(".composer-wrap").getByTestId("execution-trace"),
  ).toHaveCount(1);
  await expect(
    page.locator(".conversation").locator(completedSelector),
  ).toHaveCount(1);
  await expect(
    page.locator(".conversation").locator(pendingSelector),
  ).toHaveCount(0);
  const historical = page.locator(".conversation").locator(completedSelector);
  await historical.locator(".trace-details > summary").click();
  await historical.locator(".trace-step > summary").click();
  await expect(historical.locator("pre").last()).toContainText("fixture-42");
  await historical.locator(".trace-details > summary").click();
  const desktopViewport = page.viewportSize();
  await page.screenshot({
    path: "test-results/trace-second-task.png",
    fullPage: true,
  });
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(
    page.getByRole("button", { name: "Stop response", exact: true }),
  ).toBeVisible();
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= window.innerWidth,
    ),
  ).toBeTruthy();
  await page.screenshot({
    path: "test-results/trace-second-task-mobile.png",
    fullPage: true,
  });
  if (desktopViewport) await page.setViewportSize(desktopViewport);
  const stopped = page.waitForResponse((response) =>
    response.url().endsWith("/api/jobs/" + pending.id + "/discard"),
  );
  await page
    .getByRole("button", { name: "Stop response", exact: true })
    .click();
  expect((await stopped).status()).toBe(200);
  await expect(
    page.locator(".composer-wrap").getByTestId("execution-trace"),
  ).toHaveCount(0);
  await expect(
    page.locator(".conversation").locator(pendingSelector),
  ).toBeVisible();
  await expect(
    page
      .locator(".conversation")
      .locator(pendingSelector)
      .locator(".trace-details > summary"),
  ).toContainText("Cancelled");
  await expect(
    page.locator(".conversation").getByTestId("execution-trace"),
  ).toHaveCount(2);
  const cancelled: Session = await (
    await page.request.get("/api/sessions/" + session.id)
  ).json();
  expect(
    cancelled.messages.some(
      (message) =>
        message.role === "assistant" && message.job_id === pending.id,
    ),
  ).toBe(false);
  expect(
    cancelled.execution_traces?.find((trace) => trace.job_id === pending.id)
      ?.status,
  ).toBe("cancelled");
  await reopenProjectSession(page, project, session);
  await expect(
    page.locator(".composer-wrap").getByTestId("execution-trace"),
  ).toHaveCount(0);
  await expect(
    page.locator(".conversation").locator(completedSelector),
  ).toHaveCount(1);
  await expect(
    page.locator(".conversation").locator(pendingSelector),
  ).toHaveCount(1);
  const archived = page.locator(".conversation").locator(pendingSelector);
  await archived.locator(".trace-details > summary").click();
  await archived.locator(".trace-step > summary").click();
  await expect(archived.locator("pre").first()).toContainText(
    "tool trace slow",
  );
  await page.screenshot({
    path: "test-results/archived-cancelled-trace.png",
    fullPage: true,
  });
});

test("failed traces with no committed assistant answer stay in conversation after reload", async ({
  page,
}) => {
  const { project, session } = await projectSession(
    page,
    "Failed trace placement",
  );
  await page.getByLabel("Model", { exact: true }).selectOption("test-config");
  const failed = await startTracedPrompt(page, "tool trace failure");
  await expect
    .poll(
      async () =>
        (await (await page.request.get("/api/jobs/" + failed.id)).json())
          .status,
      { timeout: 45000 },
    )
    .toBe("failed");
  const selector = traceSelector(failed.id);
  await expect(page.locator(".conversation").locator(selector)).toBeVisible({
    timeout: 10000,
  });
  await expect(
    page.locator(".composer-wrap").getByTestId("execution-trace"),
  ).toHaveCount(0);
  const stored: Session = await (
    await page.request.get("/api/sessions/" + session.id)
  ).json();
  expect(
    stored.messages.some(
      (message) => message.role === "assistant" && message.job_id === failed.id,
    ),
  ).toBe(false);
  expect(
    stored.execution_traces?.find((trace) => trace.job_id === failed.id)
      ?.status,
  ).toBe("failed");
  await reopenProjectSession(page, project, session);
  const historical = page.locator(".conversation").locator(selector);
  await expect(historical).toHaveCount(1);
  await expect(
    page.locator(".composer-wrap").getByTestId("execution-trace"),
  ).toHaveCount(0);
  await historical.locator(".trace-details > summary").click();
  await historical.locator(".trace-step > summary").click();
  await expect(historical.locator("pre").last()).toContainText(
    "trace-before-connector-failure",
  );
  await page.screenshot({
    path: "test-results/archived-failed-trace.png",
    fullPage: true,
  });
});

test("imported duplicate job IDs attach a trace once to the last reply and undated orphans follow their task", async ({
  page,
}) => {
  const { project, session } = await projectSession(page, "Imported trace rendering");
  const completedId = "11111111-1111-4111-8111-111111111111";
  const orphanId = "22222222-2222-4222-8222-222222222222";
  const unmatchedId = "33333333-3333-4333-8333-333333333333";
  const fixture: Session = {
    ...session,
    title: "Imported trace rendering fixture",
    model: "test-config",
    pending_job: null,
    live_updates: [],
    created_at: "2026-10-05T11:59:00Z",
    updated_at: "2026-10-05T12:04:00Z",
    messages: [
      {
        role: "user",
        author: "You",
        time: "2026-10-05T12:00:00Z",
        text: "Original task",
        job_id: completedId,
      },
      {
        role: "assistant",
        author: "test-config",
        time: "2026-10-05T12:01:00Z",
        text: "First imported reply",
        job_id: completedId,
      },
      {
        role: "assistant",
        author: "test-config",
        time: "2026-10-05T12:02:00Z",
        text: "Last imported reply",
        job_id: completedId,
      },
      {
        role: "user",
        author: "You",
        time: "2026-10-05T12:03:00Z",
        text: "Undated follow-up task",
        job_id: orphanId,
      },
    ],
    execution_traces: [
      { job_id: completedId, status: "completed", steps: [] },
      { job_id: orphanId, status: "cancelled", steps: [] },
      { job_id: unmatchedId, status: "failed", steps: [] },
    ],
  };
  // These are valid session rendering states; routing isolates UI topology from runtime scheduling.
  await page.route("**/api/sessions?include_agents=true", async (route) => {
    const response = await route.fetch();
    const sessions: Session[] = await response.json();
    await route.fulfill({
      response,
      json: sessions.map((value) =>
        value.id === session.id ? fixture : value,
      ),
    });
  });
  await page.route("**/api/projects/" + project.id + "/sessions/open", (route) => route.fulfill({ status: 200, json: fixture }));
  await page.reload();
  await openProjectSession(page, project, session);
  const conversation = page.locator(".conversation");
  await expect(conversation.locator(".message.assistant:not(.partial-result)")).toHaveCount(2);
  await expect(conversation.locator(traceSelector(completedId))).toHaveCount(1);
  await expect(
    conversation
      .locator(".message.assistant:not(.partial-result)")
      .first()
      .getByTestId("execution-trace"),
  ).toHaveCount(0);
  await expect(
    conversation
      .locator(".message.assistant:not(.partial-result)")
      .last()
      .locator(traceSelector(completedId)),
  ).toHaveCount(1);
  await expect(conversation.locator(traceSelector(orphanId))).toHaveCount(1);
  await expect(
    page.locator(".composer-wrap").getByTestId("execution-trace"),
  ).toHaveCount(0);
  const orphanFollowsTask = await page.evaluate((jobId) => {
    const task = Array.from(
      document.querySelectorAll(".conversation .message.user"),
    ).find((value) => value.textContent?.includes("Undated follow-up task"));
    const trace = document.querySelector(
      '.conversation [data-testid="execution-trace"][data-job-id="' +
        jobId +
        '"]',
    );
    return (
      !!task &&
      !!trace &&
      !!(task.compareDocumentPosition(trace) & Node.DOCUMENT_POSITION_FOLLOWING)
    );
  }, orphanId);
  expect(orphanFollowsTask).toBeTruthy();
  await expect(conversation.locator(traceSelector(unmatchedId))).toHaveCount(1);
  fixture.updated_at = "2026-10-05T12:10:00Z";
  fixture.messages.push(
    {
      role: "user",
      author: "You",
      time: "2026-10-05T12:05:00Z",
      text: "Newer conversation task",
    },
    {
      role: "assistant",
      author: "test-config",
      time: "2026-10-05T12:06:00Z",
      text: "Newer conversation reply",
    },
  );
  await page.reload();
  await openProjectSession(page, project, session);
  await expect(conversation.locator(".message.assistant:not(.partial-result)").last()).toContainText(
    "Newer conversation reply",
  );
  await expect(conversation.locator(traceSelector(unmatchedId))).toHaveCount(1);
  await expect(
    page.locator(".composer-wrap").getByTestId("execution-trace"),
  ).toHaveCount(0);
  const unmatchedPrecedesNewTask = await page.evaluate((jobId) => {
    const task = Array.from(
      document.querySelectorAll(".conversation .message.user"),
    ).find((value) => value.textContent?.includes("Newer conversation task"));
    const trace = document.querySelector(
      '.conversation [data-testid="execution-trace"][data-job-id="' +
        jobId +
        '"]',
    );
    return (
      !!task &&
      !!trace &&
      !!(trace.compareDocumentPosition(task) & Node.DOCUMENT_POSITION_FOLLOWING)
    );
  }, unmatchedId);
  expect(unmatchedPrecedesNewTask).toBeTruthy();
  await expect(conversation.locator(traceSelector(completedId))).toHaveCount(1);
  await expect(conversation.locator(traceSelector(orphanId))).toHaveCount(1);
  await page.screenshot({
    path: "test-results/imported-trace-order.png",
    fullPage: true,
  });
});
