import { test, expect, type Page } from "@playwright/test";
import type { Session } from "../../frontend/src/services/api";
import { createProject, openProjectSession } from "../helpers/project-sessions";

test.setTimeout(45000);

async function conversation(page: Page, effort: string, timeout = 1, limit = 2) {
  const project = await createProject(page, "Timeout " + effort);
  expect((await page.request.patch("/api/projects/" + project.id + "/preferences", { data: { provider_response_timeout: timeout, provider_timeout_retries: limit } })).ok()).toBeTruthy();
  const created = await page.request.post("/api/sessions", {
    data: { title: "Timeout " + effort, project_id: project.id },
  });
  expect(created.status()).toBe(201);
  const session: Session = await created.json();
  expect((await page.request.patch("/api/sessions/" + session.id, { data: { effort } })).ok()).toBeTruthy();
  await page.goto("/");
  await openProjectSession(page, project, session);
  await page.getByLabel("Model", { exact: true }).selectOption("test-config");
  return session;
}

async function sendSlow(page: Page, session: Session) {
  const submitted = page.waitForResponse((response) => response.url().endsWith("/api/sessions/" + session.id + "/messages") && response.request().method() === "POST");
  await page.getByLabel("Message", { exact: true }).fill("slow");
  await page.getByRole("button", { name: /Send$/ }).click();
  expect((await submitted).status()).toBe(202);
  return (await submitted).json();
}

function retries(session: Session) {
  return (session.live_updates ?? []).filter((update) => update.kind === "status" && /retry/i.test(update.text));
}

test("Low stops on the first project timeout and Medium exhausts the project retry limit while restoring the draft", async ({ page }) => {
  for (const [effort, expectedRetries] of [["low", 0], ["medium", 2]] as const) {
    const session = await conversation(page, effort);
    const job = await sendSlow(page, session);
    expect(job.provider_response_timeout).toBe(1);
    expect(job.provider_timeout_retries).toBe(2);
    await expect.poll(async () => (await (await page.request.get("/api/jobs/" + job.id)).json()).status, { timeout: 15000 }).toBe("failed");
    const stored: Session = await (await page.request.get("/api/sessions/" + session.id)).json();
    expect(stored.pending_job).toBeNull();
    expect(stored.messages).toEqual([]);
    expect(stored.draft).toBe("slow");
    expect(retries(stored)).toHaveLength(expectedRetries);
    await expect(page.getByLabel("Message", { exact: true })).toHaveValue("slow");
    await expect(page.getByRole("button", { name: "Stop response", exact: true })).toHaveCount(0);
    await expect(page.getByTestId("task-failure")).toContainText(/No provider response/i);
  }
});

test("Stop cancels a higher-effort project retry and leaves the prompt available", async ({ page }) => {
  const session = await conversation(page, "high", 1, 10);
  const job = await sendSlow(page, session);
  await expect.poll(async () => retries(await (await page.request.get("/api/sessions/" + session.id)).json()).length, { timeout: 10000 }).toBeGreaterThan(0);
  const stopped = page.waitForResponse((response) => response.url().endsWith("/api/jobs/" + job.id + "/discard") && response.request().method() === "POST");
  await page.getByRole("button", { name: "Stop response", exact: true }).click();
  expect((await stopped).ok()).toBeTruthy();
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue("slow");
  const cancelled: Session = await (await page.request.get("/api/sessions/" + session.id)).json();
  expect(cancelled.execution_state?.phase).toBe("cancelled");
  const count = retries(cancelled).length;
  expect(count).toBeLessThan(10);
  await page.waitForTimeout(1300);
  const later: Session = await (await page.request.get("/api/sessions/" + session.id)).json();
  expect(retries(later)).toHaveLength(count);
  expect(later.draft).toBe("slow");
  expect(later.pending_job).toBeNull();
  expect((await (await page.request.get("/api/jobs/" + job.id)).json()).status).toBe("discarded");
});

test("an unlimited project waits past another project's finite timeout and remains stoppable", async ({ page }) => {
  const finite = await conversation(page, "low", 1, 0);
  const finiteJob = await sendSlow(page, finite);
  await expect.poll(async () => (await (await page.request.get("/api/jobs/" + finiteJob.id)).json()).status).toBe("failed");
  const unlimited = await conversation(page, "low", -1, 4);
  const unlimitedJob = await sendSlow(page, unlimited);
  expect(unlimitedJob.provider_response_timeout).toBe(-1);
  expect(unlimitedJob.provider_timeout_retries).toBe(4);
  await page.waitForTimeout(1500);
  expect((await (await page.request.get("/api/jobs/" + unlimitedJob.id)).json()).status).toBe("in_progress");
  const current: Session = await (await page.request.get("/api/sessions/" + unlimited.id)).json();
  expect(retries(current)).toHaveLength(0);
  const stopped = page.waitForResponse((response) => response.url().endsWith("/api/jobs/" + unlimitedJob.id + "/discard") && response.request().method() === "POST");
  await page.getByRole("button", { name: "Stop response", exact: true }).click();
  expect((await stopped).ok()).toBeTruthy();
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue("slow");
  expect((await (await page.request.get("/api/projects/" + finite.project_id + "/preferences")).json()).provider_response_timeout).toBe(1);
  expect((await (await page.request.get("/api/projects/" + unlimited.project_id + "/preferences")).json()).provider_response_timeout).toBe(-1);
});
