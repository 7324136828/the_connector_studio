import { test, expect, type Page } from "@playwright/test";
import { existsSync, lstatSync, realpathSync, rmSync } from "node:fs";
import { basename, isAbsolute, relative } from "node:path";
import type { Project, Session } from "../../frontend/src/services/api";
import {
  createProject,
  selectProject,
  writeProjectSnapshot,
} from "../helpers/project-sessions";

async function newProjectDialog(page: Page) {
  const menu = page.locator("header details").filter({
    has: page.getByText("Project", { exact: true }),
  });
  const create = menu.getByRole("button", {
    name: "New Project",
    exact: true,
  });
  if (!(await create.isVisible())) await menu.locator("summary").click();
  await create.click();
  const dialog = page.getByTestId("new-item-dialog");
  await expect(dialog).toBeVisible();
  return dialog;
}

async function createSession(page: Page, project: Project, title: string) {
  const response = await page.request.post("/api/sessions", {
    data: { project_id: project.id, title },
  });
  expect(response.status()).toBe(201);
  return (await response.json()) as Session;
}

function removeIsolatedProject(project: Project) {
  const testData = process.env.STUDIO_E2E_DATA;
  if (!testData) throw new Error("Missing isolated E2E data directory");
  const root = realpathSync(testData);
  const target = realpathSync(project.path);
  const within = relative(root, target);
  if (
    !within ||
    isAbsolute(within) ||
    within.split(/[\\/]/).includes("..") ||
    target === root ||
    basename(target) !== project.name ||
    lstatSync(project.path).isSymbolicLink()
  )
    throw new Error("Project deletion target must be an isolated E2E project");
  rmSync(target, { recursive: true, force: false });
  expect(existsSync(target)).toBe(false);
}

async function dispatchNativeFile(page: Page, selector: string, bytes: Buffer) {
  await page.locator(selector).evaluate((element, payload) => {
    const transfer = new DataTransfer();
    transfer.items.add(
      new File([Uint8Array.from(payload)], "unimported.LATTICE", {
        type: "application/octet-stream",
      }),
    );
    element.dispatchEvent(
      new DragEvent("drop", {
        bubbles: true,
        cancelable: true,
        dataTransfer: transfer,
      }),
    );
    element.dispatchEvent(
      new ClipboardEvent("paste", {
        bubbles: true,
        cancelable: true,
        clipboardData: transfer,
      }),
    );
  }, Array.from(bytes));
  await page.waitForLoadState("networkidle");
}

test("startup keeps conversations inside projects and every new-session entry point requires one", async ({
  page,
}) => {
  const project = await createProject(page, "Startup project catalog");
  const session = await createSession(
    page,
    project,
    "Existing private project conversation",
  );
  await page.goto("/");
  await expect(page.getByText("CONVERSATIONS", { exact: true })).toHaveCount(0);
  await expect(page.locator(".session-row")).toHaveCount(0);
  await expect(page.getByText(session.title, { exact: true })).toHaveCount(0);
  await expect(
    page.getByRole("button", { name: "Open Session", exact: true }),
  ).toHaveCount(0);
  await expect(
    page.getByRole("button", { name: /Import.*lattice/i }),
  ).toHaveCount(0);
  await expect(
    page.locator('input[type="file"][accept=".lattice"]'),
  ).toHaveCount(0);
  await expect(
    page
      .locator(".empty-workspace")
      .getByRole("button", { name: "New Session", exact: true }),
  ).toHaveCount(0);
  await expect(
    page.getByRole("button", { name: "Create a project", exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole("button", { name: "Add session", exact: true }),
  ).toBeDisabled();

  const creates: string[] = [];
  page.on("request", (request) => {
    if (
      request.method() === "POST" &&
      new URL(request.url()).pathname === "/api/sessions"
    )
      creates.push(request.url());
  });
  await page
    .locator("header summary")
    .filter({ hasText: /^Session$/ })
    .click();
  await expect(
    page.getByRole("button", { name: "New Session", exact: true }),
  ).toBeDisabled();
  await page
    .locator("header summary")
    .filter({ hasText: /^Session$/ })
    .click();
  const dialog = await newProjectDialog(page);
  await dialog.getByRole("tab", { name: "Sessions", exact: true }).click();
  await expect(
    dialog.getByLabel("Session name", { exact: true }),
  ).toBeDisabled();
  await expect(
    dialog.getByRole("button", { name: "Create", exact: true }),
  ).toBeDisabled();
  await dialog.getByRole("button", { name: "Cancel", exact: true }).click();
  expect(creates).toEqual([]);

  const before: Session[] = await (
    await page.request.get("/api/sessions")
  ).json();
  for (const data of [
    { title: "Orphan blocked" },
    { title: "Orphan blocked", project_id: null },
  ]) {
    const response = await page.request.post("/api/sessions", { data });
    expect(response.status()).toBe(422);
  }
  const after: Session[] = await (
    await page.request.get("/api/sessions")
  ).json();
  expect(after.map((value) => value.id)).toEqual(
    before.map((value) => value.id),
  );
});

test("Project menu has only project actions and the New dialog shows five MRU projects", async ({
  page,
}) => {
  const projects: Project[] = [];
  for (let index = 0; index < 6; index++)
    projects.push(await createProject(page, "MRU navigation " + index));
  const newest = projects.slice(1).reverse();
  const recent: Project[] = await (
    await page.request.get("/api/projects")
  ).json();
  expect(recent.map((project) => project.id)).toEqual(
    newest.map((project) => project.id),
  );
  await page.goto("/");
  const menu = page.locator("header details").filter({
    has: page.getByText("Project", { exact: true }),
  });
  await menu.locator("summary").click();
  await expect(menu.getByText(/Recent projects/i)).toHaveCount(0);
  for (const project of projects)
    await expect(
      menu.getByRole("button", { name: project.name, exact: true }),
    ).toHaveCount(0);
  const dialog = await newProjectDialog(page);
  const recents = dialog.getByRole("listbox", {
    name: "Recent projects",
    exact: true,
  });
  await expect(recents.getByRole("option")).toHaveCount(5);
  await expect(recents.getByRole("option").locator("strong")).toHaveText(
    newest.map((project) => project.name),
  );
  await dialog.getByRole("button", { name: "Cancel", exact: true }).click();

  await selectProject(page, projects[0]);
  const reopened: Project[] = await (
    await page.request.get("/api/projects")
  ).json();
  const reordered = [projects[0], ...newest.slice(0, 4)];
  expect(reopened.map((project) => project.id)).toEqual(
    reordered.map((project) => project.id),
  );
  const reopenedDialog = await newProjectDialog(page);
  await expect(
    reopenedDialog
      .getByRole("listbox", { name: "Recent projects", exact: true })
      .getByRole("option")
      .locator("strong"),
  ).toHaveText(reordered.map((project) => project.name));
});

test("missing isolated project folders are pruned from recents while session catalog records remain", async ({
  page,
}) => {
  const project = await createProject(
    page,
    "Deleted isolated navigation fixture",
  );
  const session = await createSession(
    page,
    project,
    "Retained catalog conversation",
  );
  const before: Project[] = await (
    await page.request.get("/api/projects")
  ).json();
  expect(before[0].id).toBe(project.id);
  removeIsolatedProject(project);
  const recent: Project[] = await (
    await page.request.get("/api/projects")
  ).json();
  expect(recent.some((value) => value.id === project.id)).toBe(false);
  expect(recent.map((value) => value.id)).toEqual(
    before.slice(1).map((value) => value.id),
  );
  const retained = await page.request.get("/api/sessions/" + session.id);
  expect(retained.status()).toBe(200);
  expect((await retained.json()).project_id).toBe(project.id);
  const sessions: Session[] = await (
    await page.request.get("/api/sessions")
  ).json();
  expect(sessions.some((value) => value.id === session.id)).toBe(true);
  await page.goto("/");
  const dialog = await newProjectDialog(page);
  await expect(
    dialog
      .getByRole("listbox", { name: "Recent projects", exact: true })
      .getByRole("option")
      .filter({ hasText: project.name }),
  ).toHaveCount(0);
});

test("native session drag and paste stay inert and valid files open only through project Explorer", async ({
  page,
}) => {
  const project = await createProject(page, "Native input navigation fixture");
  const session = await createSession(
    page,
    project,
    "Native Explorer conversation",
  );
  const patched = await page.request.patch("/api/sessions/" + session.id, {
    data: { draft: "Keep the original project draft.", model: "test-config" },
  });
  expect(patched.status()).toBe(200);
  const download = await page.request.get(
    "/api/sessions/" + session.id + "/download",
  );
  expect(download.status()).toBe(200);
  const bytes = await download.body();
  const file = writeProjectSnapshot(project, bytes, "unimported.lattice");
  const mutations: string[] = [];
  page.on("request", (request) => {
    const path = new URL(request.url()).pathname;
    if (
      (request.method() === "POST" &&
        (path === "/api/sessions" || path === "/api/sessions/import")) ||
      (request.method() === "PATCH" &&
        /^\/api\/sessions\/[^/]+$/.test(path) &&
        request.postDataJSON()?.attachments !== undefined)
    )
      mutations.push(path);
  });
  const initial: Session[] = await (
    await page.request.get("/api/sessions")
  ).json();
  await page.goto("/");
  await dispatchNativeFile(page, ".studio", bytes);
  await expect(page.getByRole("dialog")).toHaveCount(0);
  expect(mutations).toEqual([]);

  await selectProject(page, project);
  const opened = page.waitForResponse(
    (response) =>
      new URL(response.url()).pathname ===
        "/api/projects/" + project.id + "/sessions/open" &&
      response.request().method() === "POST",
  );
  await page.locator(".file-list").getByTitle(file, { exact: true }).click();
  const explorer: Session = await (await opened).json();
  expect(explorer.project_id).toBe(project.id);
  expect(explorer.title).toBe(session.title);
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue(
    "Keep the original project draft.",
  );
  const beforeDrop: Session[] = await (
    await page.request.get("/api/sessions")
  ).json();
  expect(beforeDrop.length).toBe(initial.length + 1);
  await dispatchNativeFile(page, 'textarea[aria-label="Message"]', bytes);
  expect(mutations).toEqual([]);
  const afterDrop: Session[] = await (
    await page.request.get("/api/sessions")
  ).json();
  expect(afterDrop.map((value) => value.id)).toEqual(
    beforeDrop.map((value) => value.id),
  );
  const unchanged: Session = await (
    await page.request.get("/api/sessions/" + explorer.id)
  ).json();
  expect(unchanged.attachments).toEqual([]);
  expect(unchanged.draft).toBe("Keep the original project draft.");
  await expect(page.locator(".attachments")).toHaveCount(0);
  await expect(
    page.locator('input[type="file"][accept=".lattice"]'),
  ).toHaveCount(0);
});
