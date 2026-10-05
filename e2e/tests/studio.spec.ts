import { test, expect } from "@playwright/test";
import type { Project, Session } from "../../frontend/src/services/api";
import { createProject, openProjectSession, selectProject } from "../helpers/project-sessions";

test("create project, chat, save, export and reload persistence", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByRole("heading", { name: "A workspace for your conversations." })).toBeVisible();
  const projectCreated = page.waitForResponse((response) =>
    new URL(response.url()).pathname === "/api/projects" && response.request().method() === "POST",
  );
  await page.getByRole("button", { name: "Create a project", exact: true }).click();
  await page.getByLabel("Project name", { exact: true }).fill("Browser project " + Date.now());
  await page.getByRole("button", { name: "Create", exact: true }).click();
  const project: Project = await (await projectCreated).json();
  await expect(page.getByRole("dialog")).not.toBeVisible();
  const sessionCreated = page.waitForResponse((response) =>
    new URL(response.url()).pathname === "/api/sessions" && response.request().method() === "POST",
  );
  await page.locator(".empty-workspace").getByRole("button", { name: "New Session", exact: true }).click();
  const session: Session = await (await sessionCreated).json();
  await page.getByLabel("Session title", { exact: true }).fill("Browser conversation");
  await page.getByLabel("Message", { exact: true }).click();
  await page.getByLabel("Model", { exact: true }).selectOption("test-config");
  await page.getByLabel("Message", { exact: true }).fill("hello browser");
  await page.getByRole("button", { name: /Send$/ }).click();
  await expect(page.locator(".message.assistant")).toContainText("Mock reply", { timeout: 15000 });
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await expect(page.getByRole("status").filter({ hasText: "Session saved." })).toBeVisible();
  const saved: Session = await (await page.request.get("/api/sessions/" + session.id)).json();
  await page.getByRole("button", { name: "Export ZIP", exact: true }).first().click();
  await expect(page.getByRole("heading", { name: "Request history" })).toBeVisible();
  const downloadPromise = page.waitForEvent("download");
  await page.getByRole("link", { name: "Download ZIP", exact: true }).first().click();
  const download = await downloadPromise;
  expect(download.suggestedFilename()).toMatch(/\.zip$/);
  await page.reload();
  await openProjectSession(page, project, saved);
  await expect(page.locator(".message.assistant")).toContainText("Mock reply");
  await page.screenshot({ path: "test-results/workspace.png", fullPage: true });
});

test("switching tabs preserves drafts and cancellation restores input", async ({ page }) => {
  const project = await createProject(page, "Draft tab project");
  await page.goto("/");
  await selectProject(page, project);
  await page.locator(".empty-workspace").getByRole("button", { name: "New Session", exact: true }).click();
  await page.getByLabel("Message", { exact: true }).fill("draft on first tab");
  await page.getByRole("button", { name: "Add session" }).click();
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue("");
  await page.getByRole("tab").first().click();
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue("draft on first tab");
  await page.getByLabel("Model", { exact: true }).selectOption("test-config");
  await page.getByLabel("Message", { exact: true }).fill("slow");
  await page.getByRole("button", { name: /Send$/ }).click();
  await expect(page.getByRole("button", { name: "Stop response" })).toBeVisible();
  await page.getByRole("button", { name: "Stop response" }).click();
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue("slow");
  await expect(page.locator(".message.user")).toHaveCount(0);
});

test("settings and narrow viewport remain usable", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("button", { name: "Connection settings" }).click();
  await expect(page.getByRole("dialog")).toBeVisible();
  await page.getByRole("button", { name: "Test Connection", exact: true }).click();
  await expect(page.getByRole("status").filter({ hasText: "Connection successful" })).toBeVisible();
  await page.getByRole("button", { name: "Close dialog" }).click();
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(page.getByRole("button", { name: "Create a project", exact: true })).toBeVisible();
  await expect(page.getByRole("button", { name: "Open a project", exact: true })).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBeTruthy();
  await page.screenshot({ path: "test-results/mobile.png", fullPage: true });
});
