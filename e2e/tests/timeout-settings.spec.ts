import { test, expect, type Page } from "@playwright/test";
import type { Connection, Job, Session } from "../../frontend/src/services/api";
import { createProject, selectProject } from "../helpers/project-sessions";

async function openSettings(page: Page) {
  await page.getByRole("button", { name: "Connection settings", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "Connection settings", exact: true });
  await expect(dialog).toBeVisible();
  return dialog;
}

function settingsPayload(settings: Connection) {
  return {
    server_url: settings.server_url,
    font_size: settings.font_size,
    max_parallel_agents: settings.max_parallel_agents,
    provider_response_timeout: settings.provider_response_timeout ?? 120,
    provider_timeout_retries: settings.provider_timeout_retries ?? 2,
  };
}

test("provider timeout and retries are tested, saved, and restored in settings", async ({ page }, testInfo) => {
  const original: Connection = await (await page.request.get("/api/settings")).json();
  expect(original.provider_response_timeout).toBe(120);
  expect(original.provider_timeout_retries).toBe(2);
  try {
    await page.goto("/");
    const dialog = await openSettings(page);
    const timeout = dialog.getByLabel("Provider response timeout (seconds)", { exact: true });
    const retries = dialog.getByLabel("Timeout retries", { exact: true });
    await expect(timeout).toHaveValue("120");
    await expect(retries).toHaveValue("2");
    await expect(dialog).toContainText("Low effort stops immediately");
    await expect(dialog).toContainText("Stop interrupts requests and retries");
    await timeout.fill("21.5");
    await retries.fill("4");
    const tested = page.waitForResponse((response) =>
      new URL(response.url()).pathname === "/api/settings/test" && response.request().method() === "POST",
    );
    await dialog.getByRole("button", { name: "Test Connection", exact: true }).click();
    const testResponse = await tested;
    expect(testResponse.status()).toBe(200);
    expect(testResponse.request().postDataJSON()).toMatchObject({
      provider_response_timeout: 21.5,
      provider_timeout_retries: 4,
    });
    const saved = page.waitForResponse((response) =>
      new URL(response.url()).pathname === "/api/settings" && response.request().method() === "POST",
    );
    await dialog.getByRole("button", { name: "Save", exact: true }).click();
    expect((await saved).status()).toBe(200);
    await expect(dialog).not.toBeVisible();
    expect(await (await page.request.get("/api/settings")).json()).toMatchObject({
      provider_response_timeout: 21.5,
      provider_timeout_retries: 4,
    });
    await page.reload();
    await page.setViewportSize({ width: 390, height: 844 });
    const restored = await openSettings(page);
    await expect(restored.getByLabel("Provider response timeout (seconds)", { exact: true })).toHaveValue("21.5");
    await expect(restored.getByLabel("Timeout retries", { exact: true })).toHaveValue("4");
    await restored.getByLabel("Timeout retries", { exact: true }).scrollIntoViewIfNeeded();
    const bounds = await restored.boundingBox();
    expect(bounds).not.toBeNull();
    expect(bounds!.x).toBeGreaterThanOrEqual(0);
    expect(bounds!.x + bounds!.width).toBeLessThanOrEqual(390);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBeTruthy();
    await page.screenshot({ path: testInfo.outputPath("timeout-settings-mobile.png"), fullPage: true });
  } finally {
    expect((await page.request.post("/api/settings", { data: settingsPayload(original) })).ok()).toBeTruthy();
  }
});

test("settings reject invalid timeout ranges and fractional retry counts before submission", async ({ page }) => {
  await page.goto("/");
  const dialog = await openSettings(page);
  const timeout = dialog.getByLabel("Provider response timeout (seconds)", { exact: true });
  const retries = dialog.getByLabel("Timeout retries", { exact: true });
  let submitted = 0;
  page.on("request", (request) => {
    if (new URL(request.url()).pathname === "/api/settings" && request.method() === "POST") submitted++;
  });
  for (const [seconds, attempts, invalid] of [
    ["0", "2", timeout],
    ["3601", "2", timeout],
    ["120", "11", retries],
    ["120", "1.5", retries],
  ] as const) {
    await timeout.fill(seconds);
    await retries.fill(attempts);
    expect(await invalid.evaluate((input: HTMLInputElement) => input.validity.valid)).toBeFalsy();
    await dialog.getByRole("button", { name: "Save", exact: true }).click();
    await expect(dialog).toBeVisible();
    expect(submitted).toBe(0);
  }
  await timeout.fill("1");
  await retries.fill("0");
  expect(await timeout.evaluate((input: HTMLInputElement) => input.validity.valid)).toBeTruthy();
  expect(await retries.evaluate((input: HTMLInputElement) => input.validity.valid)).toBeTruthy();
});

test("History cannot continue unavailable or orphan sessions even while a project is open", async ({ page }) => {
  const project = await createProject(page, "History access checks");
  const created = await page.request.post("/api/sessions", { data: { project_id: project.id, title: "History project conversation" } });
  expect(created.status()).toBe(201);
  const session: Session = await created.json();
  const orphan = { ...session, id: "fixture-orphan-session", title: "Legacy orphan conversation", project_id: null, saved_path: null };
  const jobs: Job[] = [
    { id: "fixture-missing-job", session_id: "fixture-unavailable-session", filename: "Unavailable conversation", file_size: 0, kind: "chat", status: "completed", progress: 100, created_at: new Date().toISOString(), completed_at: new Date().toISOString(), error_message: null, logs: [], download_available: false },
    { id: "fixture-orphan-job", session_id: orphan.id, filename: orphan.title, file_size: 0, kind: "chat", status: "completed", progress: 100, created_at: new Date().toISOString(), completed_at: new Date().toISOString(), error_message: null, logs: [], download_available: false },
    { id: "fixture-project-job", session_id: session.id, filename: session.title, file_size: 0, kind: "chat", status: "completed", progress: 100, created_at: new Date().toISOString(), completed_at: new Date().toISOString(), error_message: null, logs: [], download_available: false },
  ];
  await page.route("**/api/sessions?include_agents=true", async (route) => {
    const response = await route.fetch();
    const sessions: Session[] = await response.json();
    await route.fulfill({ response, json: [...sessions, orphan] });
  });
  await page.route("**/api/jobs", (route) => route.fulfill({ json: jobs }));
  await page.goto("/");
  await selectProject(page, project);
  await page.locator("header").getByRole("button", { name: /^History/ }).click();
  const rows = page.locator(".history tbody tr");
  await expect(rows.filter({ hasText: "Unavailable conversation" }).getByRole("button", { name: "Continue", exact: true })).toBeDisabled();
  await expect(rows.filter({ hasText: "Legacy orphan conversation" }).getByRole("button", { name: "Continue", exact: true })).toBeDisabled();
  const resume = rows.filter({ hasText: session.title }).getByRole("button", { name: "Continue", exact: true });
  await expect(resume).toBeEnabled();
  await resume.click();
  await expect(page.getByLabel("Session title", { exact: true })).toHaveValue(session.title);
});
