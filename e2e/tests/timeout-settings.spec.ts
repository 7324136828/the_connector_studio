import { test, expect, type Page } from "@playwright/test";
import type { Job, Project, ProjectPreferences, Session } from "../../frontend/src/services/api";
import { createProject, selectProject } from "../helpers/project-sessions";

async function openSettings(page: Page) {
  await page.getByRole("button", { name: "Connection settings", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "Connection settings", exact: true });
  await expect(dialog).toBeVisible();
  return dialog;
}

async function preferences(page: Page, project: Project): Promise<ProjectPreferences> {
  return (await page.request.get("/api/projects/" + project.id + "/preferences")).json();
}

function policyInputs(dialog: ReturnType<Page["getByRole"]>) {
  return {
    timeout: dialog.getByLabel("Provider response timeout (seconds)", { exact: true }),
    retries: dialog.getByLabel("Timeout retries", { exact: true }),
  };
}

test("project timeout and retries are tested, saved, and restored without changing its model", async ({ page }, testInfo) => {
  const project = await createProject(page, "Saved task policy");
  const created = await page.request.post("/api/sessions", { data: { project_id: project.id, title: "Policy model preference" } });
  const session: Session = await created.json();
  expect((await page.request.patch("/api/sessions/" + session.id, { data: { model: "test-config" } })).ok()).toBeTruthy();
  expect(await preferences(page, project)).toMatchObject({ provider_response_timeout: -1, provider_timeout_retries: 2, default_model: "test-config" });
  await page.goto("/");
  await selectProject(page, project);
  const dialog = await openSettings(page);
  await expect(dialog.getByRole("group", { name: "Project task settings: " + project.name, exact: true })).toBeVisible();
  const { timeout, retries } = policyInputs(dialog);
  await expect(timeout).toBeEnabled();
  await expect(timeout).toHaveValue("-1");
  await expect(retries).toHaveValue("2");
  await expect(dialog).toContainText("-1 waits forever; 0 times out immediately");
  await timeout.fill("21.5");
  await retries.fill("4");
  const tested = page.waitForResponse((response) => new URL(response.url()).pathname === "/api/settings/test" && response.request().method() === "POST");
  await dialog.getByRole("button", { name: "Test Connection", exact: true }).click();
  const testResponse = await tested;
  expect(testResponse.status()).toBe(200);
  expect(testResponse.request().postDataJSON()).toMatchObject({ project_id: project.id, provider_response_timeout: 21.5 });
  expect(testResponse.request().postDataJSON()).not.toHaveProperty("provider_timeout_retries");
  const globalSaved = page.waitForResponse((response) => new URL(response.url()).pathname === "/api/settings" && response.request().method() === "POST");
  const policySaved = page.waitForResponse((response) => new URL(response.url()).pathname === "/api/projects/" + project.id + "/preferences" && response.request().method() === "PATCH");
  await dialog.getByRole("button", { name: "Save", exact: true }).click();
  const globalResponse = await globalSaved;
  expect(globalResponse.status()).toBe(200);
  expect(globalResponse.request().postDataJSON()).not.toHaveProperty("provider_response_timeout");
  expect(globalResponse.request().postDataJSON()).not.toHaveProperty("provider_timeout_retries");
  const policyResponse = await policySaved;
  expect(policyResponse.status()).toBe(200);
  expect(policyResponse.request().postDataJSON()).toEqual({ provider_response_timeout: 21.5, provider_timeout_retries: 4 });
  await expect(dialog).not.toBeVisible();
  expect(await preferences(page, project)).toMatchObject({ provider_response_timeout: 21.5, provider_timeout_retries: 4, default_model: "test-config" });
  await page.reload();
  await selectProject(page, project);
  await page.setViewportSize({ width: 390, height: 844 });
  const restored = await openSettings(page);
  await expect(policyInputs(restored).timeout).toHaveValue("21.5");
  await expect(policyInputs(restored).retries).toHaveValue("4");
  await policyInputs(restored).retries.scrollIntoViewIfNeeded();
  const bounds = await restored.boundingBox();
  expect(bounds).not.toBeNull();
  expect(bounds!.x).toBeGreaterThanOrEqual(0);
  expect(bounds!.x + bounds!.width).toBeLessThanOrEqual(390);
  await page.screenshot({ path: testInfo.outputPath("timeout-settings-mobile.png"), fullPage: true });
});

test("project settings allow unlimited, zero, and uncapped timeouts but reject other negatives and invalid retries", async ({ page }) => {
  const project = await createProject(page, "Policy validation");
  await page.goto("/");
  const unscoped = await openSettings(page);
  await expect(policyInputs(unscoped).timeout).toBeDisabled();
  await expect(policyInputs(unscoped).retries).toBeDisabled();
  await expect(unscoped).toContainText("Open a project to configure its response timeout and retries.");
  await unscoped.getByRole("button", { name: "Cancel", exact: true }).click();
  await selectProject(page, project);
  const dialog = await openSettings(page);
  const { timeout, retries } = policyInputs(dialog);
  await expect(timeout).toBeEnabled();
  let submitted = 0;
  page.on("request", (request) => {
    if (request.method() === "POST" && new URL(request.url()).pathname === "/api/settings") submitted++;
    if (request.method() === "PATCH" && request.url().endsWith("/preferences")) submitted++;
  });
  for (const [seconds, attempts, invalid] of [["-2", "2", timeout], ["-0.5", "2", timeout], ["-1", "11", retries], ["-1", "1.5", retries]] as const) {
    await timeout.fill(seconds);
    await retries.fill(attempts);
    expect(await invalid.evaluate((input: HTMLInputElement) => input.validity.valid)).toBeFalsy();
    await dialog.getByRole("button", { name: "Save", exact: true }).click();
    await expect(dialog).toBeVisible();
    expect(submitted).toBe(0);
  }
  for (const seconds of ["-1", "0", "0.5", "1000000"]) {
    await timeout.fill(seconds);
    await retries.fill("0");
    expect(await timeout.evaluate((input: HTMLInputElement) => input.validity.valid)).toBeTruthy();
    expect(await retries.evaluate((input: HTMLInputElement) => input.validity.valid)).toBeTruthy();
  }
  await timeout.fill("-0.5");
  expect(await timeout.evaluate((input: HTMLInputElement) => input.validity.valid)).toBeFalsy();
  await dialog.getByRole("button", { name: "Cancel", exact: true }).click();
  const reopened = await openSettings(page);
  await expect(policyInputs(reopened).timeout).toBeEnabled();
  await expect(policyInputs(reopened).timeout).toHaveValue("-1");
  expect(await policyInputs(reopened).timeout.evaluate((input: HTMLInputElement) => input.validity.valid)).toBeTruthy();
  expect(await preferences(page, project)).toMatchObject({ provider_response_timeout: -1, provider_timeout_retries: 2 });
});

test("project policy drafts are isolated, cancelled edits are discarded, and stale loads cannot overwrite another project", async ({ page }) => {
  const first = await createProject(page, "Policy scope first");
  const second = await createProject(page, "Policy scope second");
  for (const [project, seconds, retries] of [[first, 17, 3], [second, 24, 1]] as const) {
    expect((await page.request.patch("/api/projects/" + project.id + "/preferences", { data: { provider_response_timeout: seconds, provider_timeout_retries: retries } })).ok()).toBeTruthy();
  }
  await page.goto("/");
  await selectProject(page, first);
  let dialog = await openSettings(page);
  await expect(policyInputs(dialog).timeout).toHaveValue("17");
  await policyInputs(dialog).timeout.fill("99");
  await policyInputs(dialog).retries.fill("5");
  await dialog.getByRole("button", { name: "Cancel", exact: true }).click();
  expect(await preferences(page, first)).toMatchObject({ provider_response_timeout: 17, provider_timeout_retries: 3 });
  let release!: () => void;
  const held = new Promise<void>((resolve) => { release = resolve; });
  let captured!: () => void;
  const observed = new Promise<void>((resolve) => { captured = resolve; });
  const endpoint = "**/api/projects/" + first.id + "/preferences";
  await page.route(endpoint, async (route) => {
    if (route.request().method() !== "GET") { await route.continue(); return; }
    const response = await route.fetch();
    captured();
    await held;
    await route.fulfill({ response });
  });
  try {
    dialog = await openSettings(page);
    await observed;
    await expect(policyInputs(dialog).timeout).toBeDisabled();
    await dialog.getByRole("button", { name: "Cancel", exact: true }).click();
    await selectProject(page, second);
    dialog = await openSettings(page);
    await expect(policyInputs(dialog).timeout).toHaveValue("24");
    release();
    await page.unrouteAll({ behavior: "wait" });
    await page.evaluate(() => new Promise<void>((resolve) => requestAnimationFrame(() => requestAnimationFrame(() => resolve()))));
    await expect(policyInputs(dialog).timeout).toHaveValue("24");
    await expect(policyInputs(dialog).retries).toHaveValue("1");
    await policyInputs(dialog).timeout.fill("6000");
    await policyInputs(dialog).retries.fill("2");
    const saved = page.waitForResponse((response) => response.url().endsWith("/api/projects/" + second.id + "/preferences") && response.request().method() === "PATCH");
    await dialog.getByRole("button", { name: "Save", exact: true }).click();
    expect((await saved).status()).toBe(200);
    await expect(dialog).not.toBeVisible();
    expect(await preferences(page, first)).toMatchObject({ provider_response_timeout: 17, provider_timeout_retries: 3 });
    expect(await preferences(page, second)).toMatchObject({ provider_response_timeout: 6000, provider_timeout_retries: 2 });
  } finally {
    release();
    await page.unroute(endpoint);
  }
});

test("a project preference error blocks policy saving instead of using fallback defaults", async ({ page }) => {
  const project = await createProject(page, "Invalid project preferences");
  await page.route("**/api/projects/" + project.id + "/preferences", (route) => route.fulfill({
    json: { project_id: project.id, default_model: "", error: "Project preference file is invalid." },
  }));
  await page.goto("/");
  await selectProject(page, project);
  const dialog = await openSettings(page);
  await expect(dialog.getByRole("alert")).toContainText("Project preference file is invalid.");
  await expect(policyInputs(dialog).timeout).toBeDisabled();
  await expect(policyInputs(dialog).retries).toBeDisabled();
  await expect(dialog.getByRole("button", { name: "Save", exact: true })).toBeDisabled();
  await expect(dialog.getByRole("button", { name: "Test Connection", exact: true })).toBeDisabled();
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
