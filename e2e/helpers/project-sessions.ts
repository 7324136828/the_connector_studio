import { expect, type Page } from "@playwright/test";
import { realpathSync, writeFileSync } from "node:fs";
import { isAbsolute, join, relative } from "node:path";
import type { Project, Session } from "../../frontend/src/services/api";

export async function createProject(page: Page, label: string): Promise<Project> {
  const response = await page.request.post("/api/projects", {
    data: { name: label + " " + Date.now() },
  });
  expect(response.status()).toBe(201);
  return response.json();
}

export async function selectProject(page: Page, project: Pick<Project, "id" | "name" | "path">) {
  // Reopening makes this project recent even when snapshot checks opened others.
  expect((await page.request.post("/api/projects/open", { data: { path: project.path } })).ok()).toBeTruthy();
  const menu = page.locator("header summary").filter({ hasText: /^Project$/ });
  if (!(await page.getByRole("button", { name: "New Project", exact: true }).isVisible())) await menu.click();
  await page.getByRole("button", { name: "New Project", exact: true }).click();
  const dialog = page.getByTestId("new-item-dialog");
  await dialog.getByRole("option").filter({ hasText: project.name }).click();
  await dialog.getByRole("button", { name: "Open Selected", exact: true }).click();
  await expect(dialog).not.toBeVisible();
}

export async function openProjectSession(page: Page, project: Pick<Project, "id" | "name" | "path">, session: Pick<Session, "saved_path">) {
  await selectProject(page, project);
  if (!session.saved_path) throw new Error("Session has no project file");
  await page.locator(".file-list").getByTitle(relative(project.path, session.saved_path).replaceAll("\\", "/"), { exact: true }).click();
}

export function writeProjectSnapshot(project: Pick<Project, "path">, bytes: Buffer, filename = "snapshot.lattice") {
  const testRoot = process.env.STUDIO_E2E_DATA;
  if (!testRoot) throw new Error("Missing isolated E2E data directory");
  const parent = realpathSync(join(project.path, "files"));
  const within = relative(realpathSync(testRoot), parent);
  if (!within || isAbsolute(within) || within === ".." || within.startsWith("..\\") || within.startsWith("../")) throw new Error("Snapshot target must be inside E2E data");
  if (filename.includes("/") || filename.includes("\\")) throw new Error("Snapshot filename must be a basename");
  writeFileSync(join(parent, filename), bytes, { flag: "wx" });
  return "files/" + filename;
}

export async function readNativeSnapshot(page: Page, bytes: Buffer, label = "Inspect disk snapshot"): Promise<Session> {
  // Decode an actual file copy, rather than returning the session cached in SQLite.
  const project = await createProject(page, label);
  const path = writeProjectSnapshot(project, bytes);
  const response = await page.request.post("/api/projects/" + project.id + "/sessions/open", { data: { path } });
  expect(response.status()).toBe(200);
  return response.json();
}
