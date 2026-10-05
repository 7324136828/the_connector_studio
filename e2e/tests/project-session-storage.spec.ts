import { test, expect, type Page } from "@playwright/test";
import { existsSync, readFileSync } from "node:fs";
import { basename, dirname, join, relative } from "node:path";
import { selectProject as selectRecentProject, readNativeSnapshot, writeProjectSnapshot } from "../helpers/project-sessions";

type Project = { id: string; name: string; path: string };
type StoredSession = {
  id: string;
  title: string;
  project_id: string | null;
  project_path: string;
  draft: string;
  saved_path: string | null;
  save_error: string | null;
  messages: { role: string; text: string }[];
};

async function openProject(page: Page, name: string): Promise<Project> {
  const response = await page.request.post("/api/projects", {
    data: { name: name + " " + Date.now() },
  });
  expect(response.status()).toBe(201);
  const project: Project = await response.json();
  await page.goto("/");
  await selectRecentProject(page, project);
  return project;
}

function canonicalPath(project: Project, session: StoredSession) {
  expect(session.saved_path).not.toBeNull();
  const path = session.saved_path!;
  expect(dirname(path)).toBe(join(project.path, "sessions"));
  expect(basename(path)).toBe(session.title + ".lattice");
  return path;
}

function explorerPath(project: Project, path: string) {
  return relative(project.path, path).replaceAll("\\", "/");
}

async function diskSession(page: Page, path: string) {
  expect(existsSync(path)).toBeTruthy();
  return readNativeSnapshot(page, readFileSync(path));
}

test("quick project sessions have a disk file before their first reply and autosave without manual Save", async ({
  page,
}) => {
  const project = await openProject(page, "Automatic conversation storage");
  const manualSaves: string[] = [];
  page.on("request", (request) => {
    if (
      request.method() === "POST" &&
      /\/api\/sessions\/[^/]+\/save$/.test(request.url())
    )
      manualSaves.push(request.url());
  });
  const createdResponse = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/sessions") &&
      response.request().method() === "POST",
  );
  await page.getByRole("button", { name: "Add session", exact: true }).click();
  const response = await createdResponse;
  expect(response.status()).toBe(201);
  const created: StoredSession = await response.json();
  const path = canonicalPath(project, created);
  expect(created.project_id).toBe(project.id);
  expect(created.saved_path).toBe(path);
  expect(created.save_error).toBeNull();
  expect((await diskSession(page, path)).messages).toEqual([]);

  await page.getByLabel("Model", { exact: true }).selectOption("test-config");
  await page
    .getByLabel("Message", { exact: true })
    .fill("Automatically persist my first conversation");
  await page.getByRole("button", { name: /Send$/ }).click();
  await expect(page.locator(".message.assistant")).toContainText(
    "Automatically persist my first conversation",
    { timeout: 15000 },
  );
  const saved = await diskSession(page, path);
  expect(saved.messages).toHaveLength(2);
  expect(saved.messages[0].text).toBe(
    "Automatically persist my first conversation",
  );
  expect(saved.messages[1].text).toContain(
    "Automatically persist my first conversation",
  );
  expect(saved.draft).toBe("");
  expect(manualSaves).toEqual([]);

  await page.reload();
  await selectRecentProject(page, project);
  await page
    .locator(".file-list")
    .getByTitle(explorerPath(project, path), { exact: true })
    .click();
  await expect(page.locator(".message.assistant")).toContainText(
    "Automatically persist my first conversation",
  );
});

test("opening a native file from another project stores a copy in project sessions", async ({ page }) => {
  const sourceProject = await openProject(page, "Source conversation storage");
  const sourceResponse = await page.request.post("/api/sessions", {
    data: { title: "Copied project conversation", project_id: sourceProject.id },
  });
  expect(sourceResponse.status()).toBe(201);
  const source: StoredSession = await sourceResponse.json();
  const draft = "Carry this saved draft into the project";
  expect((await page.request.patch("/api/sessions/" + source.id, { data: { draft, model: "test-config" } })).status()).toBe(200);
  const download = await page.request.get("/api/sessions/" + source.id + "/download");
  expect(download.status()).toBe(200);
  const bytes = await download.body();
  const project = await openProject(page, "Copied conversation storage");
  const relativePath = writeProjectSnapshot(project, bytes, "portable-session.lattice");
  // Refresh Explorer to include the file created on disk.
  await selectRecentProject(page, project);
  const openedResponse = page.waitForResponse((response) => response.url().endsWith("/api/projects/" + project.id + "/sessions/open") && response.request().method() === "POST");
  await page.locator(".file-list").getByTitle(relativePath, { exact: true }).click();
  const response = await openedResponse;
  expect(response.status()).toBe(200);
  const opened: StoredSession = await response.json();
  const path = canonicalPath(project, opened);
  expect(opened.id).not.toBe(source.id);
  expect(opened.project_id).toBe(project.id);
  expect(opened.project_path).toBe(project.path);
  expect(opened.save_error).toBeNull();
  await expect(page.getByLabel("Session title", { exact: true })).toHaveValue(opened.title);
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue(draft);
  expect((await diskSession(page, path)).draft).toBe(draft);
  expect(readFileSync(join(project.path, "files", "portable-session.lattice"))).toEqual(bytes);
  expect(readFileSync(source.saved_path!)).toEqual(bytes);

  await page.reload();
  await selectRecentProject(page, project);
  const reopenedResponse = page.waitForResponse((response) => response.url().endsWith("/api/projects/" + project.id + "/sessions/open") && response.request().method() === "POST");
  await page.locator(".file-list").getByTitle(explorerPath(project, path), { exact: true }).click();
  expect((await (await reopenedResponse).json()).id).toBe(opened.id);
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue(draft);
});
