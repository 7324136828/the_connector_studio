import { test, expect } from "@playwright/test";
import { join } from "node:path";

test("New Project uses a browsable parent folder and the Plugins button is removed", async ({
  page,
}) => {
  await page.goto("/");
  await expect(
    page.getByRole("button", { name: "plugins", exact: true }),
  ).toHaveCount(0);
  const roots = await (await page.request.get("/api/filesystem/roots")).json();
  const parentName = "Browse target " + Date.now();
  const parentResponse = await page.request.post("/api/filesystem/folders", {
    data: { parent_path: roots.default_path, name: parentName },
  });
  expect(parentResponse.status()).toBe(201);
  const parent = await parentResponse.json();
  for (const name of [
    "Documents",
    "Downloads",
    "Pictures",
    "Research",
    "Sessions",
    "Templates",
    "Videos",
    "Work",
  ]) {
    expect(
      (
        await page.request.post("/api/filesystem/folders", {
          data: { parent_path: parent.path, name },
        })
      ).status(),
    ).toBe(201);
  }

  await page.getByRole("button", { name: "New Project", exact: true }).click();
  const dialog = page.getByTestId("new-item-dialog");
  await expect(
    dialog.getByRole("tab", { name: "Projects", exact: true }),
  ).toHaveAttribute("aria-selected", "true");
  await expect(
    dialog.getByRole("tab", { name: "Files", exact: true }),
  ).toBeVisible();
  await expect(
    dialog.getByRole("tab", { name: "Sessions", exact: true }),
  ).toBeVisible();
  await dialog
    .getByLabel("Project name", { exact: true })
    .fill("Chosen parent project");
  await page.screenshot({
    path: "test-results/new-project.png",
    fullPage: true,
  });
  await dialog.getByRole("button", { name: /Browse/ }).click();

  const browser = page.getByTestId("file-browser-dialog");
  await expect(browser).toBeVisible();
  await expect(browser.getByLabel("Folder path", { exact: true })).toHaveValue(
    roots.default_path,
  );
  await browser
    .getByLabel("Search folders and files", { exact: true })
    .fill(parentName);
  const entry = browser
    .getByTestId("browser-entry")
    .filter({ hasText: parentName });
  await expect(entry).toHaveCount(1);
  await entry.dblclick();
  await expect(browser.getByLabel("Folder path", { exact: true })).toHaveValue(
    parent.path,
  );
  await expect(browser.getByTestId("browser-entry")).toHaveCount(8);
  await page.screenshot({
    path: "test-results/file-browser.png",
    fullPage: true,
  });
  await browser
    .getByRole("button", { name: "Select Folder", exact: true })
    .click();
  await expect(browser).not.toBeVisible();
  await expect(dialog.getByLabel("Parent folder", { exact: true })).toHaveValue(
    parent.path,
  );
  await dialog.getByRole("button", { name: "Create", exact: true }).click();
  await expect(dialog).not.toBeVisible();

  const projects = await (await page.request.get("/api/projects")).json();
  const created = projects.find(
    (project: { name: string }) => project.name === "Chosen parent project",
  );
  expect(created.path).toBe(
    parent.path +
      (parent.path.includes("\\") ? "\\" : "/") +
      "Chosen parent project",
  );
  await expect(page.locator(".project-name")).toContainText(
    "Chosen parent project",
  );
  const files = await (
    await page.request.get("/api/projects/" + created.id + "/files")
  ).json();
  expect(files.map((file: { name: string }) => file.name)).toEqual(
    expect.arrayContaining(["files", "sessions", "project.connector"]),
  );
});

test("filesystem picker displays real files and opens existing project folders", async ({
  page,
}) => {
  const roots = await (await page.request.get("/api/filesystem/roots")).json();
  const projectName = "Existing project " + Date.now();
  const projectResponse = await page.request.post("/api/projects", {
    data: { name: projectName, parent_path: roots.default_path },
  });
  expect(projectResponse.status()).toBe(201);
  const project = await projectResponse.json();
  await page.request.post("/api/projects/" + project.id + "/files", {
    data: { name: "notes.md", content: "Browser fixture" },
  });
  await page.goto("/");
  await page.getByRole("button", { name: "New Project", exact: true }).click();
  const dialog = page.getByTestId("new-item-dialog");
  await dialog.getByRole("button", { name: /Open Existing/ }).click();
  const browser = page.getByTestId("file-browser-dialog");
  await browser
    .getByTestId("browser-entry")
    .filter({ hasText: projectName })
    .dblclick();
  await expect(
    browser
      .getByTestId("browser-entry")
      .filter({ hasText: "project.connector" }),
  ).toHaveAttribute("data-entry-kind", "file");
  await browser
    .getByTestId("browser-entry")
    .filter({ hasText: /^files/ })
    .dblclick();
  await expect(
    browser.getByTestId("browser-entry").filter({ hasText: "notes.md" }),
  ).toHaveAttribute("data-entry-kind", "file");
  await browser
    .getByRole("button", { name: "Up one folder", exact: true })
    .click();
  await expect(browser.getByLabel("Folder path", { exact: true })).toHaveValue(
    project.path,
  );
  await browser
    .getByRole("button", { name: "Select Folder", exact: true })
    .click();
  await expect(dialog).not.toBeVisible();
  await expect(page.locator(".project-name")).toContainText(projectName);
});

test("folder picker rejects an invalid path and keeps the New dialog on cancel", async ({
  page,
}) => {
  await page.goto("/");
  await page.getByRole("button", { name: "New Project", exact: true }).click();
  const dialog = page.getByTestId("new-item-dialog");
  await dialog
    .getByLabel("Project name", { exact: true })
    .fill("Uncreated project");
  let firstListing = true;
  let releaseListing = () => {};
  const pendingListing = new Promise<void>((resolve) => {
    releaseListing = resolve;
  });
  await page.route(/\/api\/filesystem\/list\?/, async (route) => {
    if (firstListing) {
      firstListing = false;
      const response = await route.fetch();
      await pendingListing;
      await route.fulfill({ response });
    } else {
      await route.continue();
    }
  });
  await dialog.getByRole("button", { name: /Browse/ }).click();
  const browser = page.getByTestId("file-browser-dialog");
  await browser
    .getByLabel("Folder path", { exact: true })
    .fill("/__connector_browser_forbidden__");
  releaseListing();
  await expect(
    browser.getByRole("button", { name: "Select Folder", exact: true }),
  ).toBeEnabled();
  await expect(browser.getByLabel("Folder path", { exact: true })).toHaveValue(
    "/__connector_browser_forbidden__",
  );
  await browser.getByLabel("Folder path", { exact: true }).press("Enter");
  await expect(browser.getByRole("alert")).toBeVisible();
  await browser.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(browser).not.toBeVisible();
  await expect(dialog.getByLabel("Project name", { exact: true })).toHaveValue(
    "Uncreated project",
  );
  await dialog.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(dialog).not.toBeVisible();
});

test("the picker creates folders, switches views and preserves keyboard focus", async ({
  page,
}) => {
  await page.goto("/");
  await page.getByRole("button", { name: "New Project", exact: true }).click();
  const dialog = page.getByTestId("new-item-dialog");
  const browse = dialog.getByRole("button", { name: /Browse/ });
  await browse.click();
  const browser = page.getByTestId("file-browser-dialog");
  const initial = await browser
    .getByLabel("Folder path", { exact: true })
    .inputValue();
  await browser
    .getByRole("button", { name: "New Folder", exact: true })
    .click();
  await browser
    .getByLabel("Folder name", { exact: true })
    .fill("UI folder " + Date.now());
  await browser
    .getByRole("button", { name: "Create folder", exact: true })
    .click();
  await expect(
    browser.getByLabel("Folder path", { exact: true }),
  ).not.toHaveValue(initial);
  const createdPath = await browser
    .getByLabel("Folder path", { exact: true })
    .inputValue();
  await expect(
    browser.getByRole("button", { name: "Select Folder", exact: true }),
  ).toBeEnabled();
  await browser.getByRole("button", { name: "Back", exact: true }).click();
  await expect(browser.getByLabel("Folder path", { exact: true })).toHaveValue(
    initial,
  );
  await expect(
    browser.getByTestId("browser-entry").filter({ hasText: "UI folder " }),
  ).toBeVisible();
  await browser.getByRole("button", { name: "List view", exact: true }).click();
  await expect(browser.locator(".fs-entries")).toHaveClass(/fs-list/);
  await browser.getByRole("button", { name: "Forward", exact: true }).click();
  await expect(browser.getByLabel("Folder path", { exact: true })).toHaveValue(
    createdPath,
  );
  await page.keyboard.press("Escape");
  await expect(browser).not.toBeVisible();
  await expect(browse).toBeFocused();
  await expect(dialog).toBeVisible();
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(
    dialog.getByRole("button", { name: "Create", exact: true }),
  ).toBeVisible();
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= window.innerWidth,
    ),
  ).toBeTruthy();
  await page.screenshot({
    path: "test-results/new-project-mobile.png",
    fullPage: true,
  });
  await browse.click();
  await expect(
    browser.getByRole("button", { name: "Select Folder", exact: true }),
  ).toBeEnabled();
  expect(
    await browser.evaluate(
      (element) => element.scrollWidth <= element.clientWidth,
    ),
  ).toBeTruthy();
  await page.screenshot({
    path: "test-results/file-browser-mobile.png",
    fullPage: true,
  });
});

test("direct Open Project starts in the allowed root and displays open failures inside the picker", async ({
  page,
}) => {
  await page.goto("/");
  const roots = await (await page.request.get("/api/filesystem/roots")).json();
  await page
    .locator(".menu")
    .filter({ hasText: "New Project" })
    .locator("summary")
    .click();
  await page.getByRole("button", { name: "Open Project", exact: true }).click();
  const browser = page.getByTestId("file-browser-dialog");
  await expect(browser.getByLabel("Folder path", { exact: true })).toHaveValue(
    roots.default_path,
  );
  await expect(
    browser.getByRole("button", { name: "Select Folder", exact: true }),
  ).toBeEnabled();
  await page.route("**/api/projects/open", (route) =>
    route.fulfill({
      status: 422,
      contentType: "application/json",
      body: JSON.stringify({ detail: "Project cannot be opened" }),
    }),
  );
  await browser
    .getByRole("button", { name: "Select Folder", exact: true })
    .click();
  await expect(browser.getByRole("alert")).toContainText(
    "Project cannot be opened",
  );
  await expect(browser).toBeVisible();
  await expect(
    browser.getByRole("button", { name: "Select Folder", exact: true }),
  ).toBeEnabled();
});

test("New dialog tabs create project files and saved work sessions", async ({
  page,
}) => {
  await page.goto("/");
  const roots = await (await page.request.get("/api/filesystem/roots")).json();
  await page.getByRole("button", { name: "New Project", exact: true }).click();
  const dialog = page.getByTestId("new-item-dialog");
  await dialog.getByRole("tab", { name: "Sessions", exact: true }).click();
  await expect(dialog.getByLabel("Session name", { exact: true })).toBeDisabled();
  await expect(dialog.getByRole("button", { name: "Create", exact: true })).toBeDisabled();
  await dialog.getByRole("tab", { name: "Projects", exact: true }).click();
  await expect(dialog.getByLabel("Parent folder", { exact: true })).toHaveValue(
    roots.default_path,
  );
  const projectName = "Tabbed project " + Date.now();
  await dialog.getByLabel("Project name", { exact: true }).fill(projectName);
  await dialog.getByRole("button", { name: "Create", exact: true }).click();
  await expect(dialog).not.toBeVisible();
  await page
    .locator(".menu")
    .filter({ hasText: "New Project" })
    .locator("summary")
    .click();
  await page.getByRole("button", { name: "New Project", exact: true }).click();
  await dialog.getByRole("tab", { name: "Files", exact: true }).click();
  await dialog.getByLabel("File name", { exact: true }).fill("ui-notes.md");
  await dialog.getByRole("button", { name: "Create", exact: true }).click();
  await expect(dialog).not.toBeVisible();
  await expect(page.locator(".file-list")).toContainText("ui-notes.md");
  await page
    .locator(".menu")
    .filter({ hasText: "New Project" })
    .locator("summary")
    .click();
  await page.getByRole("button", { name: "New Project", exact: true }).click();
  await dialog.getByRole("tab", { name: "Sessions", exact: true }).click();
  await dialog
    .getByLabel("Session name", { exact: true })
    .fill("Saved UI session");
  await dialog.getByRole("button", { name: "Create", exact: true }).click();
  await expect(dialog).not.toBeVisible();
  await expect(page.getByLabel("Session title", { exact: true })).toHaveValue(
    "Saved UI session",
  );
  const sessions = await (await page.request.get("/api/sessions")).json();
  const saved = sessions.find(
    (session: { title: string }) => session.title === "Saved UI session",
  );
  expect(saved.saved_path).toContain(projectName);
  expect(saved.saved_path).toMatch(/\.lattice$/);
});

test("local folder creation and project browsing work outside the workspace", async ({
  page,
}) => {
  const testData = process.env.STUDIO_E2E_DATA;
  if (!testData) throw new Error("Missing test data directory");
  const roots = await (await page.request.get("/api/filesystem/roots")).json();
  const parentResponse = await page.request.post("/api/filesystem/folders", {
    headers: { Origin: "http://127.0.0.1:5173" },
    data: { parent_path: testData, name: "External folder " + Date.now() },
  });
  expect(parentResponse.status()).toBe(201);
  const parent = await parentResponse.json();
  expect(parent.path.startsWith(roots.default_path)).toBeFalsy();
  await page.goto("/");
  await page.getByRole("button", { name: "New Project", exact: true }).click();
  const dialog = page.getByTestId("new-item-dialog");
  await dialog
    .getByLabel("Project name", { exact: true })
    .fill("External UI project");
  await dialog.getByRole("button", { name: /Browse/ }).click();
  const browser = page.getByTestId("file-browser-dialog");
  await expect(
    browser.getByRole("button", { name: "Select Folder", exact: true }),
  ).toBeEnabled();
  await browser.getByLabel("Folder path", { exact: true }).fill(parent.path);
  await browser.getByLabel("Folder path", { exact: true }).press("Enter");
  await expect(
    browser.getByLabel("Selected folder", { exact: true }),
  ).toHaveValue(parent.path);
  await browser
    .getByRole("button", { name: "New Folder", exact: true })
    .click();
  await browser
    .getByLabel("Folder name", { exact: true })
    .fill("Chosen parent");
  await browser
    .getByRole("button", { name: "Create folder", exact: true })
    .click();
  const chosenParent = join(parent.path, "Chosen parent");
  await expect(
    browser.getByLabel("Selected folder", { exact: true }),
  ).toHaveValue(chosenParent);
  await browser
    .getByRole("button", { name: "Select Folder", exact: true })
    .click();
  await expect(dialog.getByLabel("Parent folder", { exact: true })).toHaveValue(
    chosenParent,
  );
  await dialog.getByRole("button", { name: "Create", exact: true }).click();
  await expect(dialog).not.toBeVisible();
  const projects = await (await page.request.get("/api/projects")).json();
  const created = projects.find(
    (project: { name: string }) => project.name === "External UI project",
  );
  expect(created.path).toBe(join(chosenParent, "External UI project"));
});
