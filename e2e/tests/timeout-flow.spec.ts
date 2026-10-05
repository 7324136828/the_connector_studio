import { test, expect, type Page } from "@playwright/test";
import type { Session } from "../../frontend/src/services/api";
import { createProject, openProjectSession } from "../helpers/project-sessions";

test.setTimeout(45000);

function policy(settings: Record<string, unknown>, seconds: number, retries: number) {
  return {
    server_url: settings.server_url,
    font_size: settings.font_size,
    max_parallel_agents: settings.max_parallel_agents,
    provider_response_timeout: seconds,
    provider_timeout_retries: retries,
  };
}

async function conversation(page: Page, effort: string) {
  const project = await createProject(page, "Timeout " + effort);
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

test("Low stops on the first timeout and Medium exhausts configured retries while restoring the original draft", async ({ page }) => {
  const before = await (await page.request.get("/api/settings")).json();
  try {
    expect((await page.request.post("/api/settings", { data: policy(before, 1, 2) })).ok()).toBeTruthy();
    for (const [effort, expectedRetries] of [["low", 0], ["medium", 2]] as const) {
      const session = await conversation(page, effort);
      const job = await sendSlow(page, session);
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
  } finally {
    expect((await page.request.post("/api/settings", { data: policy(before, before.provider_response_timeout, before.provider_timeout_retries) })).ok()).toBeTruthy();
  }
});

test("Stop cancels a higher-effort provider retry and leaves the prompt available", async ({ page }) => {
  const before = await (await page.request.get("/api/settings")).json();
  try {
    expect((await page.request.post("/api/settings", { data: policy(before, 1, 10) })).ok()).toBeTruthy();
    const session = await conversation(page, "high");
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
  } finally {
    expect((await page.request.post("/api/settings", { data: policy(before, before.provider_response_timeout, before.provider_timeout_retries) })).ok()).toBeTruthy();
  }
});
