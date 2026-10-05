import { test, expect, type Page } from "@playwright/test";
import type { Job, Project, Session } from "../../frontend/src/services/api";

import { createProject, openProjectSession, readNativeSnapshot } from "../helpers/project-sessions";

test.setTimeout(60000);

async function conversation(page: Page, title: string, effort = "low") {
  const project = await createProject(page, title);
  const created = await page.request.post("/api/sessions", {
    data: { title, project_id: project.id },
  });
  expect(created.status()).toBe(201);
  const session: Session = await created.json();
  if (effort !== "low") {
    expect((await page.request.patch("/api/sessions/" + session.id, {
      data: { effort },
    })).status()).toBe(200);
  }
  await page.goto("/");
  await openProjectSession(page, project, session);
  await expect(page.getByLabel("Session title", { exact: true })).toHaveValue(title);
  await page.getByLabel("Model", { exact: true }).selectOption("test-config");
  return { project, session };
}

async function send(page: Page, text: string, reply: string) {
  const submitted = page.waitForResponse((response) =>
    /\/api\/sessions\/[^/]+\/messages$/.test(response.url()) &&
    response.request().method() === "POST",
  );
  await page.getByLabel("Message", { exact: true }).fill(text);
  await page.getByRole("button", { name: /Send$/ }).click();
  const response = await submitted;
  expect(response.status()).toBe(202);
  const job: Job = await response.json();
  await expect.poll(async () =>
    (await (await page.request.get("/api/jobs/" + job.id)).json()).status,
    { timeout: 45000 },
  ).toBe("completed");
  await expect(page.locator(".message.assistant:not(.partial-result)").last())
    .toContainText(reply, { timeout: 10000 });
  return job;
}

function savedTrace(page: Page, jobId: string) {
  return page.locator('.conversation [data-testid="execution-trace"][data-job-id="' + jobId + '"]');
}

async function reopen(page: Page, project: Project, session: Session) {
  await page.reload();
  await openProjectSession(page, project, session);
  await expect(page.getByLabel("Session title", { exact: true })).toHaveValue(session.title);
}

test("public progress and tool observations persist inside collapsed conversation traces", async ({ page }) => {
  const { project, session } = await conversation(page, "Collapsed public tool activity");
  const job = await send(page, "tool public progress", "Public progress task complete.");
  const trace = savedTrace(page, job.id);
  await expect(trace).toHaveCount(1);
  await expect(trace.locator(".trace-content")).not.toBeVisible();
  await expect(trace.getByTestId("trace-progress-update")).not.toBeVisible();
  await trace.locator(".trace-details > summary").click();
  await expect(trace.getByTestId("trace-progress-update"))
    .toContainText("Public progress: checking the project inputs.");
  await trace.locator(".trace-step > summary").click();
  await expect(trace.locator(".trace-step pre").last())
    .toContainText("public-progress-observation");
  await trace.locator(".trace-details > summary").click();
  await expect(trace.locator(".trace-content")).not.toBeVisible();
  const download = await page.request.get("/api/sessions/" + session.id + "/download");
  expect(download.status()).toBe(200);
  const bytes = await download.body();
  expect(bytes.toString("utf8")).toContain("Public progress: checking the project inputs.");
  expect(bytes.toString("utf8")).not.toContain("fixture-private-reasoning");
  await reopen(page, project, session);
  await expect(savedTrace(page, job.id).locator(".trace-content")).not.toBeVisible();
  await savedTrace(page, job.id).locator(".trace-details > summary").click();
  await expect(savedTrace(page, job.id).getByTestId("trace-progress-update"))
    .toContainText("Public progress: checking the project inputs.");
  await expect(page.locator("body")).not.toContainText("fixture-private-reasoning");
  await page.screenshot({ path: "test-results/conversation-public-activity.png", fullPage: true });
  const restored = await readNativeSnapshot(page, bytes, "Public activity disk restore");
  expect(restored.live_updates?.some((update) => update.text === "Public progress: checking the project inputs.")).toBeTruthy();
  expect(restored.execution_traces?.[0].job_id).toBe(job.id);
});

test("completed public progress without tool calls has an expandable saved conversation trace", async ({ page }) => {
  const { project, session } = await conversation(page, "Collapsed continuation activity", "medium");
  const job = await send(page, "effort continue", "Final result: completed the remaining checks.");
  const trace = savedTrace(page, job.id);
  await expect(trace).toHaveCount(1);
  await expect(trace.locator(".trace-content")).not.toBeVisible();
  await expect(page.locator(".composer-wrap").getByTestId("execution-trace")).toHaveCount(0);
  await trace.locator(".trace-details > summary").click();
  await expect(trace.getByTestId("trace-progress-update"))
    .toContainText("Partial result: inspected the first input.");
  await expect(trace.locator(".trace-step")).toHaveCount(0);
  await expect(trace.locator(".trace-content")).not.toContainText("Waiting for the first tool action");
  await reopen(page, project, session);
  await expect(savedTrace(page, job.id).locator(".trace-content")).not.toBeVisible();
  await savedTrace(page, job.id).locator(".trace-details > summary").click();
  await expect(savedTrace(page, job.id).getByTestId("trace-progress-update"))
    .toContainText("Partial result: inspected the first input.");
});
