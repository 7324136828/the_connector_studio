import { test, expect, type Page } from "@playwright/test";
import type {
  Resource,
  Session,
} from "../../frontend/src/services/api";

import { createProject, openProjectSession } from "../helpers/project-sessions";

test.setTimeout(60000);

async function newConversation(page: Page, label: string) {
  const project = await createProject(page, label);
  const sessionResponse = await page.request.post("/api/sessions", {
    data: { title: label, project_id: project.id },
  });
  expect(sessionResponse.status()).toBe(201);
  const session: Session = await sessionResponse.json();
  await page.goto("/");
  await openProjectSession(page, project, session);
  await expect(page.getByLabel("Message", { exact: true })).toBeVisible();
  await page.getByLabel("Model", { exact: true }).selectOption("test-config");
  return { project, session };
}

async function setEffort(page: Page, session: Session, effort: string) {
  const changed = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/sessions/" + session.id) &&
      response.request().method() === "PATCH" &&
      response.request().postDataJSON()?.effort === effort,
  );
  await page.getByLabel("Effort", { exact: true }).selectOption(effort);
  expect((await changed).status()).toBe(200);
  await expect(page.getByLabel("Effort", { exact: true })).toHaveValue(effort);
}

async function send(page: Page, text: string) {
  const submitted = page.waitForResponse(
    (response) =>
      /\/api\/sessions\/[^/]+\/messages$/.test(response.url()) &&
      response.request().method() === "POST",
  );
  await page.getByLabel("Message", { exact: true }).fill(text);
  await page.getByRole("button", { name: /Send$/ }).click();
  const response = await submitted;
  expect(response.status()).toBe(202);
  return {
    job: await response.json(),
    request: response.request().postDataJSON(),
  };
}

test("effort persists and multi-agent skills require explicit Max opt-in", async ({
  page,
}) => {
  const { project, session } = await newConversation(
    page,
    "Effort resource gating",
  );
  await expect(page.getByLabel("Effort", { exact: true })).toHaveValue("low");
  await page
    .getByRole("button", { name: "Session controls", exact: true })
    .click();
  for (const name of ["run_agent", "create_agent"]) {
    const tool = page.getByLabel(name, { exact: true });
    await expect(tool).toBeDisabled();
    await expect(tool).not.toBeChecked();
  }
  await expect(page.locator(".resource-restriction")).toHaveCount(2);
  await page.screenshot({
    path: "test-results/effort-gating.png",
    fullPage: true,
  });
  await setEffort(page, session, "max");
  for (const name of ["run_agent", "create_agent"]) {
    await expect(page.getByLabel(name, { exact: true })).toBeEnabled();
    await expect(page.getByLabel(name, { exact: true })).not.toBeChecked();
  }
  await page.getByLabel("run_agent", { exact: true }).check();
  await expect(page.getByLabel("run_agent", { exact: true })).toBeChecked();
  await setEffort(page, session, "medium");
  await expect(page.getByLabel("run_agent", { exact: true })).toBeDisabled();
  await expect(page.getByLabel("run_agent", { exact: true })).not.toBeChecked();
  const stored: Session = await (
    await page.request.get("/api/sessions/" + session.id)
  ).json();
  expect(stored.effort).toBe("medium");
  const resources: Resource[] = await (
    await page.request.get(
      "/api/projects/" + project.id + "/resources?effort=medium",
    )
  ).json();
  expect(
    resources.find((resource) => resource.name === "run_agent")?.enabled,
  ).toBe(false);
  await setEffort(page, session, "max");
  await expect(page.getByLabel("run_agent", { exact: true })).not.toBeChecked();
  await page.reload();
  await openProjectSession(page, project, session);
  await expect(page.getByLabel("Effort", { exact: true })).toHaveValue("max");
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(page.getByLabel("Effort", { exact: true })).toBeVisible();
  await expect(page.getByRole("button", { name: /Send$/ })).toBeVisible();
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= window.innerWidth,
    ),
  ).toBeTruthy();
  await page.screenshot({
    path: "test-results/effort-mobile.png",
    fullPage: true,
  });
});

test("Stop replaces Send at every effort and restores the submitted input", async ({
  page,
}) => {
  const { session } = await newConversation(page, "Effort cancellation");
  for (const effort of ["low", "medium", "high", "extra_high", "max"]) {
    await setEffort(page, session, effort);
    const submitted = await send(page, "slow");
    expect(submitted.request.effort).toBe(effort);
    const stop = page.getByRole("button", {
      name: "Stop response",
      exact: true,
    });
    await expect(stop).toBeVisible();
    await expect(page.getByRole("button", { name: /Send$/ })).toHaveCount(0);
    await expect(page.getByLabel("Effort", { exact: true })).toBeDisabled();
    const stopped = page.waitForResponse((response) =>
      response.url().endsWith("/api/jobs/" + submitted.job.id + "/discard"),
    );
    await stop.click();
    expect((await stopped).status()).toBe(200);
    await expect(page.getByRole("button", { name: /Send$/ })).toBeVisible();
    const current: Session = await (
      await page.request.get("/api/sessions/" + session.id)
    ).json();
    expect(current.pending_job).toBeNull();
    expect(current.messages).toHaveLength(0);
    expect(current.draft, "cancelled draft at " + effort + " effort").toBe("slow");
    await expect(page.getByLabel("Message", { exact: true })).toHaveValue(
      "slow",
    );
  }
});

test("public intermediate results appear before completion and persist after cancellation", async ({
  page,
}) => {
  const { project, session } = await newConversation(
    page,
    "Public continuation progress",
  );
  await setEffort(page, session, "medium");
  const first = await send(page, "effort continue");
  await expect(page.getByTestId("partial-result")).toContainText(
    "Partial result: inspected the first input.",
    { timeout: 15000 },
  );
  await expect(
    page.getByRole("button", { name: "Stop response", exact: true }),
  ).toBeVisible();
  await page.screenshot({
    path: "test-results/effort-live-results.png",
    fullPage: true,
  });
  await expect
    .poll(
      async () =>
        (await (await page.request.get("/api/jobs/" + first.job.id)).json())
          .status,
      { timeout: 20000 },
    )
    .toBe("completed");
  await expect(page.locator(".message.assistant:not(.partial-result)").last()).toContainText(
    "Final result: completed the remaining checks.",
    { timeout: 10000 },
  );
  await setEffort(page, session, "high");
  const second = await send(page, "effort cancel");
  await expect(page.getByTestId("partial-result").last()).toContainText(
    "Partial result: ready to continue.",
    { timeout: 15000 },
  );
  await page
    .getByRole("button", { name: "Stop response", exact: true })
    .click();
  await expect(page.getByRole("button", { name: /Send$/ })).toBeVisible();
  await expect(page.getByTestId("partial-result").last()).toContainText(
    "Partial result: ready to continue.",
  );
  const cancelled: Session = await (
    await page.request.get("/api/sessions/" + session.id)
  ).json();
  expect(cancelled.pending_job).toBeNull();
  expect(
    cancelled.live_updates?.some(
      (update) =>
        update.job_id === second.job.id &&
        update.kind === "partial" &&
        update.text.includes("Partial result: ready to continue."),
    ),
  ).toBeTruthy();
  const download = await page.request.get(
    "/api/sessions/" + session.id + "/download",
  );
  expect(download.status()).toBe(200);
  expect((await download.body()).toString("utf8")).toContain(
    "Partial result: ready to continue.",
  );
  await page.reload();
  await openProjectSession(page, project, session);
  await expect(page.getByLabel("Effort", { exact: true })).toHaveValue("high");
  await expect(page.getByTestId("partial-result").last()).toContainText(
    "Partial result: ready to continue.",
  );
  await expect(page.locator("body")).not.toContainText(
    "fixture-private-reasoning",
  );
});

test("a running read-only agent has its own Stop overlay and leaves its parent running", async ({
  page,
}) => {
  const { session } = await newConversation(
    page,
    "Individual agent cancellation",
  );
  await setEffort(page, session, "max");
  await page
    .getByRole("button", { name: "Session controls", exact: true })
    .click();
  await page.getByLabel("run_agent", { exact: true }).check();
  await page.getByLabel("create_agent", { exact: true }).check();
  const submitted = await send(page, "tool cancellable agents");
  const trace = page.locator(
    '[data-testid="execution-trace"][data-job-id="' + submitted.job.id + '"]',
  );
  await expect(trace).toHaveCount(1);
  await trace.locator(".trace-details > summary").click();
  const alpha = trace.getByTestId("agent-card").filter({ hasText: "Alpha" });
  await expect(alpha).toBeVisible({ timeout: 15000 });
  await alpha.getByRole("button", { name: "View trace", exact: true }).click();
  await expect(page.getByLabel("Session title", { exact: true })).toHaveValue(
    "Alpha",
  );
  await expect(
    page.getByLabel("Session title", { exact: true }),
  ).toBeDisabled();
  await expect(page.getByLabel("Message", { exact: true })).toHaveCount(0);
  const stop = page.getByRole("button", { name: "Stop agent", exact: true });
  await expect(stop).toBeVisible();
  await page.screenshot({
    path: "test-results/effort-agent-stop.png",
    fullPage: true,
  });
  const stopped = page.waitForResponse(
    (response) =>
      /\/api\/jobs\/[^/]+\/discard$/.test(response.url()) &&
      response.request().method() === "POST",
  );
  await stop.click();
  const response = await stopped;
  expect(response.status()).toBe(200);
  expect(response.url()).not.toContain(submitted.job.id);
  await expect(stop).not.toBeVisible();
  await page
    .getByRole("button", { name: "Return to parent conversation", exact: true })
    .click();
  await expect
    .poll(
      async () =>
        (await (await page.request.get("/api/jobs/" + submitted.job.id)).json())
          .status,
      { timeout: 40000 },
    )
    .toBe("completed");
  await expect(page.locator(".message.assistant:not(.partial-result)").last()).toContainText(
    "Agent delegation complete; stopped agents stayed cancelled.",
    { timeout: 10000 },
  );
  const parent: Session = await (
    await page.request.get("/api/sessions/" + session.id)
  ).json();
  const step = parent.execution_traces
    ?.flatMap((trace) => trace.steps)
    .find((item) => item.agent_name === "Alpha");
  expect(step?.status).toBe("cancelled");
  const child: Session = await (
    await page.request.get("/api/sessions/" + step!.agent_session_id)
  ).json();
  expect(child.agent_status).toBe("cancelled");
  expect(child.pending_job).toBeNull();
  await expect(page.locator("body")).not.toContainText(
    "fixture-private-reasoning",
  );
});
