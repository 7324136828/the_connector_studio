import { test, expect, type Page } from "@playwright/test";
import { mkdirSync, realpathSync, writeFileSync } from "node:fs";
import { join, relative, sep } from "node:path";
import type { Project, Session } from "../../frontend/src/services/api";

test.setTimeout(60000);

async function makeProjectTree(page: Page, label: string) {
  const response = await page.request.post("/api/projects", {
    data: { name: label + " " + Date.now() },
  });
  expect(response.status()).toBe(201);
  const project: Project = await response.json();
  const testRoot = realpathSync(process.env.STUDIO_E2E_DATA!);
  expect(realpathSync(project.path).startsWith(testRoot + sep)).toBeTruthy();
  mkdirSync(join(project.path, "files", "source", "nested"), {
    recursive: true,
  });
  mkdirSync(join(project.path, "files", "source-backup"), { recursive: true });
  writeFileSync(join(project.path, "files", "top.txt"), "top-level fixture");
  writeFileSync(
    join(project.path, "files", "source", "inner.txt"),
    "nested fixture",
  );
  writeFileSync(
    join(project.path, "files", "source", "nested", "deep.txt"),
    "deep fixture",
  );
  writeFileSync(
    join(project.path, "files", "source-backup", "outside.txt"),
    "sibling fixture",
  );
  return project;
}

async function selectProject(page: Page, project: Project) {
  await page
    .locator("header summary")
    .filter({ hasText: /^Project$/ })
    .click();
  await page
    .locator("header .dropdown")
    .getByRole("button", { name: "New Project", exact: true })
    .click();
  const dialog = page.getByTestId("new-item-dialog");
  await dialog.getByRole("option").filter({ hasText: project.name }).click();
  await dialog.getByRole("button", { name: "Open Selected", exact: true }).click();
  await expect(page.locator(".project-name")).toContainText(project.name);
  await expect(
    page.locator(".file-list").getByTitle("files/source", { exact: true }),
  ).toBeVisible();
}

function entry(page: Page, path: string) {
  return page.locator(".file-list").getByTitle(path, { exact: true });
}

async function makeSession(page: Page, project: Project, title: string) {
  const response = await page.request.post("/api/sessions", {
    data: { title, project_id: project.id },
  });
  expect(response.status()).toBe(201);
  const session: Session = await response.json();
  expect(session.saved_path).toBeTruthy();
  return {
    session,
    path: relative(project.path, session.saved_path!).split(sep).join("/"),
  };
}

test("Explorer folders toggle accessibly, preserve nested state, and keep file/session clicks working", async ({
  page,
}) => {
  const project = await makeProjectTree(page, "Expandable Explorer");
  const first = await makeSession(page, project, "Explorer file interactions");
  const second = await makeSession(page, project, "Saved Explorer discussion");
  await page.goto("/");
  await selectProject(page, project);
  const source = entry(page, "files/source");
  const nested = entry(page, "files/source/nested");
  const deep = entry(page, "files/source/nested/deep.txt");
  await expect(source).toBeEnabled();
  await expect(source).toHaveAttribute("aria-expanded", "true");
  await expect(nested).toHaveAttribute("aria-expanded", "true");
  await expect(deep).toBeVisible();
  await nested.click();
  await expect(nested).toHaveAttribute("aria-expanded", "false");
  await expect(deep).toHaveCount(0);
  await expect(entry(page, "files/source/inner.txt")).toBeVisible();
  await source.click();
  await expect(source).toHaveAttribute("aria-expanded", "false");
  await expect(nested).toHaveCount(0);
  await expect(entry(page, "files/source/inner.txt")).toHaveCount(0);
  await expect(entry(page, "files/source-backup/outside.txt")).toBeVisible();
  await source.click();
  await expect(nested).toHaveAttribute("aria-expanded", "false");
  await expect(deep).toHaveCount(0);
  await nested.focus();
  await page.keyboard.press("Enter");
  await expect(nested).toHaveAttribute("aria-expanded", "true");
  await expect(deep).toBeVisible();
  await entry(page, first.path).click();
  await expect(page.getByLabel("Session title", { exact: true })).toHaveValue(
    first.session.title,
  );
  await deep.click();
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue(
    " @files/source/nested/deep.txt",
  );
  await entry(page, second.path).click();
  await expect(page.getByLabel("Session title", { exact: true })).toHaveValue(
    second.session.title,
  );
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue("");
  const sessionsFolder = entry(page, "sessions");
  await sessionsFolder.click();
  await expect(sessionsFolder).toHaveAttribute("aria-expanded", "false");
  await expect(entry(page, first.path)).toHaveCount(0);
  await expect(entry(page, second.path)).toHaveCount(0);
  await sessionsFolder.click();
  await expect(entry(page, second.path)).toBeVisible();
  await page.screenshot({
    path: "test-results/explorer-expanded.png",
    fullPage: true,
  });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.getByRole("button", { name: "files", exact: true }).click();
  await expect(source).toBeVisible();
  await source.click();
  await expect(source).toHaveAttribute("aria-expanded", "false");
  await expect(deep).toHaveCount(0);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= window.innerWidth,
    ),
  ).toBeTruthy();
  await page.screenshot({
    path: "test-results/explorer-mobile.png",
    fullPage: true,
  });
});

test("Explorer collapse is project-scoped and survives directory polling", async ({
  page,
}) => {
  const first = await makeProjectTree(page, "Explorer scope one");
  const second = await makeProjectTree(page, "Explorer scope two");
  await page.goto("/");
  await selectProject(page, first);
  await entry(page, "files/source/nested").click();
  await entry(page, "files/source").click();
  await selectProject(page, second);
  await expect(entry(page, "files/source")).toHaveAttribute(
    "aria-expanded",
    "true",
  );
  await expect(entry(page, "files/source/nested")).toHaveAttribute(
    "aria-expanded",
    "true",
  );
  await expect(entry(page, "files/source/nested/deep.txt")).toBeVisible();
  await selectProject(page, first);
  await expect(entry(page, "files/source")).toHaveAttribute(
    "aria-expanded",
    "false",
  );
  await expect(entry(page, "files/source/nested")).toHaveCount(0);
  await entry(page, "files/source").click();
  await expect(entry(page, "files/source/nested")).toHaveAttribute(
    "aria-expanded",
    "false",
  );
  const refreshed = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/projects/" + first.id + "/files") &&
      response.request().method() === "GET",
  );
  writeFileSync(
    join(first.path, "files", "source", "nested", "arrived-after-refresh.txt"),
    "polling fixture",
  );
  expect((await refreshed).status()).toBe(200);
  await expect(entry(page, "files/source/nested")).toHaveAttribute(
    "aria-expanded",
    "false",
  );
  await expect(
    entry(page, "files/source/nested/arrived-after-refresh.txt"),
  ).toHaveCount(0);
  await entry(page, "files/source/nested").click();
  await expect(
    entry(page, "files/source/nested/arrived-after-refresh.txt"),
  ).toBeVisible({ timeout: 10000 });
  await page.screenshot({
    path: "test-results/explorer-project-scope.png",
    fullPage: true,
  });
});
